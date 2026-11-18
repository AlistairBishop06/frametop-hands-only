#include "nets.h"
#include "clahe.h"

#include <allocator.h>
#include <mat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kPalmSize = 192, kHandSize = 224;
const int kRoiLandmarks[] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};

// 2x3 affine taking crop pixels (0..out) to image pixels.
void crop_matrix(V2 center, double size, double rotation, int out, float tm[6]) {
    const double c = std::cos(rotation), s = std::sin(rotation), k = size / out;
    tm[0] = float(c * k), tm[1] = float(-s * k), tm[3] = float(s * k), tm[4] = float(c * k);
    tm[2] = float(center[0] - (tm[0] + tm[1]) * out / 2.0);
    tm[5] = float(center[1] - (tm[3] + tm[4]) * out / 2.0);
}

V2 to_image(const float tm[6], double x, double y) {
    return {tm[0] * x + tm[1] * y + tm[2], tm[3] * x + tm[4] * y + tm[5]};
}

// Linear stretch of the 1st..99th percentile to 0..255, in place.
void stretch(uint8_t *img, int n) {
    int hist[256] = {};
    const int total = n * n;
    for (int i = 0; i < total; ++i) ++hist[img[i]];
    int lo = 0, hi = 255, acc = 0;
    for (int v = 0; v < 256; ++v)
        if ((acc += hist[v]) > total / 100) { lo = v; break; }
    acc = 0;
    for (int v = 255; v >= 0; --v)
        if ((acc += hist[v]) > total / 100) { hi = v; break; }
    if (hi <= lo) return;
    for (int i = 0; i < total; ++i) img[i] = uint8_t(std::clamp((img[i] - lo) * 255 / (hi - lo), 0, 255));
}

// Parallelism is across camera crops. Each worker owns its intermediate
// tensors and temporary image, so it can reuse memory without allocator locks.
struct InferenceScratch {
    ncnn::UnlockedPoolAllocator blobs, workspace;
    uint8_t pixels[kHandSize * kHandSize];

    ncnn::Extractor extractor(const ncnn::Net &net) {
        auto ex = net.create_extractor();
        ex.set_blob_allocator(&blobs);
        ex.set_workspace_allocator(&workspace);
        return ex;
    }
};
thread_local InferenceScratch scratch;

// Both models take three identical float planes. Normalize once, then copy the
// plane; this is the same byte-to-float multiplication as ncnn's Scale layer.
template<int N>
ncnn::Mat crop(const Image &img, const float tm[6], const Contrast &contrast) {
    uint8_t *patch = scratch.pixels;
    ncnn::warpaffine_bilinear_c1(img.data, img.width, img.height, img.stride, patch, N, N, N, tm, 0, 0);
    if (contrast.mode == Contrast::Clahe) fh_crop::clahe<N>(patch, contrast.clip);
    else if (contrast.mode == Contrast::Stretch) stretch(patch, N);
    ncnn::Mat m(N, N, 3, sizeof(float), &scratch.blobs);
    if (m.empty()) return m;
    float *plane = m.channel(0);
    for (int i = 0; i < N * N; ++i) plane[i] = patch[i] * (1 / 255.f);
    std::memcpy(m.channel(1).data, plane, N * N * sizeof(float));
    std::memcpy(m.channel(2).data, plane, N * N * sizeof(float));
    return m;
}

}  // namespace

bool Contrast::parse(const std::string &s, Contrast &out) {
    if (s == "none") return out.mode = None, true;
    if (s == "stretch") return out.mode = Stretch, true;
    if (s.rfind("clahe", 0) == 0) {
        out.mode = Clahe;
        out.clip = s.size() > 6 && s[5] == ':' ? std::atof(s.c_str() + 6) : 2.0;
        return out.clip > 0;
    }
    return false;
}

bool Contrast::parse_pair(const std::string &s, Contrast &palm, Contrast &hand) {
    const size_t slash = s.find('/');
    if (slash == std::string::npos) return parse(s, palm) && parse(s, hand);
    return parse(s.substr(0, slash), palm) && parse(s.substr(slash + 1), hand);
}

namespace {

bool load_net(ncnn::Net &net, const std::string &base, std::string &err) {
    net.opt.num_threads = 1;
    net.opt.use_local_pool_allocator = false; // each worker supplies its own pool
    net.opt.use_vulkan_compute = false;
    net.opt.use_fp16_packed = net.opt.use_fp16_storage = net.opt.use_fp16_arithmetic = true;
    if (net.load_param((base + ".param").c_str()) || net.load_model((base + ".bin").c_str())) {
        err = "can't load " + base + ".param/.bin";
        return false;
    }
    return true;
}

}  // namespace

Roi Palm::roi() const {
    const V2 a = kp[0], b = kp[2];
    const double rot = wrap_angle(M_PI / 2 - std::atan2(-(b[1] - a[1]), b[0] - a[0]));
    const double h = size[1];
    const V2 shift{-h * -0.5 * std::sin(rot), h * -0.5 * std::cos(rot)};
    return {center + shift, std::max(size[0], size[1]) * 2.6, rot};
}

Roi roi_from_points(const V2 *p) {
    const V2 w = p[0];
    V2 m = (p[5] + p[13]) * 0.5;
    m = (m + p[9]) * 0.5;
    const double rot = wrap_angle(M_PI / 2 - std::atan2(-(m[1] - w[1]), m[0] - w[0]));
    V2 lo{1e9, 1e9}, hi{-1e9, -1e9};
    for (int i : kRoiLandmarks)
        for (int k = 0; k < 2; ++k) lo[k] = std::min(lo[k], p[i][k]), hi[k] = std::max(hi[k], p[i][k]);
    V2 center = (lo + hi) * 0.5;
    const double c = std::cos(-rot), s = std::sin(-rot);
    V2 qlo{1e9, 1e9}, qhi{-1e9, -1e9};
    for (int i : kRoiLandmarks) {
        const V2 d = p[i] - center;
        const V2 q{d[0] * c - d[1] * s, d[0] * s + d[1] * c};
        for (int k = 0; k < 2; ++k) qlo[k] = std::min(qlo[k], q[k]), qhi[k] = std::max(qhi[k], q[k]);
    }
    const V2 mid = (qlo + qhi) * 0.5;
    const double c2 = std::cos(rot), s2 = std::sin(rot);
    center = center + V2{mid[0] * c2 - mid[1] * s2, mid[0] * s2 + mid[1] * c2};
    const double w2 = qhi[0] - qlo[0], h2 = qhi[1] - qlo[1];
    center = center + V2{-h2 * -0.1 * s2, h2 * -0.1 * c2};
    return {center, std::max(w2, h2) * 2.0, rot};
}

Roi Landmarks::next_roi() const { return roi_from_points(pts); }

bool Nets::load(const std::string &dir, bool int8, std::string &err) {
    const std::string suffix = int8 ? "-int8.ncnn" : ".ncnn";
    if (!load_net(palm_, dir + "/palm" + suffix, err) || !load_net(hand_, dir + "/hand" + suffix, err)) return false;
    // SSD anchors of palm_detection_full: strides 8 (2 per cell) and 16 (6 per cell)
    for (auto [stride, per] : {std::pair{8, 2}, std::pair{16, 6}}) {
        const int n = kPalmSize / stride;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                for (int k = 0; k < per; ++k) anchors_.push_back({(x + 0.5) / n * kPalmSize, (y + 0.5) / n * kPalmSize});
    }
    return true;
}

std::vector<Palm> Nets::palms(const Image &img, V2 center, double size, double rotation) const {
    float tm[6];
    crop_matrix(center, size, rotation, kPalmSize, tm);
    auto ex = scratch.extractor(palm_);
    const auto input = crop<kPalmSize>(img, tm, palm_contrast_);
    ncnn::Mat boxes, scores;
    if (input.empty() || ex.input("in0", input) || ex.extract("out0", boxes) ||
        ex.extract("out1", scores) || boxes.empty() || scores.empty() ||
        boxes.total() < anchors_.size() * 18 || scores.total() < anchors_.size()) return {};
    const float *raw = boxes, *logit = scores;
    const int n = int(anchors_.size());
    const float min_logit = std::log(0.5f / 0.5f);  // score 0.5
    struct Cand { V2 c, s; V2 kp[7]; double score; };
    std::vector<Cand> cand;
    for (int i = 0; i < n; ++i) {
        if (logit[i] <= min_logit) continue;
        const float *r = raw + i * 18;
        Cand c;
        c.c = {r[0] + anchors_[i][0], r[1] + anchors_[i][1]};
        c.s = {r[2], r[3]};
        for (int k = 0; k < 7; ++k) c.kp[k] = {r[4 + 2 * k] + anchors_[i][0], r[5 + 2 * k] + anchors_[i][1]};
        c.score = 1 / (1 + std::exp(-std::clamp(double(logit[i]), -100.0, 100.0)));
        cand.push_back(c);
    }
    // MediaPipe's weighted NMS: overlapping boxes are averaged, weighted by score
    std::sort(cand.begin(), cand.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
    std::vector<bool> used(cand.size());
    std::vector<Palm> out;
    for (size_t i = 0; i < cand.size(); ++i) {
        if (used[i]) continue;
        double wsum = 0;
        Cand acc{};
        for (size_t j = i; j < cand.size(); ++j) {
            if (used[j]) continue;
            const double ix = std::max(0.0, std::min(cand[i].c[0] + cand[i].s[0] / 2, cand[j].c[0] + cand[j].s[0] / 2) -
                                                std::max(cand[i].c[0] - cand[i].s[0] / 2, cand[j].c[0] - cand[j].s[0] / 2));
            const double iy = std::max(0.0, std::min(cand[i].c[1] + cand[i].s[1] / 2, cand[j].c[1] + cand[j].s[1] / 2) -
                                                std::max(cand[i].c[1] - cand[i].s[1] / 2, cand[j].c[1] - cand[j].s[1] / 2));
            const double inter = ix * iy;
            const double uni = cand[i].s[0] * cand[i].s[1] + cand[j].s[0] * cand[j].s[1] - inter;
            if (j != i && inter / (uni + 1e-9) <= 0.3) continue;
            used[j] = true;
            const double w = cand[j].score;
            wsum += w;
            acc.c = acc.c + cand[j].c * w;
            acc.s = acc.s + cand[j].s * w;
            for (int k = 0; k < 7; ++k) acc.kp[k] = acc.kp[k] + cand[j].kp[k] * w;
        }
        Palm p;
        const V2 c = acc.c * (1 / wsum);
        p.center = to_image(tm, c[0], c[1]);
        p.size = acc.s * (1 / wsum * size / kPalmSize);
        for (int k = 0; k < 7; ++k) {
            const V2 q = acc.kp[k] * (1 / wsum);
            p.kp[k] = to_image(tm, q[0], q[1]);
        }
        p.score = cand[i].score;
        out.push_back(p);
    }
    return out;
}

Landmarks Nets::landmarks(const Image &img, const Roi &roi) const {
    float tm[6];
    crop_matrix(roi.center, roi.size, roi.rotation, kHandSize, tm);
    auto ex = scratch.extractor(hand_);
    const auto input = crop<kHandSize>(img, tm, hand_contrast_);
    ncnn::Mat screen, presence, right, world;
    if (input.empty() || ex.input("in0", input) || ex.extract("out0", screen) || ex.extract("out1", presence) ||
        presence.empty() || !std::isfinite(presence[0]) ||
        ex.extract("out2", right) || ex.extract("out3", world) ||
        screen.empty() || right.empty() || world.empty() || screen.total() < 63 || world.total() < 63) return {};
    Landmarks lm;
    const float *s = screen, *w = world;
    if (!std::isfinite(right[0])) return {};
    for (int i = 0; i < 21; ++i) {
        if (!std::isfinite(s[3 * i]) || !std::isfinite(s[3 * i + 1])) return {};
        lm.pts[i] = to_image(tm, s[3 * i], s[3 * i + 1]);
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(w[3 * i + k])) return {};
            lm.world[i][k] = w[3 * i + k];
        }
    }
    lm.presence = presence[0];
    lm.right = right[0];
    return lm;
}

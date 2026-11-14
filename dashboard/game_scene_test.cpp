#include "game_scene.h"
#include <cassert>

int main() {
    assert(!IsGameScene(""));
    assert(!IsGameScene("openvr.tool.steamvr_environments"));
    assert(!IsGameScene("steam.app.250820"));
    assert(!IsGameScene("steam.app.353380"));
    assert(!IsGameScene("steam.app.unknown"));
    assert(IsGameScene("steam.app.546560"));
}

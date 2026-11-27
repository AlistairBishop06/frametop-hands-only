#pragma once

#include "ft_conf.h"

namespace framehands {

enum class InteractionMode { Aim, Touch, Gaze };

inline InteractionMode ReadInteractionMode() {
    const auto mode = ft_conf::Setting("HANDS_INTERACTION_MODE", "");
    if (mode == "aim") return InteractionMode::Aim;
    if (mode == "touch") return InteractionMode::Touch;
    if (mode == "gaze") return InteractionMode::Gaze;
    return ft_conf::Setting("HANDS_TOUCH", "0") == "1" ? InteractionMode::Touch : InteractionMode::Aim;
}

}  // namespace framehands

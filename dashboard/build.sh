#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/include
. ../scripts/openvr.sh
read -r -a openvr_libs <<< "$OPENVR_LIBS"
${CXX:-g++} -std=c++17 -O2 -Wall -o build/game_scene_test game_scene_test.cpp
build/game_scene_test
${CXX:-g++} -std=c++17 -O2 -Wall -Ibuild/include -o build/framehands-dashboard \
    framehands-dashboard.cpp "${openvr_libs[@]}"
strip --strip-unneeded build/framehands-dashboard

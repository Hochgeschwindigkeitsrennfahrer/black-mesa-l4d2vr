#pragma once

#include <string>

// The packaged SteamVR manifest already contains the two skeletal actions in
// /actions/base. Keep using that manifest so existing SteamVR bindings remain valid.
std::string BuildVrHandsActionManifestPath(const char* originalManifestPath);

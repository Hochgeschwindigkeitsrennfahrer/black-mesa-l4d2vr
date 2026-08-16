#pragma once

#include "vr_hand_types.h"

#include <string>

class VrHandAssetLoader
{
public:
    static bool LoadGlb(const std::string& path, VrHandMeshAsset& outAsset, std::string& outError);
    static bool LoadStaticGlb(const std::string& path, VrHandMeshAsset& outAsset, std::string& outError);
};

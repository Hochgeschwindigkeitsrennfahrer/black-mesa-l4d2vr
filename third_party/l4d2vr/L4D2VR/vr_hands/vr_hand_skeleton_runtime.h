#pragma once

#include "openvr.h"
#include "vr_hand_types.h"

#include <memory>
#include <string>
#include <vector>

class VrHandSkeletonRuntime
{
public:
    VrHandSkeletonRuntime();
    ~VrHandSkeletonRuntime();

    VrHandSkeletonRuntime(const VrHandSkeletonRuntime&) = delete;
    VrHandSkeletonRuntime& operator=(const VrHandSkeletonRuntime&) = delete;

    bool Initialize(vr::IVRInput* input, vr::VRActionHandle_t action, std::string& outError);
    bool Update(vr::IVRInput* input, vr::EVRSkeletalMotionRange motionRange, std::string& outError);
    bool BuildSkinningPalette(
        const VrHandMeshAsset& asset,
        std::vector<VrHandMatrixRows3x4>& outPalette,
        std::string& outError,
        const VrHandFingerCurlOverride* fingerCurlOverride = nullptr,
        const std::array<float, 5>* fingerMaxCurl = nullptr) const;

    bool IsInitialized() const;
    bool HasPose() const;
    int JointCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

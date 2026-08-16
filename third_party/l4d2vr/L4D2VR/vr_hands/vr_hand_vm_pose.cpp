#include "vr_hand_vm_pose.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <mutex>

namespace
{
    std::mutex s_VmPoseMutex;
    VrHandVmPose::Snapshot s_VmPoseSnapshot;
    std::uint32_t s_VmPoseSequence = 0;
}

namespace VrHandVmPose
{
    void Capture(
        const char* modelName,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const VrHandMatrix4& modelWorldMatrix,
        const std::vector<VrHandMatrix4>& boneWorldMatrices,
        bool autoGripAligned)
    {
        if (!modelName || !*modelName || boneNames.empty() || boneWorldMatrices.size() < boneNames.size())
            return;

        std::lock_guard<std::mutex> lock(s_VmPoseMutex);
        s_VmPoseSnapshot.modelName = modelName;
        s_VmPoseSnapshot.boneNames = boneNames;
        s_VmPoseSnapshot.boneParents = boneParents;
        s_VmPoseSnapshot.modelWorldMatrix = modelWorldMatrix;
        s_VmPoseSnapshot.boneWorldMatrices = boneWorldMatrices;
        s_VmPoseSnapshot.autoGripAligned = autoGripAligned;
        s_VmPoseSnapshot.tick = GetTickCount();
        s_VmPoseSnapshot.sequence = ++s_VmPoseSequence;
        s_VmPoseSnapshot.valid = true;
    }

    bool GetLatest(Snapshot& outSnapshot, std::uint32_t maxAgeMs)
    {
        std::lock_guard<std::mutex> lock(s_VmPoseMutex);
        if (!s_VmPoseSnapshot.valid)
            return false;

        const std::uint32_t now = GetTickCount();
        if (maxAgeMs != 0 && now - s_VmPoseSnapshot.tick > maxAgeMs)
            return false;

        outSnapshot = s_VmPoseSnapshot;
        return true;
    }
}

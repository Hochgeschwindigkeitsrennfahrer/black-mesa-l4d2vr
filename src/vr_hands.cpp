#include "vr_hands.h"

#include "bmvr_flags.h"
#include "game.h"
#include "vr.h"
#include "vr_hand_asset_loader.h"
#include "vr_hand_math.h"
#include "vr_hand_renderer_d3d9.h"

#include "openvr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr float kEyeZNear = 1.f;
    constexpr float kEyeZFar = 28377.f;
    constexpr float kSceneLightScale = 1.f;

    int FindNameIndex(const std::vector<std::string>& names, const std::string& name)
    {
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    VrHandMatrix4 BuildBindLocalMatrix(const VrHandMeshAsset& asset, int joint)
    {
        const int parent = asset.jointParents[static_cast<size_t>(joint)];
        if (parent >= 0 && parent < static_cast<int>(asset.inverseBindMatrices.size()))
        {
            return VrHandMath::Multiply(
                asset.inverseBindMatrices[static_cast<size_t>(parent)],
                asset.bindMatrices[static_cast<size_t>(joint)]);
        }
        return asset.bindMatrices[static_cast<size_t>(joint)];
    }

    VrHandMatrix4 MakeLocalZRotation(float radians)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        VrHandMath::Set(out, 0, 0, c);
        VrHandMath::Set(out, 0, 1, -s);
        VrHandMath::Set(out, 1, 0, s);
        VrHandMath::Set(out, 1, 1, c);
        return out;
    }

    // L4D2VR VrHandSkeletonRuntime::BuildSummaryCurlPalette — GLB bind locals
    // plus OpenVR summary curls. No ozz.
    bool AssetIsHevGlove(const VrHandMeshAsset& asset)
    {
        const std::string& path = asset.sourcePath;
        for (size_t i = 0; i + 8 < path.size(); ++i)
        {
            if ((path[i] == 'h' || path[i] == 'H')
                && (path[i + 1] == 'e' || path[i + 1] == 'E')
                && (path[i + 2] == 'v' || path[i + 2] == 'V')
                && path[i + 3] == '_'
                && (path[i + 4] == 'g' || path[i + 4] == 'G'))
            {
                return true;
            }
        }
        return false;
    }

    bool BuildSummaryCurlPalette(
        const VrHandMeshAsset& asset,
        const vr::VRSkeletalSummaryData_t& summary,
        std::vector<VrHandMatrixRows3x4>& outPalette,
        float gripCurlMin)
    {
        const bool rightHand = FindNameIndex(asset.jointNames, "wrist_r") >= 0;
        const bool leftHand = FindNameIndex(asset.jointNames, "wrist_l") >= 0;
        if (!rightHand && !leftHand)
            return false;

        const char suffix = rightHand ? 'r' : 'l';
        static const char* kFingerNames[vr::VRFinger_Count] =
        {
            "thumb",
            "index",
            "middle",
            "ring",
            "pinky"
        };
        static const float kMaxCurlRadians[vr::VRFinger_Count][3] =
        {
            { 0.75f, 0.90f, 0.65f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
        };

        std::vector<VrHandMatrix4> localMatrices(asset.jointNames.size(), VrHandMath::Identity());
        for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
            localMatrices[joint] = BuildBindLocalMatrix(asset, static_cast<int>(joint));

        // HEV ValveBiped index–pinky hinges are opposite SteamVR glove +Z.
        // Thumb already curls inward on the ripped HEV mesh. Do not negate
        // SteamVR vr_glove fallback assets.
        const bool hev = AssetIsHevGlove(asset);
        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            float curl = std::clamp(summary.flFingerCurl[finger], 0.0f, 1.0f);
            if (gripCurlMin > 0.f)
            {
                // Thumb (finger 0): keep nearly open so it does not clip into
                // the pistol grip. Other fingers use the grip floor.
                const float minCurl = (finger == 0) ? 0.05f : gripCurlMin;
                if (curl < minCurl)
                    curl = minCurl;
            }
            const float sign = (hev && finger > 0) ? -1.f : 1.f;
            for (int segment = 0; segment < 3; ++segment)
            {
                const std::string jointName = std::string("finger_") +
                    kFingerNames[finger] + "_" + static_cast<char>('0' + segment) + "_" + suffix;
                const int joint = FindNameIndex(asset.jointNames, jointName);
                if (joint < 0)
                    return false;

                localMatrices[static_cast<size_t>(joint)] = VrHandMath::Multiply(
                    localMatrices[static_cast<size_t>(joint)],
                    MakeLocalZRotation(sign * curl * kMaxCurlRadians[finger][segment]));
            }
        }

        std::vector<VrHandMatrix4> modelMatrices(asset.jointNames.size(), VrHandMath::Identity());
        std::vector<bool> resolved(asset.jointNames.size(), false);
        size_t unresolved = asset.jointNames.size();
        for (size_t pass = 0; pass < asset.jointNames.size() && unresolved > 0; ++pass)
        {
            bool progressed = false;
            for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
            {
                if (resolved[joint])
                    continue;

                const int parent = asset.jointParents[joint];
                if (parent >= 0 && (parent >= static_cast<int>(resolved.size()) || !resolved[static_cast<size_t>(parent)]))
                    continue;

                modelMatrices[joint] = (parent >= 0)
                    ? VrHandMath::Multiply(modelMatrices[static_cast<size_t>(parent)], localMatrices[joint])
                    : localMatrices[joint];
                resolved[joint] = true;
                --unresolved;
                progressed = true;
            }
            if (!progressed)
                return false;
        }

        outPalette.resize(asset.jointNames.size());
        for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
        {
            outPalette[joint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(
                modelMatrices[joint],
                asset.inverseBindMatrices[joint]));
        }
        return true;
    }

    void BuildBindPosePalette(const VrHandMeshAsset& asset, std::vector<VrHandMatrixRows3x4>& outPalette)
    {
        outPalette.resize(asset.jointNames.size());
        const VrHandMatrixRows3x4 identity = VrHandMath::ToRows3x4(VrHandMath::Identity());
        for (size_t i = 0; i < outPalette.size(); ++i)
            outPalette[i] = identity;
    }

    bool TryGetSummary(vr::IVRInput* input, vr::VRActionHandle_t action, vr::VRSkeletalSummaryData_t& out)
    {
        out = {};
        if (!input || action == vr::k_ulInvalidActionHandle)
            return false;
        return input->GetSkeletalSummaryData(action, vr::VRSummaryType_FromAnimation, &out)
            == vr::VRInputError_None;
    }
}

struct BmVrGloves::Impl
{
    struct Hand
    {
        const char* fileName = nullptr;
        VrHandMeshAsset asset;
        std::vector<VrHandMatrixRows3x4> palette;
    };

    Hand hands[2];
    VrHandRendererD3D9 renderer;
    bool assetAttempted = false;
    bool assetsLoaded = false;
    bool unavailable = false;
    bool loggedReady = false;
    std::string failure;

    void Fail(const std::string& reason)
    {
        unavailable = true;
        failure = reason;
        Game::logMsg("VR gloves fail: %s", reason.c_str());
    }

    bool ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const
    {
        outPath.clear();
        auto tryRoot = [&](const std::filesystem::path& root) -> bool {
            if (root.empty())
                return false;
            const std::filesystem::path path = root / "resources" / "rendermodels" / "vr_glove" / fileName;
            if (!std::filesystem::exists(path))
                return false;
            outPath = path.string();
            return true;
        };

        std::vector<char> buffer(vr::k_unMaxPropertyStringSize, '\0');
        uint32_t required = 0;
        if (vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required))
        {
            if (required > buffer.size())
            {
                buffer.assign(static_cast<size_t>(required) + 1u, '\0');
                vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required);
            }
            if (buffer.front() != '\0' && tryRoot(std::filesystem::path(buffer.data())))
                return true;
        }

        static const char* kFallbacks[] = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR",
            "C:\\Program Files\\Steam\\steamapps\\common\\SteamVR",
        };
        for (const char* root : kFallbacks)
        {
            if (tryRoot(root))
                return true;
        }
        return false;
    }

    bool ResolveLocalGlb(const char* fileName, std::string& outPath) const
    {
        outPath.clear();
        auto tryPath = [&](const std::filesystem::path& path) -> bool {
            if (path.empty() || !std::filesystem::exists(path))
                return false;
            outPath = path.string();
            return true;
        };

        wchar_t exeBuf[MAX_PATH]{};
        wchar_t dllBuf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        HMODULE mod = bmvr::DllModule();
        if (mod)
            GetModuleFileNameW(mod, dllBuf, MAX_PATH);

        const std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        const std::filesystem::path dllDir = dllBuf[0] ? std::filesystem::path(dllBuf).parent_path() : std::filesystem::path();
        const std::filesystem::path names[] = {
            exeDir / "VR" / "hands" / fileName,
            dllDir / "VR" / "hands" / fileName,
            exeDir / "VR" / fileName,
            dllDir / fileName,
        };
        for (const auto& path : names)
        {
            if (tryPath(path))
                return true;
        }
        return false;
    }

    bool EnsureAssets()
    {
        if (unavailable)
            return false;
        if (assetsLoaded)
            return true;
        if (assetAttempted)
            return false;
        assetAttempted = true;

        const bool wantHev = bmvr::g_VrHandsUseHevGloves;
        const char* hevNames[2] = { "hev_glove_left_model.glb", "hev_glove_right_model.glb" };
        const char* steamNames[2] = { "vr_glove_left_model.glb", "vr_glove_right_model.glb" };
        bool usingHev = false;
        if (wantHev)
        {
            std::string leftPath, rightPath;
            usingHev = ResolveLocalGlb(hevNames[0], leftPath) && ResolveLocalGlb(hevNames[1], rightPath);
        }

        auto loadPair = [&](const char* leftName, const char* rightName, bool local) -> bool {
            hands[0].fileName = leftName;
            hands[1].fileName = rightName;
            for (Hand& hand : hands)
            {
                std::string path;
                const bool found = local
                    ? ResolveLocalGlb(hand.fileName, path)
                    : ResolveSteamVrAssetPath(hand.fileName, path);
                if (!found)
                    return false;
                std::string error;
                if (!VrHandAssetLoader::LoadGlb(path, hand.asset, error))
                {
                    Game::logMsg("VR glove load failed %s: %s", path.c_str(), error.c_str());
                    return false;
                }
                BuildBindPosePalette(hand.asset, hand.palette);
            }
            return true;
        };

        if (usingHev && loadPair(hevNames[0], hevNames[1], true))
        {
            assetsLoaded = true;
            Game::logMsg(
                "VR gloves loaded source=hev left=%u verts right=%u verts",
                static_cast<unsigned>(hands[0].asset.vertices.size()),
                static_cast<unsigned>(hands[1].asset.vertices.size()));
            return true;
        }
        if (usingHev)
            Game::logMsg("HEV gloves missing or invalid, falling back to SteamVR GLB");
        if (!loadPair(steamNames[0], steamNames[1], false))
        {
            Fail("missing SteamVR glove assets");
            return false;
        }

        assetsLoaded = true;
        Game::logMsg(
            "VR gloves loaded source=steamvr left=%u verts right=%u verts",
            static_cast<unsigned>(hands[0].asset.vertices.size()),
            static_cast<unsigned>(hands[1].asset.vertices.size()));
        return true;
    }
};

BmVrGloves::BmVrGloves()
    : m_Impl(std::make_unique<Impl>())
{
}

BmVrGloves::~BmVrGloves() = default;

void BmVrGloves::OnDeviceLost()
{
    if (m_Impl)
        m_Impl->renderer.OnDeviceLost();
}

bool BmVrGloves::AssetsReady() const
{
    return m_Impl && m_Impl->assetsLoaded;
}

bool BmVrGloves::Failed() const
{
    return m_Impl && m_Impl->unavailable;
}

const std::string& BmVrGloves::FailureReason() const
{
    static const std::string kEmpty;
    return m_Impl ? m_Impl->failure : kEmpty;
}

bool BmVrGloves::WarmupGpu(IDirect3DDevice9* device)
{
    if (!device || !m_Impl || !m_Impl->EnsureAssets())
        return false;
    std::string error;
    bool ok = m_Impl->renderer.EnsureHandMesh(device, 0, m_Impl->hands[0].asset, error);
    if (!ok && !error.empty())
    {
        static int s_warmLog;
        if (s_warmLog < 4)
        {
            Game::logMsg("VR glove warmup hand=0: %s", error.c_str());
            ++s_warmLog;
        }
    }
    std::string rightError;
    if (!m_Impl->renderer.EnsureHandMesh(device, 1, m_Impl->hands[1].asset, rightError)
        && !rightError.empty())
    {
        static int s_warmRightLog;
        if (s_warmRightLog < 4)
        {
            Game::logMsg("VR glove warmup hand=1: %s", rightError.c_str());
            ++s_warmRightLog;
        }
    }
    return ok;
}

bool BmVrGloves::DrawForEye(
    IDirect3DDevice9* device,
    int stereoEye,
    const Vector& eyeOrigin,
    const Vector& viewAngles,
    float horizontalFovDegrees,
    float aspectRatio,
    float sourceUnitsPerMeter,
    float modelScale,
    vr::IVRInput* input,
    vr::VRActionHandle_t leftSkeleton,
    vr::VRActionHandle_t rightSkeleton,
    bool leftOk,
    const Vector& leftWorld,
    const QAngle& leftAngles,
    bool rightOk,
    const Vector& rightWorld,
    const QAngle& rightAngles)
{
    (void)stereoEye;
    if (!device || !m_Impl || !m_Impl->EnsureAssets())
        return false;

    const float scale = std::clamp(modelScale, 0.2f, 2.0f);
    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(
        horizontalFovDegrees, aspectRatio, kEyeZNear, kEyeZFar);
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(eyeOrigin, viewAngles);

    bool drew = false;
    const vr::VRActionHandle_t actions[2] = { leftSkeleton, rightSkeleton };
    const bool valid[2] = { leftOk, rightOk };
    const Vector* origins[2] = { &leftWorld, &rightWorld };
    const QAngle* angles[2] = { &leftAngles, &rightAngles };

    for (int i = 0; i < 2; ++i)
    {
        const bool rightHand = (i == 1);
        const bool showRight = g_Game && g_Game->m_VR
            ? g_Game->m_VR->WantsRightGloveVisible()
            : bmvr::g_VrHandsRightEnabled;
        if (rightHand && !showRight)
            continue;
        if (!valid[i])
            continue;

        Impl::Hand& hand = m_Impl->hands[i];
        vr::VRSkeletalSummaryData_t summary{};
        const bool gripCurl = rightHand && g_Game && g_Game->m_VR
            && g_Game->m_VR->WantsRightGloveWeaponGripCurl();
        if (TryGetSummary(input, actions[i], summary))
        {
            if (!BuildSummaryCurlPalette(hand.asset, summary, hand.palette, gripCurl ? 0.40f : 0.f))
                BuildBindPosePalette(hand.asset, hand.palette);
        }
        else if (hand.palette.empty())
        {
            BuildBindPosePalette(hand.asset, hand.palette);
        }

        const Vector rotOffset(
            bmvr::g_VrHandsPoseRotX + (rightHand && gripCurl ? bmvr::g_VrHandsRightGripRotX : 0.f),
            bmvr::g_VrHandsPoseRotY + (rightHand && gripCurl ? bmvr::g_VrHandsRightGripRotY : 0.f),
            bmvr::g_VrHandsPoseRotZ + (rightHand && gripCurl ? bmvr::g_VrHandsRightGripRotZ : 0.f));
        Vector posOffset(
            bmvr::g_VrHandsPoseOffX,
            bmvr::g_VrHandsPoseOffY,
            bmvr::g_VrHandsPoseOffZ);
        if (i == 0)
        {
            posOffset.x += bmvr::g_VrHandsLeftPoseOffX;
            posOffset.y += bmvr::g_VrHandsLeftPoseOffY;
            posOffset.z += bmvr::g_VrHandsLeftPoseOffZ;
        }
        else
        {
            posOffset.x += bmvr::g_VrHandsRightPoseOffX;
            posOffset.y += bmvr::g_VrHandsRightPoseOffY;
            posOffset.z += bmvr::g_VrHandsRightPoseOffZ;
            if (g_Game && g_Game->m_VR)
            {
                Vector palm{};
                g_Game->m_VR->GetRightGlovePalmOffsetMeters(palm);
                posOffset.x += palm.x;
                posOffset.y += palm.y;
                posOffset.z += palm.z;
            }
        }
        const VrHandMatrix4 world = VrHandMath::BuildControllerWorld(
            *origins[i],
            *angles[i],
            sourceUnitsPerMeter,
            scale,
            posOffset,
            rotOffset);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));

        std::string error;
        if (!m_Impl->renderer.Draw(
                device,
                i,
                hand.asset,
                hand.palette,
                world,
                wvp,
                stereoEye == 0 ? VrHandDrawPass::OverlayNoDepth : VrHandDrawPass::WorldDepth,
                kSceneLightScale,
                error))
        {
            if (!error.empty())
            {
                static int s_drawFailLog[2]{};
                if (s_drawFailLog[i] < 4)
                {
                    Game::logMsg("VR glove draw hand=%d: %s", i, error.c_str());
                    ++s_drawFailLog[i];
                }
            }
            continue;
        }
        drew = true;
    }

    if (drew && !m_Impl->loggedReady)
    {
        m_Impl->loggedReady = true;
        Game::logMsg("VR gloves drew eye=%d fov=%.1f aspect=%.3f scale=%.2f",
            stereoEye, horizontalFovDegrees, aspectRatio, scale);
    }
    return drew;
}

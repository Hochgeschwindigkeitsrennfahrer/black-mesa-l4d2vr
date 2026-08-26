#include "vr_hand_renderer_d3d9.h"

#include "vr_hand_math.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    constexpr int kMaxShaderBones = 64;
    constexpr int kBoneConstantStart = 8;
    constexpr float kWorldDepthRangeMin = 0.0f;
    constexpr float kWorldDepthRangeMax = 1.0f;
    // VR hands and detached viewmodel components are placed in world space and
    // share the eye scene projection. Keep their depth comparable with Source's
    // world depth so nearby geometry can occlude them.
    constexpr float kViewmodelDepthRangeMax = 1.0f;
    constexpr DWORD kVrHandOcclusionStencilBit = 0x80u;

    const char* kVertexShaderSource = R"HLSL(
float4 gWorldViewProjectionRows[4] : register(c0);
float4 gLightDirectionAmbient : register(c4);
float4 gWorldNormalRows[3] : register(c5);
float4 gBoneRows[192] : register(c8);

struct VS_INPUT
{
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float4 weights : BLENDWEIGHT0;
    float4 joints : BLENDINDICES0;
};

struct VS_OUTPUT
{
    float4 position : POSITION0;
    float2 uv : TEXCOORD0;
    float light : TEXCOORD1;
};

float3 TransformBonePosition(float3 position, int joint)
{
    int row = joint * 3;
    float4 v = float4(position, 1.0);
    return float3(dot(v, gBoneRows[row + 0]), dot(v, gBoneRows[row + 1]), dot(v, gBoneRows[row + 2]));
}

float3 TransformBoneNormal(float3 normal, int joint)
{
    int row = joint * 3;
    return float3(dot(normal, gBoneRows[row + 0].xyz), dot(normal, gBoneRows[row + 1].xyz), dot(normal, gBoneRows[row + 2].xyz));
}

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    int4 joints = int4(input.joints);
    float3 skinnedPosition =
        TransformBonePosition(input.position, joints.x) * input.weights.x +
        TransformBonePosition(input.position, joints.y) * input.weights.y +
        TransformBonePosition(input.position, joints.z) * input.weights.z +
        TransformBonePosition(input.position, joints.w) * input.weights.w;
    float3 skinnedNormal = normalize(
        TransformBoneNormal(input.normal, joints.x) * input.weights.x +
        TransformBoneNormal(input.normal, joints.y) * input.weights.y +
        TransformBoneNormal(input.normal, joints.z) * input.weights.z +
        TransformBoneNormal(input.normal, joints.w) * input.weights.w);
    float3 worldNormal = normalize(float3(
        dot(skinnedNormal, gWorldNormalRows[0].xyz),
        dot(skinnedNormal, gWorldNormalRows[1].xyz),
        dot(skinnedNormal, gWorldNormalRows[2].xyz)));
    float4 p = float4(skinnedPosition, 1.0);
    output.position = float4(
        dot(p, gWorldViewProjectionRows[0]),
        dot(p, gWorldViewProjectionRows[1]),
        dot(p, gWorldViewProjectionRows[2]),
        dot(p, gWorldViewProjectionRows[3]));
    output.uv = input.uv;
    float rawLight = (gLightDirectionAmbient.w +
        max(dot(worldNormal, -gLightDirectionAmbient.xyz), 0.0) * 0.62) * gWorldNormalRows[2].w;
    float visibleLight = saturate(rawLight);
    float sceneScale = saturate(gWorldNormalRows[2].w);
    float rescue = 0.16 * sqrt(sceneScale) * saturate((0.18 - visibleLight) / 0.18);
    output.light = saturate(visibleLight + rescue);
    return output;
}
)HLSL";

    const char* kPixelShaderSource = R"HLSL(
sampler2D gTexture : register(s0);

float4 main(float2 uv : TEXCOORD0, float light : TEXCOORD1) : COLOR0
{
    float4 color = tex2D(gTexture, uv);
    return float4(color.rgb * light, 1.0);
}
)HLSL";

    template <typename T>
    void SafeRelease(T*& pointer)
    {
        if (!pointer)
            return;
        pointer->Release();
        pointer = nullptr;
    }

    using tD3DCompile = pD3DCompile;

    tD3DCompile ResolveD3DCompile()
    {
        static tD3DCompile fn = nullptr;
        static bool tried = false;
        if (tried)
            return fn;
        tried = true;
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        if (!dll)
            dll = LoadLibraryA("d3dcompiler_43.dll");
        if (!dll)
            return nullptr;
        fn = reinterpret_cast<tD3DCompile>(GetProcAddress(dll, "D3DCompile"));
        return fn;
    }

    bool CompileHlsl(
        IDirect3DDevice9* device,
        const char* source,
        const char* profile,
        bool vertex,
        void** outShader,
        std::string& outError)
    {
        tD3DCompile compile = ResolveD3DCompile();
        if (!compile)
        {
            outError = "d3dcompiler_47.dll D3DCompile missing (VR gloves)";
            return false;
        }

        ID3DBlob* bytecode = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT compileResult = compile(
            source,
            std::strlen(source),
            "vr_hands.hlsl",
            nullptr,
            nullptr,
            "main",
            profile,
            0,
            0,
            &bytecode,
            &errors);
        if (FAILED(compileResult) || !bytecode)
        {
            if (errors && errors->GetBufferPointer())
                outError = static_cast<const char*>(errors->GetBufferPointer());
            else
                outError = vertex
                    ? "D3DCompile failed for VR hand vertex shader"
                    : "D3DCompile failed for VR hand pixel shader";
            SafeRelease(errors);
            SafeRelease(bytecode);
            return false;
        }
        SafeRelease(errors);

        HRESULT createResult = E_FAIL;
        if (vertex)
        {
            createResult = device->CreateVertexShader(
                static_cast<const DWORD*>(bytecode->GetBufferPointer()),
                reinterpret_cast<IDirect3DVertexShader9**>(outShader));
        }
        else
        {
            createResult = device->CreatePixelShader(
                static_cast<const DWORD*>(bytecode->GetBufferPointer()),
                reinterpret_cast<IDirect3DPixelShader9**>(outShader));
        }
        SafeRelease(bytecode);
        if (FAILED(createResult) || !*outShader)
        {
            outError = vertex ? "CreateVertexShader failed for VR hands" : "CreatePixelShader failed for VR hands";
            return false;
        }
        return true;
    }

    bool CompileVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9** outShader, std::string& outError)
    {
        return CompileHlsl(device, kVertexShaderSource, "vs_3_0", true, reinterpret_cast<void**>(outShader), outError);
    }

    bool CompilePixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9** outShader, std::string& outError)
    {
        return CompileHlsl(device, kPixelShaderSource, "ps_3_0", false, reinterpret_cast<void**>(outShader), outError);
    }

    bool CreateDefaultTextureFromSysMem(
        IDirect3DDevice9* device,
        IDirect3DTexture9* sysMem,
        IDirect3DTexture9** outTexture,
        std::string& outError)
    {
        *outTexture = nullptr;
        if (!device || !sysMem)
            return false;
        D3DSURFACE_DESC desc{};
        if (FAILED(sysMem->GetLevelDesc(0, &desc)))
        {
            outError = "VR hand texture GetLevelDesc failed";
            return false;
        }
        IDirect3DTexture9* gpu = nullptr;
        if (FAILED(device->CreateTexture(
                desc.Width, desc.Height, 1, 0, desc.Format, D3DPOOL_DEFAULT, &gpu, nullptr))
            || !gpu)
        {
            outError = "CreateTexture D3DPOOL_DEFAULT failed for VR hands";
            return false;
        }
        if (FAILED(device->UpdateTexture(sysMem, gpu)))
        {
            outError = "UpdateTexture failed for VR hands";
            SafeRelease(gpu);
            return false;
        }
        *outTexture = gpu;
        return true;
    }

    bool CreateFallbackTexture(
        IDirect3DDevice9* device,
        std::uint32_t argb,
        IDirect3DTexture9** outTexture,
        std::string& outError)
    {
        IDirect3DTexture9* sys = nullptr;
        if (FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
        {
            outError = "cannot create VR hand fallback SYSTEMMEM texture";
            return false;
        }
        D3DLOCKED_RECT lock{};
        if (FAILED(sys->LockRect(0, &lock, nullptr, 0)))
        {
            outError = "cannot initialize VR hand fallback texture";
            SafeRelease(sys);
            return false;
        }
        *static_cast<DWORD*>(lock.pBits) = argb;
        sys->UnlockRect(0);
        const bool ok = CreateDefaultTextureFromSysMem(device, sys, outTexture, outError);
        SafeRelease(sys);
        return ok;
    }

    bool CreateTextureFromMemoryWic(
        IDirect3DDevice9* device,
        const std::uint8_t* bytes,
        size_t byteCount,
        IDirect3DTexture9** outTexture)
    {
        if (!device || !bytes || byteCount < 8 || !outTexture)
            return false;
        *outTexture = nullptr;

        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = (hrCo == S_OK);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
            return false;

        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        IDirect3DTexture9* sys = nullptr;
        bool ok = false;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr) && factory &&
            SUCCEEDED(factory->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes), static_cast<DWORD>(byteCount))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(
                stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) && decoder &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && frame)
        {
            UINT width = 0, height = 0;
            if (SUCCEEDED(frame->GetSize(&width, &height)) &&
                width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
                SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
                SUCCEEDED(converter->Initialize(
                    frame,
                    GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(device->CreateTexture(
                    width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
                sys)
            {
                D3DLOCKED_RECT lock{};
                if (SUCCEEDED(sys->LockRect(0, &lock, nullptr, 0)))
                {
                    hr = converter->CopyPixels(
                        nullptr,
                        lock.Pitch,
                        lock.Pitch * height,
                        static_cast<BYTE*>(lock.pBits));
                    if (SUCCEEDED(hr))
                    {
                        // HEV/SteamVR GLBs store unused alpha 0 on opaque
                        // rubber. WorldDepth ignores it; the desktop overlay
                        // can still dest-alpha composite those texels away.
                        auto* bits = static_cast<BYTE*>(lock.pBits);
                        for (UINT y = 0; y < height; ++y)
                        {
                            auto* row = reinterpret_cast<DWORD*>(bits + static_cast<size_t>(y) * lock.Pitch);
                            for (UINT x = 0; x < width; ++x)
                                row[x] |= 0xFF000000u;
                        }
                    }
                    sys->UnlockRect(0);
                    if (SUCCEEDED(hr))
                    {
                        std::string promoteError;
                        ok = CreateDefaultTextureFromSysMem(device, sys, outTexture, promoteError);
                    }
                }
                SafeRelease(sys);
            }
        }

        SafeRelease(converter);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(stream);
        SafeRelease(factory);
        if (needUninit)
            CoUninitialize();
        return ok;
    }
}

VrHandRendererD3D9::VrHandRendererD3D9() = default;

VrHandRendererD3D9::~VrHandRendererD3D9()
{
    OnDeviceLost();
}

bool VrHandRendererD3D9::EnsureSharedResources(IDirect3DDevice9* device, std::string& outError)
{
    if (!device)
    {
        outError = "VR hand renderer received no D3D9 device";
        return false;
    }
    if (m_Device != device)
    {
        OnDeviceLost();
        m_Device = device;
        m_Device->AddRef();
    }
    if (m_VertexDeclaration && m_VertexShader && m_PixelShader)
        return true;

    const D3DVERTEXELEMENT9 elements[] =
    {
        { 0, static_cast<WORD>(offsetof(VrHandVertex, position)), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, normal)), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, uv)), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, weights)), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
        { 0, static_cast<WORD>(offsetof(VrHandVertex, joints)), D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
        D3DDECL_END()
    };

    if (FAILED(device->CreateVertexDeclaration(elements, &m_VertexDeclaration)) || !m_VertexDeclaration)
    {
        outError = "CreateVertexDeclaration failed for VR hands";
        return false;
    }
    if (!CompileVertexShader(device, &m_VertexShader, outError))
        return false;
    if (!CompilePixelShader(device, &m_PixelShader, outError))
        return false;
    return true;
}

bool VrHandRendererD3D9::CreateTexture(IDirect3DDevice9* device, const VrHandMeshAsset& asset, IDirect3DTexture9** outTexture, std::string& outError)
{
    *outTexture = nullptr;
    if (!asset.baseColorTextureBytes.empty() &&
        CreateTextureFromMemoryWic(
            device,
            asset.baseColorTextureBytes.data(),
            asset.baseColorTextureBytes.size(),
            outTexture))
    {
        return true;
    }
    return CreateFallbackTexture(device, asset.fallbackColorArgb, outTexture, outError);
}

bool VrHandRendererD3D9::EnsureMeshResources(IDirect3DDevice9* device, int handIndex, const VrHandMeshAsset& asset, std::string& outError)
{
    if (handIndex < 0 || handIndex >= static_cast<int>(m_Meshes.size()))
    {
        outError = "invalid VR hand index";
        return false;
    }
    if (!asset.IsValid())
    {
        outError = "VR hand mesh asset is invalid";
        return false;
    }

    MeshResources& mesh = m_Meshes[static_cast<size_t>(handIndex)];
    if (mesh.vertexBuffer && mesh.indexBuffer && mesh.texture && mesh.sourcePath == asset.sourcePath)
        return true;
    ReleaseMesh(mesh);

    const UINT vertexBytes = static_cast<UINT>(asset.vertices.size() * sizeof(VrHandVertex));
    const UINT indexBytes = static_cast<UINT>(asset.indices.size() * sizeof(std::uint16_t));
    // D3D9Ex (DXVK OpenVR path) rejects D3DPOOL_MANAGED. DEFAULT + DYNAMIC
    // is lockable and valid. WRITEONLY-only MANAGED is why the left glove
    // never uploaded (right independent glove is disabled).
    constexpr DWORD kDynWrite = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    HRESULT hrVb = device->CreateVertexBuffer(
        vertexBytes, kDynWrite, 0, D3DPOOL_DEFAULT, &mesh.vertexBuffer, nullptr);
    if (FAILED(hrVb) || !mesh.vertexBuffer)
    {
        char buf[128];
        sprintf_s(buf, "CreateVertexBuffer failed for VR hands hr=0x%08X bytes=%u",
            static_cast<unsigned>(hrVb), vertexBytes);
        outError = buf;
        return false;
    }
    HRESULT hrIb = device->CreateIndexBuffer(
        indexBytes, kDynWrite, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &mesh.indexBuffer, nullptr);
    if (FAILED(hrIb) || !mesh.indexBuffer)
    {
        char buf[128];
        sprintf_s(buf, "CreateIndexBuffer failed for VR hands hr=0x%08X bytes=%u",
            static_cast<unsigned>(hrIb), indexBytes);
        outError = buf;
        ReleaseMesh(mesh);
        return false;
    }

    void* vertexData = nullptr;
    if (FAILED(mesh.vertexBuffer->Lock(0, vertexBytes, &vertexData, D3DLOCK_DISCARD)) || !vertexData)
    {
        outError = "cannot lock VR hand vertex buffer";
        ReleaseMesh(mesh);
        return false;
    }
    std::memcpy(vertexData, asset.vertices.data(), vertexBytes);
    mesh.vertexBuffer->Unlock();

    void* indexData = nullptr;
    if (FAILED(mesh.indexBuffer->Lock(0, indexBytes, &indexData, D3DLOCK_DISCARD)) || !indexData)
    {
        outError = "cannot lock VR hand index buffer";
        ReleaseMesh(mesh);
        return false;
    }
    std::memcpy(indexData, asset.indices.data(), indexBytes);
    mesh.indexBuffer->Unlock();

    if (!CreateTexture(device, asset, &mesh.texture, outError))
    {
        ReleaseMesh(mesh);
        return false;
    }

    mesh.sourcePath = asset.sourcePath;
    mesh.vertexCount = static_cast<unsigned int>(asset.vertices.size());
    mesh.indexCount = static_cast<unsigned int>(asset.indices.size());
    return true;
}

bool VrHandRendererD3D9::EnsureHandMesh(
    IDirect3DDevice9* device,
    int handIndex,
    const VrHandMeshAsset& asset,
    std::string& outError)
{
    outError.clear();
    return EnsureSharedResources(device, outError) && EnsureMeshResources(device, handIndex, asset, outError);
}

bool VrHandRendererD3D9::ClearViewmodelOcclusionStencil(IDirect3DDevice9* device, std::string& outError)
{
    outError.clear();
    if (!device)
    {
        outError = "VR hand stencil clear received a null D3D9 device";
        return false;
    }

    IDirect3DSurface9* depthStencil = nullptr;
    D3DSURFACE_DESC depthStencilDesc{};
    if (FAILED(device->GetDepthStencilSurface(&depthStencil)) || !depthStencil || FAILED(depthStencil->GetDesc(&depthStencilDesc)))
    {
        SafeRelease(depthStencil);
        outError = "VR hand viewmodel occlusion requires an active depth-stencil surface";
        return false;
    }
    const bool hasStencil =
        depthStencilDesc.Format == D3DFMT_D15S1 ||
        depthStencilDesc.Format == D3DFMT_D24S8 ||
        depthStencilDesc.Format == D3DFMT_D24X4S4 ||
        depthStencilDesc.Format == D3DFMT_D24FS8;
    SafeRelease(depthStencil);
    if (!hasStencil)
    {
        outError = "VR hand viewmodel occlusion requires a stencil-capable depth surface";
        return false;
    }

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
    {
        outError = "CreateStateBlock failed before VR hand stencil-bit clear";
        return false;
    }
    stateBlock->Capture();

    D3DVIEWPORT9 viewport{};
    const bool haveViewport = SUCCEEDED(device->GetViewport(&viewport));
    HRESULT drawResult = E_FAIL;
    if (haveViewport)
    {
        struct ClearStencilVertex
        {
            float x, y, z, rhw;
        };
        const float left = static_cast<float>(viewport.X) - 0.5f;
        const float top = static_cast<float>(viewport.Y) - 0.5f;
        const float right = static_cast<float>(viewport.X + viewport.Width) - 0.5f;
        const float bottom = static_cast<float>(viewport.Y + viewport.Height) - 0.5f;
        const ClearStencilVertex vertices[4] =
        {
            { left,  top,    0.0f, 1.0f },
            { right, top,    0.0f, 1.0f },
            { left,  bottom, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f }
        };

        // Clear only the reserved VR-hand stencil bit. D3D9 Clear(D3DCLEAR_STENCIL)
        // would erase every stencil bit and could disturb Source's own later passes.
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0u);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRS_STENCILREF, 0u);
        device->SetRenderState(D3DRS_STENCILMASK, kVrHandOcclusionStencilBit);
        device->SetRenderState(D3DRS_STENCILWRITEMASK, kVrHandOcclusionStencilBit);
        device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        device->SetVertexDeclaration(nullptr);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetTexture(0, nullptr);
        device->SetFVF(D3DFVF_XYZRHW);
        drawResult = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ClearStencilVertex));
    }

    stateBlock->Apply();
    SafeRelease(stateBlock);
    if (!haveViewport || FAILED(drawResult))
    {
        outError = "D3D9 reserved stencil-bit clear failed before VR hand world-depth mask";
        return false;
    }
    return true;
}

bool VrHandRendererD3D9::Draw(
    IDirect3DDevice9* device,
    int handIndex,
    const VrHandMeshAsset& asset,
    const std::vector<VrHandMatrixRows3x4>& palette,
    const VrHandMatrix4& world,
    const VrHandMatrix4& worldViewProjection,
    VrHandDrawPass drawPass,
    float sceneLightScale,
    std::string& outError)
{
    outError.clear();
    static_assert(sizeof(VrHandMatrixRows3x4) == sizeof(float) * 12u, "VR hand palette rows must be tightly packed");
    if (palette.empty() || palette.size() > kMaxShaderBones)
    {
        outError = "VR hand skinning palette count is invalid";
        return false;
    }
    if (!EnsureSharedResources(device, outError) || !EnsureMeshResources(device, handIndex, asset, outError))
        return false;

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
    {
        outError = "CreateStateBlock failed before VR hand draw";
        return false;
    }
    stateBlock->Capture();

    const MeshResources& mesh = m_Meshes[static_cast<size_t>(handIndex)];
    const std::array<float, 16> wvpRows = VrHandMath::ToRows4x4(worldViewProjection);
    VrHandMatrixRows3x4 worldNormalRows = VrHandMath::ToRows3x4(world);

    const bool maskPass = drawPass == VrHandDrawPass::WorldVisibilityMask;
    const bool compositePass = drawPass == VrHandDrawPass::ViewmodelComposite;
    const bool standaloneViewmodelPass = drawPass == VrHandDrawPass::ViewmodelStandalone;
    const bool overlayNoDepth = drawPass == VrHandDrawPass::OverlayNoDepth;
    const bool viewmodelDepthPass = compositePass || standaloneViewmodelPass;
    const bool standaloneGeneratedBox =
        handIndex == 2 &&
        asset.sourcePath.rfind("generated:magazine_box:", 0) == 0;
    const bool opaqueStandaloneMagazine = handIndex == 2 && !standaloneGeneratedBox;
    const bool opaqueStandaloneDebugBox = handIndex == 2 && standaloneGeneratedBox;
    const bool opaqueStandaloneMesh = opaqueStandaloneMagazine || opaqueStandaloneDebugBox;

    // VR gloves use a lightweight directional-light approximation. Reusing that
    // approximation for a replacement magazine made the same exported texture
    // visibly darker than Blender and the Source viewmodel material. Keep the
    // standalone magazine opaque and sample its base-color texture at full
    // intensity. Exact Source-material parity still depends on exporting the same
    // skin texture used by the active weapon replacement.
    worldNormalRows.v[11] = opaqueStandaloneMesh
        ? 1.0f
        : std::clamp(sceneLightScale, 0.06f, 1.25f);
    const float gloveLight[4] = { 0.35f, -0.45f, -0.82f, 0.14f };
    const float magazineUnlit[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float* light = opaqueStandaloneMesh ? magazineUnlit : gloveLight;
    device->SetRenderState(D3DRS_ZENABLE, overlayNoDepth ? FALSE : TRUE);
    // The final color pass must write depth as well. Otherwise every triangle of the
    // same glove blends through the others, so folded fingers remain visible through
    // the palm. The standalone magazine skips the VR-hand stencil test because it is
    // a viewmodel component, but it still uses the full world-comparable depth range.
    const bool writeDepth = !overlayNoDepth && (drawPass == VrHandDrawPass::WorldDepth || viewmodelDepthPass);
    device->SetRenderState(D3DRS_ZWRITEENABLE, writeDepth ? TRUE : FALSE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, standaloneGeneratedBox ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    // Detached magazine meshes need opaque rendering because exported materials may
    // carry an unused zero alpha channel. Generated debug boxes render as wireframe
    // so they do not hide the weapon while calibrating.
    // Some exported materials keep an unused zero alpha channel, which made a successfully
    // loaded GLB invisible. Hands retain normal alpha blending; standalone helpers render opaque.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, (opaqueStandaloneMesh || drawPass == VrHandDrawPass::WorldDepth || overlayNoDepth) ? FALSE : TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    // Desktop overlay: force opaque replace. Source/HUD may leave dest-alpha
    // or SRCALPHA state that punches through black HEV rubber (A was 0).
    if (overlayNoDepth)
    {
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        // Write opaque alpha so later HUD/desktop composites cannot punch
        // through HEV fabric via dest-alpha (HMD WorldDepth path unchanged).
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
            | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        // Fixed-function safety if the PS path is bypassed mid-frame.
        device->SetRenderState(D3DRS_COLORVERTEX, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    }
    else
    {
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            maskPass ? 0u : (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA));
    }
    device->SetRenderState(D3DRS_STENCILENABLE, (maskPass || compositePass) ? TRUE : FALSE);
    device->SetRenderState(D3DRS_STENCILFUNC, maskPass ? D3DCMP_ALWAYS : D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILREF, kVrHandOcclusionStencilBit);
    device->SetRenderState(D3DRS_STENCILMASK, kVrHandOcclusionStencilBit);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, maskPass ? kVrHandOcclusionStencilBit : 0u);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILPASS, maskPass ? D3DSTENCILOP_REPLACE : D3DSTENCILOP_KEEP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetFVF(0);
    device->SetVertexDeclaration(m_VertexDeclaration);
    device->SetVertexShader(m_VertexShader);
    device->SetPixelShader(m_PixelShader);
    device->SetTexture(0, mesh.texture);
    device->SetStreamSource(0, mesh.vertexBuffer, 0, sizeof(VrHandVertex));
    device->SetIndices(mesh.indexBuffer);
    device->SetVertexShaderConstantF(0, wvpRows.data(), 4);
    device->SetVertexShaderConstantF(4, light, 1);
    device->SetVertexShaderConstantF(5, worldNormalRows.v.data(), 3);
    device->SetVertexShaderConstantF(kBoneConstantStart, palette.front().v.data(), static_cast<UINT>(palette.size() * 3u));

    D3DVIEWPORT9 oldViewport{};
    const bool haveOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));
    if (haveOldViewport)
    {
        D3DVIEWPORT9 handViewport = oldViewport;
        handViewport.MinZ = kWorldDepthRangeMin;
        handViewport.MaxZ = viewmodelDepthPass ? kViewmodelDepthRangeMax : kWorldDepthRangeMax;
        device->SetViewport(&handViewport);
    }

    HRESULT drawResult = device->DrawIndexedPrimitive(
        D3DPT_TRIANGLELIST,
        0,
        0,
        mesh.vertexCount,
        0,
        mesh.indexCount / 3u);

    if (SUCCEEDED(drawResult) && standaloneGeneratedBox && !asset.vertices.empty())
    {
        Vector mins(
            asset.vertices.front().position[0],
            asset.vertices.front().position[1],
            asset.vertices.front().position[2]);
        Vector maxs = mins;
        for (const VrHandVertex& vertex : asset.vertices)
        {
            mins.x = std::min(mins.x, vertex.position[0]);
            mins.y = std::min(mins.y, vertex.position[1]);
            mins.z = std::min(mins.z, vertex.position[2]);
            maxs.x = std::max(maxs.x, vertex.position[0]);
            maxs.y = std::max(maxs.y, vertex.position[1]);
            maxs.z = std::max(maxs.z, vertex.position[2]);
        }

        const Vector center = (mins + maxs) * 0.5f;
        const Vector span = maxs - mins;
        const float markerX = std::max(0.025f, span.x * 0.12f);
        const float markerY = std::max(0.025f, span.y * 0.12f);
        const float markerZ = std::max(0.025f, span.z * 0.12f);
        VrHandVertex marker[12]{};
        int markerVertexCount = 0;
        auto addMarkerVertex = [&](const Vector& position)
        {
            VrHandVertex& vertex = marker[markerVertexCount++];
            vertex.position[0] = position.x;
            vertex.position[1] = position.y;
            vertex.position[2] = position.z;
            vertex.normal[2] = 1.0f;
            vertex.weights[0] = 1.0f;
            vertex.joints[0] = 0;
        };
        auto addMarkerLine = [&](const Vector& a, const Vector& b)
        {
            addMarkerVertex(a);
            addMarkerVertex(b);
        };
        addMarkerLine(
            center + Vector(-markerX, -markerY, 0.0f),
            center + Vector(markerX, markerY, 0.0f));
        addMarkerLine(
            center + Vector(-markerX, markerY, 0.0f),
            center + Vector(markerX, -markerY, 0.0f));
        addMarkerLine(
            center + Vector(-markerX, 0.0f, -markerZ),
            center + Vector(markerX, 0.0f, markerZ));
        addMarkerLine(
            center + Vector(-markerX, 0.0f, markerZ),
            center + Vector(markerX, 0.0f, -markerZ));
        addMarkerLine(
            center + Vector(0.0f, -markerY, -markerZ),
            center + Vector(0.0f, markerY, markerZ));
        addMarkerLine(
            center + Vector(0.0f, -markerY, markerZ),
            center + Vector(0.0f, markerY, -markerZ));

        const HRESULT markerResult = device->DrawPrimitiveUP(
            D3DPT_LINELIST,
            static_cast<UINT>(markerVertexCount / 2),
            marker,
            sizeof(VrHandVertex));
        if (FAILED(markerResult))
            drawResult = markerResult;
    }

    if (haveOldViewport)
        device->SetViewport(&oldViewport);

    stateBlock->Apply();
    SafeRelease(stateBlock);
    if (FAILED(drawResult))
    {
        outError = "DrawIndexedPrimitive failed for VR hands";
        return false;
    }
    return true;
}

void VrHandRendererD3D9::ReleaseMesh(MeshResources& mesh)
{
    SafeRelease(mesh.vertexBuffer);
    SafeRelease(mesh.indexBuffer);
    SafeRelease(mesh.texture);
    mesh.sourcePath.clear();
    mesh.vertexCount = 0;
    mesh.indexCount = 0;
}

void VrHandRendererD3D9::ReleaseShared()
{
    SafeRelease(m_VertexDeclaration);
    SafeRelease(m_VertexShader);
    SafeRelease(m_PixelShader);
}

void VrHandRendererD3D9::OnDeviceLost()
{
    for (MeshResources& mesh : m_Meshes)
        ReleaseMesh(mesh);
    ReleaseShared();
    SafeRelease(m_Device);
}

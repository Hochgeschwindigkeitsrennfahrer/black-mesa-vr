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

    // Save/restore around the hand draws. The old code created a D3DSBT_ALL
    // block per call; on DXVK that Capture()/Apply() walks every render
    // state, all sampler and texture-stage states, 260 transforms and ~480
    // shader constants (one SetXxx each), plus a ~40KB allocation — twice per
    // hand per eye. This block is recorded once with exactly the states
    // Draw() / ClearViewmodelOcclusionStencil() touch. Values recorded are
    // irrelevant; only the set of touched states matters.
    IDirect3DDevice9* g_HandStateBlockDevice = nullptr;
    IDirect3DStateBlock9* g_HandStateBlock = nullptr;

    void ReleaseHandStateBlock()
    {
        if (g_HandStateBlock)
        {
            g_HandStateBlock->Release();
            g_HandStateBlock = nullptr;
        }
        g_HandStateBlockDevice = nullptr;
    }

    IDirect3DStateBlock9* AcquireHandStateBlock(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl)
    {
        if (!device)
            return nullptr;
        if (g_HandStateBlock && g_HandStateBlockDevice == device)
            return g_HandStateBlock;
        ReleaseHandStateBlock();
        if (FAILED(device->BeginStateBlock()))
            return nullptr;
        static const D3DRENDERSTATETYPE kRenderStates[] = {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_FILLMODE,
            D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
            D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE,
            D3DRS_COLORWRITEENABLE, D3DRS_TEXTUREFACTOR, D3DRS_COLORVERTEX, D3DRS_LIGHTING,
            D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF,
            D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
            D3DRS_STENCILPASS,
        };
        for (D3DRENDERSTATETYPE rs : kRenderStates)
            device->SetRenderState(rs, 0);
        static const D3DTEXTURESTAGESTATETYPE kStage0[] = {
            D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
        };
        for (D3DTEXTURESTAGESTATETYPE ts : kStage0)
            device->SetTextureStageState(0, ts, 0);
        device->SetTextureStageState(1, D3DTSS_COLOROP, 0);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, 0);
        static const D3DSAMPLERSTATETYPE kSampler[] = {
            D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW,
        };
        for (D3DSAMPLERSTATETYPE ss : kSampler)
        {
            device->SetSamplerState(0, ss, 0);
            device->SetSamplerState(1, ss, 0);
        }
        // SetFVF is the same device state as the vertex declaration.
        device->SetVertexDeclaration(decl);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetTexture(0, nullptr);
        device->SetTexture(1, nullptr);
        // DrawPrimitiveUP also resets stream 0.
        device->SetStreamSource(0, nullptr, 0, 0);
        device->SetIndices(nullptr);
        static const float kZero[4 * (kBoneConstantStart + kMaxShaderBones * 3)]{};
        device->SetVertexShaderConstantF(0, kZero, kBoneConstantStart + kMaxShaderBones * 3);
        device->SetPixelShaderConstantF(0, kZero, 5);
        D3DVIEWPORT9 vp{};
        vp.Width = 1;
        vp.Height = 1;
        vp.MaxZ = 1.f;
        device->SetViewport(&vp);
        IDirect3DStateBlock9* block = nullptr;
        if (FAILED(device->EndStateBlock(&block)) || !block)
            return nullptr;
        g_HandStateBlock = block;
        g_HandStateBlockDevice = device;
        return block;
    }

    // Recorded block when available, else the old per-call D3DSBT_ALL.
    IDirect3DStateBlock9* BeginHandStateScope(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl, bool& owned)
    {
        owned = false;
        IDirect3DStateBlock9* block = AcquireHandStateBlock(device, decl);
        if (!block)
        {
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &block)) || !block)
                return nullptr;
            owned = true;
        }
        block->Capture();
        return block;
    }

    void EndHandStateScope(IDirect3DStateBlock9* block, bool owned)
    {
        if (!block)
            return;
        block->Apply();
        if (owned)
            block->Release();
    }

    const char* kVertexShaderSource = R"HLSL(
float4 gWorldViewProjectionRows[4] : register(c0);
float4 gWorldRows[3] : register(c5);
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
    float3 worldNormal : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
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
    float4 sp = float4(skinnedPosition, 1.0);
    output.worldPos = float3(
        dot(sp, gWorldRows[0]),
        dot(sp, gWorldRows[1]),
        dot(sp, gWorldRows[2]));
    output.worldNormal = normalize(float3(
        dot(skinnedNormal, gWorldRows[0].xyz),
        dot(skinnedNormal, gWorldRows[1].xyz),
        dot(skinnedNormal, gWorldRows[2].xyz)));
    output.position = float4(
        dot(sp, gWorldViewProjectionRows[0]),
        dot(sp, gWorldViewProjectionRows[1]),
        dot(sp, gWorldViewProjectionRows[2]),
        dot(sp, gWorldViewProjectionRows[3]));
    output.uv = input.uv;
    return output;
}
)HLSL";

    const char* kPixelShaderSource = R"HLSL(
sampler2D gTexture : register(s0);
samplerCUBE gEnv : register(s1);
float4 gSceneLightRgb : register(c0);
float4 gEyePosition : register(c1);
float4 gLightDirectionAmbient : register(c2);
float4 gEnvAmount : register(c3);

float4 gFace : register(c4);

struct PS_INPUT
{
    float2 uv : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float face : VFACE;
};

float4 main(PS_INPUT input) : COLOR0
{
    float4 color = tex2D(gTexture, input.uv);
    // Inside of the glove (seen through the open cuff): back faces, drawn
    // dark instead of lit like the outside (user 2026-11-19: hollow hands).
    // gFace.x = sign of a front face's VFACE, 0 = off.
    if (gFace.x != 0.0 && input.face * gFace.x < 0.0)
        return float4(color.rgb * gSceneLightRgb.xyz * 0.12, 1.0);
    float3 N = normalize(input.worldNormal);
    float3 V = normalize(gEyePosition.xyz - input.worldPos);
    float3 L = normalize(-gLightDirectionAmbient.xyz);
    float3 scene = gSceneLightRgb.xyz;
    float exposure = saturate(gSceneLightRgb.w);

    float wrap = saturate(dot(N, L) * 0.5 + 0.5);
    float diffuse = 0.35 + wrap * 0.65;
    float3 lit = color.rgb * diffuse * scene;

    float chroma = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    float orange = saturate((color.r - color.g) * 5.0 + 0.15)
        * saturate((color.r - color.b) * 2.8)
        * saturate((chroma - 0.045) * 12.0)
        * saturate((color.r - 0.08) * 6.0);
    float metal = orange * saturate(gLightDirectionAmbient.w);

    float ndotv = saturate(dot(N, V));
    float f = 1.0 - ndotv;
    float3 H = normalize(L + V);

    float rubberFresnel = lerp(0.03, 0.16, saturate(f * 1.2));
    float rubberPhong = pow(saturate(dot(N, H)), 18.0) * 0.20 * rubberFresnel;
    float3 spec = color.rgb * rubberPhong * scene * (1.0 - metal);

    float metalFresnel = lerp(0.35, 1.15, saturate(f * 1.1)) + f * f * 1.2;
    float metalPhong = pow(saturate(dot(N, H)), 36.0) * 2.4 * saturate(metalFresnel);
    float3 metalSpecColor = lerp(color.rgb, float3(1.0, 0.72, 0.28), 0.40);
    spec += metalSpecColor * metalPhong * scene * metal;

    float3 R = reflect(-V, N);
    float sky = saturate(R.z * 0.55 + 0.45);
    float3 fake = lerp(scene * 0.20, scene * 1.15 + float3(0.18, 0.10, 0.04), sky) * color.rgb;
    float3 cubeDir = float3(R.x, R.z, R.y);
    float3 cubeSample = texCUBE(gEnv, cubeDir).rgb * lerp(color.rgb, float3(1.0, 1.0, 1.0), 0.22);
    float cubeLuma = dot(cubeSample, float3(0.30, 0.59, 0.11));
    float useCube = saturate(gEnvAmount.x) * saturate(cubeLuma * 3.5);
    float3 envCol = lerp(fake, cubeSample, useCube);
    float envDim = lerp(0.55, 1.0, saturate(exposure * 1.8));
    spec += envCol * saturate(metalFresnel) * (0.85 * metal) * envDim;

    return float4(saturate(lit + spec), 1.0);
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

    bool ownedBlock = false;
    IDirect3DStateBlock9* stateBlock = BeginHandStateScope(device, m_VertexDeclaration, ownedBlock);
    if (!stateBlock)
    {
        outError = "CreateStateBlock failed before VR hand stencil-bit clear";
        return false;
    }

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

    EndHandStateScope(stateBlock, ownedBlock);
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
    const Vector& sceneLightRgb,
    const Vector& eyeOrigin,
    float envExposure,
    IDirect3DBaseTexture9* envCubemap,
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

    bool ownedBlock = false;
    IDirect3DStateBlock9* stateBlock = BeginHandStateScope(device, m_VertexDeclaration, ownedBlock);
    if (!stateBlock)
    {
        outError = "CreateStateBlock failed before VR hand draw";
        return false;
    }

    const MeshResources& mesh = m_Meshes[static_cast<size_t>(handIndex)];
    const std::array<float, 16> wvpRows = VrHandMath::ToRows4x4(worldViewProjection);
    const VrHandMatrixRows3x4 worldRows = VrHandMath::ToRows3x4(world);

    const bool maskPass = drawPass == VrHandDrawPass::WorldVisibilityMask;
    const bool compositePass = drawPass == VrHandDrawPass::ViewmodelComposite;
    const bool standaloneViewmodelPass = drawPass == VrHandDrawPass::ViewmodelStandalone;
    const bool overlayNoDepth = drawPass == VrHandDrawPass::OverlayNoDepth;
    const bool viewmodelDepthPass = compositePass || standaloneViewmodelPass;
    const bool standaloneGeneratedBox =
        asset.sourcePath.rfind("generated:magazine_box:", 0) == 0;
    const bool magazineAsset =
        standaloneGeneratedBox
        || asset.sourcePath.find("magazine") != std::string::npos;
    const bool opaqueStandaloneMagazine = magazineAsset && !standaloneGeneratedBox;
    const bool opaqueStandaloneDebugBox = standaloneGeneratedBox;
    const bool opaqueStandaloneMesh = opaqueStandaloneMagazine || opaqueStandaloneDebugBox;

    const float sceneConst[4] = {
        opaqueStandaloneMesh ? 1.0f : sceneLightRgb.x,
        opaqueStandaloneMesh ? 1.0f : sceneLightRgb.y,
        opaqueStandaloneMesh ? 1.0f : sceneLightRgb.z,
        opaqueStandaloneMesh ? 1.0f : std::clamp(envExposure, 0.03f, 1.5f) };
    const float eyeConst[4] = { eyeOrigin.x, eyeOrigin.y, eyeOrigin.z, 1.0f };
    const float hevOrangeMetal =
        asset.sourcePath.find("hev_glove") != std::string::npos ? 1.0f : 0.0f;
    const float lightConst[4] = { 0.35f, -0.45f, -0.82f, hevOrangeMetal };
    const float envAmt[4] = { (envCubemap && hevOrangeMetal > 0.5f) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
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
    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(1, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    device->SetFVF(0);
    device->SetVertexDeclaration(m_VertexDeclaration);
    device->SetVertexShader(m_VertexShader);
    device->SetPixelShader(m_PixelShader);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetTexture(0, mesh.texture);
    device->SetTexture(1, envCubemap);
    device->SetStreamSource(0, mesh.vertexBuffer, 0, sizeof(VrHandVertex));
    device->SetIndices(mesh.indexBuffer);
    device->SetVertexShaderConstantF(0, wvpRows.data(), 4);
    device->SetVertexShaderConstantF(5, worldRows.v.data(), 3);
    device->SetVertexShaderConstantF(kBoneConstantStart, palette.front().v.data(), static_cast<UINT>(palette.size() * 3u));
    device->SetPixelShaderConstantF(0, sceneConst, 1);
    device->SetPixelShaderConstantF(1, eyeConst, 1);
    device->SetPixelShaderConstantF(2, lightConst, 1);
    device->SetPixelShaderConstantF(3, envAmt, 1);
    // Glove inside dark (back faces); not for standalone meshes. Front faces
    // have VFACE > 0 with these meshes' winding (BMVR_TEST_HAND_FACE=-1/0
    // flips / disables, for checking).
    static const float s_faceSign = [] {
        char buf[16]{};
        return GetEnvironmentVariableA("BMVR_TEST_HAND_FACE", buf, sizeof(buf)) ? static_cast<float>(atof(buf)) : 1.f;
    }();
    const float faceConst[4] = { opaqueStandaloneMesh ? 0.f : s_faceSign, 0.f, 0.f, 0.f };
    device->SetPixelShaderConstantF(4, faceConst, 1);

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

    EndHandStateScope(stateBlock, ownedBlock);
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
    ReleaseHandStateBlock();
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

#pragma once
#include "openxr_cpu_bridge.h"
#include <cmath>
#include <cstring>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <vector>
// The UI uses projected eye-pixel vertices. Avoid POSITIONT fixed-function
// emulation on MoltenVK; submit homogeneous clip vertices with explicit shaders.
inline HRESULT DrawVrUiPrimitive(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, UINT primitives,
                                 const void *data, UINT stride)
{
    if (!BMVR_UseCpuBridge())
        return device ? device->DrawPrimitiveUP(type, primitives, data, stride) : E_POINTER;
    if (!device || !data || (stride != 20 && stride != 28))
        return device ? device->DrawPrimitiveUP(type, primitives, data, stride) : E_POINTER;
    static IDirect3DDevice9 *owner = nullptr;
    static IDirect3DVertexShader9 *vs = nullptr;
    static IDirect3DPixelShader9 *solid = nullptr;
    static IDirect3DPixelShader9 *textured = nullptr;
    static IDirect3DVertexDeclaration9 *decl = nullptr;
    static bool tried = false;
    if (owner != device)
    {
        if (vs)
            vs->Release();
        if (solid)
            solid->Release();
        if (textured)
            textured->Release();
        if (decl)
            decl->Release();
        vs = nullptr;
        solid = nullptr;
        textured = nullptr;
        decl = nullptr;
        tried = false;
        owner = device;
    }
    if (!tried)
    {
        tried = true;
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        if (!dll)
            dll = LoadLibraryA("d3dcompiler_43.dll");
        auto compile = dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
        auto shader = [&](const char *hlsl, const char *profile, bool vertex, void **output) {
            if (!compile)
                return false;
            ID3DBlob *code = nullptr;
            ID3DBlob *errors = nullptr;
            HRESULT hr = compile(hlsl, strlen(hlsl), "bmvr_ui", nullptr, nullptr, "main", profile, 0, 0,
                                 &code, &errors);
            if (errors)
                errors->Release();
            if (FAILED(hr) || !code)
                return false;
            hr = vertex ? device->CreateVertexShader(static_cast<const DWORD *>(code->GetBufferPointer()),
                                                     reinterpret_cast<IDirect3DVertexShader9 **>(output))
                        : device->CreatePixelShader(static_cast<const DWORD *>(code->GetBufferPointer()),
                                                    reinterpret_cast<IDirect3DPixelShader9 **>(output));
            code->Release();
            return SUCCEEDED(hr);
        };
        shader("struct V { float4 p:POSITION; float4 c:COLOR0; float2 uv:TEXCOORD0; }; V main(V v) { return "
               "v; }",
               "vs_3_0", true, reinterpret_cast<void **>(&vs));
        shader("float4 main(float4 c:COLOR0):COLOR0 { return c; }", "ps_3_0", false,
               reinterpret_cast<void **>(&solid));
        shader("sampler2D t:register(s0); float4 main(float4 c:COLOR0,float2 uv:TEXCOORD0):COLOR0 { return "
               "c*tex2D(t,uv); }",
               "ps_3_0", false, reinterpret_cast<void **>(&textured));
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};
        device->CreateVertexDeclaration(elements, &decl);
        Game::logMsg("[BMVR UI v5] explicit shaders ready=%d", vs && solid && textured && decl ? 1 : 0);
    }
    if (!vs || !solid || !textured || !decl)
        return device->DrawPrimitiveUP(type, primitives, data, stride);
    UINT count = 0;
    switch (type)
    {
    case D3DPT_TRIANGLELIST:
        count = primitives * 3;
        break;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        count = primitives + 2;
        break;
    default:
        return device->DrawPrimitiveUP(type, primitives, data, stride);
    }
    D3DVIEWPORT9 vp{};
    if (FAILED(device->GetViewport(&vp)) || !vp.Width || !vp.Height)
        return D3DERR_INVALIDCALL;
    struct V
    {
        float x, y, z, w;
        DWORD c;
        float u, v;
    };
    static std::vector<V> vertices;
    vertices.resize(count);
    for (UINT i = 0; i < count; ++i)
    {
        const unsigned char *src = static_cast<const unsigned char *>(data) + i * stride;
        float p[4];
        memcpy(p, src, 16);
        const float w = std::isfinite(p[3]) && fabsf(p[3]) > 1e-8f ? 1.f / p[3] : 1.f;
        auto &v = vertices[i];
        v.x = ((p[0] - vp.X) * 2.f / vp.Width - 1.f) * w;
        v.y = (1.f - (p[1] - vp.Y) * 2.f / vp.Height) * w;
        v.z = p[2] * w;
        v.w = w;
        memcpy(&v.c, src + 16, 4);
        v.u = v.v = 0;
        if (stride == 28)
            memcpy(&v.u, src + 20, 8);
    }
    IDirect3DVertexShader9 *oldVs = nullptr;
    IDirect3DPixelShader9 *oldPs = nullptr;
    IDirect3DVertexDeclaration9 *oldDecl = nullptr;
    device->GetVertexShader(&oldVs);
    device->GetPixelShader(&oldPs);
    device->GetVertexDeclaration(&oldDecl);
    device->SetVertexDeclaration(decl);
    device->SetVertexShader(vs);
    device->SetPixelShader(stride == 28 ? textured : solid);
    HRESULT hr = device->DrawPrimitiveUP(type, primitives, vertices.data(), sizeof(V));
    device->SetVertexShader(oldVs);
    device->SetPixelShader(oldPs);
    device->SetVertexDeclaration(oldDecl);
    if (oldVs)
        oldVs->Release();
    if (oldPs)
        oldPs->Release();
    if (oldDecl)
        oldDecl->Release();
    return hr;
}

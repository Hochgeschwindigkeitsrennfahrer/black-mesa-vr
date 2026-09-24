#!/usr/bin/env python3
"""Build research/dxvk-gplasync/drop-in-dxvk_new from GPLALL 2.6.8-2 + live VR hunks.

Does not touch third_party/l4d2vr/dxvk_new, CMakeLists.txt, or the game install.
"""
from __future__ import annotations

import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = Path(__file__).resolve().parent
STAGING = RESEARCH / "staging" / "dxvk-gplall-2.6.8-2"
LIVE = ROOT / "third_party" / "l4d2vr" / "dxvk_new"
DROP = RESEARCH / "drop-in-dxvk_new"
SOURCES_OUT = RESEARCH / "drop-in-dxvk_sources.txt"
LIVE_SOURCES = ROOT / "tools" / "dxvk_sources.txt"

VR_CPP = [
    "src/d3d9/d3d9_vr.cpp",
    "src/d3d9/d3d9_vr.h",
    "src/d3d9/d3d9_reshade_vr.cpp",
    "src/d3d9/d3d9_reshade_vr.h",
    "src/d3d9/d3d9_reshade_api.h",
]


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def once_replace(text: str, needle: str, repl: str, name: str) -> str:
    n = text.count(needle)
    if n != 1:
        die(f"{name}: expected 1 occurrence of marker, found {n}")
    return text.replace(needle, repl, 1)


def extract_between(text: str, start: str, end: str, name: str, keep_start: bool = True) -> str:
    i = text.find(start)
    if i < 0:
        die(f"{name}: start marker not found")
    j = text.find(end, i + len(start) if not keep_start else i)
    if j < 0:
        die(f"{name}: end marker not found")
    if keep_start:
        return text[i:j]
    return text[i + len(start) : j]


def copytree(src: Path, dst: Path, ignore_git: bool = True) -> None:
    if dst.exists():
        shutil.rmtree(dst)

    def _ignore(directory: str, names: list[str]) -> set[str]:
        skip = set()
        if ignore_git:
            if ".git" in names:
                skip.add(".git")
        return skip

    shutil.copytree(src, dst, ignore=_ignore, dirs_exist_ok=False)


def overlay_dir(src: Path, dst: Path) -> None:
    if not src.exists():
        die(f"missing overlay source {src}")
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def copy_file(rel: str) -> None:
    src = LIVE / rel
    dst = DROP / rel
    if not src.exists():
        die(f"missing live file {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def patch_device_header() -> None:
    path = DROP / "src/d3d9/d3d9_device.h"
    text = read(path)

    text = once_replace(
        text,
        '#include "../util/util_unmap.h"\n\nnamespace dxvk {',
        '#include "../util/util_unmap.h"\n\nclass VR;\n\nnamespace dxvk {',
        "device.h class VR",
    )

    text = once_replace(
        text,
        """    D3D9DeviceLock LockDevice() {
      return m_multithread.AcquireLock();
    }

    const D3D9Options* GetOptions() const {""",
        """    D3D9DeviceLock LockDevice() {
      return m_multithread.AcquireLock();
    }

    D3D9DeviceLock LockDeviceExclusive() {
      return m_multithread.AcquireExclusiveLock();
    }

    bool BeginSourceFrameOwnership() {
      return m_multithread.BeginSourceFrameOwnership();
    }

    bool EndSourceFrameOwnership() {
      return m_multithread.EndSourceFrameOwnership();
    }

    const D3D9Options* GetOptions() const {""",
        "device.h exclusive lock",
    )

    text = once_replace(
        text,
        """    bool Is9On12Device() const {
      return m_d3d9On12Args.Enable9On12;
    }

  private:""",
        """    bool Is9On12Device() const {
      return m_d3d9On12Args.Enable9On12;
    }

    void DrawQueuedEyeSubmitOverlays(VR* vr);
    void UpdateReShadeVrDepthAtlas(VR* vr);

  private:""",
        "device.h VR methods",
    )

    text = once_replace(
        text,
        """    template<bool AllowFlush = true, typename Cmd>
    void EmitCs(Cmd&& command, bool disableFlush=false) {
      if (unlikely(!m_csChunk->push(command))) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();

        if constexpr (AllowFlush)
          if (!disableFlush)
          ConsiderFlush(GpuFlushType::ImplicitWeakHint);

        m_csChunk->push(command);
      }
    }

    void EmitCsChunk(DxvkCsChunkRef&& chunk);

    void FlushCsChunk() {
      if (likely(!m_csChunk->empty())) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();
      }
    }""",
        """    template<bool AllowFlush = true, typename Cmd>
    void EmitCs(Cmd&& command, bool disableFlush=false) {
      // L4D2VR queued rendering can hit unusual cross-thread Present/EndFrame
      // timing. Do not assume m_csChunk is always non-null after a move/flush.
      if (unlikely(!m_csChunk))
        m_csChunk = AllocCsChunk();

      if (unlikely(!m_csChunk))
        return;

      if (unlikely(!m_csChunk->push(command))) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();

        if constexpr (AllowFlush) {
          if (!disableFlush)
            ConsiderFlush(GpuFlushType::ImplicitWeakHint);
        }

        if (unlikely(!m_csChunk))
          return;

        m_csChunk->push(command);
      }
    }

    void EmitCsChunk(DxvkCsChunkRef&& chunk);

    void FlushCsChunk() {
      if (unlikely(!m_csChunk)) {
        m_csChunk = AllocCsChunk();
        return;
      }

      if (likely(!m_csChunk->empty())) {
        EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();
      }
    }""",
        "device.h EmitCs",
    )

    text = once_replace(
        text,
        """    D3D9Adapter*                    m_adapter;
    Rc<DxvkDevice>                  m_dxvkDevice;

    MemoryFilePool                  m_memoryAllocator;""",
        """    D3D9Adapter*                    m_adapter;
    Rc<DxvkDevice>                  m_dxvkDevice;

    Rc<DxvkImage>                   m_reShadeVrDepthAtlas;
    Rc<DxvkImageView>               m_reShadeVrDepthAtlasView;
    VkExtent3D                      m_reShadeVrDepthEyeExtent = { 0u, 0u, 0u };
    VkFormat                        m_reShadeVrDepthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits           m_reShadeVrDepthSourceSamples = VK_SAMPLE_COUNT_1_BIT;
    bool                            m_reShadeVrDepthMismatchLogged = false;

    MemoryFilePool                  m_memoryAllocator;""",
        "device.h reshade members",
    )

    write(path, text)


def extract_live_device_helpers(live: str) -> tuple[str, str]:
    msaa = extract_between(
        live,
        "namespace {\n\n    D3DMULTISAMPLE_TYPE MapToMultisampleType",
        "\n}\n\nnamespace dxvk {",
        "MapToMultisampleType",
        keep_start=True,
    )
    helpers = extract_between(
        live,
        "    namespace {\n\n        struct VrAimLineOverlayVertex {",
        "    D3D9DeviceEx::D3D9DeviceEx(",
        "VR helpers + ReShade/overlay methods",
        keep_start=True,
    )
    helpers = helpers.replace(
        "const bool manageScene = !m_flags.test(D3D9DeviceFlag::InScene);",
        "const bool manageScene = !m_inScene;",
    )
    return msaa, helpers


def extract_live_func(live: str, sig: str, next_sig: str, name: str) -> str:
    i = live.find(sig)
    if i < 0:
        die(f"{name}: signature not found")
    j = live.find(next_sig, i + len(sig))
    if j < 0:
        die(f"{name}: next signature not found")
    return live[i:j].rstrip() + "\n\n\n"


def patch_device_cpp() -> None:
    live = read(LIVE / "src/d3d9/d3d9_device.cpp")
    path = DROP / "src/d3d9/d3d9_device.cpp"
    text = read(path)

    msaa, helpers = extract_live_device_helpers(live)
    present_ex = extract_live_func(
        live,
        "    HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx(",
        "    HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx(",
        "PresentEx",
    )

    text = once_replace(
        text,
        '#include "d3d9_initializer.h"\n\n#include <algorithm>',
        """#include "d3d9_initializer.h"

#include "L4D2VR/game.h"
#include "L4D2VR/vr.h"
#include "L4D2VR/sdk/sdk.h"
#include "d3d9_vr.h"
#include "d3d9_reshade_vr.h"
#include "bmvr_flags.h"

#include <algorithm>""",
        "device.cpp VR includes",
    )

    text = once_replace(
        text,
        "#include <algorithm>\n#include <cfloat>\n#ifdef MSC_VER",
        """#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#ifdef MSC_VER""",
        "device.cpp extra std includes",
    )

    text = once_replace(
        text,
        "#endif\n\nnamespace dxvk {\n\n  D3D9DeviceEx::D3D9DeviceEx(",
        "#endif\n\n" + msaa + "\n\n}\n\nnamespace dxvk {\n\n" + helpers + "  D3D9DeviceEx::D3D9DeviceEx(",
        "device.cpp helpers insert",
    )

    text = once_replace(
        text,
        """  D3D9DeviceEx::~D3D9DeviceEx() {
    // Avoids hanging when in this state, see comment
    // in DxvkDevice::~DxvkDevice.""",
        """  D3D9DeviceEx::~D3D9DeviceEx() {
    if (m_dxvkDevice)
      D3D9ReShadeVrClearDepth(m_dxvkDevice->handle());

    // Avoids hanging when in this state, see comment
    // in DxvkDevice::~DxvkDevice.""",
        "device.cpp dtor reshade",
    )

    reset_preamble = """  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    // Source applies video settings through IDirect3DDevice9::Reset.
    // Black Mesa's saved mode is exclusive fullscreen; mutating the
    // caller's Windowed flag made it Reset every frame. Keep a local
    // windowed copy and skip identical Resets.
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS* caller = pPresentationParameters;
    D3DPRESENT_PARAMETERS local = *pPresentationParameters;
    BmvrKeepDesktopWindowed(&local);
    BmvrForceHmdAspectWindowedBackbuffer(&local);
    BmvrSyncCallerBackbufferSize(caller, &local);
    if (BmvrShouldSkipIdenticalWindowedReset(m_implicitSwapchain.ptr(), &local)
     || BmvrShouldSkipWindowedSizeReset(m_implicitSwapchain.ptr(), &local)) {
      static int s_skipLog;
      if (s_skipLog < 4) {
        Logger::info(str::format(
          "BMVR skipping identical windowed Reset ",
          local.BackBufferWidth, "x", local.BackBufferHeight));
        ++s_skipLog;
      }
      m_deviceLostState = D3D9DeviceLostState::Ok;
      return D3D_OK;
    }

    pPresentationParameters = &local;

    VR* vr = (g_Game != nullptr) ? g_Game->m_VR : nullptr;
    if (vr) {
      vr->ReleaseVRRenderTargetsForDeviceReset();
      VrForceSwapchainBackBufferToEyeSize(vr, pPresentationParameters);
    }

    D3D9DeviceLock lock = LockDevice();
"""
    text = once_replace(
        text,
        "  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {\n    D3D9DeviceLock lock = LockDevice();\n",
        reset_preamble,
        "device.cpp Reset preamble",
    )

    text = once_replace(
        text,
        """    if (unlikely(m_losableResourceCounter.load() != 0 && !IsExtended() && m_d3d9Options.countLosableResources)) {
      Logger::warn(str::format("Device reset failed because device still has alive losable resources: Device not reset. Remaining resources: ", m_losableResourceCounter.load()));
      m_deviceLostState = D3D9DeviceLostState::NotReset;
      // D3D8 returns D3DERR_DEVICELOST here, whereas D3D9 returns D3DERR_INVALIDCALL.
      return m_isD3D8Compatible ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
    }""",
        """    if (unlikely(m_losableResourceCounter.load() != 0 && !IsExtended() && m_d3d9Options.countLosableResources)) {
      const uint32_t remainingLosableResources = m_losableResourceCounter.load();
      if (g_Game && g_Game->m_VR) {
        Logger::warn(str::format(
          "Device reset continuing after L4D2VR texture cleanup despite remaining losable resources: ",
          remainingLosableResources));
      } else {
        Logger::warn(str::format("Device reset failed because device still has alive losable resources: Device not reset. Remaining resources: ", remainingLosableResources));
        m_deviceLostState = D3D9DeviceLostState::NotReset;
        // D3D8 returns D3DERR_DEVICELOST here, whereas D3D9 returns D3DERR_INVALIDCALL.
        return m_isD3D8Compatible ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
      }
    }""",
        "device.cpp Reset losable",
    )

    text = once_replace(
        text,
        """    Flush();
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    if (m_d3d9Options.deferSurfaceCreation)
      m_resetCtr++;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Present(""",
        """    Flush();
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    if (m_d3d9Options.deferSurfaceCreation)
      m_resetCtr++;

    // RefreshBackBufferTexture takes the VR texture mutex and re-enters D3D9.
    lock = D3D9DeviceLock();
    if (g_Game && g_Game->m_VR)
      g_Game->m_VR->RefreshBackBufferTexture(true);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Present(""",
        "device.cpp Reset epilogue",
    )

    text = once_replace(
        text,
        """    desc.IsLockable         = Pool != D3DPOOL_DEFAULT
                            || (Usage & D3DUSAGE_DYNAMIC)
                            || IsVendorFormat(EnumerateFormat(Format));

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_TEXTURE, &desc)))
      return D3DERR_INVALIDCALL;""",
        """    desc.IsLockable         = Pool != D3DPOOL_DEFAULT
                            || (Usage & D3DUSAGE_DYNAMIC)
                            || IsVendorFormat(EnumerateFormat(Format));

    if (g_Game && g_Game->m_VR
     && (g_Game->m_VR->m_CreatingTextureID == VR::Texture_LeftEye
      || g_Game->m_VR->m_CreatingTextureID == VR::Texture_RightEye)) {
      dxvk::Logger::info(str::format("Creating texture with MSAA ", g_Game->m_VR->m_AntiAliasing));
      desc.MultiSample = MapToMultisampleType(g_Game->m_VR->m_AntiAliasing);
    }

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_TEXTURE, &desc)))
      return D3DERR_INVALIDCALL;""",
        "device.cpp CreateTexture MSAA",
    )

    text = once_replace(
        text,
        """      m_initializer->InitTexture(texture->GetCommonTexture(), initialData);
      *ppTexture = texture.ref();

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVolumeTexture(""",
        """      m_initializer->InitTexture(texture->GetCommonTexture(), initialData);
      *ppTexture = texture.ref();

      if (g_Game && g_Game->m_VR && g_Game->m_VR->m_CreatingTextureID != VR::Texture_None && g_D3DVR9) {
        SharedTextureHolder* textureTarget = nullptr;
        D3D9_TEXTURE_VR_DESC texDesc = { };
        VR::TextureID texID = g_Game->m_VR->m_CreatingTextureID;
        HRESULT vrDescResult = D3DERR_INVALIDCALL;

        if (texID == VR::Texture_LeftEye) {
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9LeftEyeSurface);
          if (desc.MultiSample == D3DMULTISAMPLE_NONE) {
            textureTarget = &g_Game->m_VR->m_VKLeftEye;
            vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9LeftEyeSurface, &texDesc);
          }
        }
        else if (texID == VR::Texture_RightEye) {
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9RightEyeSurface);
          if (desc.MultiSample == D3DMULTISAMPLE_NONE) {
            textureTarget = &g_Game->m_VR->m_VKRightEye;
            vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9RightEyeSurface, &texDesc);
          }
        }
        else if (texID == VR::Texture_LeftEyeSubmit) {
          textureTarget = &g_Game->m_VR->m_VKLeftEye;
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9LeftEyeSubmitSurface);
          vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9LeftEyeSubmitSurface, &texDesc);
        }
        else if (texID == VR::Texture_RightEyeSubmit) {
          textureTarget = &g_Game->m_VR->m_VKRightEye;
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9RightEyeSubmitSurface);
          vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9RightEyeSubmitSurface, &texDesc);
        }
        else if (texID == VR::Texture_NekoPostLinearInput) {
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9NekoPostLinearInputSurface);
        }
        else if (texID == VR::Texture_NekoPostSmallInput) {
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9NekoPostSmallInputSurface);
        }
        else if (texID == VR::Texture_HUD) {
          textureTarget = &g_Game->m_VR->m_VKHUD;
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9HUDSurface);
          vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9HUDSurface, &texDesc);
        }
        else if (texID == VR::Texture_Scope) {
          textureTarget = &g_Game->m_VR->m_VKScope;
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9ScopeSurface);
          vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9ScopeSurface, &texDesc);
        }
        else if (texID == VR::Texture_RearMirror) {
          textureTarget = &g_Game->m_VR->m_VKRearMirror;
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9RearMirrorSurface);
          vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9RearMirrorSurface, &texDesc);
        }
        else if (texID == VR::Texture_DesktopMirror) {
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9DesktopMirrorSurface);
        }
        else if (texID == VR::Texture_Blank) {
          textureTarget = &g_Game->m_VR->m_VKBlankTexture;
          texture.ref()->GetSurfaceLevel(0, &g_Game->m_VR->m_D9BlankSurface);
          vrDescResult = g_D3DVR9->GetVRDesc(g_Game->m_VR->m_D9BlankSurface, &texDesc);
        }

        if (textureTarget != nullptr && SUCCEEDED(vrDescResult)) {
          std::memcpy(&textureTarget->m_VulkanData, &texDesc, sizeof(vr::VRVulkanTextureData_t));
          textureTarget->m_VRTexture.handle = &textureTarget->m_VulkanData;
          textureTarget->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
          textureTarget->m_VRTexture.eType = vr::TextureType_Vulkan;
        }
      }

      if (desc.Pool == D3DPOOL_DEFAULT)
        m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVolumeTexture(""",
        "device.cpp CreateTexture VR capture",
    )

    text = once_replace(
        text,
        """  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetViewport(const D3DVIEWPORT9* pViewport) {
    D3D9DeviceLock lock = LockDevice();

    // Outright crashes on native, but let's be
    // somewhat more elegant about it.
    if (unlikely(pViewport == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetViewport(pViewport);

    if (m_state.viewport == *pViewport)
      return D3D_OK;

    m_state.viewport.X = pViewport->X;
    m_state.viewport.Y = pViewport->Y;
    m_state.viewport.Width = pViewport->Width;
    m_state.viewport.Height = pViewport->Height;
    m_state.viewport.MinZ = pViewport->MinZ;
    m_state.viewport.MaxZ = pViewport->MinZ < pViewport->MaxZ ? pViewport->MaxZ : pViewport->MinZ + 0.001f;""",
        """  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetViewport(const D3DVIEWPORT9* pViewport) {
    D3D9DeviceLock lock = LockDevice();

    // Outright crashes on native, but let's be
    // somewhat more elegant about it.
    if (unlikely(pViewport == nullptr))
      return D3DERR_INVALIDCALL;

    D3DVIEWPORT9 effectiveViewport = *pViewport;

    // Do not call IEngineClient or IMaterialSystem from SetViewport.
    // Only rewrite when RT0 is one of our private eye/HUD surfaces.
    if (g_Game && g_Game->m_VR && m_state.renderTargets[0] != nullptr) {
      VR* vr = g_Game->m_VR;
      IDirect3DSurface9* currentRt =
        static_cast<IDirect3DSurface9*>(m_state.renderTargets[0].ptr());
      const bool forceVrViewport =
          currentRt == vr->m_D9HUDSurface ||
          currentRt == vr->m_D9LeftEyeSurface ||
          currentRt == vr->m_D9RightEyeSurface ||
          currentRt == vr->m_D9LeftEyeSubmitSurface ||
          currentRt == vr->m_D9RightEyeSubmitSurface ||
          currentRt == vr->m_D9DesktopMirrorSurface ||
          currentRt == vr->m_D9BlankSurface;

      if (forceVrViewport) {
        uint32_t width = vr->m_RenderWidth;
        uint32_t height = vr->m_RenderHeight;
        const VkExtent2D rtSize = m_state.renderTargets[0]->GetSurfaceExtent();
        if (rtSize.width != 0 && rtSize.height != 0) {
          width = rtSize.width;
          height = rtSize.height;
        }
        effectiveViewport.X = 0;
        effectiveViewport.Y = 0;
        effectiveViewport.Width = width;
        effectiveViewport.Height = height;
      }
    }

    if (unlikely(ShouldRecord()))
      return m_recorder->SetViewport(&effectiveViewport);

    if (m_state.viewport == effectiveViewport)
      return D3D_OK;

    m_state.viewport.X = effectiveViewport.X;
    m_state.viewport.Y = effectiveViewport.Y;
    m_state.viewport.Width = effectiveViewport.Width;
    m_state.viewport.Height = effectiveViewport.Height;
    m_state.viewport.MinZ = effectiveViewport.MinZ;
    m_state.viewport.MaxZ = effectiveViewport.MinZ < effectiveViewport.MaxZ
      ? effectiveViewport.MaxZ
      : effectiveViewport.MinZ + 0.001f;""",
        "device.cpp SetViewport VR",
    )

    present_start = text.find("  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx(")
    present_end = text.find("  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx(")
    if present_start < 0 or present_end < 0 or present_end <= present_start:
        die("device.cpp PresentEx range not found")
    text = text[:present_start] + present_ex + text[present_end:]

    text = once_replace(
        text,
        """    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = TRUE;
    desc.IsLockable         = IsLockableDepthStencilFormat(desc.Format);

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_SURFACE, &desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      m_losableResourceCounter++;

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ResetEx(""",
        """    desc.IsBackBuffer       = FALSE;
    desc.IsAttachmentOnly   = TRUE;
    desc.IsLockable         = IsLockableDepthStencilFormat(desc.Format);

    VR* vr = (g_Game != nullptr) ? g_Game->m_VR : nullptr;
    const VR::TextureID creatingTextureId = vr ? vr->m_CreatingTextureID : VR::Texture_None;
    const bool creatingVrEyeDepth =
        creatingTextureId == VR::Texture_LeftEye ||
        creatingTextureId == VR::Texture_RightEye;

    if (creatingVrEyeDepth) {
      dxvk::Logger::info(str::format("Creating depth/stencil surface with MSAA ", vr->m_AntiAliasing));
      desc.MultiSample = MapToMultisampleType(vr->m_AntiAliasing);
      if (vr->m_ReShadeVRCompat)
        desc.IsAttachmentOnly = FALSE;
    }

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(this, D3DRTYPE_SURFACE, &desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      m_losableResourceCounter++;

      if (vr && vr->m_ReShadeVRCompat && creatingVrEyeDepth) {
        IDirect3DSurface9*& capturedDepth =
            creatingTextureId == VR::Texture_LeftEye
            ? vr->m_D9LeftEyeDepthSurface
            : vr->m_D9RightEyeDepthSurface;

        if (capturedDepth != *ppSurface) {
          D3D9ReShadeVrInvalidateBinding();
          if (capturedDepth)
            capturedDepth->Release();
          capturedDepth = *ppSurface;
          capturedDepth->AddRef();
        }
      }

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ResetEx(""",
        "device.cpp CreateDepthStencilSurfaceEx VR",
    )

    text = once_replace(
        text,
        """  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ResetEx(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3D9DeviceLock lock = LockDevice();

    HRESULT hr;
    if (likely(m_deviceType != D3DDEVTYPE_NULLREF)) {""",
        """  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ResetEx(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS* caller = pPresentationParameters;
    D3DPRESENT_PARAMETERS local = *pPresentationParameters;
    BmvrKeepDesktopWindowed(&local);
    BmvrForceHmdAspectWindowedBackbuffer(&local);
    BmvrSyncCallerBackbufferSize(caller, &local);
    if (BmvrShouldSkipIdenticalWindowedReset(m_implicitSwapchain.ptr(), &local)
     || BmvrShouldSkipWindowedSizeReset(m_implicitSwapchain.ptr(), &local)) {
      m_deviceLostState = D3D9DeviceLostState::Ok;
      return D3D_OK;
    }
    pPresentationParameters = &local;

    VR* vr = (g_Game != nullptr) ? g_Game->m_VR : nullptr;
    if (vr) {
      vr->ReleaseVRRenderTargetsForDeviceReset();
      VrForceSwapchainBackBufferToEyeSize(vr, pPresentationParameters, pFullscreenDisplayMode);
    }

    D3D9DeviceLock lock = LockDevice();

    HRESULT hr;
    if (likely(m_deviceType != D3DDEVTYPE_NULLREF)) {""",
        "device.cpp ResetEx preamble",
    )

    text = once_replace(
        text,
        """    hr = ResetSwapChain(pPresentationParameters, pFullscreenDisplayMode);
    if (FAILED(hr))
      return hr;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayModeEx(""",
        """    hr = ResetSwapChain(pPresentationParameters, pFullscreenDisplayMode);
    if (FAILED(hr))
      return hr;

    lock = D3D9DeviceLock();
    if (g_Game && g_Game->m_VR)
      g_Game->m_VR->RefreshBackBufferTexture(true);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayModeEx(""",
        "device.cpp ResetEx epilogue",
    )

    text = once_replace(
        text,
        """  void D3D9DeviceEx::NotifyWindowActivated(HWND window, bool activated) {
    D3D9DeviceLock lock = LockDevice();

    if (likely(!m_d3d9Options.deviceLossOnFocusLoss || IsExtended()))
      return;""",
        """  void D3D9DeviceEx::NotifyWindowActivated(HWND window, bool activated) {
    D3D9DeviceLock lock = LockDevice();

    if (g_Game && g_Game->m_VR && g_Game->m_VR->m_IsVREnabled)
      return;

    if (likely(!m_d3d9Options.deviceLossOnFocusLoss || IsExtended()))
      return;""",
        "device.cpp NotifyWindowActivated",
    )

    write(path, text)
    if "PresentEx_REPLACED_ANCHOR" in text:
        die("PresentEx leftover anchor remained")
    if text.count("HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx(") != 1:
        die("PresentEx count != 1 after splice")


def patch_interface() -> None:
    live = read(LIVE / "src/d3d9/d3d9_interface.cpp")
    path = DROP / "src/d3d9/d3d9_interface.cpp"
    text = read(path)

    helpers = extract_between(
        live,
        "  static bool IsNoHmdLaunchArgPresent() {",
        "  Singleton<DxvkInstance> g_dxvkInstance;",
        "interface helpers",
        keep_start=True,
    )
    create_device = extract_live_func(
        live,
        "  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CreateDevice(\n",
        "  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::EnumAdapterModes(\n",
        "CreateDevice",
    )

    text = once_replace(
        text,
        '#include "d3d9_window.h"\n\n#include "../util/util_singleton.h"',
        """#include "d3d9_window.h"
#include "d3d9_vr.h"

#include <openvr.h>
#include "bmvr_flags.h"

#include "../util/util_singleton.h"
#include <shellapi.h>""",
        "interface includes",
    )

    text = once_replace(
        text,
        "namespace dxvk {\n\n  Singleton<DxvkInstance> g_dxvkInstance;",
        "namespace dxvk {\n\n" + helpers + "  Singleton<DxvkInstance> g_dxvkInstance;",
        "interface helpers insert",
    )

    # Replace GPLALL CreateDevice (thin forwarder) with live VR CreateDevice.
    gplall_cd = extract_between(
        text,
        "  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::CreateDevice(\n",
        "  HRESULT STDMETHODCALLTYPE D3D9InterfaceEx::EnumAdapterModes(\n",
        "gplall CreateDevice",
        keep_start=True,
    )
    text = once_replace(text, gplall_cd, create_device, "interface CreateDevice replace")
    write(path, text)


def patch_window() -> None:
    path = DROP / "src/d3d9/d3d9_window.cpp"
    text = read(path)
    text = once_replace(
        text,
        '#include "d3d9_swapchain.h"\n\nnamespace dxvk',
        '#include "d3d9_swapchain.h"\n#include "L4D2VR/game.h"\n#include "L4D2VR/vr.h"\n\nnamespace dxvk',
        "window includes",
    )
    text = once_replace(
        text,
        """    D3D9DeviceEx* device = windowData.swapchain->GetParent();

    if (message == WM_DESTROY)
      ResetWindowProc(window);
    else if (message == WM_ACTIVATEAPP) {
      D3DDEVICE_CREATION_PARAMETERS create_parms;
      device->GetCreationParameters(&create_parms);

      if (!(create_parms.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {""",
        """    D3D9DeviceEx* device = windowData.swapchain->GetParent();
    const bool vrSession = g_Game && g_Game->m_VR && g_Game->m_VR->m_IsVREnabled;

    if (message == WM_DESTROY)
      ResetWindowProc(window);
    else if (message == WM_ACTIVATEAPP) {
      D3DDEVICE_CREATION_PARAMETERS create_parms;
      device->GetCreationParameters(&create_parms);

      if (!vrSession && !(create_parms.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {""",
        "window WM_ACTIVATEAPP vrSession",
    )
    text = once_replace(
        text,
        "      if (!(create_parms.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES) && !IsIconic(window))",
        "      if (!vrSession && !(create_parms.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES) && !IsIconic(window))",
        "window WM_SIZE vrSession",
    )
    write(path, text)


def patch_main() -> None:
    path = DROP / "src/d3d9/d3d9_main.cpp"
    text = read(path)
    text = once_replace(
        text,
        '#include "d3d9_shader_validator.h"\n\n#include "d3d9_annotation.h"',
        '#include "d3d9_shader_validator.h"\n#include "d3d9_reshade_vr.h"\n\n#include "d3d9_annotation.h"',
        "main include",
    )
    text = once_replace(
        text,
        """  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9(UINT nSDKVersion) {
    IDirect3D9Ex* pDirect3D = nullptr;
    dxvk::CreateD3D9(false, &pDirect3D, nullptr, 0);""",
        """  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9(UINT nSDKVersion) {
    dxvk::D3D9ReShadeVrPrepareConfiguration();

    IDirect3D9Ex* pDirect3D = nullptr;
    dxvk::CreateD3D9(false, &pDirect3D, nullptr, 0);""",
        "main Create9",
    )
    text = once_replace(
        text,
        """  DLLEXPORT HRESULT __stdcall Direct3DCreate9Ex(UINT nSDKVersion, IDirect3D9Ex** ppDirect3D9Ex) {
    return dxvk::CreateD3D9(true, ppDirect3D9Ex, nullptr, 0);""",
        """  DLLEXPORT HRESULT __stdcall Direct3DCreate9Ex(UINT nSDKVersion, IDirect3D9Ex** ppDirect3D9Ex) {
    dxvk::D3D9ReShadeVrPrepareConfiguration();
    return dxvk::CreateD3D9(true, ppDirect3D9Ex, nullptr, 0);""",
        "main Create9Ex",
    )
    write(path, text)


def patch_config() -> None:
    path = DROP / "src/util/config/config.cpp"
    text = read(path)
    # File bytes: R"(\\ASN_App_PcDx9_Final\.exe$)" — two backslashes then ASN.
    old = (
        '    { R"(\\\\ASN_App_PcDx9_Final\\.exe$)", {{\n'
        '      { "d3d9.cachedWriteOnlyBuffers",      "True" },\n'
        '    }} },\n'
        '    /* Final Fantasy XIV - Direct3D 9 mode        *\n'
        '     * Can crash with unmapping                   */\n'
        '    { R"(\\\\ffxiv\\.exe$)", {{'
    )
    new = (
        '    { R"(\\\\ASN_App_PcDx9_Final\\.exe$)", {{\n'
        '      { "d3d9.cachedWriteOnlyBuffers",      "True" },\n'
        '    }} },\n'
        '    /* Black Mesa VR                              *\n'
        '     * Artifacts & broken flashlight on Intel.    *\n'
        '     * Stock dxvk.conf also sets deviceLossOnFocusLoss=True, which marks the\n'
        '     * device Lost on alt-tab and races VR eye-texture Reset/Submit. */\n'
        '    { R"(\\\\bms\\.exe$)", {{\n'
        '      { "d3d9.customVendorId",              "10de" },\n'
        '      { "d3d9.deviceLossOnFocusLoss",       "False" },\n'
        '    }} },\n'
        '    /* Final Fantasy XIV - Direct3D 9 mode        *\n'
        '     * Can crash with unmapping                   */\n'
        '    { R"(\\\\ffxiv\\.exe$)", {{'
    )
    text = once_replace(text, old, new, "config.cpp bms.exe profile")
    write(path, text)


def patch_openvr() -> None:
    path = DROP / "src/dxvk/dxvk_openvr.cpp"
    text = read(path)
    text = once_replace(
        text,
        '#include <openvr/openvr.hpp>\n\nusing VR_InitInternalProc',
        """#include <openvr/openvr.hpp>

#ifdef _WIN32
#include <shellapi.h>
#endif

using VR_InitInternalProc""",
        "openvr shellapi",
    )
    text = once_replace(
        text,
        """namespace dxvk {
  
  struct VrFunctions {""",
        """namespace dxvk {

  static bool IsNoHmdLaunchArgPresent() {
#ifdef _WIN32
    int nArgs = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (!argList)
      return false;

    bool noHmd = false;
    for (int i = 0; i < nArgs; i++) {
      if (_wcsicmp(argList[i], L"-nohmd") == 0) {
        noHmd = true;
        break;
      }
    }

    LocalFree(argList);
    return noHmd;
#else
    return false;
#endif
  }

  struct VrFunctions {""",
        "openvr helper",
    )
    text = once_replace(
        text,
        """  VrInstance:: VrInstance() {
    m_no_vr = env::getEnvVar("DXVK_NO_VR") == "1";
  }""",
        """  VrInstance:: VrInstance() {
    m_no_vr = env::getEnvVar("DXVK_NO_VR") == "1"
           || IsNoHmdLaunchArgPresent();
  }""",
        "openvr ctor",
    )
    write(path, text)


def write_version() -> None:
    write(
        DROP / "include" / "version.h",
        '#pragma once\n\n#define DXVK_VERSION "v2.6.8-2+bmvr"\n',
    )
    write(DROP / "RELEASE", "2.6.8-2+bmvr\n")
    write(
        DROP / "BMVR_GPLALL_MERGE.txt",
        "This tree is DXVK-GPLALL 2.6.8-2 with Black Mesa VR overlays replayed.\n"
        "It is a staging drop-in for third_party/l4d2vr/dxvk_new.\n"
        "See research/dxvk-gplasync/HOW-TO-APPLY.md — do not copy a prebuilt d3d9.dll.\n",
    )


def write_sources() -> None:
    live_list = [
        line.strip()
        for line in LIVE_SOURCES.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    out: list[str] = []
    missing: list[str] = []
    for rel in live_list:
        if (DROP / rel).exists():
            out.append(rel)
        else:
            missing.append(rel)

    skip_prefixes = (
        "src/d3d8/",
        "src/d3d10/",
        "src/d3d11/",
        "src/dxgi/",
        "src/wsi/glfw/",
        "src/wsi/sdl2/",
        "src/wsi/sdl3/",
    )
    extras = []
    for dirpath, _, files in os.walk(DROP / "src"):
        for fn in files:
            if not fn.endswith((".cpp", ".c")):
                continue
            full = Path(dirpath) / fn
            rel = full.relative_to(DROP).as_posix()
            if rel.startswith(skip_prefixes):
                continue
            if rel not in out:
                extras.append(rel)
    extras.sort()
    out.extend(extras)

    SOURCES_OUT.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"sources: kept {len(out) - len(extras)} from live list, dropped {len(missing)}, added {len(extras)}")
    for m in missing:
        print(f"  dropped missing: {m}")
    for e in extras:
        print(f"  added: {e}")


def verify() -> None:
    checks = {
        DROP / "src/d3d9/d3d9_vr.cpp": "Direct3DCreateVRImpl",
        DROP / "src/d3d9/d3d9_device.cpp": "g_Game->m_VR->Update()",
        DROP / "src/d3d9/d3d9_interface.cpp": "Direct3DCreateVRImpl",
        DROP / "src/d3d9/d3d9_window.cpp": "vrSession",
        DROP / "src/util/config/config.cpp": "bms\\\\.exe",
        DROP / "include/vulkan/include/vulkan/vulkan.h": "vulkan_core.h",
        DROP / "include/spirv/include/spirv/unified1/spirv.h": "spv",
        DROP / "include/shaders/dxvk_present_frag.h": "",
        DROP / "include/version.h": "v2.6.8-2+bmvr",
        DROP / "src/dxvk/framepacer/dxvk_calibrated_device_timestamps.cpp": "Calibrated",
        DROP / "src/d3d9/d3d9_multithread.h": "AcquireExclusiveLock",
    }
    for path, token in checks.items():
        if not path.exists():
            die(f"verify missing {path}")
        if token:
            data = path.read_text(encoding="utf-8", errors="ignore")
            if token not in data and token.replace("\\\\", "\\") not in data:
                # config uses raw string R"(\\bms\.exe$)"
                if token == "bms\\\\.exe":
                    if r"\bms\.exe$" not in data and r"\\bms\\.exe$" not in data:
                        if "bms.exe" not in data:
                            die(f"verify token {token} not in {path}")
                else:
                    die(f"verify token {token} not in {path}")


def main() -> None:
    if not STAGING.exists():
        die(f"missing staging clone {STAGING}")
    if not LIVE.exists():
        die(f"missing live dxvk_new {LIVE}")

    print(f"copying GPLALL 2.6.8-2 -> {DROP}")
    copytree(STAGING, DROP)
    print("copying vulkan / spirv / shaders / libdisplay-info from live dxvk_new")
    overlay_dir(LIVE / "include" / "vulkan", DROP / "include" / "vulkan")
    overlay_dir(LIVE / "include" / "spirv", DROP / "include" / "spirv")
    overlay_dir(LIVE / "include" / "shaders", DROP / "include" / "shaders")
    overlay_dir(LIVE / "subprojects" / "libdisplay-info", DROP / "subprojects" / "libdisplay-info")

    print("overlay VR-only files + exclusive D3D9 lock")
    for rel in VR_CPP:
        copy_file(rel)
    copy_file("src/d3d9/d3d9_multithread.h")

    print("patching GPLALL sources")
    patch_device_header()
    patch_device_cpp()
    patch_interface()
    patch_window()
    patch_main()
    patch_config()
    patch_openvr()
    write_version()
    write_sources()
    verify()
    print("drop-in ready")


if __name__ == "__main__":
    main()

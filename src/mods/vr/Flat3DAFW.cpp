#include <filesystem>

#include <spdlog/spdlog.h>
#include <windows.h>

#include <utility/Module.hpp>

#include "Flat3DAFW.hpp"

namespace vrmod {
bool Flat3DAFW::init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (m_renderer != nullptr) {
        return true;
    }

    if (m_attempted || device == nullptr || queue == nullptr) {
        return false;
    }

    m_attempted = true;

    // The plugin ships beside the game executable (same directory dinput8.dll loads from).
    auto mod = GetModuleHandleW(L"PDAFWPlugin.dll");

    if (mod == nullptr) {
        const auto exe_path = utility::get_module_pathw(GetModuleHandleW(nullptr));

        if (exe_path) {
            const auto dll_path = std::filesystem::path{*exe_path}.parent_path() / L"PDAFWPlugin.dll";
            mod = LoadLibraryW(dll_path.c_str());
        }
    }

    if (mod == nullptr) {
        m_status = "PDAFWPlugin.dll not found (drop it beside the game exe)";
        spdlog::info("[Flat3D-AFW] {}", m_status);
        return false;
    }

    m_init_device_fn = (pd::InitDeviceFn)GetProcAddress(mod, "InitDevice");
    m_init_framewarp_fn = (pd::InitFrameWarpFn)GetProcAddress(mod, "InitFrameWarp");
    m_evaluate_fn = (pd::EvaluateFrameWarpFn)GetProcAddress(mod, "EvaluateFrameWarp");

    if (m_init_device_fn == nullptr || m_init_framewarp_fn == nullptr || m_evaluate_fn == nullptr) {
        m_status = "PDAFWPlugin.dll exports missing";
        spdlog::error("[Flat3D-AFW] {} (InitDevice={:x} InitFrameWarp={:x} EvaluateFrameWarp={:x})", m_status,
            (uintptr_t)m_init_device_fn, (uintptr_t)m_init_framewarp_fn, (uintptr_t)m_evaluate_fn);
        return false;
    }

    pd::DeviceParams params{};
    params.d3d12Device = device;
    params.d3d12Queue = queue;
    m_renderer = m_init_device_fn(params);

    if (m_renderer == nullptr) {
        // The UEVR-3D repo builds a no-op dummy under the same name; it returns null here.
        m_status = "PDAFWPlugin InitDevice returned null (dummy dll?) - AFW unavailable";
        spdlog::warn("[Flat3D-AFW] {}", m_status);
        return false;
    }

    m_status = "initialized";
    spdlog::info("[Flat3D-AFW] plugin initialized (renderer={:x})", (uintptr_t)m_renderer);
    return true;
}

bool Flat3DAFW::ensure_io_textures(uint32_t w, uint32_t h, DXGI_FORMAT depth_fmt, DXGI_FORMAT mv_fmt) {
    if (m_renderer == nullptr || w == 0 || h == 0) {
        return false;
    }

    static uint64_t s_sig = 0;
    const uint64_t sig = (uint64_t)w ^ ((uint64_t)h << 16) ^ ((uint64_t)depth_fmt << 32) ^ ((uint64_t)mv_fmt << 44);

    if (io_ready && sig == s_sig) {
        return true;
    }

    constexpr auto k_state = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    io_depth_dil = pd::TextureDesc{};
    if (!m_renderer->CreateTexture((int)w, (int)h, depth_fmt, k_state, io_depth_dil, true)) {
        spdlog::error("[Flat3D-AFW] io depth-dilation CreateTexture failed (fmt={})", (uint32_t)depth_fmt);
        io_ready = false;
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        io_depth[i] = pd::TextureDesc{};
        // type stays Image: the working UEVR-3D implementation creates its depth io textures with
        // the default Image type (type=Depth changes the views CreateTexture builds).
        if (!m_renderer->CreateTexture((int)w, (int)h, depth_fmt, k_state, io_depth[i], true)) {
            spdlog::error("[Flat3D-AFW] io depth CreateTexture failed (fmt={})", (uint32_t)depth_fmt);
            io_ready = false;
            return false;
        }
        io_mv[i] = pd::TextureDesc{};
        io_mv_corr[i] = pd::TextureDesc{};
        io_mv_dlss[i] = pd::TextureDesc{};
        if (!m_renderer->CreateTexture((int)w, (int)h, mv_fmt, k_state, io_mv[i], true)
                || !m_renderer->CreateTexture((int)w, (int)h, mv_fmt, k_state, io_mv_corr[i], true)
                || !m_renderer->CreateTexture((int)w, (int)h, mv_fmt, k_state, io_mv_dlss[i], true)) {
            spdlog::error("[Flat3D-AFW] io MV CreateTexture failed (fmt={})", (uint32_t)mv_fmt);
            io_ready = false;
            return false;
        }
    }

    s_sig = sig;
    io_w = w;
    io_h = h;
    io_mv_fmt = mv_fmt;
    io_ready = true;
    spdlog::info("[Flat3D-AFW] io textures created {}x{} depth_fmt={} mv_fmt={}", w, h, (uint32_t)depth_fmt, (uint32_t)mv_fmt);
    return true;
}

bool Flat3DAFW::ensure_ui_textures(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (m_renderer == nullptr || w == 0 || h == 0) {
        return false;
    }

    static uint64_t s_sig = 0;
    const uint64_t sig = (uint64_t)w ^ ((uint64_t)h << 16) ^ ((uint64_t)fmt << 40);

    if (ui_ready && sig == s_sig) {
        return true;
    }

    constexpr auto k_state = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    io_hudless = pd::TextureDesc{};
    io_final = pd::TextureDesc{};
    io_ui[0] = pd::TextureDesc{};
    io_ui[1] = pd::TextureDesc{};
    if (!m_renderer->CreateTexture((int)w, (int)h, fmt, k_state, io_hudless, true)
            || !m_renderer->CreateTexture((int)w, (int)h, fmt, k_state, io_final, true)
            || !m_renderer->CreateTexture((int)w, (int)h, DXGI_FORMAT_R8G8B8A8_UNORM, k_state, io_ui[0], true)
            || !m_renderer->CreateTexture((int)w, (int)h, DXGI_FORMAT_R8G8B8A8_UNORM, k_state, io_ui[1], true)) {
        spdlog::error("[Flat3D-AFW] ui texture CreateTexture failed");
        ui_ready = false;
        return false;
    }

    s_sig = sig;
    ui_ready = true;
    spdlog::info("[Flat3D-AFW] ui textures created {}x{}", w, h);
    return true;
}

bool Flat3DAFW::ensure_eye_buffers(uint32_t width, uint32_t height, DXGI_FORMAT eye_format, DXGI_FORMAT backbuffer_format) {
    if (m_renderer == nullptr || m_init_framewarp_fn == nullptr || width == 0 || height == 0) {
        return false;
    }

    if (m_eye_buffers_ready && m_last_w == width && m_last_h == height
            && m_last_eye_fmt == eye_format && m_last_bb_fmt == backbuffer_format) {
        return true;
    }

    pd::FrameWarpInitParams params{};
    params.hmdWidth = (int)width;
    params.hmdHeight = (int)height;
    params.eyeFormat = eye_format;
    params.backbufferFormat = backbuffer_format;

    m_eye_buffers = m_init_framewarp_fn(params);
    m_eye_buffers_ready = m_eye_buffers.eyeFrameBuffers[0].color.pTexture != nullptr
        && m_eye_buffers.eyeFrameBuffers[1].color.pTexture != nullptr;
    m_last_w = width;
    m_last_h = height;
    m_last_eye_fmt = eye_format;
    m_last_bb_fmt = backbuffer_format;

    spdlog::info("[Flat3D-AFW] InitFrameWarp {}x{} eyeFmt={} bbFmt={} -> ready={}",
        width, height, (uint32_t)eye_format, (uint32_t)backbuffer_format, m_eye_buffers_ready);

    return m_eye_buffers_ready;
}

void Flat3DAFW::on_device_reset() {
    if (m_renderer == nullptr) {
        return; // never initialized - nothing cached
    }

    // Clearing the ready flags is what actually forces the rebuild: every ensure_* guard is
    // `ready && signature matches`, and a reset at an unchanged resolution matches the signature.
    // The size/format caches are zeroed too so the comparison can't accidentally pass later.
    io_ready = false;
    ui_ready = false;
    m_eye_buffers_ready = false;
    m_last_w = 0;
    m_last_h = 0;
    m_last_eye_fmt = DXGI_FORMAT_UNKNOWN;
    m_last_bb_fmt = DXGI_FORMAT_UNKNOWN;
    io_w = 0;
    io_h = 0;
    io_mv_fmt = DXGI_FORMAT_UNKNOWN;

    // Null the descs rather than leaving dangling pTextures: the present path tests them directly
    // (e.g. `afw.io_depth[fresh].pTexture != nullptr`) and would otherwise hand the plugin
    // pointers into resources that died with the old swapchain.
    io_depth[0] = pd::TextureDesc{};
    io_depth[1] = pd::TextureDesc{};
    io_depth_dil = pd::TextureDesc{};
    io_mv[0] = pd::TextureDesc{};
    io_mv[1] = pd::TextureDesc{};
    io_mv_corr[0] = pd::TextureDesc{};
    io_mv_corr[1] = pd::TextureDesc{};
    io_mv_dlss[0] = pd::TextureDesc{};
    io_mv_dlss[1] = pd::TextureDesc{};
    io_hudless = pd::TextureDesc{};
    io_final = pd::TextureDesc{};
    io_ui[0] = pd::TextureDesc{};
    io_ui[1] = pd::TextureDesc{};
    m_eye_buffers = pd::EyeFrameBuffers{};

    // The camera history and the NGX harvest both describe frames from before the reset; feeding
    // them to the warp would reproject against a discontinuity.
    prev_frames = 0;
    ngx_last_frame = -1000;

    // Signals run_flat3d_afw's own static TextureDesc caches (plugin-created depth/MV and the
    // wrapped color/hudless/post descs) to rebuild - they cannot detect the reset themselves.
    ++reset_epoch;

    spdlog::info("[Flat3D-AFW] device reset - dropped io/ui/eye buffers, rebuilding next frame");
}
} // namespace vrmod

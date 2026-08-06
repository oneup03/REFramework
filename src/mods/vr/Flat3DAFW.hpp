#pragma once

#include <d3d12.h>

#include "pdafw/PDAFWPlugin.h"

namespace vrmod {
// Forward-Z [0,1] projection -> reversed-Z (z' = w - z). Same transform UEVR-3D applies before
// handing projections to the PDAFWPlugin (its VR.cpp to_reverseZ): the ENGINE-RECORDED projection
// is forward-Z (RE: M22=-1, M23=-near) while the DEPTH BUFFER the plugin samples is reversed-Z
// (verified on Wilds: readbacks match -near/z exactly) - the plugin needs matrices that match the
// depth, or its reprojection collapses to a depth-flat warp.
inline glm::mat4 afw_to_reverse_z(const glm::mat4& proj) {
    static const glm::mat4 t{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -1, 0,
        0, 0, 1, 1};
    return t * proj;
}

// AFW (alternate frame warp) for Flat3D: single-camera AFR renders one eye per frame; the missing
// eye is synthesized by the PDAFWPlugin (depth + motion-vector reprojection). This class owns the
// runtime binding to PDAFWPlugin.dll (loaded from the game directory beside dinput8.dll; the
// UEVR-3D dummy returns null from InitDevice, in which case AFW stays cleanly unavailable) and the
// plugin-side per-eye frame buffers.
class Flat3DAFW {
public:
    // Idempotent; retries until the D3D12 device/queue exist. Returns true when the REAL plugin is
    // initialized and AFW is usable.
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue);

    bool is_available() const { return m_renderer != nullptr; }
    bool attempted() const { return m_attempted; }
    const char* status() const { return m_status; }

    pd::D3D12RendererAPI* renderer() const { return m_renderer; }
    pd::EyeFrameBuffers& eye_buffers() { return m_eye_buffers; }
    bool eye_buffers_ready() const { return m_eye_buffers_ready; }

    // (Re)allocate the plugin-owned per-eye framebuffers (color+depth+MV each). Safe to call per
    // frame; only reallocates on size/format change.
    bool ensure_eye_buffers(uint32_t width, uint32_t height, DXGI_FORMAT eye_format, DXGI_FORMAT backbuffer_format);

    // Plugin-owned per-eye SAMPLED input textures for the warp (the plugin can only sample textures
    // it created). Filled by the NGX (DLSS) hook on the game's own command list.
    bool ensure_io_textures(uint32_t w, uint32_t h, DXGI_FORMAT depth_fmt, DXGI_FORMAT mv_fmt);
    pd::TextureDesc io_depth[2]{};
    // Foreground-dilated depth (reversed-Z max filter) - the warp samples this instead when
    // depth-edge dilation is enabled (reduces the silhouette disocclusion-halo flicker).
    pd::TextureDesc io_depth_dil{};
    pd::TextureDesc io_mv[2]{};
    bool io_ready{false};

    // UI extraction (output-res, cache format): hudless input + PER-EYE extracted UI+alpha outputs.
    // Each frame the FRESH eye's UI is extracted into its slot; the warp composites the WARPED eye's
    // OWN slot (one frame stale but eye-correct - world-anchored icons keep their true parallax).
    bool ensure_ui_textures(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    pd::TextureDesc io_hudless{};
    pd::TextureDesc io_final{}; // same-stage post-overlay (with GUI) - ExtractUI's finalWithUI input
    pd::TextureDesc io_ui[2]{};
    bool ui_ready{false};
    // NGX-harvested state (written by the evaluate hook, read at warp time).
    float ngx_mv_scale[2]{1.0f, 1.0f};
    int32_t ngx_last_frame{-1000};

    // Synthetic camera-only MV fields. io_mv_corr = the warp's eye-jump field (present-time
    // dispatch). io_mv_dlss = the SAME-EYE N->N-2 field substituted into the per-eye DLSS
    // features (computed on our own queue-ordered list at evaluate-hook time). The view hook
    // records BOTH eyes' matrices every frame; prev_* = frame N-1, prev2_* = frame N-2 (rolled by
    // run_flat3d_afw after the warp).
    pd::TextureDesc io_mv_corr[2]{};
    pd::TextureDesc io_mv_dlss[2]{};
    glm::mat4 prev_view[2]{glm::mat4{1.0f}, glm::mat4{1.0f}};
    glm::mat4 prev_proj[2]{glm::mat4{1.0f}, glm::mat4{1.0f}};
    glm::mat4 prev2_view[2]{glm::mat4{1.0f}, glm::mat4{1.0f}};
    glm::mat4 prev2_proj[2]{glm::mat4{1.0f}, glm::mat4{1.0f}};
    uint32_t prev_frames{0}; // valid history depth (need >= 2 for the DLSS feed)
    // io texture metadata for the present-time dispatch (set by ensure_io_textures / the NGX hook).
    uint32_t io_w{0};
    uint32_t io_h{0};
    DXGI_FORMAT io_mv_fmt{DXGI_FORMAT_UNKNOWN};
    float ngx_mv_scale_raw[2]{1.0f, 1.0f}; // render-space MV scale (pre output scaling)

    void evaluate(pd::FrameWarpEvaluateParams& params) {
        if (m_evaluate_fn != nullptr) {
            m_evaluate_fn(params);
        }
    }

    // A device reset (swapchain recreate / device loss) invalidates every resource the plugin
    // allocated for us, but each ensure_* above short-circuits purely on size+format - so at an
    // unchanged resolution NOTHING got rebuilt and AFW kept running against the dead swapchain's
    // buffers while the rest of the pipeline was recreated around it. Drop the caches so the next
    // frame reallocates. The plugin's renderer itself is deliberately NOT re-created: InitDevice is
    // a one-shot with no teardown entry point (uevr-3d guards it the same way), so it stays bound
    // to the device it was given.
    void on_device_reset();

private:
    bool m_attempted{false};
    const char* m_status{"not attempted"};

    pd::InitDeviceFn m_init_device_fn{nullptr};
    pd::InitFrameWarpFn m_init_framewarp_fn{nullptr};
    pd::EvaluateFrameWarpFn m_evaluate_fn{nullptr};

    pd::D3D12RendererAPI* m_renderer{nullptr};
    pd::EyeFrameBuffers m_eye_buffers{};
    bool m_eye_buffers_ready{false};
    uint32_t m_last_w{0};
    uint32_t m_last_h{0};
    DXGI_FORMAT m_last_eye_fmt{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT m_last_bb_fmt{DXGI_FORMAT_UNKNOWN};
};
} // namespace vrmod

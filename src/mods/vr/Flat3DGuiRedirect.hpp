#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <d3d12.h>
#include <wrl.h>

namespace vrmod {
// True GUI separation for Flat3D AFW (the overlay-RT redirect, D3D12-level variant).
//
// The engine has no separate GUI texture - the overlay layer blends the GUI directly onto the
// main render target, which is the root cause of the world-GUI alternation under AFR+warp.
// Engine-level redirection (own via TargetState/RTV) is a dead end on Wilds: create_target_state
// is unresolvable and create_render_target_view device-removes from a layer hook (see
// docs/FLAT3D_WILDS_RE.md 2.9). So this redirects at the D3D12 layer instead, on objects we
// fully own:
//
//   - The pre/post overlay clone copies that VR.cpp already records every AFW frame bracket the
//     GUI draw EXACTLY in the engine's command stream. At D3D12 translate time they surface as
//     CopyTextureRegion/CopyResource calls with known dst resources - they are our in-stream
//     MARKERS (no new engine commands needed).
//   - Between marker A (pre) and marker B (post), any OMSetRenderTargets that binds the overlay
//     main target is substituted with our own committed GUI texture (cleared to transparent
//     black at marker A). A handle->resource map built from a CreateRenderTargetView device hook
//     identifies the target; a proactive bind at marker A covers the no-rebind case.
//   - At marker B the previous binding is restored and the GUI texture transitions to
//     PIXEL_SHADER_RESOURCE for the compose, which composites it per-eye at the GUI plane's
//     disparity (both halves identical -> zero alternation by construction).
//
// The engine's main target never receives the GUI, so the AFR caches and the warp inputs are
// naturally hudless - no extraction anywhere.
class Flat3DGuiRedirect {
public:
    static Flat3DGuiRedirect& get();

    // Installs the global detours (device vtable + command-list vtable via a throwaway list).
    // Safe to call repeatedly; only the first successful call installs.
    bool init(ID3D12Device* device);

    // Publish this frame's resources from the overlay hooks (render thread, record time).
    // overlay_target = the overlay layer's main target native; marker_pre/post = the pre/post
    // clone natives whose copies bracket the GUI draw. fresh_eye (0=left, 1=right) selects which
    // per-eye capture this frame's GUI lands in - world-anchored GUI elements are projected
    // through the alternating AFR camera, so each eye keeps its own capture (correct anchor
    // parallax, no alternation jitter). Null disables until re-armed.
    void arm(ID3D12Resource* overlay_target, ID3D12Resource* marker_pre, ID3D12Resource* marker_post, uint32_t fresh_eye);
    void disarm();

    bool is_armed() const { return m_armed.load(std::memory_order_acquire); }

    // The captured GUI texture for an eye (premultiplied-on-transparent content,
    // PIXEL_SHADER_RESOURCE state after its frame's marker B). Null until that eye's first
    // capture completes (callers fall back to the other eye's texture).
    ID3D12Resource* gui_texture(uint32_t eye) const {
        return (eye < 2 && m_captured[eye].load(std::memory_order_acquire)) ? m_gui_tex[eye].Get() : nullptr;
    }

    void on_reset();

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Shadow { // last engine OM binding seen on a command list (pre-substitution values)
        uint32_t num{0};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvs{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        bool has_dsv{false};
        bool valid{false};
    };

    void ensure_gui_texture(ID3D12Resource* overlay_target);
    void begin_window(ID3D12GraphicsCommandList* list);
    void end_window(ID3D12GraphicsCommandList* list);
    void handle_marker(ID3D12GraphicsCommandList* list, ID3D12Resource* dst);

    static void STDMETHODCALLTYPE hk_create_rtv(ID3D12Device* device, ID3D12Resource* resource,
        const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle);
    static void STDMETHODCALLTYPE hk_copy_texture_region(ID3D12GraphicsCommandList* list,
        const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box);
    static void STDMETHODCALLTYPE hk_copy_resource(ID3D12GraphicsCommandList* list,
        ID3D12Resource* dst, ID3D12Resource* src);
    static void STDMETHODCALLTYPE hk_om_set_render_targets(ID3D12GraphicsCommandList* list,
        UINT num, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL single_range,
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);

    ComPtr<ID3D12Device> m_device{};
    std::array<ComPtr<ID3D12Resource>, 2> m_gui_tex{};        // per eye (0=left, 1=right)
    ComPtr<ID3D12DescriptorHeap> m_gui_rtv_heap{};            // 2 slots
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> m_gui_rtv{};
    DXGI_FORMAT m_gui_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_gui_w{0};
    uint32_t m_gui_h{0};
    uint32_t m_rtv_increment{0};

    // Armed-frame resources (published at record time, consumed at translate time).
    std::atomic<ID3D12Resource*> m_overlay_target{nullptr};
    std::atomic<ID3D12Resource*> m_marker_pre{nullptr};
    std::atomic<ID3D12Resource*> m_marker_post{nullptr};
    std::atomic<uint32_t> m_fresh_eye{0};
    std::atomic<bool> m_armed{false};
    std::array<std::atomic<bool>, 2> m_captured{};

    // GUI-draw window (between markers). The executor translates the stream in order; the
    // window may span command lists, so it is global, not per-list.
    std::atomic<bool> m_window_active{false};
    // Which eye's texture the OPEN window is drawing into (latched at begin so an arm() racing
    // mid-window can't split the begin/end pair across textures).
    uint32_t m_window_eye{0};
    // Tracks whether each gui texture currently sits in RENDER_TARGET (inside a window) or
    // PIXEL_SHADER_RESOURCE (outside). Only touched from the marker path.
    std::array<bool, 2> m_gui_in_rt_state{};

    std::mutex m_mutex; // guards m_rtv_map + m_shadows + texture (re)creation
    std::unordered_map<SIZE_T, ID3D12Resource*> m_rtv_map{};
    std::unordered_map<ID3D12GraphicsCommandList*, Shadow> m_shadows{};

    // Diagnostics
    std::atomic<uint64_t> m_diag_windows{0};
    std::array<std::atomic<uint64_t>, 2> m_diag_windows_eye{};
    std::atomic<uint64_t> m_diag_om_in_window{0};
    std::atomic<uint64_t> m_diag_substituted{0};
    std::atomic<uint64_t> m_diag_unknown_in_window{0};

    bool m_hooks_installed{false};
};
} // namespace vrmod

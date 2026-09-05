#include <spdlog/spdlog.h>

#include "utility/FunctionHook.hpp"

#include "REFramework.hpp"

#include "Flat3DGuiRedirect.hpp"

namespace vrmod {
namespace {
std::unique_ptr<FunctionHook> g_create_rtv_hook{};
std::unique_ptr<FunctionHook> g_copy_texture_region_hook{};
std::unique_ptr<FunctionHook> g_copy_resource_hook{};
std::unique_ptr<FunctionHook> g_om_set_render_targets_hook{};

// Set around our own recording inside the detours so they pass straight through.
thread_local bool s_internal_call = false;

// Function TYPES (get_original<T> returns T*). x64 has one calling convention - no
// STDMETHODCALLTYPE needed in the alias.
using CreateRTVFn = void(ID3D12Device*, ID3D12Resource*,
    const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyTextureRegionFn = void(ID3D12GraphicsCommandList*,
    const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResourceFn = void(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using OMSetRenderTargetsFn = void(ID3D12GraphicsCommandList*, UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);

DXGI_FORMAT typed_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return f;
    }
}
} // namespace

Flat3DGuiRedirect& Flat3DGuiRedirect::get() {
    static Flat3DGuiRedirect inst{};
    return inst;
}

bool Flat3DGuiRedirect::init(ID3D12Device* device) {
    if (m_hooks_installed) {
        return true;
    }

    if (device == nullptr) {
        return false;
    }

    m_device = device;
    m_rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // A throwaway command list of the same implementation class exposes the shared vtable.
    ComPtr<ID3D12CommandAllocator> alloc{};
    ComPtr<ID3D12GraphicsCommandList> list{};

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
        spdlog::error("[Flat3D-GUIR] init: CreateCommandAllocator failed");
        return false;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        spdlog::error("[Flat3D-GUIR] init: CreateCommandList failed");
        return false;
    }

    list->Close();

    auto* device_vtbl = *(void***)device;
    auto* list_vtbl = *(void***)list.Get();

    // ID3D12Device: 20 = CreateRenderTargetView.
    // ID3D12GraphicsCommandList: 16 = CopyTextureRegion, 17 = CopyResource, 46 = OMSetRenderTargets.
    g_create_rtv_hook = std::make_unique<FunctionHook>((uintptr_t)device_vtbl[20], (uintptr_t)&hk_create_rtv);
    g_copy_texture_region_hook = std::make_unique<FunctionHook>((uintptr_t)list_vtbl[16], (uintptr_t)&hk_copy_texture_region);
    g_copy_resource_hook = std::make_unique<FunctionHook>((uintptr_t)list_vtbl[17], (uintptr_t)&hk_copy_resource);
    g_om_set_render_targets_hook = std::make_unique<FunctionHook>((uintptr_t)list_vtbl[46], (uintptr_t)&hk_om_set_render_targets);

    bool ok = g_create_rtv_hook->create() && g_copy_texture_region_hook->create()
        && g_copy_resource_hook->create() && g_om_set_render_targets_hook->create();

    if (!ok) {
        spdlog::error("[Flat3D-GUIR] init: detour installation failed (rtv={} ctr={} cr={} om={})",
            g_create_rtv_hook->is_valid(), g_copy_texture_region_hook->is_valid(),
            g_copy_resource_hook->is_valid(), g_om_set_render_targets_hook->is_valid());
        g_create_rtv_hook.reset();
        g_copy_texture_region_hook.reset();
        g_copy_resource_hook.reset();
        g_om_set_render_targets_hook.reset();
        return false;
    }

    m_hooks_installed = true;
    spdlog::info("[Flat3D-GUIR] D3D12 detours installed (CreateRTV/CopyTextureRegion/CopyResource/OMSetRenderTargets)");
    return true;
}

void Flat3DGuiRedirect::tick() {
    // One call per present. A capture only counts as live while windows keep completing; the
    // quest-rewards screen starves them entirely (measured: the 600-window counter log went silent
    // for minutes while the screen was up), and compositing a capture nobody is refreshing any more
    // is what paints a dead HUD layer over the screen.
    const auto n = m_frames_since_capture.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (n == CAPTURE_STALE_FRAMES) {
        spdlog::info("[Flat3D-GUIR] no GUI window completed for {} frames - dropping the GUI layer "
            "(armed={} idle_frames={}); scene shift/zoom neutralised and AFW warp bypassed until it returns",
            n, m_armed.load(std::memory_order_acquire), m_idle_frames.load(std::memory_order_acquire));
    }
}

bool Flat3DGuiRedirect::is_full_frame_target(ID3D12Resource* target) {
    if (target == nullptr) {
        return false;
    }

    auto& hook = g_framework->get_d3d12_hook();

    if (hook == nullptr) {
        return true; // can't tell - assume yes rather than disabling the redirect outright
    }

    auto* swapchain = hook->get_swap_chain();

    if (swapchain == nullptr) {
        return true;
    }

    DXGI_SWAP_CHAIN_DESC sd{};

    if (FAILED(swapchain->GetDesc(&sd)) || sd.BufferDesc.Width == 0 || sd.BufferDesc.Height == 0) {
        return true;
    }

    const auto d = target->GetDesc();

    // Half the backbuffer is the threshold rather than an exact match, so the engine's sub-region
    // rendering under the native-output override still passes.
    return d.Width * 2 >= sd.BufferDesc.Width && d.Height * 2 >= sd.BufferDesc.Height;
}

void Flat3DGuiRedirect::arm(ID3D12Resource* overlay_target, ID3D12Resource* marker_pre, ID3D12Resource* marker_post, uint32_t fresh_eye) {
    if (!m_hooks_installed || overlay_target == nullptr || marker_pre == nullptr || marker_post == nullptr) {
        disarm();
        return;
    }

    // The overlay layer does NOT always drive the full-frame HUD target. On the quest-rewards
    // screen it drives a 320x320 icon target for EVERY window (measured: ~55 rejections/s against
    // ~55 windows/s), and the real HUD target is never armed at all.
    //
    // Publishing that as m_overlay_target is actively harmful: substitution keys on
    // (rtv_map[handle] == m_overlay_target), so every real HUD bind stops matching (the redirect
    // goes idle and the UI stays in the engine's target = the scene texture), while an icon bind
    // that DOES match gets our full-size GUI RTV swapped in - a 320x320 draw redirected into the
    // 2560x1440 HUD texture, which the compose then samples at raw [0,1] as the whole HUD layer.
    //
    // So refuse it and keep the established target. Track how long we have gone without a
    // full-frame target: that is the signal for "this is a full-screen UI screen" (see
    // is_ui_screen), which the compose and the AFW warp both need.
    if (!is_full_frame_target(overlay_target)) {
        const auto idle = m_idle_frames.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (idle == UI_SCREEN_IDLE_FRAMES) {
            spdlog::info("[Flat3D-GUIR] no full-frame overlay target for {} frames - treating this as a "
                "full-screen UI screen (scene shift/zoom neutralised, AFW warp bypassed)", idle);
        }

        disarm();
        return;
    }

    if (m_idle_frames.exchange(0, std::memory_order_acq_rel) >= UI_SCREEN_IDLE_FRAMES) {
        spdlog::info("[Flat3D-GUIR] full-frame overlay target is back - resuming redirect + AFW warp");
    }

    if (m_window_active.exchange(false)) {
        // Post-marker never surfaced last frame (copy skipped / stream reordered) - the window
        // would otherwise leak across frames and swallow scene passes.
        spdlog::warn("[Flat3D-GUIR] GUI window was still open at arm time - force-closed");
    }

    m_overlay_target.store(overlay_target, std::memory_order_release);
    m_marker_pre.store(marker_pre, std::memory_order_release);
    m_marker_post.store(marker_post, std::memory_order_release);
    m_fresh_eye.store(fresh_eye & 1, std::memory_order_release);
    m_armed.store(true, std::memory_order_release);

    // Bound the shadow map (lists are reused by the executor, but be safe).
    std::lock_guard _{m_mutex};
    if (m_shadows.size() > 64) {
        m_shadows.clear();
    }
}

void Flat3DGuiRedirect::disarm() {
    m_armed.store(false, std::memory_order_release);
    m_window_active.store(false, std::memory_order_release);
}

void Flat3DGuiRedirect::on_reset() {
    disarm();
    std::lock_guard _{m_mutex};
    for (auto& c : m_captured) {
        c.store(false, std::memory_order_release);
    }
    for (auto& t : m_gui_tex) {
        t.Reset();
    }
    m_gui_rtv_heap.Reset();
    m_gui_format = DXGI_FORMAT_UNKNOWN;
    m_gui_w = 0;
    m_gui_h = 0;
    m_pending_format = DXGI_FORMAT_UNKNOWN;
    m_pending_w = 0;
    m_pending_h = 0;
    m_pending_count = 0;
    m_gui_in_rt_state = {};
    m_rtv_map.clear();
    m_shadows.clear();
}

void Flat3DGuiRedirect::ensure_gui_texture(ID3D12Resource* overlay_target) {
    const auto src_desc = overlay_target->GetDesc();
    const auto fmt = typed_format(src_desc.Format);

    // The overlay layer also runs for small offscreen renders (item/monster preview targets and
    // the like). Adopting one is destructive: a 320x320 R11G11B10_FLOAT target turned up
    // mid-session, tore down the real 3840x2160 capture and rebuilt it at icon size, after which
    // the compose sampled a 320x320 HDR texture as the entire HUD layer - and the GUI draws we
    // substitute got a render target far smaller than the viewport bound with it. Only the
    // full-frame target carries the HUD. Half the backbuffer is the threshold rather than an exact
    // match, so the engine's sub-region rendering under the native-output override still passes.
    if (auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
        if (auto* swapchain = hook->get_swap_chain(); swapchain != nullptr) {
            DXGI_SWAP_CHAIN_DESC sd{};

            if (SUCCEEDED(swapchain->GetDesc(&sd)) && sd.BufferDesc.Width != 0 && sd.BufferDesc.Height != 0
                    && (src_desc.Width * 2 < sd.BufferDesc.Width || src_desc.Height * 2 < sd.BufferDesc.Height)) {
                static uint32_t s_rejected = 0;

                if ((s_rejected++ % 600) == 0) {
                    spdlog::info("[Flat3D-GUIR] ignoring {}x{} fmt {} overlay target (backbuffer {}x{}) - "
                        "not the full-frame HUD target [x{}]",
                        (uint32_t)src_desc.Width, src_desc.Height, (int)fmt,
                        sd.BufferDesc.Width, sd.BufferDesc.Height, s_rejected);
                }

                return;
            }
        }
    }

    if (m_gui_tex[0] != nullptr && m_gui_tex[1] != nullptr
        && m_gui_w == (uint32_t)src_desc.Width && m_gui_h == src_desc.Height && m_gui_format == fmt) {
        m_pending_count = 0; // the established HUD target is still live this window
        return;
    }

    // A size/format CHANGE while a capture already exists. Rebuilding on the spot is what makes
    // certain full-screen menus (the quest-rewards screen) look zoomed AND stutter: when two
    // differently-sized overlay targets alternate, every frame tears the textures down and
    // rebuilds them at the other size, which
    //   (a) resets m_captured, so the compose alternates between having and not having a GUI
    //       (the stutter), and
    //   (b) leaves the compose sampling a texture whose dimensions no longer match the HUD it
    //       stands in for - the GUI is sampled at raw [0,1] with no content_frac (the zoom).
    // Both targets pass the half-the-backbuffer size gate above, so that check cannot catch this.
    // Require a new size to PERSIST before adopting it: a transient screen is ignored and the
    // established HUD texture keeps being used, while a genuine permanent change still lands.
    if (m_gui_tex[0] != nullptr && m_gui_tex[1] != nullptr) {
        if (m_pending_w != (uint32_t)src_desc.Width || m_pending_h != src_desc.Height || m_pending_format != fmt) {
            m_pending_w = (uint32_t)src_desc.Width;
            m_pending_h = src_desc.Height;
            m_pending_format = fmt;
            m_pending_count = 0;

            static uint32_t s_changes = 0;

            if ((s_changes++ % 120) == 0) {
                spdlog::info("[Flat3D-GUIR] overlay target {}x{} fmt {} differs from the established "
                    "{}x{} fmt {} - holding the established capture [x{}]",
                    m_pending_w, m_pending_h, (int)fmt, m_gui_w, m_gui_h, (int)m_gui_format, s_changes);
            }
        }

        if (++m_pending_count < GUI_SIZE_CHANGE_WINDOWS) {
            return;
        }

        spdlog::info("[Flat3D-GUIR] adopting {}x{} fmt {} after {} consecutive windows (established was {}x{})",
            m_pending_w, m_pending_h, (int)fmt, m_pending_count, m_gui_w, m_gui_h);
    }

    m_pending_count = 0;

    for (auto& t : m_gui_tex) {
        t.Reset();
    }
    m_gui_rtv_heap.Reset();
    for (auto& c : m_captured) {
        c.store(false, std::memory_order_release);
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = src_desc.Width;
    desc.Height = src_desc.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = fmt;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;

    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gui_rtv_heap)))) {
        spdlog::error("[Flat3D-GUIR] gui rtv heap creation failed");
        return;
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&m_gui_tex[eye])))) {
            spdlog::error("[Flat3D-GUIR] gui texture creation failed (eye {}, {}x{} fmt {})",
                eye, (uint32_t)src_desc.Width, src_desc.Height, (int)fmt);
            for (auto& t : m_gui_tex) {
                t.Reset();
            }
            m_gui_rtv_heap.Reset();
            return;
        }

        m_gui_rtv[eye] = m_gui_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        m_gui_rtv[eye].ptr += (SIZE_T)eye * m_rtv_increment;

        // Callers hold m_mutex and CreateRenderTargetView routes through our own detour - the
        // internal flag makes it skip the map (and the lock).
        s_internal_call = true;
        m_device->CreateRenderTargetView(m_gui_tex[eye].Get(), nullptr, m_gui_rtv[eye]);
        s_internal_call = false;

        m_gui_in_rt_state[eye] = true; // created in RENDER_TARGET
    }

    m_gui_w = (uint32_t)src_desc.Width;
    m_gui_h = src_desc.Height;
    m_gui_format = fmt;

    spdlog::info("[Flat3D-GUIR] per-eye gui textures created: {}x{} fmt {} (overlay target fmt {})",
        m_gui_w, m_gui_h, (int)fmt, (int)src_desc.Format);
}

void Flat3DGuiRedirect::begin_window(ID3D12GraphicsCommandList* list) {
    std::lock_guard _{m_mutex};

    const auto target = m_overlay_target.load(std::memory_order_acquire);
    if (target == nullptr) {
        return;
    }

    ensure_gui_texture(target);
    if (m_gui_tex[0] == nullptr || m_gui_tex[1] == nullptr) {
        return;
    }

    // Latch the eye for this window so the end-marker closes the SAME texture even if an arm()
    // for the next frame races in between.
    m_window_eye = m_fresh_eye.load(std::memory_order_acquire) & 1;
    const auto eye = m_window_eye;

    s_internal_call = true;

    if (!m_gui_in_rt_state[eye]) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_gui_tex[eye].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        m_gui_in_rt_state[eye] = true;
    }

    const float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(m_gui_rtv[eye], clear, 0, nullptr);

    // Proactively bind: if the engine never re-binds during the GUI (target left bound from a
    // previous pass), the substitution hook alone would miss every draw. Keep the engine's DSV
    // if this list had one bound.
    const auto it = m_shadows.find(list);
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv = (it != m_shadows.end() && it->second.valid && it->second.has_dsv)
        ? &it->second.dsv : nullptr;
    list->OMSetRenderTargets(1, &m_gui_rtv[eye], FALSE, dsv);

    s_internal_call = false;

    m_window_active.store(true, std::memory_order_release);

    const auto n = m_diag_windows.fetch_add(1) + 1;
    m_diag_windows_eye[eye].fetch_add(1);
    if (n == 1 || (n % 600) == 0) {
        spdlog::info("[Flat3D-GUIR] windows={} (L={} R={}) om_in_window={} substituted={} unknown_binds={}",
            n, m_diag_windows_eye[0].load(), m_diag_windows_eye[1].load(),
            m_diag_om_in_window.load(), m_diag_substituted.load(), m_diag_unknown_in_window.load());
    }
}

void Flat3DGuiRedirect::end_window(ID3D12GraphicsCommandList* list) {
    std::lock_guard _{m_mutex};

    const auto eye = m_window_eye;

    if (m_gui_tex[eye] != nullptr && m_gui_in_rt_state[eye]) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_gui_tex[eye].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        m_gui_in_rt_state[eye] = false;
        m_captured[eye].store(true, std::memory_order_release);
        m_frames_since_capture.store(0, std::memory_order_release);
    }

    // Re-bind what the engine last set on this list (pre-substitution values) so any pass that
    // assumes the binding persists keeps working.
    const auto it = m_shadows.find(list);
    if (it != m_shadows.end() && it->second.valid && it->second.num > 0) {
        s_internal_call = true;
        list->OMSetRenderTargets(it->second.num, it->second.rtvs.data(), FALSE,
            it->second.has_dsv ? &it->second.dsv : nullptr);
        s_internal_call = false;
    }

    m_window_active.store(false, std::memory_order_release);
}

void Flat3DGuiRedirect::handle_marker(ID3D12GraphicsCommandList* list, ID3D12Resource* dst) {
    if (!m_armed.load(std::memory_order_acquire) || dst == nullptr) {
        return;
    }

    if (dst == m_marker_pre.load(std::memory_order_acquire)) {
        if (!m_window_active.load(std::memory_order_acquire)) {
            begin_window(list);
        }
    } else if (dst == m_marker_post.load(std::memory_order_acquire)) {
        if (m_window_active.load(std::memory_order_acquire)) {
            end_window(list);
        }
    }
}

void STDMETHODCALLTYPE Flat3DGuiRedirect::hk_create_rtv(ID3D12Device* device, ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    auto& inst = get();

    if (!s_internal_call && inst.m_hooks_installed && resource != nullptr) {
        std::lock_guard _{inst.m_mutex};
        inst.m_rtv_map[handle.ptr] = resource;
        if (inst.m_rtv_map.size() > 100000) { // stale-handle runaway guard
            inst.m_rtv_map.clear();
        }
    }

    g_create_rtv_hook->get_original<CreateRTVFn>()(device, resource, desc, handle);
}

void STDMETHODCALLTYPE Flat3DGuiRedirect::hk_copy_texture_region(ID3D12GraphicsCommandList* list,
    const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    g_copy_texture_region_hook->get_original<CopyTextureRegionFn>()(list, dst, x, y, z, src, box);

    if (!s_internal_call && dst != nullptr && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        get().handle_marker(list, dst->pResource);
    }
}

void STDMETHODCALLTYPE Flat3DGuiRedirect::hk_copy_resource(ID3D12GraphicsCommandList* list,
    ID3D12Resource* dst, ID3D12Resource* src) {
    g_copy_resource_hook->get_original<CopyResourceFn>()(list, dst, src);

    if (!s_internal_call) {
        get().handle_marker(list, dst);
    }
}

void STDMETHODCALLTYPE Flat3DGuiRedirect::hk_om_set_render_targets(ID3D12GraphicsCommandList* list,
    UINT num, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL single_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    auto& inst = get();

    if (s_internal_call || !inst.m_armed.load(std::memory_order_acquire) || num > 8) {
        g_om_set_render_targets_hook->get_original<OMSetRenderTargetsFn>()(list, num, handles, single_range, dsv);
        return;
    }

    // Expand to an explicit handle array (handles single_range too) so we can shadow the values
    // (the caller's array is stack-transient) and substitute individual slots.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> expanded{};
    for (UINT i = 0; i < num; ++i) {
        expanded[i] = single_range ? D3D12_CPU_DESCRIPTOR_HANDLE{handles[0].ptr + (SIZE_T)i * inst.m_rtv_increment}
                                   : handles[i];
    }

    {
        std::lock_guard _{inst.m_mutex};

        auto& shadow = inst.m_shadows[list];
        shadow.num = num;
        shadow.rtvs = expanded;
        shadow.has_dsv = dsv != nullptr;
        if (dsv != nullptr) {
            shadow.dsv = *dsv;
        }
        shadow.valid = true;

        if (inst.m_window_active.load(std::memory_order_acquire) && inst.m_gui_tex[inst.m_window_eye] != nullptr) {
            inst.m_diag_om_in_window.fetch_add(1);
            const auto target = inst.m_overlay_target.load(std::memory_order_acquire);
            bool any_known = false;

            for (UINT i = 0; i < num; ++i) {
                const auto it = inst.m_rtv_map.find(expanded[i].ptr);
                if (it != inst.m_rtv_map.end()) {
                    any_known = true;
                    if (it->second == target) {
                        expanded[i] = inst.m_gui_rtv[inst.m_window_eye];
                        inst.m_diag_substituted.fetch_add(1);
                    }
                }
            }

            if (!any_known && num > 0) {
                inst.m_diag_unknown_in_window.fetch_add(1);

                // LEARN-AND-HIJACK: the overlay main target's RTV descriptor predates our hook
                // install (created once at startup), so the map can never resolve it - yet the
                // only single-RTV bind the engine issues INSIDE the GUI window is the main
                // target itself (the GUI pass re-binding its output right after our marker-A
                // bind; intermediates use dynamically created, mapped descriptors). Substitute
                // it AND record the mapping so it resolves normally from now on.
                if (num == 1) {
                    inst.m_rtv_map[expanded[0].ptr] = target;
                    spdlog::info("[Flat3D-GUIR] learned main-target RTV handle {:x} (in-window unknown single bind)",
                        (uintptr_t)expanded[0].ptr);
                    expanded[0] = inst.m_gui_rtv[inst.m_window_eye];
                    inst.m_diag_substituted.fetch_add(1);
                }
            }
        }
    }

    g_om_set_render_targets_hook->get_original<OMSetRenderTargetsFn>()(list, num, expanded.data(), FALSE, dsv);
}
} // namespace vrmod

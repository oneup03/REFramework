#pragma once

#include <array>
#include <optional>
#include <vector>
#include <unordered_set>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <wrl.h>

#include <../../directxtk12-src/Inc/GraphicsMemory.h>
#include <../../directxtk12-src/Inc/SpriteBatch.h>

#include "d3d12/CommandContext.hpp"
#include "d3d12/TextureContext.hpp"
#include "Flat3DCompose.hpp"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openvr.h>

class VR;

namespace vrmod {
class D3D12Component {
public:
    vr::EVRCompositorError on_frame(VR* vr);
    void on_post_present(VR* vr);

    void on_reset(VR* vr);

    // Flatscreen 3D multipass: convert a harvested per-eye texture (scene color,
    // R11G11B10_FLOAT on MHWilds) into its 8-bit eye cache via a format-converting
    // blit (the eye textures aren't CopyResource-compatible with the caches).
    // src_state is the source's resting state; if not PIXEL_SHADER_RESOURCE it is
    // transitioned to shader-readable for the blit and restored afterwards.
    void fill_flat3d_eye_converting(ID3D12Resource* src, uint32_t eye,
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // GUI-match: convert a pre-overlay (scene-only) eye clone into its 8-bit cache (left=diff source +
    // depth base, right=depth base for the shifted GUI).
    void fill_flat3d_pre(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, bool is_left);

    // AFW: feed the fresh AFR eye + harvested depth/MV to the PDAFWPlugin, warp into the other eye's
    // cache. No-op unless AFW is active and all inputs are ready.
    void run_flat3d_afw(VR* vr, uint32_t backbuffer_index);

    // AFW: snapshot the live engine depth/MV into our committed locals NOW. Called from the overlay
    // hook (render thread, mid-frame): the copy executes on the queue after the PREVIOUS frame's
    // work, while that frame's depth is still intact (at present time the engine may have already
    // cleared it -> zero depth -> flat warp).
    void afw_snapshot_depth_mv(VR* vr, ID3D12Resource* live_depth, ID3D12Resource* live_mv);

    void force_reset() { m_force_reset = true; }

    // Public wrapper so the UI-extraction path (VR.cpp) can tile-back a cloned overlay RTV texture
    // (Wilds create_texture hands back an unmapped reserved resource - the engine would draw the UI
    // onto nothing without this).
    void back_reserved(ID3D12Resource* native) { back_reserved_texture(native); }

    // UI extraction: issue an explicit resource-state transition on the engine's command queue for
    // a create_texture clone the engine can't transition itself (unregistered). Executes
    // immediately, so calling it in the pre-overlay hook queues it BEFORE the engine's frame submit.
    void flat3d_ui_barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    const auto& get_backbuffer_size() const { return m_backbuffer_size; }

    auto is_initialized() const { return m_openvr.left_eye_tex[0].texture != nullptr; }

    auto& openxr() { return m_openxr; }

    auto& get_sprite_batch() {
        return m_sprite_batch;
    }

private:
    void setup();
    void setup_sprite_batch_pso(DXGI_FORMAT output_format);
    void render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state);
    Flat3DCompose::RepackParams build_flat3d_params(VR* vr, uint32_t out_width, uint32_t out_height);

    // MHWilds create_texture returns a RESERVED (tiled) resource with NO tiles mapped (zero physical
    // backing) - so the engine copy_texture into our clone lands on nothing (reads black). Map the
    // resource's tiles to a dedicated heap so it becomes real, writable memory. Idempotent per native.
    void back_reserved_texture(ID3D12Resource* native);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    d3d12::TextureContext m_backbuffer_copy{};
    d3d12::TextureContext m_converted_eye_tex{};
    std::array<d3d12::CommandContext, 3> m_generic_copiers{};
    std::array<d3d12::CommandContext, 3> m_backbuffer_copy_commands{};
    d3d12::CommandContext m_flat3d_ui_barrier_cmd{}; // UI extraction: explicit state transitions

    // Dedicated tile-mapping heaps for RESERVED create_texture clones (kept alive), and the set of
    // natives we've already backed (idempotency).
    std::vector<ComPtr<ID3D12Heap>> m_reserved_tile_heaps{};
    std::unordered_set<void*> m_backed_reserved_natives{};

    std::unique_ptr<DirectX::DX12::SpriteBatch> m_sprite_batch{};

    // Mimicking what OpenXR does.
    struct OpenVR {
        d3d12::TextureContext& get_left() {
            auto& ctx = this->left_eye_tex[this->texture_counter % left_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& get_right() {
            auto& ctx = this->right_eye_tex[this->texture_counter % right_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& acquire_left() {
            auto& ctx = get_left();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        d3d12::TextureContext& acquire_right() {
            auto& ctx = get_right();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        void copy_left(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_left();
            ctx.commands.copy(src, ctx.texture.Get(), src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        void copy_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_right();
            ctx.commands.copy(src, ctx.texture.Get(), src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        std::array<d3d12::TextureContext, 3> left_eye_tex{};
        std::array<d3d12::TextureContext, 3> right_eye_tex{};
        uint32_t texture_counter{0};
        DXGI_FORMAT last_format{};
    } m_openvr;

    struct OpenXR {
        void initialize(XrSessionCreateInfo& session_info);
        std::optional<std::string> create_swapchains();
        void destroy_swapchains();
        using CopyFn = std::function<void(d3d12::CommandContext& ctx, d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state)>;
        void copy(uint32_t swapchain_idx, ID3D12Resource* src, D3D12_BOX* src_box, D3D12_RESOURCE_STATES src_state, CopyFn copy_fn = nullptr);
        void wait_for_all_copies() {
            std::scoped_lock _{this->mtx};

            for (auto& ctx : this->contexts) {
                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }
            }
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

        struct SwapchainContext {
            std::vector<XrSwapchainImageD3D12KHR> textures{};
            std::vector<std::unique_ptr<d3d12::TextureContext>> texture_contexts{};
            uint32_t num_textures_acquired{0};
        };

        std::vector<SwapchainContext> contexts{};
        std::recursive_mutex mtx{};
        std::array<uint32_t, 2> last_resolution{};
        DXGI_FORMAT last_format{};
    } m_openxr;

    Flat3DCompose m_flat3d_compose{};

    // Flat3D multipass: per-eye SRV-able intermediates wrapping the harvested
    // scene-color resource, so it can be blitted (format-converted) into the cache.
    std::array<d3d12::TextureContext, 2> m_flat3d_src{};

    // Flat3D GUI-match: SRV wrapper for the pre-overlay (scene-only) primary clone, plus its 8-bit
    // converted cache. The compose diffs the primary eye's post-overlay against this to isolate the
    // GUI and paint it onto the clone eye, keeping backgrounds stereo.
    d3d12::TextureContext m_flat3d_pre_src{};
    d3d12::TextureContext m_flat3d_pre_left_cache{};
    d3d12::TextureContext m_flat3d_pre_right_src{};
    d3d12::TextureContext m_flat3d_pre_right_cache{};

    // AFW: our committed snapshots of the live engine depth/MV, copied at present with our own
    // command list + explicit barriers (the engine copy path crashes on Wilds for depth). The depth
    // snapshot is a plain R32_FLOAT texture filled via planar CopyTextureRegion from D32S8 plane 0 -
    // typeless views can't be sampled by the plugin (it read zero -> flat 2D warp).
    ComPtr<ID3D12Resource> m_afw_depth_local{};
    ComPtr<ID3D12Resource> m_afw_mv_local{};
    ComPtr<ID3D12Resource> m_afw_depth_readback{}; // diagnostic: CPU depth verification
    // Plugin-extracted UI+alpha for THIS frame (set by run_flat3d_afw when the hudless chain is
    // live; borrowed plugin-owned textures) - the compose composites it into both eyes, with the
    // scene depth driving the per-pixel depth-adaptive UI mode.
    ID3D12Resource* m_flat3d_afw_ui_tex{nullptr};
    ID3D12Resource* m_flat3d_afw_ui_depth_tex{nullptr};

    // Set when an eye cache was filled at draw time (VR::on_prepare_output_layer_draw
    // -> fill_flat3d_eye_converting). on_frame then skips the backbuffer fallback and
    // composes the caches; reset after each compose.
    std::array<bool, 2> m_flat3d_eye_filled{};

    uint32_t m_backbuffer_size[2]{};
    bool m_backbuffer_is_8bit{false};
    bool m_force_reset{false};
};
} // namespace vrmod

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <d3d12.h>
#include <wrl.h>

#include <sdk/Renderer.hpp>
#include <sdk/intrusive_ptr.hpp>

#include "d3d12/CommandContext.hpp"

namespace vrmod {
// Depth-buffer sampling for flatscreen 3D auto-convergence and the dynamic
// crosshair. Two stages:
//  - Engine side (render thread, inside a layer callback with a live
//    RenderContext): keep an engine-side clone of the scene depth buffer up
//    to date via the engine's own state-safe CopyTexture command.
//  - Present side: asynchronously copy a sparse set of rows from the clone
//    into a READBACK buffer and reduce them on the CPU (robust percentile
//    "nearest" + median "center", per the dynamic3d skill).
class Flat3DDepth {
public:
    // Called from a render-layer hook. Clones the depth texture lazily and
    // copies into the clone every frame using the engine's copy command.
    void update_engine_copy(sdk::renderer::RenderContext* context, sdk::renderer::layer::Scene* scene_layer);

    // Called once per present. Polls the in-flight readback (never blocks),
    // reduces completed data, then kicks the next copy.
    void on_frame(float nearz);

    // External depth source override (the NGX/DLSS-harvested R32_FLOAT io texture): the engine
    // clone reads ZERO on Wilds (decoy accessors), but the DLSS input depth is real. Borrowed
    // pointer, refreshed by the caller every frame; resting state = PIXEL|NON_PIXEL shader
    // resource. Null falls back to the engine-clone path.
    void set_external_source(ID3D12Resource* tex) { m_external = tex; }

    void reset();

    // View-space meters; <= 0 when no valid sample yet.
    float get_nearest_depth() const { return m_nearest_m.load(); }
    float get_center_depth() const { return m_center_ema_m.load(); }

private:
    void create_readback(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
    void process(float nearz);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    static constexpr uint32_t NUM_ROWS = 24;    // sparse rows across the middle ~90% height
    static constexpr uint32_t NUM_COLUMNS = 64; // samples per row across the middle ~85% width

    std::recursive_mutex m_mtx{};

    // Engine side
    sdk::intrusive_ptr<sdk::renderer::Texture> m_depth_clone{};
    ID3D12Resource* m_last_depth{nullptr}; // change tracking only, not owning
    ID3D12Resource* m_external{nullptr};   // external source override (borrowed, per-frame)

    // Present side
    ComPtr<ID3D12Resource> m_readback{};
    d3d12::CommandContext m_commands{};
    bool m_commands_setup{false};
    bool m_copy_in_flight{false};

    uint64_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_row_pitch{0};
    DXGI_FORMAT m_copy_format{DXGI_FORMAT_UNKNOWN};
    std::array<uint32_t, NUM_ROWS> m_row_ys{};

    // Results (view-space meters), written on the present thread, read anywhere.
    std::atomic<float> m_nearest_m{-1.0f};
    std::atomic<float> m_center_ema_m{-1.0f};
};
} // namespace vrmod

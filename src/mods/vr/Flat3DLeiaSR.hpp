#pragma once

#include <d3d12.h>
#include <dxgi.h>

namespace SimulatedReality {
class SRInterfaceDX12;
}

namespace vrmod {
// LeiaSR (Simulated Reality) autostereo weaver hand-off. The compose pass
// produces a Side-by-Side intermediate; the SR weaver interleaves it for the
// lenticular panel using live eye tracking.
//
// Built on SR-lib's SRInterfaceDX12 wrapper (dependencies/SR-lib, api_expansion),
// which owns the SRContext/weaver lifecycle: it probes the delay-loaded SR DLLs
// before touching any SDK entry point, creates the context and weaver in the
// order the SDK requires (SRContext::create -> CreateDX12Weaver ->
// context->initialize, or eye tracking never binds to the weaver), converts the
// SDK's exceptions into an HRESULT, and pairs create() with deleteSRContext().
//
// Compiled against the SDK only when REF_LEIASR is defined (cmake option);
// otherwise every method is a stub returning false and the LeiaSR output mode
// falls back to plain Side-by-Side. Even when compiled in, the SR DLLs are
// delay-loaded so the mod still runs on machines without the SR runtime.
class Flat3DLeiaSR {
public:
    ~Flat3DLeiaSR() { destroy_weaver(); }

    // Idempotent. Creates the SR interface on first call; a failure is latched
    // so we don't retry the (relatively expensive) probe every frame.
    bool init(ID3D12Device* device, HWND window);

    bool is_ready() const { return m_sr != nullptr; }

    // Points the weaver at the SbS input texture. Pass the FULL combined
    // side-by-side texture (2W x H for a W-per-eye pair) - the DX12 weaver
    // samples exactly width x height texels, which SR-lib reads off the
    // resource desc so the weaver can't be told a size the texture isn't.
    void set_input(ID3D12Resource* sbs_texture);

    // Records the weave onto cmd_list targeting the currently bound render
    // target. The caller must have already reset the command list's own
    // rasterizer viewport/scissor to the destination extent (D3D12 rasterizes
    // against RSSetViewports, not against what the weaver is told here).
    // Returns false on failure (caller should fall back to plain SbS).
    bool weave(ID3D12GraphicsCommandList* cmd_list, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, DXGI_FORMAT output_format);

    // Tears down the weaver and, once nothing else is using it, the SRContext.
    void destroy_weaver();

private:
    SimulatedReality::SRInterfaceDX12* m_sr{nullptr};

    DXGI_FORMAT m_last_output_format{DXGI_FORMAT_UNKNOWN};
    bool m_init_attempted{false};
};
} // namespace vrmod

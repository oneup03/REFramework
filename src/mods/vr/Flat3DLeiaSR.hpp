#pragma once

#include <d3d12.h>
#include <dxgi.h>

namespace SR {
class SRContext;
class IDX12Weaver1;
}

namespace vrmod {
// LeiaSR (Simulated Reality) autostereo weaver hand-off. The compose pass
// produces a Side-by-Side intermediate; the SR weaver interleaves it for the
// lenticular panel using live eye tracking.
//
// Compiled against the SR SDK only when REF_LEIASR is defined (cmake option);
// otherwise every method is a stub returning false and the LeiaSR output mode
// falls back to plain Side-by-Side. Even when compiled in, the SR DLLs are
// delay-loaded and preflighted with LoadLibraryW so the mod still runs on
// machines without the SR runtime installed.
class Flat3DLeiaSR {
public:
    ~Flat3DLeiaSR() { destroy_weaver(); }

    // True when the SR runtime DLLs are present (cached LoadLibraryW preflight).
    bool available();

    // Idempotent. Creates the SRContext + DX12 weaver on first call
    // (SRContext::create -> CreateDX12Weaver -> setShaderSRGBConversion ->
    // context->initialize, in that order - the context must be initialized
    // AFTER the weaver exists or eye tracking never binds to it).
    bool init(ID3D12Device* device, HWND window);

    bool is_ready() const { return m_weaver != nullptr; }

    // Points the weaver at the SbS input texture (call whenever it changes).
    void set_input(ID3D12Resource* sbs_texture, int width, int height, DXGI_FORMAT format);

    // Records the weave onto cmd_list targeting the currently bound render
    // target. Returns false on failure (caller should fall back to plain SbS).
    bool weave(ID3D12GraphicsCommandList* cmd_list, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, DXGI_FORMAT output_format);

    // Destroys the weaver (kept separate from the SRContext, which survives
    // for the process lifetime once initialized).
    void destroy_weaver();

private:
    SR::SRContext* m_context{nullptr};
    SR::IDX12Weaver1* m_weaver{nullptr};

    ID3D12Resource* m_last_input{nullptr}; // change tracking only
    DXGI_FORMAT m_last_output_format{DXGI_FORMAT_UNKNOWN};

    bool m_checked_available{false};
    bool m_available{false};
    bool m_init_attempted{false};
};
} // namespace vrmod

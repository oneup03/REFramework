#include <algorithm>

#include <spdlog/spdlog.h>

#include <utility/Scan.hpp>
#include <utility/Module.hpp>

#include "Application.hpp"
#include "RETypeDB.hpp"
#include "RETypes.hpp"
#include "SceneManager.hpp"
#include "REGameObject.hpp"

#include "Renderer.hpp"

namespace detail {
using AddSceneViewFn = void (*)(void*);
AddSceneViewFn get_add_scene_view() {
    static void (*add_scene_view_fn)(void*) = nullptr;

    if (add_scene_view_fn == nullptr) {
        spdlog::info("[Renderer] Finding add_scene_view_fn");

        /*
        .text:0000000142952B16 41 8B 86 B4 0A 00 00                          mov     eax, [r14+0AB4h]
        .text:0000000142952B1D 83 E0 01                                      and     eax, 1
        .text:0000000142952B20 48 83 C0 1C                                   add     rax, 1Ch
        .text:0000000142952B24 48 69 C0 88 00 00 00                          imul    rax, 88h
        .text:0000000142952B2B 49 03 C6                                      add     rax, r14
        .text:0000000142952B2E 49 89 86 F0 0F 00 00                          mov     [r14+0FF0h], rax
        .text:0000000142952B35 48 8B 3D 4C 0C 39 06                          mov     rdi, cs:g_scene_manager
        .text:0000000142952B3C 48 8B 5F 20                                   mov     rbx, [rdi+20h]
        .text:0000000142952B40 48 8B CB                                      mov     rcx, rbx        ; lpCriticalSection
        .text:0000000142952B43 FF 15 67 E7 F6 01                             call    cs:EnterCriticalSection
        .text:0000000142952B49 45 33 C9                                      xor     r9d, r9d
        .text:0000000142952B4C 4C 8D 05 8D 0F 00 00                          lea     r8, addSceneView(via::SceneView*)
        */
        // String refs in the function containing this pattern:
        // L"Renderer::DelayEndTask"
        // L"Renderer::DelayReleaseTask"
        const auto mod = utility::get_executable();
        auto ref = utility::scan(mod, "4C 8D 05 ? ? ? ? 48 8D ? ? 48 8D ? 08 E8 ? ? ? ? 48 ? ? FF 15");

        if (!ref) {
            spdlog::error("[Renderer] Failed to find add_scene_view_fn");
            return nullptr;
        }

        add_scene_view_fn = (decltype(add_scene_view_fn))utility::calculate_absolute(*ref + 3);

        spdlog::info("[Renderer] add_scene_view_fn: {:x}", (uintptr_t)add_scene_view_fn);
    }

    return add_scene_view_fn;
}
}

namespace sdk {
namespace renderer {
RenderLayer* RenderLayer::add_layer(::REType* layer_type, uint32_t priority, uint8_t offset) {
    // can be found inside addSceneView
    static RenderLayer* (*add_layer_fn)(RenderLayer*, ::REType*, uint32_t, uint8_t) = nullptr;
    
    if (add_layer_fn == nullptr) {
        spdlog::info("[Renderer] Finding RenderLayer::AddLayer");

        const auto mod = utility::get_executable();
        
        auto ref = utility::scan(mod, "41 B8 00 00 00 05 48 8B F8 E8 ? ? ? ?"); // mov r8d, 5000000h; call add_layer

        if (!ref) {
            // Fallback pattern
            ref = utility::scan(mod, "41 B8 00 00 00 05 48 89 C7 E8 ? ? ? ?"); // mov r8d, 5000000h; call add_layer

            if (!ref) {
                auto add_scene_view_fn = detail::get_add_scene_view();

                if (add_scene_view_fn != nullptr) {
                    // Use a disassembler to scan through the function
                    // to find the call to add_scene_view_fn
                    // the function will be called multiple times, and will be the most called function within AddSceneView
                    spdlog::info("[Renderer] Scanning for RenderLayer::AddLayer using disassembler");
                    const auto potential_jmp = utility::scan_opcode((uintptr_t)add_scene_view_fn, 4, 0xE9);

                    if (potential_jmp) {
                        add_scene_view_fn = (decltype(add_scene_view_fn))utility::calculate_absolute(*potential_jmp + 1);
                        spdlog::info("[Renderer] Jmp detected, add_scene_view_fn: {:x}", (uintptr_t)add_scene_view_fn);
                    }

                    uintptr_t ip = (uintptr_t)add_scene_view_fn;

                    std::unordered_map<uintptr_t, uint32_t> calls;
                    uintptr_t best_call = 0;

                    for (auto i = 0 ; i < 150; ++i) {
                        const auto decoded = utility::decode_one((uint8_t*)ip);

                        if (!decoded) {
                            spdlog::error("[Renderer] Failed to decode instruction @ 0x{:x} ({:x})", ip, ip - (uintptr_t)add_scene_view_fn);
                            break;
                        }

                        if (std::string_view{decoded->Mnemonic}.starts_with("RET") || std::string_view{decoded->Mnemonic}.starts_with("INT3")) {
                            spdlog::error("[Renderer] Encountering RET or INT3 @ 0x{:x} ({:x})", ip, ip - (uintptr_t)add_scene_view_fn);
                            break;
                        }

                        if (*(uint8_t*)ip == 0xE8) {
                            const auto addr = utility::calculate_absolute(ip + 1);
                            calls[addr]++;

                            if (best_call != 0) {
                                if (calls[best_call] < calls[addr]) {
                                    best_call = addr;
                                }
                            } else {
                                best_call = addr;
                            }

                            if (calls[addr] >= 3) {
                                spdlog::info("[Renderer] Found 3 calls to add_scene_view_fn, stopping scan");
                                break;
                            }
                        }

                        ip += decoded->Length;
                    }

                    if (best_call != 0) {
                        spdlog::info("[Renderer] RenderLayer::AddLayer found at {:x}", best_call);
                        add_layer_fn = (decltype(add_layer_fn))best_call;
                    } else {
                        spdlog::error("[Renderer] Failed to find RenderLayer::AddLayer using a disassembler");
                    }
                }

                if (!ref && add_layer_fn == nullptr) {
                    spdlog::error("[Renderer] Failed to find add_layer");
                    return nullptr;
                }
            }
        }

        if (add_layer_fn == nullptr) {
            add_layer_fn = (decltype(add_layer_fn))utility::calculate_absolute(*ref + 10);

            if (add_layer_fn == nullptr || IsBadReadPtr(add_layer_fn, sizeof(add_layer_fn))) {
                spdlog::error("[Renderer] Failed to calculate add_layer");
                return nullptr;
            }
        }

        spdlog::info("[Renderer] RenderLayer::AddLayer: {:x}", (uintptr_t)add_layer_fn);
    }

    return add_layer_fn(this, layer_type, priority, offset);
}

sdk::NativeArray<RenderLayer*>& RenderLayer::get_layers() {
    static uint32_t layers_offset = 0;

    if (layers_offset == 0) {
        spdlog::info("[Renderer] Finding RenderLayer::layers");

        const auto root_layer = sdk::renderer::get_root_layer();
        
        if (root_layer == nullptr) {
            spdlog::error("[Renderer] Failed to find root layer");
            throw std::runtime_error("[Renderer] Failed to find root layer");
        }
        
        // Scan through the root layer for a pointer to a RenderLayer object
        for (auto i = 0; i < 0x500; i += sizeof(void*)) {
            auto ptr = *(RenderLayer***)((uintptr_t)root_layer + i);

            if (ptr == nullptr || IsBadReadPtr(ptr, sizeof(ptr))) {
                continue;
            }

            const auto potential_layer = *ptr;

            if (potential_layer == nullptr || IsBadReadPtr(potential_layer, sizeof(potential_layer))) {
                continue;
            }

            if (!REManagedObject::is_managed_object(potential_layer)) {
                continue;
            }

            if (potential_layer->is_a("via.render.RenderLayer")) {
                layers_offset = i;
                break;
            }
        }

        spdlog::info("[Renderer] RenderLayer::layers: {:x}", layers_offset);
    }

    return *(sdk::NativeArray<RenderLayer*>*)((uintptr_t)this + layers_offset);
}

RenderLayer** RenderLayer::find_layer(::REType* layer_type) {
    const auto& layers = get_layers();

    for (auto& layer : layers) {
        if (layer->info == nullptr || layer->info->get_class_info() == nullptr) {
            continue;
        }

        const auto t = layer->get_type();

        if (t == layer_type) {
            return &layer;
        }
    }

    return nullptr;
}

std::tuple<RenderLayer*, RenderLayer**> RenderLayer::find_layer_recursive(const ::REType* layer_type) {
    const auto& layers = get_layers();

    for (auto& layer : layers) {
        if (layer->info == nullptr || layer->info->get_class_info() == nullptr) {
            continue;
        }

        const auto t = layer->get_type();

        if (t == layer_type) {
            return std::make_tuple<RenderLayer*, RenderLayer**>(this, &layer);
        }

        if (auto f = layer->find_layer_recursive(layer_type); std::get<0>(f) != nullptr && std::get<1>(f) != nullptr) {
            return f;
        }
    }

    return std::make_tuple<RenderLayer*, RenderLayer**>(nullptr, nullptr);
}

std::tuple<RenderLayer*, RenderLayer**> RenderLayer::find_layer_recursive(std::string_view type_name) {
    const auto def = sdk::find_type_definition(type_name);

    if (def == nullptr) {
        return std::make_tuple<RenderLayer*, RenderLayer**>(nullptr, nullptr);
    }

    const auto t = def->get_type();

    if (t == nullptr) {
        return std::make_tuple<RenderLayer*, RenderLayer**>(nullptr, nullptr);
    }

    return find_layer_recursive(t);
}

std::vector<RenderLayer*> RenderLayer::find_layers(::REType* layer_type) {
    std::vector<RenderLayer*> out{};

    const auto& layers = get_layers();

    for (auto& layer : layers) {
        if (layer->info == nullptr || layer->info->get_class_info() == nullptr) {
            continue;
        }

        const auto t = layer->get_type();

        if (t == layer_type) {
            out.push_back(layer);
        }
    }

    return out;
}

std::vector<layer::Scene*> RenderLayer::find_all_scene_layers() {
    static auto scene_type = sdk::find_type_definition("via.render.layer.Scene")->get_type();

    if (scene_type == nullptr) {
        return {};
    }

    auto layers = find_layers(scene_type);

    if (layers.empty()) {
        return {};
    }

    return *(std::vector<layer::Scene*>*)&layers;
}

std::vector<layer::Scene*> RenderLayer::find_fully_rendered_scene_layers() {
    auto layers = find_all_scene_layers();

    if (layers.empty()) {
        return {};
    }

    std::erase_if(layers, [](auto& layer) {
        return !layer->is_fully_rendered();
    });

    std::sort(layers.begin(), layers.end(), [](auto& a, auto& b) {
        return a->get_view_id() < b->get_view_id();
    });

    return layers;
}

RenderLayer* RenderLayer::get_parent() {
    return sdk::call_object_func<RenderLayer*>(this, "get_Parent", sdk::get_thread_context(), this);
}

void RenderLayer::set_parent(RenderLayer* layer) {
    static std::optional<uint32_t> offset = std::nullopt;

    if (!offset) {
        const auto parent = get_parent();

        if (parent != nullptr) {
            for (auto i = 0; i < 0x100; i += sizeof(void*)) {
                if (*(RenderLayer**)((uintptr_t)this + i) == parent) {
                    offset = i;
                    spdlog::info("[Renderer] Parent offset: {:x}", i);
                    break;
                }
            }
        }
    }

    if (offset.has_value()) {
        *(RenderLayer**)((uintptr_t)this + *offset) = layer;
    }
}

RenderLayer* RenderLayer::find_parent(::REType* layer_type) {
    for (auto parent = get_parent(); parent != nullptr; parent = parent->get_parent()) {
        if (parent->info == nullptr || parent->info->get_class_info() == nullptr) {
            break;
        }

        const auto t = parent->get_type();

        if (t == layer_type) {
            return parent;
        }
    }

    return nullptr;
}

RenderLayer* RenderLayer::clone(bool recursive) {
    auto new_layer = (RenderLayer*)this->get_type_definition()->create_instance_full();

    if (new_layer == nullptr) {
        spdlog::error("[Renderer] Failed to clone layer");
        return nullptr;
    }

    new_layer->clone(this, recursive);

    return new_layer;
}

void RenderLayer::clone(RenderLayer* other, bool recursive) {
    this->m_parent = other->m_parent;
    this->m_priority = other->m_priority;

    for (uint32_t i = 0; i < sdk::renderer::RenderLayer::get_num_priority_offsets(); ++i) {
        this->m_priority_offsets[i] = other->m_priority_offsets[i];
    }

    this->clone_layers(other, recursive);
}

void RenderLayer::clone_layers(RenderLayer* other, bool recursive) {
    for (auto child_layer : other->get_layers()) {
        if (child_layer == this) {
            continue;
        }

        const auto def = child_layer->get_type_definition();

        if (def == nullptr) {
            continue;
        }

        const auto t = def->get_type();

        if (t == nullptr) {
            continue;
        }

        if (this->find_layer(t) != nullptr) {
            continue;
        }

        auto new_child_layer = add_layer(t, child_layer->m_priority);

        if (recursive && new_child_layer != nullptr) {
            new_child_layer->clone_layers(child_layer, recursive);
        }
    }
}

::sdk::renderer::TargetState* RenderLayer::get_target_state(std::string_view name) {
    return this->get_reflection_property<::sdk::renderer::TargetState*>(name);
}

void RenderContext::set_pipeline_state(sdk::renderer::PipelineState* pipeline_state) {
    using Fn = void (*)(RenderContext*, sdk::renderer::PipelineState*);
    static Fn set_pipeline_state_fn = []() -> Fn {
        spdlog::info("[RenderContext::set_pipeline_state] Searching for RenderContext::set_pipeline_state");

        const auto game = utility::get_executable();
        const auto string_data = utility::scan_string(game, "UpdateDepthBlockerState");

        if (!string_data) {
            spdlog::error("[RenderContext::set_pipeline_state] Failed to find UpdateDepthBlockerState string");
            return nullptr;
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string_data);

        if (!string_ref) {
            spdlog::error("[RenderContext::set_pipeline_state] Failed to find UpdateDepthBlockerState reference");
            return nullptr;
        }

        std::optional<uintptr_t> current_function_call{};
        uintptr_t current_ip{*string_ref + 4};

        // First one is murmur hash calc function
        // second: MasterMaterialResource::find
        // third: RenderResource::add_ref
        // fourth: RenderContext::set_pipeline_state
        for (size_t i = 0; i < 4; ++i) {
            current_function_call = utility::scan_mnemonic(current_ip, 100, "CALL");

            if (!current_function_call) {
                spdlog::error("[RenderContext::set_pipeline_state] Failed to find next CALL instruction");
                return nullptr;
            }

            current_ip = *current_function_call + 5;
        }

        const auto result = utility::resolve_displacement(*current_function_call);

        if (!result) {
            spdlog::error("[RenderContext::set_pipeline_state] Failed to resolve displacement");
            return nullptr;
        }

        spdlog::info("[RenderContext::set_pipeline_state] Found RenderContext::set_pipeline_state at {:x}", *result);

        return (Fn)*result;
    }();

    if (set_pipeline_state_fn == nullptr) {
        return;
    }

    set_pipeline_state_fn(this, pipeline_state);
}

void RenderContext::dispatch_ray(uint32_t tgx, uint32_t tgy, uint32_t tgz, Fence& fence) {
    using Fn = void (*)(RenderContext*, uint32_t, uint32_t, uint32_t, Fence*);
    static auto func = []() -> Fn {
        spdlog::info("[RenderContext::dispatch_ray] Searching for RenderContext::dispatch_ray");

        const auto game = utility::get_executable();
        const auto string_data = utility::scan_string(game, "PathSpaceRayTracing");

        if (!string_data) {
            spdlog::error("[RenderContext::dispatch_ray] Failed to find PathSpaceRayTracing string");
            return nullptr;
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string_data);

        if (!string_ref) {
            spdlog::error("[RenderContext::dispatch_ray] Failed to find PathSpaceRayTracing reference");
            return nullptr;
        }

        std::optional<uintptr_t> current_function_call{};
        uintptr_t current_ip{*string_ref + 4};

        // First one is murmur hash calc function
        // second: MasterMaterialResource::find
        // third: RenderResource::add_ref
        // fourth: RenderContext::set_pipeline_state
        // fifth: RenderResource::release
        // sixth: RenderContext::dispatch_ray
        for (size_t i = 0; i < 6; ++i) {
            current_function_call = utility::scan_mnemonic(current_ip, 100, "CALL");

            if (!current_function_call) {
                spdlog::error("[RenderContext::dispatch_ray] Failed to find next CALL instruction");
                return nullptr;
            }

            current_ip = *current_function_call + 5;
        }

        const auto result = utility::resolve_displacement(*current_function_call);

        if (!result) {
            spdlog::error("[RenderContext::dispatch_ray] Failed to resolve displacement");
            return nullptr;
        }

        spdlog::info("[RenderContext::dispatch_ray] Found RenderContext::dispatch_ray at {:x}", *result);

        return (Fn)*result;
    }();

    if (func == nullptr) {
        return;
    }

    func(this, tgx, tgy, tgz, &fence);
}

void RenderContext::dispatch_32bit_constant(uint32_t tgx, uint32_t tgy, uint32_t tgz, uint32_t constant, bool disable_uav_barrier) {
    using Fn = void (*)(RenderContext*, uint32_t, uint32_t, uint32_t, uint32_t, bool);
    static auto func = []() -> Fn {
        spdlog::info("[RenderContext::dispatch_32bit_constant] Searching for RenderContext::dispatch_32bit_constant");

        const auto game = utility::get_executable();
        const auto string_data = utility::scan_string(game, "ClearDepthBlockerState");

        if (!string_data) {
            spdlog::error("[RenderContext::dispatch_32bit_constant] Failed to find ClearDepthBlockerState string");
            return nullptr;
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string_data);

        if (!string_ref) {
            spdlog::error("[RenderContext::dispatch_32bit_constant] Failed to find ClearDepthBlockerState reference");
            return nullptr;
        }

        std::optional<uintptr_t> current_function_call{};
        uintptr_t current_ip{*string_ref + 4};

        // First one is murmur hash calc function
        // second: MasterMaterialResource::find
        // third: RenderResource::add_ref
        // fourth: RenderContext::set_pipeline_state
        // fifth: RenderResource::release
        // sixth: RenderContext::dispatch_32bit_constant
        for (size_t i = 0; i < 6; ++i) {
            current_function_call = utility::scan_mnemonic(current_ip, 100, "CALL");

            if (!current_function_call) {
                spdlog::error("[RenderContext::dispatch_32bit_constant] Failed to find next CALL instruction");
                return nullptr;
            }

            current_ip = *current_function_call + 5;
        }

        const auto result = utility::resolve_displacement(*current_function_call);

        if (!result) {
            spdlog::error("[RenderContext::dispatch_32bit_constant] Failed to resolve displacement");
            return nullptr;
        }

        spdlog::info("[RenderContext::dispatch_32bit_constant] Found RenderContext::dispatch_32bit_constant at {:x}", *result);

        return (Fn)*result;
    }();

    if (func == nullptr) {
        return;
    }

    func(this, tgx, tgy, tgz, constant, disable_uav_barrier);
}

void RenderContext::dispatch(uint32_t tgx, uint32_t tgy, uint32_t tgz, bool disable_uav_barrier) {
    using Fn = void (*)(RenderContext*, uint32_t, uint32_t, uint32_t, bool);
    static auto func = []() -> Fn {
        spdlog::info("[RenderContext::dispatch] Searching for RenderContext::dispatch");

        const auto game = utility::get_executable();
        std::optional<uintptr_t> string_data{};
        const auto all_strings = utility::scan_strings(game, "Reconstruct", true); // part of path space filter routine

        if (all_strings.empty()) {
            spdlog::error("[RenderContext::dispatch] Failed to find Reconstruct strings");
            return nullptr;
        }

        for (const auto& str : all_strings) {
            if (*(uint8_t*)(str - 1) == 0) { // Makes sure this string is standalone and not in the middle of another string
                string_data = str;
                break;
            }
        }

        if (!string_data) {
            spdlog::error("[RenderContext::dispatch] Failed to find correct Reconstruct string");
            return nullptr;
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string_data);

        if (!string_ref) {
            spdlog::error("[RenderContext::dispatch] Failed to find Reconstruct reference");
            return nullptr;
        }

        std::optional<uintptr_t> current_function_call{};
        uintptr_t current_ip{*string_ref + 4};

        // First one is murmur hash calc function
        // second: MasterMaterialResource::find
        // third: RenderResource::add_ref
        // fourth: RenderContext::set_pipeline_state
        // fifth: RenderResource::release
        // sixth: RenderContext::dispatch
        for (size_t i = 0; i < 6; ++i) {
            current_function_call = utility::scan_mnemonic(current_ip, 100, "CALL");

            if (!current_function_call) {
                spdlog::error("[RenderContext::dispatch] Failed to find next CALL instruction");
                return nullptr;
            }

            current_ip = *current_function_call + 5;
        }

        const auto result = utility::resolve_displacement(*current_function_call);

        if (!result) {
            spdlog::error("[RenderContext::dispatch] Failed to resolve displacement");
            return nullptr;
        }

        spdlog::info("[RenderContext::dispatch] Found RenderContext::dispatch at {:x}", *result);

        return (Fn)*result;
    }();

    if (func == nullptr) {
        return;
    }

    func(this, tgx, tgy, tgz, disable_uav_barrier);
}

sdk::renderer::command::Base* RenderContext::alloc(uint32_t t, uint32_t size) {
    // I am just being very lazy right now and just using a pattern instead of 
    // using copy_texture and scanning through the function for the first call
    static auto func = []() -> sdk::renderer::command::Base* (*)(RenderContext*, uint32_t, uint32_t) {
        spdlog::info("Searching for RenderContext::alloc");

        /*
            // In wilds this looks more like this
            BA 09 00 00 00    mov     edx, 9
            41 B8 30 00 00 00 mov     r8d, 30h
            E8 ? ? ? ?        call    alloc
        */
        const auto game = utility::get_executable();
        const auto scan_result = utility::scan(game, "48 8b ? 44 8d 42 38 e8 ? ? ? ?");

        if (!scan_result) {
            const auto midfn_result = utility::scan(game, "81 FF ? 08 00 00 *[32] 8D ? 0F 83 ? f0");

            if (midfn_result) {
                const auto fn_start = utility::find_function_start_unwind(*midfn_result);
                if (!fn_start) {
                    spdlog::error("Failed to find start of function for potential RenderContext::alloc");
                    return nullptr;
                }
                spdlog::info("Found potential RenderContext::alloc at {:x} using mid-function pattern", *fn_start);
                return (sdk::renderer::command::Base* (*)(RenderContext*, uint32_t, uint32_t))*fn_start;
            }

            spdlog::error("Failed to find RenderContext::alloc");
            return nullptr;
        }

        const auto result = utility::calculate_absolute(*scan_result + 8);

        spdlog::info("Found RenderContext::alloc at {:x}", result);

        return (sdk::renderer::command::Base* (*)(RenderContext*, uint32_t, uint32_t))result;
    }();

    return func(this, t, size);
}

static_assert(offsetof(command::Clear, clear_color) == 0x28, "Clear::clear_color offset is wrong");
static_assert(offsetof(command::Clear, view) == 0x20, "Clear::view offset is wrong");
static_assert(offsetof(command::Clear, target) == 0x18, "Clear::target offset is wrong");

void RenderContext::clear_rtv(sdk::renderer::RenderTargetView* rtv, float color[4], bool delay) {
    if (rtv == nullptr) {
        return;
    }

    static const auto clear_typeid = sdk::get_enum_value<uint32_t>("via.render.command.TypeId", "Clear");
    auto new_command = (command::Clear*)alloc(clear_typeid, sizeof(command::Clear));

    if (new_command != nullptr) {
        const auto protect_frame = get_protect_frame();

        if (rtv->m_render_frame != protect_frame) {
            rtv->m_render_frame = protect_frame;
        }

        new_command->target = get_render_target();

        if (delay && is_delay_enabled()) {
            new_command->clear_type = 128;
        } else {
            new_command->clear_type = 0;
        }

        new_command->view.rtv = rtv;
        new_command->clear_color[0] = color[0];
        new_command->clear_color[1] = color[1];
        new_command->clear_color[2] = color[2];
        new_command->clear_color[3] = color[3];
    }
}

/*
- 0x9B CopyImage
+ 0x93 ReadonlyDepth
- 0x8A TemporalDenoiserGBufferCombine
+ 0x85 PrevAODepth
- 0x75 InputVelocity
- 0x6D ModifiedGBufferSRV
+ 0x66 g_BilateralUpscaleDownscaledDepth
- 0x62 RE_POSTPROCESS_Color
- 0x5B CopyImage
+ 0x48 PostEffect Copy
- 0x2D InputVelocity
- 0x24 InterleaveNormalDepth
- 0x1D InterleaveNormalDepthHalf
- 0x1D RE_POSTPROCESS_Color
- 0x14 InterleaveNormalDepthWithoutGBuffer
- 0x11 CopyImage
- 0xD InterleaveNormalDepthHalfWithoutGBuffer
*/
void RenderContext::copy_texture(Texture* dest, Texture* src, Fence& fence) {
    // Okay it was actually this simple in older games but it isn't anymore in DD2+
    // There's some extra garbage going on that I don't want to deal with right now
    // so will just call the function directly
/*#if TDB_VER >= 73
    static const auto copy_texture_typeid = sdk::get_enum_value<uint32_t>("via.render.command.TypeId", "CopyTexture");
    auto new_command = (command::CopyTexture*)alloc(copy_texture_typeid, sizeof(command::CopyTexture));

    if (new_command != nullptr) {
        const auto protect_frame = get_protect_frame();

        if (dest->m_render_frame != protect_frame) {
            dest->m_render_frame = protect_frame;
        }

        if (src->m_render_frame != protect_frame) {
            src->m_render_frame = protect_frame;
        }

        new_command->dst = dest;
        new_command->src = src;
        new_command->fence = fence;
        new_command->dst_subresource = -1;
        new_command->src_subresource = -1;
    }
    
    return;
#else*/

    // Two implementations depending on TDB version: the legacy (<82) form takes
    // a single source/dest pair; the modern (>=82) form takes per-texture subresource
    // indices. They resolve to different native functions with different signatures,
    // so in universal builds we dispatch at runtime but both branches must compile.
#if defined(REFRAMEWORK_UNIVERSAL) || TDB_VER < 82
    auto copy_legacy = [&]() {
        using CopyTexFn = void (*)(RenderContext*, Texture*, Texture*, Fence&);
        static auto func = []() -> CopyTexFn {
            spdlog::info("Searching for RenderContext::copy_texture");

            std::vector<std::string> string_choices {
            };
            if (sdk::GameIdentity::get().tdb_ver() < 73) {
                string_choices.push_back("InterleaveNormalDepthHalfWithoutGBuffer");
            }
            string_choices.push_back("opyImage");
            string_choices.push_back("CopyImage");
            {

            const auto game = utility::get_executable();

            for (const auto& str_choice : string_choices) {
                spdlog::info("Scanning for string: {}", str_choice);

                const auto string = utility::scan_string(game, str_choice, true);

                if (!string) {
                    spdlog::error("Failed to find copy_texture (no string)");
                    continue;
                }

                const auto string_ref = utility::scan_displacement_reference(game, *string);

                if (!string_ref) {
                    spdlog::error("Failed to find copy_texture (no string ref)");
                    continue;
                }

                uintptr_t ip = *string_ref;

                for (auto i = 0; i < 20; ++i) {
                    const auto resolved = utility::resolve_instruction(ip);

                    if (!resolved) {
                        spdlog::error("Failed to find copy_texture (could not resolve instruction)");
                        continue;
                    }

                    ip = resolved->addr;

                    if (*(uint8_t*)ip == 0xE8) {
                        const auto result = (CopyTexFn)utility::calculate_absolute(ip + 1);

                        spdlog::info("Found copy_texture: {:x}", (uintptr_t)result);
                        return result;
                    }

                    ip -= 1;
                }
            }
            }

            spdlog::error("Could not find copy_texture");
            return (CopyTexFn)nullptr;
        }();

        if (func != nullptr) {
            func(this, dest, src, fence);
        }
    };
#endif

#if defined(REFRAMEWORK_UNIVERSAL) || TDB_VER >= 82
    auto copy_modern = [&]() -> bool {
        using CopyTexFn = void (*)(RenderContext*, Texture*, int32_t, Texture*, int32_t, Fence&);
        static auto func = []() -> CopyTexFn {
            spdlog::info("Searching for RenderContext::copy_texture (>= TDB82)");

            const auto game = utility::get_executable();
            // constants 0x301 (the typeid 1 or'd with something) 0x36, 0x3f, 0x2a.
            const auto mid_result = utility::scan(game, "01 03 00 00 *[64] 36 *[32] 3f *[32] 2a");

            if (!mid_result) {
                spdlog::error("Failed to find copy_texture (>= TDB82)");
                return (CopyTexFn)nullptr;
            }

            const auto fn_start = utility::find_function_start_unwind(*mid_result);

            if (!fn_start) {
                spdlog::error("Failed to find copy_texture function start (>= TDB82)");
                return (CopyTexFn)nullptr;
            }

            spdlog::info("Found copy_texture (>= TDB82) at {:x}", *fn_start);

            return (CopyTexFn)*fn_start;
        }();

        if (func != nullptr) {
            // src, src_subresource, dst, dst_subresource, fence
            func(this, src, -1, dest, -1, fence);
            return true;
        }

        return false;
    };
#endif

#if defined(REFRAMEWORK_UNIVERSAL) || TDB_VER >= 73
    // Fallback for modern engines (e.g. MHWilds, TDB 81) that don't expose a
    // standalone copy_texture we can resolve by scan. Instead of calling the
    // function directly we build a CopyTexture render command through the
    // RenderContext command allocator, exactly like clear_rtv() builds a Clear.
    //
    // Verified by static analysis of the MHWilds executable:
    //   * RenderContext::alloc resolves (mid-function pattern in Renderer.cpp).
    //   * via.render.command.TypeId::CopyTexture == 1 (reflection enum registration).
    //   * The engine's command executor dispatch table handles typeid 1 (CopyTexture)
    //     right beside typeid 0 (Clear), so a command allocated this way is drained
    //     and executed by the same path clear_rtv already relies on.
    // This is the "extra garbage" path the direct-call code originally punted on:
    // on TDB>=82 the direct scan works and is preferred; only when it fails do we
    // fall back here.
    auto copy_alloc = [&]() -> bool {
        static const auto copy_texture_typeid = sdk::get_enum_value<uint32_t>("via.render.command.TypeId", "CopyTexture");

        // CopyTexture is enum value 1; if reflection lookup came back 0 (not found),
        // fall back to the known-correct constant rather than aliasing onto Clear (0).
        const uint32_t t = copy_texture_typeid != 0 ? copy_texture_typeid : 1u;

        auto new_command = (command::CopyTexture*)alloc(t, sizeof(command::CopyTexture));

        if (new_command == nullptr) {
            return false;
        }

        const auto protect_frame = get_protect_frame();

        if (dest->m_render_frame != protect_frame) {
            dest->m_render_frame = protect_frame;
        }

        if (src->m_render_frame != protect_frame) {
            src->m_render_frame = protect_frame;
        }

        new_command->src = (sdk::renderer::RenderResource*)src;
        new_command->dst = (sdk::renderer::RenderResource*)dest;
        new_command->fence = fence;
        new_command->src_subresource = -1;
        new_command->dst_subresource = -1;

        // MHWilds: the real CopyTexture command is 0x30 bytes laid out as
        //   [0x10]=src  [0x18]=dst  [0x20]={sync token}  [0x28]=src_subres  [0x2c]=dst_subres
        // There is NO Fence member - the SDK's CopyBase model (fence at 0x20-0x2f, subres at
        // 0x30/0x34) is wrong for Wilds. copy_alloc's `new_command->fence = fence` therefore
        // writes garbage over BOTH the sync token (0x20) and the subresource slots (0x28/0x2c).
        // Fix them to what the engine's own builder writes:
        //   * sync token [0x20] = {id:-2 "immediate/no-fence", val:0} - anything else here makes
        //     the executor defer or drop the copy (destination stays BLACK).
        //   * subresources [0x28]/[0x2c] = 0 (single-subresource; matches the primed tracker).
        *(uint64_t*)((uintptr_t)new_command + 0x20) = 0x00000000fffffffeull;
        *(int32_t*)((uintptr_t)new_command + 0x28) = 0;
        *(int32_t*)((uintptr_t)new_command + 0x2c) = 0;

        return true;
    };

    // MHWilds: call the engine's OWN CopyTexture command builder instead of hand-assembling the
    // command (copy_alloc did that and produced a no-op/black copy). The builder writes the exact
    // 0x30-byte layout the executor expects. Signature (verified from its 58 call sites):
    //   f(RenderContext* ctx /*rcx*/, RenderResource* dst /*rdx*/, RenderResource* src /*r8*/,
    //     Tracking* /*r9*/) where Tracking = { int32 id; int32 val; int64 subresource_bitmap };
    // id = -2 ("immediate / no fence"), val = 0, bitmap = 0 (no per-subresource marking).
    auto copy_engine = [&]() -> bool {
        using CopyEngineFn = void (*)(void*, void*, void*, void*);
        static auto fn = []() -> CopyEngineFn {
            const auto game = utility::get_executable();
            const auto match = utility::scan(game,
                "41 56 56 57 53 48 83 EC 28 4C 89 CE 4C 89 C3 48 89 D7 49 89 CE BA 01 00 00 00 41 B8 30 00 00 00 E8");

            if (!match) {
                spdlog::error("Failed to find engine copy_texture builder");
                return nullptr;
            }

            spdlog::info("Found engine copy_texture builder: {:x}", *match);
            return (CopyEngineFn)*match;
        }();

        if (fn == nullptr) {
            return false;
        }

        struct Tracking { int32_t id; int32_t val; uint64_t bitmap; } tracking{ -2, 0, 0 };
        fn(this, (void*)dest, (void*)src, &tracking);
        return true;
    };
#endif

    // MHWilds (TDB 81) is "82-like" (it carries the same tdb82_padding proven by
    // the RenderResource offset dump). Its copy_texture is not a resolvable
    // standalone function (the "CopyImage" string is gone and the >=82 byte pattern
    // doesn't match), so call the engine's command builder directly. TDB>=82 titles
    // (DD2/RE4) keep using the working direct-call scan.
    if (sdk::GameIdentity::get().tdb_ver() < 81) {
        copy_legacy();
    } else if (!copy_engine() && !copy_modern()) {
        copy_alloc();
    }
//#endif
}

std::optional<uint32_t> Renderer::get_render_frame() const {
    static auto tdef = sdk::find_type_definition("via.render.Renderer");
    static auto m = tdef != nullptr ? tdef->get_method("get_RenderFrame") : nullptr;

    if (m == nullptr) {
        return std::nullopt;
    }

    return m->call<uint32_t>(sdk::get_thread_context(), this);
}

ConstantBuffer* Renderer::get_constant_buffer(std::string_view name) const {
    static auto tdef = sdk::find_type_definition("via.render.Renderer");
    static auto t = tdef->get_type();
    const auto field_desc = utility::re_type::get_field_desc(t, name);
    return ((::REManagedObject*)this)->get_reflection_property<ConstantBuffer*>(field_desc);
}

Renderer* get_renderer() {
    return (Renderer*)sdk::get_native_singleton("via.render.Renderer");
}

void wait_rendering() {
    static auto wait_rendering_entry = sdk::Application::get()->get_function("WaitRendering");

    return wait_rendering_entry->func(wait_rendering_entry->entry);
}

void begin_rendering() {
    static auto begin_rendering_entry = sdk::Application::get()->get_function("BeginRendering");

    return begin_rendering_entry->func(begin_rendering_entry->entry);
}

void end_rendering() {
    static auto end_rendering_entry = sdk::Application::get()->get_function("EndRendering");

    return end_rendering_entry->func(end_rendering_entry->entry);
}

void begin_update_primitive() {
    static auto begin_update_primitive_entry = sdk::Application::get()->get_function("BeginUpdatePrimitive");

    return begin_update_primitive_entry->func(begin_update_primitive_entry->entry);
}

void update_primitive() {
    static auto update_primitive_entry = sdk::Application::get()->get_function("UpdatePrimitive");

    return update_primitive_entry->func(update_primitive_entry->entry);
}

void end_update_primitive() {
    static auto end_update_primitive_entry = sdk::Application::get()->get_function("EndUpdatePrimitive");

    return end_update_primitive_entry->func(end_update_primitive_entry->entry);
}

void add_scene_view(void* scene_view) {
    detail::get_add_scene_view()(scene_view);
}

void remove_scene_view(void* scene_view) {
    static void (*remove_scene_view_fn)(void*) = nullptr;

    if (remove_scene_view_fn == nullptr) {
        spdlog::info("[Renderer] Finding remove_scene_view_fn");

        // Almost the same as add_scene_view pattern, is set up right after add_scene_view
        const auto mod = utility::get_executable();
        auto ref = utility::scan(mod, "4C 8D 05 ? ? ? ? 48 8D ? ? ? 48 8D ? 28 E8 ? ? ? ? 48 ? ? FF 15");

        if (!ref) {
            spdlog::error("[Renderer] Failed to find remove_scene_view_fn");
            return;
        }

        remove_scene_view_fn = (decltype(remove_scene_view_fn))utility::calculate_absolute(*ref + 3);

        spdlog::info("[Renderer] remove_scene_view_fn: {:x}", (uintptr_t)remove_scene_view_fn);
    }

    remove_scene_view_fn(scene_view);
}

RenderLayer* get_root_layer() {
    auto renderer = sdk::get_native_singleton("via.render.Renderer");

    if (renderer == nullptr) {
        spdlog::error("[Renderer] Failed to find renderer");
        return nullptr;
    }

    static uint32_t root_layer_offset = 0;

    if (root_layer_offset == 0) {
        spdlog::info("[Renderer] Finding root_layer_offset");

        auto get_output_layer_fn = sdk::find_native_method("via.render.Renderer", "getOutputLayer");

        if (get_output_layer_fn == nullptr) {
            spdlog::error("[Renderer] Failed to find getOutputLayer");

            // Hacky fix for >= TDB74
            for (uint32_t i = 0; i < 0x10000; i += sizeof(void*)) {
                const auto ptr = *(REManagedObject**)((uintptr_t)renderer + i);

                if (ptr == nullptr) {
                    continue;
                }

                if (!REManagedObject::is_managed_object(ptr)) {
                    continue;
                }

                if (ptr->is_a("via.render.RenderLayer")) {
                    root_layer_offset = i;
                    spdlog::info("[Renderer] Found root_layer_offset with fallback: {:x}", root_layer_offset);
                    return *(RenderLayer**)((uintptr_t)renderer + root_layer_offset);
                }
            }

            spdlog::error("[Renderer] Failed to find root_layer_offset with fallback");

            return nullptr;
        }

        // Resolve the jmp to the real function
        if (((uint8_t*)get_output_layer_fn)[0] == 0xE9) {
            get_output_layer_fn = (decltype(get_output_layer_fn))utility::calculate_absolute((uintptr_t)get_output_layer_fn + 1);
        } else {
            // Scan for jump with disassembler
            spdlog::info("[Renderer] Scanning for getOutputLayer jmp");

            const auto potential_jmp = utility::scan_opcode((uintptr_t)get_output_layer_fn, 10, 0xE9);

            if (potential_jmp) {
                get_output_layer_fn = (decltype(get_output_layer_fn))utility::calculate_absolute(*potential_jmp + 1);
                spdlog::info("[Renderer] Found getOutputLayer jmp, new function {:x}", (uintptr_t)get_output_layer_fn);
            } else {
                spdlog::info("[Renderer] No jmp found");
            }
        }

        spdlog::info("[Renderer] Real getOutputLayer: {:x}", (uintptr_t)get_output_layer_fn);

        // Find the offset to the root layer (RE3, RE8)
        auto ref = utility::scan((uintptr_t)get_output_layer_fn, 0x100, "48 8B 81 ? ? ? ?");

        if (!ref) {
            // Fallback pattern to scan for (RE2)
            ref = utility::scan((uintptr_t)get_output_layer_fn, 0x100, "4C 8B 80 ? ? ? ?");

            // fallback pattern to scan for (RE7)
            if (!ref) {
                ref = utility::scan((uintptr_t)get_output_layer_fn, 0x100, "4C 8B 89 ? ? ? ?"); // mov r9, [rcx+?]
            }

            if (!ref) {
                spdlog::error("[Renderer] Failed to find root_layer_offset");
                return nullptr;
            }
        }

        root_layer_offset = *(uint32_t*)(*ref + 3);

        spdlog::info("[Renderer] root_layer_offset: {:x}", root_layer_offset);
    }

    return *(RenderLayer**)((uintptr_t)renderer + root_layer_offset);
}

RenderLayer* find_layer(::REType* layer_type) {
    auto renderer = sdk::get_native_singleton("via.render.Renderer");

    if (renderer == nullptr) {
        spdlog::error("[Renderer] Failed to find renderer");
        return nullptr;
    }

    static uint32_t layers_offset = 0;

    // Scan through the renderer object to find a RenderLayer pointer
    if (layers_offset == 0) {
        spdlog::info("[Renderer] Finding layers_offset");

        for (uint32_t i = 0; i < 0x10000; i += sizeof(void*)) {
            const auto ptr = *(REManagedObject**)((uintptr_t)renderer + i);

            if (ptr == nullptr) {
                continue;
            }

            if (!REManagedObject::is_managed_object(ptr)) {
                continue;
            }

            if (ptr->is_a("via.render.RenderLayer")) {
                layers_offset = i;
                break;
            }
        }

        if (layers_offset == 0) {
            spdlog::error("[Renderer] Failed to find layers_offset");
            return nullptr;
        }

        spdlog::info("[Renderer] layers_offset: {:x}", layers_offset);
    }

    const auto& layers = *(std::array<RenderLayer*, 256>*)((uintptr_t)renderer + layers_offset);

    for (auto& layer : layers) {
        if (layer->info == nullptr || layer->info->get_class_info() == nullptr) {
            continue;
        }

        const auto t = layer->get_type();

        if (t == layer_type) {
            return layer;
        }
    }

    return nullptr;
}

sdk::renderer::layer::Output* get_output_layer() {
    auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    if (renderer_t == nullptr) {
        spdlog::error("[Renderer] Failed to find via.render.Renderer type");
        return nullptr;
    }

    static auto get_output_layer_method = renderer_t->get_method("getOutputLayer");

    if (get_output_layer_method == nullptr) {
        auto root = get_root_layer();

        if (root == nullptr) {
            return nullptr;
        }

        static auto output_t = sdk::find_type_definition("via.render.layer.Output");
        static auto output_retype = output_t != nullptr ? output_t->get_type() : nullptr;

        auto [parent, found] = root->find_layer_recursive(output_retype);

        if (found == nullptr) {
            return nullptr;
        }

        return (sdk::renderer::layer::Output*)*found;
    }

    return sdk::call_native_func<sdk::renderer::layer::Output*>(nullptr, renderer_t, "getOutputLayer", sdk::get_thread_context(), nullptr);
}

std::optional<Vector2f> world_to_screen(const Vector3f& world_pos) {
    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return std::nullopt;
    }

    auto main_view = sdk::get_main_view();

    if (main_view == nullptr) {
        return std::nullopt;
    }

    auto context = sdk::get_thread_context();

    static auto transform_def = sdk::find_type_definition("via.Transform");
    static auto math_t = sdk::find_type_definition("via.math");

    static auto get_gameobject_method = transform_def->get_method("get_GameObject");
    static auto get_axisz_method = transform_def->get_method("get_AxisZ");
    static auto world_to_screen = math_t->get_method("worldPos2ScreenPos(via.vec3, via.mat4, via.mat4, via.Size)");

    auto camera_gameobject = get_gameobject_method->call<REGameObject*>(context, camera);
    auto camera_transform = camera_gameobject->get_transform();

    Matrix4x4f proj{}, view{};
    float screen_size[2]{};

    auto camera_origin = sdk::get_transform_position(camera_transform);
    camera_origin.w = 1.0f;

    Vector4f camera_forward{};
    get_axisz_method->call<void*>(&camera_forward, context, camera_transform);

    camera_forward.w = 1.0f;

    sdk::call_object_func<void*>(camera, "get_ProjectionMatrix", &proj, context, camera);
    sdk::call_object_func<void*>(camera, "get_ViewMatrix", &view, context, camera);
    sdk::call_object_func<void*>(main_view, "get_WindowSize", &screen_size, context, main_view);

    const Vector4f pos = Vector4f{world_pos, 1.0f};
    Vector4f screen_pos{};

    const auto delta = pos - camera_origin;

    // behind camera
    if (glm::dot(Vector3f{delta}, Vector3f{-camera_forward}) <= 0.0f) {
        return std::nullopt;
    }

    world_to_screen->call<void*>(&screen_pos, context, &pos, &view, &proj, &screen_size);

    return Vector2f{screen_pos.x, screen_pos.y};
}

/*
- 0x4B VortexelTurbulenceGPU::VelocitiesX
- 0x4A systems/shader/rayTracingDenoiserOld/rayTracingSimulation.sdf
- 0x46 systems/rendering/NullWhite.tex
- 0x46 systems/effect/Noise3D_MSK4.tex
- 0x19 UpdateDepthBlocker
- 0x19 Deinterlace
- 0x19 cbGeneratePolyline
- 0x19 cbTransformBasePoints
- 0x19 CBBakeType
- 0x19 cbGenerateBasePoints
*/
ConstantBuffer* create_constant_buffer(void* desc) {
    static auto fn = []() -> ConstantBuffer* (*)(void*, void*) {
        spdlog::info("Searching for create_constant_buffer");

        const auto game = utility::get_executable();
        const auto string = utility::scan_string(game, "cbTransformBasePoints");

        if (!string) {
            spdlog::error("Failed to find create_constant_buffer (no string)");
            return nullptr;
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string);

        if (!string_ref) {
            spdlog::error("Failed to find create_constant_buffer (no string ref)");
            return nullptr;
        }

        uintptr_t ip = *string_ref;

        for (auto i = 0; i < 20; ++i) {
            const auto resolved = utility::resolve_instruction(ip);

            if (!resolved) {
                spdlog::error("Failed to find create_constant_buffer (could not resolve instruction)");
                return nullptr;
            }

            ip = resolved->addr;

            if (*(uint8_t*)ip == 0xE8) {
                const auto result = (ConstantBuffer* (*)(void*, void*))utility::calculate_absolute(ip + 1);

                spdlog::info("Found create_constant_buffer: {:x}", (uintptr_t)result);
                return result;
            }

            ip -= 1;
        }

        return nullptr;
    }();

    return fn(nullptr, desc);
}

/*
+ 0x8B CircularDOF_WorkComponent0Im
+ 0x83 CircularDOF_WorkTexture
- 0x82 omposite
- 0x79 systems/effect/Stochastic_Sample8_MSK4.tex
+ 0x75 HDRImage
+ 0x73 HDRImage
- 0x6A HDRImage
- 0x67 systems/effect/Stochastic_Sample4_MSK4.tex
- 0x67 DensityMapTexture
- 0x5C systems/shader/advancedSystem.sdf
- 0x5A BaseColorTextrure
- 0x52 tSrc
- 0x4E HDRImage
- 0x4C CircularDOF_NearCOCFilteredHQ
- 0x3F CircularDOF_SceneMipTexture
*/
TargetState* create_target_state(TargetState::Desc* desc) {
    static auto fn = []() -> TargetState* (*)(void*, TargetState::Desc*) {
        spdlog::info("Searching for create_target_state");

        const auto game = utility::get_executable();
        const auto string = utility::scan_string(game, "CircularDOF_SceneMipTexture");

        if (!string) {
            spdlog::error("Failed to find create_target_state (no string)");
            return nullptr;
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string);

            spdlog::error("Failed to find create_target_state (no string ref)");
        if (!string_ref) {
            return nullptr;
        }

        uintptr_t ip = *string_ref;
        uint32_t found_count = 0;

        for (auto i = 0; i < 50; ++i) {
            const auto resolved = utility::resolve_instruction(ip);

            if (!resolved) {
                spdlog::error("Failed to find create_target_state (could not resolve instruction)");
                return nullptr;
            }

            ip = resolved->addr;

            if (*(uint8_t*)ip == 0xE8) {
                ++found_count;
            }

            // third call back from this string reference is the one we want
            if (*(uint8_t*)ip == 0xE8 && found_count == 3) {
                const auto result = (TargetState* (*)(void*, TargetState::Desc*))utility::calculate_absolute(ip + 1);

                spdlog::info("Found create_target_state: {:x}", (uintptr_t)result);
                return result;
            }

            ip -= 1;
        }

        return nullptr;
    }();

    // Guard: on games where the scan fails (e.g. MHWilds, where the
    // "CircularDOF_SceneMipTexture" anchor string is absent) fn is null; calling it
    // would jump to address 0 (RIP=0 crash). Fail gracefully so callers like
    // TargetState::clone() return null instead of crashing.
    if (fn == nullptr) {
        return nullptr;
    }

    return fn(nullptr, desc);
}

/*
+ 0x217 Wrinkle_VertAreaSkin
- 0x20A EchoParam
+ 0x203 CapturePlane
+ 0x1F9 systems/shader/systemDevelop.sdf
+ 0x1F3 Wrinkle_ProbagateDupVertex
+ 0x1CF Wrinkle_ProbagateDupVertex_MaxMode
- 0x1CA PrevLDRImage
+ 0x1AB Wrinkle_DrawAreaToTexture2
- 0x1A5 MeshToUVTextureMap_2ndUVto1stUV
- 0x18A LDRImage
+ 0x187 Wrinkle_DrawAreaToTexture2_MaxMode
+ 0x163 Wrinkle_CheapBlur
- 0x145 MeshToUVTextureSkin2nd_Pos
+ 0xD9 systems/shader/speedTree/speedTree.sdf
- 0x18 width=%u,height=%u,depth=%u,mip=%u,array=%u,format=%u,usage=%u,bind=%u
*/
// The MHWilds texture factory takes the render device as arg1 (rdx) - NOT the RTV-pool
// device (that's a separate object; see resolve_rtv_pool_device). It's the memory/heap
// device at singleton->[0x18], where the singleton pointer lives at a global resolved from
// its init site (mov ecx,0x45950; call alloc; mov [global],rax). This is exactly the device
// the engine's own create_texture wrapper passes.
static void* resolve_texture_memory_device() {
    static void** const singleton_global = []() -> void** {
        const auto game = utility::get_executable();
        const auto match = utility::scan(game, "B9 50 59 04 00 E8 ? ? ? ? 48 89 05 ? ? ? ?");

        if (!match) {
            return nullptr;
        }

        return (void**)utility::calculate_absolute(*match + 13);
    }();

    if (singleton_global == nullptr || *singleton_global == nullptr) {
        return nullptr;
    }

    return *(void**)((uintptr_t)*singleton_global + 0x18);
}

// The MHWilds texture factory (the fn that CONTAINS the "width=%u,..." debug name) uses a
// 4-arg output-struct convention instead of the 2-arg (device, desc) shape the string
// walk-back resolves on other RE games:
//   factory(void* out /*rcx*/, void* device /*rdx*/, Texture::Desc* desc /*r8*/, uint32_t dim /*r9d*/)
//   out layout: { bool ok @0; Texture* tex @8; void* aux @0x10; }  (also returned in rax)
//   dim = (desc->arr >= 2) ? 5 : 4   (Texture2DArray vs Texture2D)
using create_texture_wilds_fn = void* (*)(void* /*out*/, void* /*device*/, Texture::Desc* /*desc*/, uint32_t /*dim*/);

Texture* create_texture(Texture::Desc* desc) {
    // Exactly one of these is non-null after resolution. The Wilds path needs its own call
    // adapter (output struct + explicit device + dimension), so it can't share the 2-arg
    // signature.
    struct Resolved {
        Texture* (*two_arg)(void*, Texture::Desc*) = nullptr;
        create_texture_wilds_fn wilds = nullptr;
    };

    static const Resolved resolved = []() -> Resolved {
        spdlog::info("Searching for create_texture");

        const auto game = utility::get_executable();
        const auto string = utility::scan_string(game, L"width=%u,height=%u,depth=%u,mip=%u,array=%u,format=%u,usage=%u,bind=%u");

        if (!string) {
            spdlog::error("Failed to find create_texture (no string)");
            return {};
        }

        const auto string_ref = utility::scan_displacement_reference(game, *string);

        if (!string_ref) {
            spdlog::error("Failed to find create_texture (no string ref)");
            return {};
        }

        // Wilds (TDB 81): the width=%u string is the factory's own debug NAME (used INSIDE the
        // factory, not at a call site), so the classic walk-back for a nearby CALL fails. The
        // factory is simply the function that contains the string ref, called with the 4-arg
        // convention above.
        if (sdk::GameIdentity::get().is_mhwilds()) {
            const auto fn_start = utility::find_function_start_unwind(*string_ref);

            if (!fn_start) {
                spdlog::error("Failed to find create_texture (Wilds: no function start)");
                return {};
            }

            Resolved out{};
            out.wilds = (create_texture_wilds_fn)*fn_start;
            spdlog::info("Found create_texture (Wilds 4-arg factory): {:x}", *fn_start);
            return out;
        }

        // Classic path: the string sits at a log call INSIDE a wrapper that CALLs the 2-arg
        // factory - walk back <=20 instrs to the nearest E8.
        uintptr_t ip = *string_ref;

        for (auto i = 0; i < 20; ++i) {
            const auto ins = utility::resolve_instruction(ip);

            if (!ins) {
                spdlog::error("Failed to find create_texture (could not resolve instruction)");
                return {};
            }

            ip = ins->addr;

            if (*(uint8_t*)ip == 0xE8) {
                Resolved out{};
                out.two_arg = (Texture* (*)(void*, Texture::Desc*))utility::calculate_absolute(ip + 1);
                spdlog::info("Found create_texture: {:x}", (uintptr_t)out.two_arg);
                return out;
            }

            ip -= 1;
        }

        spdlog::error("Failed to find create_texture, trying fallback");

        const auto fn_start = utility::find_function_start_with_call(*string_ref);

        if (!fn_start) {
            spdlog::error("Failed to find create_texture (no fallback)");
            return {};
        }

        const auto first_call = utility::scan_mnemonic(*fn_start, 100, "CALL");

        if (!first_call) {
            spdlog::error("Failed to find create_texture (no first call)");
            return {};
        }

        const auto second_call = utility::scan_mnemonic(*first_call + 1, 100, "CALL");

        if (!second_call) {
            spdlog::error("Failed to find create_texture (no second call)");
            return {};
        }

        Resolved out{};
        out.two_arg = (Texture* (*)(void*, Texture::Desc*))utility::calculate_absolute(*second_call + 1);
        spdlog::info("Found create_texture (fallback): {:x}", (uintptr_t)out.two_arg);
        return out;
    }();

    if (desc == nullptr) {
        return nullptr;
    }

    if (resolved.wilds != nullptr) {
        void* const device = resolve_texture_memory_device();

        if (device == nullptr) {
            return nullptr;
        }

        // Output-struct convention: { bool ok @0; Texture* tex @8; ... }. The factory writes
        // a full 0x70-byte struct (up to out+0x68) - the buffer MUST be >= 0x70 or the factory
        // smashes the stack. It moves the texture's reference into out[8], so we take the raw
        // pointer without AddRef (net refcount 1, owned by the returned pointer - matches
        // clone() semantics). The other out slots hold auxiliary refs we intentionally leak
        // (clone happens ~twice per session, cached).
        alignas(16) uint8_t out[0x80]{};
        const uint32_t dim = (desc->arr >= 2) ? 5u : 4u;

        resolved.wilds(out, device, desc, dim);

        Texture* const tex = (out[0] != 0) ? *(Texture**)(out + 8) : nullptr;

        // One-shot ground-truth diagnostic: does the factory produce a texture with a live
        // D3D12 resource? If native == 0 the texture object exists but has no GPU backing
        // (wrong arg / deferred realize); if native != 0 the texture is real and any downstream
        // fault is in the RTV link/state, not here.
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            const auto container = tex != nullptr ? tex->get_d3d12_resource_container() : nullptr;
            const auto native = container != nullptr ? container->get_native_resource() : nullptr;
            spdlog::info("[Flat3D] create_texture(Wilds): ok={} tex={:x} container={:x} native={:x} "
                "desc(w={} h={} d={} mip={} arr={} fmt={}) dim={} device={:x}",
                (int)out[0], (uintptr_t)tex, (uintptr_t)container, (uintptr_t)native,
                desc->width, desc->height, desc->depth, desc->mip, desc->arr, desc->format, dim, (uintptr_t)device);
        }

        return tex;
    }

    static auto renderer = sdk::renderer::get_renderer();

    if (resolved.two_arg == nullptr || renderer == nullptr) {
        return nullptr;
    }

    return resolved.two_arg(renderer->get_device(), desc);
}

void prime_copy_dest_state(Texture* tex) {
    prime_resource_state(tex, 0x400 /* D3D12_RESOURCE_STATE_COPY_DEST */);
}

void prime_resource_state(Texture* tex, uint32_t d3d12_state, uint32_t subresource_count_override) {
    if (tex == nullptr) {
        return;
    }

    // ===== UPDATE-FRAGILE: hardcoded T3 engine-internal struct offsets (0x100/0x158/0x30/+0/+8). =====
    // These are the single highest-risk values for a Wilds patch and are NOT reflection-derivable.
    // Full re-derivation recipe + sanity checks: docs/FLAT3D_WILDS_RE.md section 2.6.
    //
    // The engine's command executor resolves a copy resource through a per-subresource state
    // tracker whose base pointer lives at tex+0x100; each entry is 0x30 bytes with the current
    // D3D12 state at +8 and the subresource index at +0x18 (verified from the executor's
    // find() at exe+0xab509d9 / state-check at exe+0xab58289). For the copy DST the executor
    // requests COPY_DEST (0x400); if the tracked state differs it tries to record a transition
    // against the resource's registry entry - which a create_texture clone doesn't have - and
    // find() walks off the end (crash). Priming the tracked state to COPY_DEST makes the resolve
    // take the no-transition fast path, so the copy never touches the registry.
    uint8_t* const base = *(uint8_t**)((uintptr_t)tex + 0x100);

    if (base == nullptr) {
        return;
    }

    const auto d = tex->get_desc();
    uint32_t mips = (d != nullptr && d->mip != 0) ? d->mip : 1;
    uint32_t arr = (d != nullptr && d->arr != 0) ? d->arr : 1;
    uint32_t count = mips * arr;

    if (count == 0 || count > 64) {
        count = 1; // sanity clamp - our eye clones are single-mip, single-layer 2D
    }

    // Depth-stencil textures (e.g. D32S8) have TWO planes -> twice the subresources. The caller
    // knows the plane count; without the override the stencil plane stays unprimed and the
    // executor's registry walk crashes on it.
    if (subresource_count_override != 0 && subresource_count_override <= 64) {
        count = subresource_count_override;
    }

    // Per-entry (0x30 bytes): [+0] = resting state (checked when the subresource is touched for
    // the FIRST time this frame - which a fresh clone always is), [+8] = current per-frame
    // state (checked on same-frame subsequent touches), [+0x18] = last-touched frame stamp
    // (leave it to the engine - it's NOT a subresource index). Setting BOTH state fields to
    // COPY_DEST makes 0x14ab58260 return "no transition" so find() skips the registry walk.
    static bool s_logged = false;
    for (uint32_t sub = 0; sub < count; ++sub) {
        uint8_t* const entry = base + (uintptr_t)sub * 0x30;
        const uint32_t old0 = *(uint32_t*)(entry + 0);
        const uint32_t old8 = *(uint32_t*)(entry + 8);

        *(uint32_t*)(entry + 0) = d3d12_state;   // resting state (first-touch check)
        *(uint32_t*)(entry + 8) = d3d12_state;   // current state  (same-frame check)

        if (!s_logged) {
            spdlog::info("[Flat3D] prime_resource_state tex={:x} base={:x} sub={} state={:#x} old[0]={:#x} old[8]={:#x} count={}",
                (uintptr_t)tex, (uintptr_t)base, sub, d3d12_state, old0, old8, count);
        }
    }
    s_logged = true;

    // The copy-execution path (exe+0xaba8500) does a SEPARATE resource resolution that also
    // walks the alias registry keyed on the native handle, gated by tex+0x158: nonzero => walk
    // (crash for our unregistered clone), zero => read the native handle directly. Priming the
    // STATE above handles the transition logic (find() no longer walks); clearing this flag
    // makes the copy's resource-resolve take the direct-native path too. Safe now that the
    // state is COPY_DEST-primed and the resource is COMMON (implicitly promoted by the copy).
    *(volatile uint8_t*)((uintptr_t)tex + 0x158) = 0;
}

void* get_engine_native_resource_d3d12(Texture* tex) {
    if (tex == nullptr) {
        return nullptr;
    }

    // Update-resilient: Texture::get_d3d12_resource_container() bruteforce-scans for the
    // container by its via.render.RenderResource typeinfo (Renderer.cpp, TDB>=71 path) rather
    // than a hardcoded offset, and get_native_resource() reads the native at +get_runtime_size().
    // On MHWilds this resolves to the SAME ID3D12Resource the engine copy_texture writes into -
    // VERIFIED equal to the old hardcoded *(tex+0xf0)+0x20 (the "0xE0-NATIVE same=true" probe).
    // See docs/FLAT3D_WILDS_RE.md ("native resource offset").
    const auto container = tex->get_d3d12_resource_container();
    return container != nullptr ? (void*)container->get_native_resource() : nullptr;
}

/*
+ 0x20A Wrinkle_DrawAreaToTexture2
+ 0x1E6 Wrinkle_DrawAreaToTexture2_MaxMode
+ 0x1C2 Wrinkle_CheapBlur
- 0x1B8 EchoParam
+ 0x185 width=%u,height=%u,depth=%u,mip=%u,array=%u,format=%u,usage=%u,bind=%u
- 0x178 PrevLDRImage
- 0x168 systems/shader/advancedSystem.sdf
- 0x138 LDRImage
- 0xE0 BaseColorTextrure
- 0xD4 DensityMapTexture
- 0x9F BaseColorTextrure
*/
/*
48 C7 44 24 24 05 00 00 00                    mov     [rsp+78h+var_54], 5
C7 44 24 30 01 00 00 00                       mov     [rsp+78h+var_48], 1
44 89 7C 24 2C                                mov     [rsp+78h+var_4C], r15d
C7 44 24 20 1C 00 00 00                       mov     [rsp+78h+var_58], 1Ch
E8 89 EB 78 00                                call    create_render_target_view
*/

// In RE4+:
/*
- 0x269 CircularDOF_NearCOCFilteredHQ
- 0x266 CircularDOF_NearCOCMaskForTile
+ 0x261 CircularDOF_WorkComponent0Re
- 0x235 tSrc
+ 0x212 Wrinkle_CheapBlur
+ 0x212 Echo
- 0x1F1 CircularDOF_NearCOCMaskForTileHQ
+ 0x1F0 CircularDOF_WorkComponent0Im
- 0x1D0 CircularDOF_NearCOCFiltered
- 0x19A EchoParam
+ 0x15B width=%u,height=%u,depth=%u,mip=%u,array=%u,format=%u,usage=%u,bind=%u
- 0x15A PrevLDRImage
- 0x149 CircularDOF_NearCOCFilteredHQ
- 0x11A LDRImage
+ 0xC3 width=%u,height=%u,depth=%u,mip=%u,array=%u,format=%u,usage=%u,bind=%u
*/
/*
4C 8D 45 B8                                   lea     r8, [rbp+40h+var_88]
49 8B CE                                      mov     rcx, r14
E8 ? ? ? ?                                    call    create_render_target_view
48 8B 8F F0 04 00 00                          mov     rcx, [rdi+4F0h]
48 8B D8                                      mov     rbx, rax
4C 89 BF F0 04 00 00                          mov     [rdi+4F0h], r15
*/
// Optional per-thread override for create_render_target_view's arg0 (the render device /
// context). On MHWilds the RTV factory dereferences arg0 and pulls the descriptor pool
// from it; the persistent renderer->get_device() has no active pool, so a caller inside a
// render-layer hook can set the live RenderContext here first.
static thread_local void* g_create_rtv_device_override = nullptr;

void set_create_rtv_device_override(void* device) {
    g_create_rtv_device_override = device;
}

// Resolve the MHWilds render device that owns the RTV descriptor pool (device[0] = the
// descriptor-heap manager). The engine's own resource-view factory thunks load this device
// from a single global via `mov rcx,[rip+disp]` and then tail-call the worker. Every
// view-factory thunk in the exe (77 of them) reads the SAME global, so we scan for the thunk
// shape and read its device global directly - `device = *global`, no extra indirection.
//   thunk: mov rcx,[rip+devA]; test rcx,rcx; jne worker;
//          mov rcx,[rip+devB]; test rcx,rcx; jne worker2; xor ecx,ecx; jmp worker.
// (The global lives in the section the protector renamed to ".tls", but it's accessed
// RIP-relative - not gs-relative - so it's a flat regular global, readable from any thread.
// The earlier singleton->[0x18] guess resolved to a different, pool-less object whose [0]
// was persistently null.)
static void* resolve_rtv_pool_device() {
    static void** const rtv_device_global = []() -> void** {
        const auto game = utility::get_executable();
        const auto match = utility::scan(game,
            "48 8B 0D ? ? ? ? 48 85 C9 0F 85 ? ? ? ? 48 8B 0D ? ? ? ? 48 85 C9 0F 85 ? ? ? ? 31 C9 E9");

        if (!match) {
            return nullptr;
        }

        return (void**)utility::calculate_absolute(*match + 3);
    }();

    if (rtv_device_global == nullptr) {
        return nullptr;
    }

    return *rtv_device_global;
}

// True only when the RTV descriptor pool is actually live (device[0] = heap manager, whose
// PoolSize at +0x5fc is > 0). Callers MUST check this before attempting a RenderTargetView
// clone: RTV::clone() calls create_texture() first, so retrying a clone that would fail
// leaks a texture every frame and exhausts the engine resource pool (-> crash).
bool is_create_rtv_ready() {
    const auto device = resolve_rtv_pool_device();
    const auto manager = device != nullptr ? *(void**)device : nullptr;
    const int32_t pool_size = manager != nullptr ? *(int32_t*)((uintptr_t)manager + 0x5fc) : -999;
    const bool ready = manager != nullptr && pool_size > 0;

    static uint32_t dbg = 0;
    if ((dbg++ % 600) == 0) {
        spdlog::info("[Flat3D] is_create_rtv_ready={} device={:x} manager={:x} pool_size={:#x}",
            ready, (uintptr_t)device, (uintptr_t)manager, (uint32_t)pool_size);
    }

    return ready;
}

RenderTargetView* create_render_target_view(sdk::renderer::RenderResource* resource, void* desc) {
    static auto fn = []() -> RenderTargetView* (*)(void*, sdk::renderer::RenderResource* resource, void*) {
        spdlog::info("Searching for create_render_target_view");

        const auto game = utility::get_executable();
        const auto ref = utility::scan(game, "44 89 7C 24 2C C7 44 24 20 1C 00 00 00 E8 ? ? ? ?");

        if (!ref) {
            spdlog::info("Could not find first ref, performing fallback scan");
            const auto ref2 = utility::scan(game, "4C 8D 45 B8 49 8B CE E8 ? ? ? ?");

            if (ref2) {
                const auto result = (RenderTargetView* (*)(void*, sdk::renderer::RenderResource*, void*))utility::calculate_absolute(*ref2 + 8);
                spdlog::info("Found create_render_target_view: {:x}", (uintptr_t)result);

                return result;
            }

            // MHWilds (TDB 81): the call-site patterns above don't match (call sites
            // changed). Resolve the factory WORKER directly by a unique instruction
            // sequence inside it: mov rax,[rcx]; add rax,0x10; mov ecx,[rip]; mov rdx,
            // gs:[0x58]; mov r15,[rdx+rcx*8]; mov [r15+0x20],rax (grabs the per-thread
            // render context then stashes the resource). Verified unique in the exe.
            const auto mid = utility::scan(game,
                "48 8B 01 48 83 C0 10 8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00 4C 8B 3C CA 49 89 87 20 00 00 00");

            if (mid) {
                const auto fn_start = utility::find_function_start_unwind(*mid);

                if (fn_start) {
                    spdlog::info("Found create_render_target_view (worker pattern): {:x}", *fn_start);
                    return (RenderTargetView* (*)(void*, sdk::renderer::RenderResource*, void*))*fn_start;
                }
            }

            spdlog::error("Failed to find create_render_target_view (no ref)");
            return nullptr;
        }

        const auto result = (RenderTargetView* (*)(void*, sdk::renderer::RenderResource*, void*))utility::calculate_absolute(*ref + 14);
        spdlog::info("Found create_render_target_view: {:x}", (uintptr_t)result);

        return result;
    }();

    // Guard against an unresolved scan (MHWilds: neither the primary nor fallback
    // pattern matches) - calling a null fn is an RIP=0 crash.
    if (fn == nullptr) {
        return nullptr;
    }

    // arg0 is the render device. The MHWilds worker dereferences it and pulls the RTV
    // descriptor-heap manager from device[0] - so it must be the specific render-system
    // device that owns the pool, not renderer->get_device() (a different, pool-less
    // instance -> "descriptor pool is empty. PoolSize(-1)"). resolve_rtv_pool_device()
    // reads it from the same global the engine's own view-factory thunks use.
    void* device = g_create_rtv_device_override;

    if (device == nullptr) {
        device = resolve_rtv_pool_device();
    }

    if (device == nullptr) {
        const auto renderer = sdk::renderer::get_renderer();
        device = renderer != nullptr ? renderer->get_device() : nullptr;
    }

    if (device == nullptr) {
        return nullptr;
    }

    // Readiness guard: early in boot the render device's RTV descriptor pool isn't set up
    // yet - the factory does `mov r12,[device]` (device[0] = heap manager) then reads the
    // pool from it; if the manager is null (device not initialized) or its PoolSize
    // (manager[0x5fc]) is <= 0 (uninitialized/empty), calling the factory faults or asserts.
    // Return null instead (the caller retries next frame) until the pool is live.
    if (g_create_rtv_device_override == nullptr) {
        const auto manager = *(void**)device;

        if (manager == nullptr || *(int32_t*)((uintptr_t)manager + 0x5fc) <= 0) {
            return nullptr;
        }
    }

    return fn(device, resource, desc);
}

ID3D12Resource* TargetState::get_native_resource_d3d12() const {
    const auto rtv = get_rtv(0);

    if (rtv == nullptr) {
        return nullptr;
    }

    // sizeof(via.render.RenderTargetView) + 8;
    const auto tex = rtv->get_texture_d3d12();

    if (tex == nullptr) {
        /*auto target_state = rtv->get_target_state_d3d12();

        if (target_state != nullptr && target_state != this) {
            return target_state->get_native_resource_d3d12();
        }*/

        return nullptr;
    }

    const auto internal_resource = tex->get_d3d12_resource_container();

    if (internal_resource == nullptr) {
        return nullptr;
    }

    return internal_resource->get_native_resource();
}

DirectXResource<ID3D12Resource>* Texture::get_d3d12_resource_container() {
    // DMC5 (TDB <71) uses hardcoded offset; newer games bruteforce-scan.
    if (sdk::GameIdentity::get().tdb_ver() < 71) {
        return *(DirectXResource<ID3D12Resource>**)((uintptr_t)this + get_s_d3d12_resource_offset());
    }
    // fall through to bruteforce for TDB >= 71
    static std::optional<size_t> offset = std::nullopt;

    if (offset) {
        return *(DirectXResource<ID3D12Resource>**)((uintptr_t)this + *offset);
    }

    static constexpr size_t GET_TYPEINFO_FN_INDEX = 3;

    spdlog::info("Searching for Texture D3D12Resource offset (via.render.RenderResource bruteforce)");

    for (size_t i = 0x98; i < 0x200; i += sizeof(void*)) try {
        const auto ptr = *(uintptr_t*)((uintptr_t)this + i);

        if (ptr == 0 || IsBadReadPtr((void*)ptr, sizeof(void*))) {
            continue;
        }

        const auto vtable = *(uintptr_t**)ptr;

        if (vtable == 0 || IsBadReadPtr((void*)vtable, sizeof(void*))) {
            continue;
        }

        const auto get_typeinfo_fn = vtable[GET_TYPEINFO_FN_INDEX];

        if (get_typeinfo_fn == 0 || IsBadReadPtr((void*)get_typeinfo_fn, sizeof(void*))) {
            continue;
        }

        if (!utility::get_module_within(get_typeinfo_fn)) {
            continue;
        }

        // Check if this is a mov rax, [rip+disp32] instruction
        if (((uint8_t*)get_typeinfo_fn)[0] != 0x48 || ((uint8_t*)get_typeinfo_fn)[1] != 0x8B || ((uint8_t*)get_typeinfo_fn)[2] != 0x05) {
            spdlog::info("[Texture] Skipping offset {:x} because get_typeinfo_fn does not look like a mov rax", i);
            continue;
        }

        using type_info_fn_t = sdk::RETypeCLR* (*)();
        const auto type_info_fn = (type_info_fn_t)get_typeinfo_fn;
        const auto type_info = type_info_fn();

        if (type_info == nullptr || IsBadReadPtr(type_info, sizeof(void*))) {
            continue;
        }

        if (type_info->get_type_name() == nullptr || IsBadReadPtr(type_info->get_type_name(), sizeof(void*))) {
            continue;
        }

        const auto type_name = std::string_view{type_info->get_type_name()};

        if (type_name == "via.render.RenderResource") {
            spdlog::info("[Texture] Found D3D12Resource container at offset {:x}", i);
            offset = i;
            return *(DirectXResource<ID3D12Resource>**)((uintptr_t)this + *offset);
        }

        spdlog::info("[Texture] Checked offset {:x}, type name: {}", i, type_name);
    } catch(...) {
        continue;
    }

    if (offset) {
        return *(DirectXResource<ID3D12Resource>**)((uintptr_t)this + *offset);
    }

    return nullptr;

}

Texture* Texture::clone() {
    return sdk::renderer::create_texture(get_desc());
}

sdk::intrusive_ptr<RenderTargetView> RenderTargetView::clone() {
    auto tex = this->get_texture_d3d12();

    if (tex == nullptr) {
        return nullptr;
    }

    return sdk::renderer::create_render_target_view(tex->clone(), &get_desc());
}

sdk::intrusive_ptr<RenderTargetView> RenderTargetView::clone(uint32_t new_width, uint32_t new_height) {
    auto tex = this->get_texture_d3d12();

    if (tex == nullptr) {
        return nullptr;
    }

    return sdk::renderer::create_render_target_view(tex->clone(new_width, new_height), &get_desc());
}

namespace detail {
inline uintptr_t rtv_size() {
    const auto& gi = sdk::GameIdentity::get();
    const auto v = gi.tdb_ver();
    if (v >= 74) return 0xA8;
    if (v >= 71) {
        if (gi.is_sf6() || gi.is_dd2()) return 0x98;
        if (gi.is_mhrise()) return 0x88;
        return 0x98 - sizeof(void*);
    }
    if (v == 70) return 0x90 - sizeof(void*);
    if (v == 69) return 0x88 - sizeof(void*);
    return 0x88 - sizeof(void*); // TDB <= 67
}
}

sdk::intrusive_ptr<Texture>& RenderTargetView::get_texture_d3d12() const {
    // The via.render.RenderTargetView is not part of the normal TDB... I think.
    static const auto rtv_type = reframework::get_types()->get("via.render.RenderTargetView");

    // The texture and target state members are always at the very start of the RenderTargetViewDX12 structure
    // so we can very easily automate it like this, otherwise we fall back to the hardcoded offset
    if (rtv_type != nullptr && utility::re_type_accessor::get_size(rtv_type) > 0 && utility::re_type_accessor::get_size(rtv_type) < 0x1000) {
        const auto rtv_size = utility::re_type_accessor::get_size(rtv_type);

        const auto v = sdk::GameIdentity::get().tdb_ver();
        if (v >= 74) {
            // TDB >= 74: +4*ptr (0xC8-0xE0 range)
            return *(sdk::intrusive_ptr<Texture>*)((uintptr_t)this + rtv_size + (sizeof(void*) * 4));
        } else if (v < 73) {
            return *(sdk::intrusive_ptr<Texture>*)((uintptr_t)this + rtv_size + sizeof(void*));
        } else {
            return *(sdk::intrusive_ptr<Texture>*)((uintptr_t)this + rtv_size + (sizeof(void*) * 3));
        }
    }
    
    const auto v2 = sdk::GameIdentity::get().tdb_ver();
    if (v2 >= 74) {
        return *(sdk::intrusive_ptr<Texture>*)((uintptr_t)this + detail::rtv_size() + (sizeof(void*) * 4));
    } else if (v2 < 73) {
        return *(sdk::intrusive_ptr<Texture>*)((uintptr_t)this + detail::rtv_size() + sizeof(void*));
    } else {
        return *(sdk::intrusive_ptr<Texture>*)((uintptr_t)this + detail::rtv_size() + (sizeof(void*) * 3));
    }
}

sdk::intrusive_ptr<TargetState>& RenderTargetView::get_target_state_d3d12() const {
    // The via.render.RenderTargetView is not part of the normal TDB... I think.
    static const auto rtv_type = reframework::get_types()->get("via.render.RenderTargetView");

    if (rtv_type != nullptr && utility::re_type_accessor::get_size(rtv_type) > 0 && utility::re_type_accessor::get_size(rtv_type) < 0x1000) {
        const auto rtv_size = utility::re_type_accessor::get_size(rtv_type);

        return *(sdk::intrusive_ptr<TargetState>*)((uintptr_t)this + rtv_size);
    }
    
    return *(sdk::intrusive_ptr<TargetState>*)((uintptr_t)this + detail::rtv_size());
}

sdk::intrusive_ptr<TargetState> TargetState::clone() const {
    auto cloned_desc = get_desc();

    if (cloned_desc.num_rtv > 0) {
        cloned_desc.rtvs = (decltype(cloned_desc.rtvs))sdk::memory::allocate(cloned_desc.num_rtv * sizeof(void*));

        for (auto i = 0; i < cloned_desc.num_rtv; ++i) {
            auto rtv = get_rtv(i);

            if (rtv == nullptr) {
                continue;
            }

            cloned_desc.rtvs[i] = rtv->clone();
        }
    } else {
        cloned_desc.rtvs = nullptr;
    }

    return sdk::renderer::create_target_state(&cloned_desc);
}

sdk::intrusive_ptr<TargetState> TargetState::clone(const std::vector<std::array<uint32_t, 2>>& new_dimensions) const {
    auto cloned_desc = get_desc();

    if (cloned_desc.num_rtv > 0) {
        cloned_desc.rtvs = (decltype(cloned_desc.rtvs))sdk::memory::allocate(cloned_desc.num_rtv * sizeof(void*), true);

        for (auto i = 0; i < cloned_desc.num_rtv; ++i) {
            auto rtv = get_rtv(i);

            if (rtv == nullptr) {
                continue;
            }

            if (i < new_dimensions.size()) {
                if (i == 0) {
                    cloned_desc.rect.right = (float)new_dimensions[i][0];
                    cloned_desc.rect.bottom = (float)new_dimensions[i][1];
                }

                cloned_desc.rtvs[i] = rtv->clone(new_dimensions[i][0], new_dimensions[i][1]);
            } else {
                cloned_desc.rtvs[i] = rtv->clone();
            }
        }
    } else {
        cloned_desc.rtvs = nullptr;
    }

    return sdk::renderer::create_target_state(&cloned_desc);
}

void*& layer::Output::get_present_state() {
    static uint32_t output_target_offset = 0;

    if (output_target_offset == 0) {
        spdlog::info("[Renderer] Finding output_target_offset");

        auto get_scene_view_fn = sdk::find_native_method("via.render.layer.Output", "get_SceneView");

        if (get_scene_view_fn == nullptr) {
            spdlog::error("[Renderer] Failed to find get_SceneView");
            return *(void**)((uintptr_t)this + sdk::find_type_definition("via.render.RenderLayer")->get_size());
        }

        // Resolve the jmp to the real function
        if (((uint8_t*)get_scene_view_fn)[0] == 0xE9) {
            get_scene_view_fn = (decltype(get_scene_view_fn))utility::calculate_absolute((uintptr_t)get_scene_view_fn + 1);
        }

        // Find the offset to the output target
        // First instruction is a mov, so we don't need to pattern scan for it
        output_target_offset = *(uint8_t*)((uintptr_t)get_scene_view_fn + 3);

        spdlog::info("[Renderer] output_target_offset: {:x}", output_target_offset);
    }

    return *(void**)((uintptr_t)this + output_target_offset);
}

REManagedObject*& layer::Output::get_scene_view() {
    static uint32_t scene_view_offset = 0;

    if (scene_view_offset == 0) {
        spdlog::info("[Renderer] Finding scene_view_offset");

        // because if this is a manually created output layer,
        // we might not have the scene view and output state set up yet
        auto top_output_layer = sdk::renderer::get_output_layer();

        if (top_output_layer == nullptr) {
            spdlog::error("[Renderer] Failed to find top_output_layer");
            return *(REManagedObject**)((uintptr_t)this + 0);
        }

        // Call get_SceneView so we can get a scene view
        // to scan the object for
        const auto scene_view = sdk::call_object_func<void*>(top_output_layer, "get_SceneView", sdk::get_thread_context(), top_output_layer);
        const auto output_target = top_output_layer->get_present_state();

        if (scene_view == nullptr) {
            spdlog::error("[Renderer] Failed to find scene_view");
            return *(REManagedObject**)((uintptr_t)this + 0);
        }

        if (output_target == nullptr) {
            spdlog::error("[Renderer] Failed to find output_target");
            return *(REManagedObject**)((uintptr_t)this + 0);
        }

        // Find the offset to the scene view
        for (auto i = 0; i < 0x1000; i += sizeof(void*)) {
            if (*(void**)((uintptr_t)output_target + i) == scene_view) {
                scene_view_offset = i;
                break;
            }
        }

        spdlog::info("[Renderer] scene_view_offset: {:x}", scene_view_offset);
    }

    return *(REManagedObject**)((uintptr_t)get_present_state() + scene_view_offset);
}

uint32_t layer::Scene::get_view_id() const {
    static auto get_view_id_method = sdk::find_method_definition("via.render.layer.Scene", "get_ViewID");

    if (get_view_id_method == nullptr) {
        return 0;
    }

    return get_view_id_method->call<uint32_t>(sdk::get_thread_context(), this);
}

RECamera* layer::Scene::get_camera() const {
    static auto get_camera_method = sdk::find_method_definition("via.render.layer.Scene", "get_Camera");

    if (get_camera_method == nullptr) {
        return nullptr;
    }

    return get_camera_method->call<RECamera*>(sdk::get_thread_context(), this);
}

RECamera* layer::Scene::get_main_camera_if_possible() const {
    const auto camera = get_camera();

    if (camera == nullptr) {
        return nullptr;
    }

    const auto camera_gameobject = camera->get_game_object();

    if (camera_gameobject == nullptr) {
        return nullptr;
    }

    const auto name = camera_gameobject->get_name();

    static const std::vector<std::string> camera_names = {
        "MainCamera",
        "Main Camera",
        "GameCamera", // DMC5
        "ess_DefaultCamera",
        "ess_DefaultCamera_01",
        "WTMainCamera",
        "DefaultCamera",
        "Camera_mainmenu",
        "Camera_cp7mainmenu",
        "SnowCamera", // MHRise
    };

    for (const auto& camera_name : camera_names) {
        if (name.starts_with(camera_name)) {
            return camera;
        }
    }

    return nullptr;
}

REManagedObject* layer::Scene::get_mirror() const {
    static auto get_mirror_method = sdk::find_method_definition("via.render.layer.Scene", "get_Mirror");

    if (get_mirror_method == nullptr) {
        return nullptr;
    }

    return get_mirror_method->call<REManagedObject*>(sdk::get_thread_context(), this);
}

bool layer::Scene::is_enabled() const {
    static auto is_enabled_method = sdk::find_method_definition("via.render.layer.Scene", "get_Enable");

    if (is_enabled_method == nullptr) {
        return false;
    }

    return is_enabled_method->call<bool>(sdk::get_thread_context(), this);
}

sdk::renderer::SceneInfo* layer::Scene::get_scene_info() {
    return this->get_reflection_property<SceneInfo*>("SceneInfo");
}

sdk::renderer::SceneInfo* layer::Scene::get_depth_distortion_scene_info() {
    return this->get_reflection_property<SceneInfo*>("DepthDistortionSceneInfo");
}

sdk::renderer::SceneInfo* layer::Scene::get_filter_scene_info() {
    return this->get_reflection_property<SceneInfo*>("FilterSceneInfo");
}

sdk::renderer::SceneInfo* layer::Scene::get_jitter_disable_scene_info() {
    return this->get_reflection_property<SceneInfo*>("JitterDisableSceneInfo");
}

sdk::renderer::SceneInfo* layer::Scene::get_jitter_disable_post_scene_info() {
    return this->get_reflection_property<SceneInfo*>("JitterDisablePostSceneInfo");
}

sdk::renderer::SceneInfo* layer::Scene::get_z_prepass_scene_info() {
    return this->get_reflection_property<SceneInfo*>("ZPrepassSceneInfo");
}

std::optional<size_t> layer::PrepareOutput::get_output_state_offset() {
    static constexpr size_t GET_TYPEINFO_FN_INDEX = 3;
    static std::optional<size_t> s_output_state_offset = std::nullopt;

    if (s_output_state_offset) {
        return *s_output_state_offset;
    }
    for (size_t offset = 0x10; offset < 0x500; offset += sizeof(void*)) try {
        // Grab vtable.
        const auto ptr = *(uintptr_t*)((uintptr_t)this + offset);
        if (ptr == 0 || IsBadReadPtr((void*)ptr, sizeof(void*))) {
            continue;
        }

        const auto vtable = *(uintptr_t**)ptr;
        if (vtable == 0 || IsBadReadPtr((void*)vtable, sizeof(void*))) {
            continue;
        }

        const auto get_typeinfo_fn = vtable[GET_TYPEINFO_FN_INDEX];

        if (get_typeinfo_fn == 0 || IsBadReadPtr((void*)get_typeinfo_fn, sizeof(void*))) {
            continue;
        }

        if (!utility::get_module_within(get_typeinfo_fn)) {
            continue;
        }

        using type_info_fn_t = sdk::RETypeCLR* (*)();
        
        const auto type_info_fn = (type_info_fn_t)get_typeinfo_fn;
        // if this is essentially a mov rax, return it.
        if (((uint8_t*)get_typeinfo_fn)[0] != 0x48 || ((uint8_t*)get_typeinfo_fn)[1] != 0x8B || ((uint8_t*)get_typeinfo_fn)[2] != 0x05) {
            spdlog::info("[PrepareOutput] Skipping offset {:x} because get_typeinfo_fn does not look like a mov rax", offset);
            continue;
        }

        const auto type_info = type_info_fn();

        if (type_info == nullptr || IsBadReadPtr(type_info, sizeof(void*))) {
            continue;
        }

        if (type_info->get_type_name() == nullptr || IsBadReadPtr(type_info->get_type_name(), sizeof(void*))) {
            continue;
        }

        const auto type_name = std::string_view{type_info->get_type_name()};

        if (type_name == "via.render.TargetState") {
            s_output_state_offset = offset;
            spdlog::info("[PrepareOutput] Found output state offset: {:x}", offset);
            return *s_output_state_offset;
            break;
        }

        spdlog::info("[PrepareOutput] Checked offset {:x}, type name: {}", offset, type_name);
    } catch(...) {
        continue;
    }

    spdlog::warn("[PrepareOutput] Failed to find output state offset, trying next time...");

    return s_output_state_offset;
}

Texture* layer::Scene::get_depth_stencil() {
    return this->get_reflection_property<::sdk::renderer::Texture*>("DepthStencilTex");;
}

TargetState* layer::Scene::get_motion_vectors_state() {
    return this->get_reflection_property<::sdk::renderer::TargetState*>("VelocityTarget");
}

ID3D12Resource* layer::Scene::get_depth_stencil_d3d12() {
    const auto tex = this->get_reflection_property<::sdk::renderer::Texture*>("DepthStencilTex");

    if (tex == nullptr) {
        return nullptr;
    }

    const auto internal_resource = tex->get_d3d12_resource_container();

    if (internal_resource == nullptr) {
        return nullptr;
    }

    return internal_resource->get_native_resource();
}
}
}
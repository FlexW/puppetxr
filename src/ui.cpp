#include "ui.h"

#include "imgui.vert.h"
#include "imgui.frag.h"
#include "blocks.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

//
// Types
//

#define UI_INITIAL_VERTEX_BYTES ((u64)512 * 1024)
#define UI_MAX_TARGETS          8
#define UI_INITIAL_INDEX_BYTES  ((u64)128 * 1024)

#define UI_FONT_SIZE         14.0f
#define UI_PANEL_GAP         8.0f
#define UI_LOG_HEIGHT        180.0f
#define UI_PANEL_WIDTH       330.0f
#define UI_PANEL_LABEL_WIDTH 120.0f
#define UI_WELCOME_COLUMN    440.0f
#define UI_WELCOME_GAP       40.0f
#define UI_WELCOME_MARGIN    16.0f
#define UI_TOAST_SECONDS     3.0 // how long the name of a new profile shows
#define UI_WELCOME_KEY_WIDTH 150.0f
#define UI_MAX_PROBE_ITEMS   1024
#define UI_CLIPBOARD_SIZE    16384

namespace
{

enum class UI_Anchor : u8
{
    Left = 0,
    Right,
    Count,
};

struct UI_FrameBuffers
{
    R_BufferHandle vertices;
    R_BufferHandle indices;
    u64            vertex_bytes = {};
    u64            index_bytes = {};
};

// Key names for the welcome overlay, joined
struct KeyText
{
    char text[64];
};

struct ProfileLine
{
    KeyText     keys;
    const char* action;
};

struct BuildFrameCall
{
    UI_Context*     ui;
    SIM_Sim*        sim;
    b8              show_panels;
    SIM_MirrorView* mirror_view;
    UI_Requests*    requests;
};

} // namespace

namespace
{

// An item of the last frame and where it was, while probing (ui_set_probe)
struct UI_ProbeItem
{
    ImGuiID id        = {};
    char    label[48] = {}; // without the "##" part
    ImRect  rect;
};

} // namespace

struct UI_Context
{
    R_Device*          device = {};
    OS_Window*         window = {};
    R_Format           target_format = {};
    ImGuiContext*      imgui = {};
    ImPlotContext*     implot = {};
    b8                 captured = {}; // the mirror window holds the mouse (ui_set_captured)
    u64                last_frame_ns = {};
    char               clipboard[UI_CLIPBOARD_SIZE] = {}; // what ImGui last read from it
    R_RenderPassHandle render_pass;
    R_PipelineHandle   pipeline;
    UI_FrameBuffers    frames[R_MAX_FRAMES_IN_FLIGHT];

    // A framebuffer must outlive the frame that uses it, so they are kept per target image
    R_ImageHandle       target_images[UI_MAX_TARGETS];
    R_FramebufferHandle target_framebuffers[UI_MAX_TARGETS];
    u32                frame_index = {};
    f32                panel_cursor_y[(u32)UI_Anchor::Count] = {};
    f32                panel_bottom = {};
    UI_Anchor          panel_anchor = {};

    b8                 show_simulator = {};
    b8                 show_devices = {};
    b8                 show_log = {};
    b8                 show_props = {};
    u32                devices_profile = UINT32_MAX;          // the profile the device tabs last followed
    b8                 body_overlay = {};                     // the skeleton over the mirror view
    b8                 body_link = {};                        // a bone scale sets both sides; on from ui_create
    i32                body_pose = {};                        // the preset last chosen
    u32                body_joint = {};                       // the joint being edited
    i32                face_preset = {};                      // the face preset last chosen
    u32                toast_profile = UINT32_MAX;            // the profile the toast last named
    f64                toast_until = {};                      // ImGui time
    f32                welcome_height = {};                   // of the overlay's content last frame, to center it
    b8                 log_show_sim = {};
    b8                 log_show_app = {};
    u64                log_seen_sequence = {};
    UI_Status          status = {}; // of the frame being built
    char               typing[SIM_MAX_TYPED + 1] = {}; // the virtual keyboard's text field
    b8                 live_typing = {};               // every key typed into it goes to the app at once
    b8                 probe = {};
    UI_ProbeItem       probe_items[UI_MAX_PROBE_ITEMS];        // of the frame being built
    u32                probe_count = {};
    UI_ProbeItem       probed_items[UI_MAX_PROBE_ITEMS];       // of the last frame
    u32                probed_count = {};
    UI_ProbeItem       probe_pending;                          // a label given before its item, as tabs do
    ImGuiID            probed_hovered = {};                    // the item under the mouse in the last frame
    ImVec2             probed_mouse   = {};                    // where the last frame had the mouse
};

//
// Statics
//

static UI_Context* ui_probing = nullptr; // the context whose frame the test engine hooks record

static const ImVec4 UI_ACCENT     = ImVec4(0.28f, 0.48f, 0.66f, 1.00f);
static const ImVec4 UI_ACCENT_HI  = ImVec4(0.38f, 0.60f, 0.80f, 1.00f);
static const ImVec4 UI_ACCENT_LO  = ImVec4(0.22f, 0.36f, 0.50f, 1.00f);
static const ImVec4 UI_TEXT       = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
static const ImVec4 UI_MUTED      = ImVec4(0.49f, 0.51f, 0.55f, 1.00f);
static const ImVec4 UI_SOFT       = ImVec4(0.74f, 0.76f, 0.80f, 1.00f); // secondary text over the welcome overlay
static const ImVec4 UI_APP_TEXT   = ImVec4(0.62f, 0.78f, 0.92f, 1.00f);
static const ImVec4 UI_WARNING    = ImVec4(0.95f, 0.76f, 0.35f, 1.00f);
static const ImVec4 UI_ERROR      = ImVec4(0.94f, 0.42f, 0.42f, 1.00f);
static const ImVec4 UI_RECORDING  = ImVec4(0.90f, 0.30f, 0.30f, 1.00f);

// ImGui's combos report no label to the test engine hooks; these do, so probes find them
static void label_item (ImGuiID id, const char* label)
{
    ImGuiTestEngineHook_ItemInfo(ImGui::GetCurrentContext(), id, label, 0);
}

// A widget of several items (SliderFloat2) has no item of its label: its components' span, from
// `start` one item width across, is probed under it, the first half the first component
static void label_group (const char* label, ImVec2 start, f32 width)
{
    if (ui_probing && ui_probing->probe_count < UI_MAX_PROBE_ITEMS)
    {
        UI_ProbeItem& item = ui_probing->probe_items[ui_probing->probe_count++];
        item = { .id = ImGui::GetID(label), .label = {}, .rect = ImRect(start, ImVec2(start.x + width, ImGui::GetItemRectMax().y)) };
        cstr_copy(item.label, sizeof(item.label), label);
    }
}

// Text has no item ImGui's test engine sees: the text just drawn is probed under its own words,
// so tests and `puppetxrctl ui find` find the help's keys
static void label_text (const char* text)
{
    if (ui_probing && ui_probing->probe_count < UI_MAX_PROBE_ITEMS)
    {
        UI_ProbeItem& item = ui_probing->probe_items[ui_probing->probe_count++];
        item = { .id = ImGui::GetID(text), .label = {}, .rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()) };
        cstr_copy(item.label, sizeof(item.label), text);
    }
}

static b8 combo (const char* label, i32* current, const char* const items[], i32 count)
{
    const ImGuiID id      = ImGui::GetID(label);
    const b8      changed = ImGui::Combo(label, current, items, count);
    label_item(id, label);
    return changed;
}

static void apply_style ()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding           = ImVec2(10.0f, 10.0f);
    style.FramePadding            = ImVec2(8.0f, 4.0f);
    style.CellPadding             = ImVec2(6.0f, 4.0f);
    style.ItemSpacing             = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing        = ImVec2(7.0f, 4.0f);
    style.IndentSpacing           = 18.0f;
    style.ScrollbarSize           = 12.0f;
    style.GrabMinSize             = 10.0f;
    style.WindowBorderSize        = 1.0f;
    style.FrameBorderSize         = 0.0f;
    style.PopupBorderSize         = 1.0f;
    style.TabBorderSize           = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowRounding          = 4.0f;
    style.ChildRounding           = 4.0f;
    style.PopupRounding           = 4.0f;
    style.FrameRounding           = 3.0f;
    style.GrabRounding            = 3.0f;
    style.ScrollbarRounding       = 3.0f;
    style.TabRounding             = 3.0f;
    style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    style.FontSizeBase            = UI_FONT_SIZE;

    ImVec4* c                           = style.Colors;
    c[ImGuiCol_Text]                    = UI_TEXT;
    c[ImGuiCol_TextDisabled]            = UI_MUTED;
    c[ImGuiCol_WindowBg]                = ImVec4(0.110f, 0.115f, 0.125f, 0.97f);
    c[ImGuiCol_ChildBg]                 = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]                 = ImVec4(0.100f, 0.105f, 0.115f, 0.98f);
    c[ImGuiCol_Border]                  = ImVec4(0.00f, 0.00f, 0.00f, 0.45f);
    c[ImGuiCol_BorderShadow]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]                 = ImVec4(0.18f, 0.19f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered]          = ImVec4(0.24f, 0.26f, 0.29f, 1.00f);
    c[ImGuiCol_FrameBgActive]           = ImVec4(0.29f, 0.32f, 0.36f, 1.00f);
    c[ImGuiCol_TitleBg]                 = ImVec4(0.090f, 0.095f, 0.105f, 1.00f);
    c[ImGuiCol_TitleBgActive]           = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]        = ImVec4(0.090f, 0.095f, 0.105f, 0.75f);
    c[ImGuiCol_MenuBarBg]               = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_ScrollbarBg]             = ImVec4(0.06f, 0.06f, 0.07f, 0.55f);
    c[ImGuiCol_ScrollbarGrab]           = ImVec4(0.28f, 0.30f, 0.33f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]    = ImVec4(0.36f, 0.39f, 0.43f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]     = UI_ACCENT;
    c[ImGuiCol_CheckMark]               = UI_ACCENT_HI;
    c[ImGuiCol_SliderGrab]              = UI_ACCENT;
    c[ImGuiCol_SliderGrabActive]        = UI_ACCENT_HI;
    c[ImGuiCol_Button]                  = ImVec4(0.22f, 0.24f, 0.27f, 1.00f);
    c[ImGuiCol_ButtonHovered]           = UI_ACCENT_LO;
    c[ImGuiCol_ButtonActive]            = UI_ACCENT;
    c[ImGuiCol_Header]                  = ImVec4(0.19f, 0.21f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered]           = UI_ACCENT_LO;
    c[ImGuiCol_HeaderActive]            = UI_ACCENT;
    c[ImGuiCol_Separator]               = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
    c[ImGuiCol_SeparatorHovered]        = UI_ACCENT_LO;
    c[ImGuiCol_SeparatorActive]         = UI_ACCENT;
    c[ImGuiCol_ResizeGrip]              = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_ResizeGripHovered]       = UI_ACCENT_LO;
    c[ImGuiCol_ResizeGripActive]        = UI_ACCENT;
    c[ImGuiCol_Tab]                     = ImVec4(0.15f, 0.16f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]              = UI_ACCENT_LO;
    c[ImGuiCol_TabSelected]             = UI_ACCENT;
    c[ImGuiCol_TabSelectedOverline]     = UI_ACCENT_HI;
    c[ImGuiCol_TabDimmed]               = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]       = UI_ACCENT_LO;
    c[ImGuiCol_TextSelectedBg]          = ImVec4(UI_ACCENT.x, UI_ACCENT.y, UI_ACCENT.z, 0.40f);
    c[ImGuiCol_DragDropTarget]          = UI_ACCENT_HI;
    c[ImGuiCol_NavCursor]               = UI_ACCENT_HI;
    c[ImGuiCol_PlotLines]               = ImVec4(0.70f, 0.72f, 0.75f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]        = UI_ACCENT_HI;
    c[ImGuiCol_PlotHistogram]           = UI_ACCENT;
    c[ImGuiCol_PlotHistogramHovered]    = UI_ACCENT_HI;
    c[ImGuiCol_ModalWindowDimBg]        = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

static void why_disabled (b8 disabled, const char* reason)
{
    if (disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
    {
        ImGui::SetTooltip("%s", reason);
    }
}

static b8 ensure_buffers (const UI_Context* ui, UI_FrameBuffers& frame, u64 vertex_bytes, u64 index_bytes)
{
    if (frame.vertex_bytes < vertex_bytes)
    {
        r_destroy_buffer(ui->device, frame.vertices);
        frame.vertex_bytes = zm::max(vertex_bytes, frame.vertex_bytes * 2);
        frame.vertices     = r_create_buffer(ui->device, { .size = frame.vertex_bytes, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    }
    if (frame.index_bytes < index_bytes)
    {
        r_destroy_buffer(ui->device, frame.indices);
        frame.index_bytes = zm::max(index_bytes, frame.index_bytes * 2);
        frame.indices     = r_create_buffer(ui->device, { .size = frame.index_bytes, .usage_flags = (u32)R_BufferUsageFlags::Index, .host_visible = true });
    }
    return r_is_valid(ui->device, frame.vertices) && r_is_valid(ui->device, frame.indices);
}

// Uploads `texture`'s pixels (RGBA32 or Alpha8 expanded to RGBA32) into `image`, which must be in ShaderReadOnly layout
static b8 upload_texture (UI_Context* ui, ImTextureData* texture, R_ImageHandle image)
{
    const u32            width  = (u32)texture->Width;
    const u32            height = (u32)texture->Height;
    const u64            size   = (u64)width * height * 4;
    const R_BufferHandle staging = r_create_buffer(ui->device, { .size = size, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    if (!staging)
    {
        return false;
    }
    defer { r_destroy_buffer(ui->device, staging); };

    u8*       out = (u8*)r_get_buffer_mapped_data(ui->device, staging);
    const u8* in  = (const u8*)texture->GetPixels();
    if (texture->Format == ImTextureFormat_RGBA32)
    {
        memory_copy(out, in, size);
    }
    else
    {
        for (u64 i = 0; i < (u64)width * height; i++)
        {
            out[i * 4 + 0] = 255;
            out[i * 4 + 1] = 255;
            out[i * 4 + 2] = 255;
            out[i * 4 + 3] = in[i];
        }
    }

    r_flush_buffer(ui->device, staging, 0, size);

    const R_CmdBufferHandle cmd = r_begin_immediate(ui->device);
    r_cmd_transition_image(ui->device, cmd, image, R_ImageLayout::ShaderReadOnly, R_ImageLayout::TransferDst);
    r_cmd_copy_buffer_to_image(ui->device, cmd, staging, image, 0);
    r_cmd_transition_image(ui->device, cmd, image, R_ImageLayout::TransferDst, R_ImageLayout::ShaderReadOnly);
    r_end_immediate(ui->device, cmd);
    return true;
}

// ImGui asks the renderer to create, update or destroy its textures through ImDrawData::Textures
static void update_textures (UI_Context* ui, const ImVector<ImTextureData*>* textures)
{
    if (!textures)
    {
        return;
    }
    for (ImTextureData* texture : *textures)
    {
        switch (texture->Status)
        {
        case ImTextureStatus_WantCreate:
        {
            const R_ImageHandle image = r_create_image(ui->device, { .width = (u32)texture->Width, .height = (u32)texture->Height, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::Sampled, .initial_layout = R_ImageLayout::ShaderReadOnly });
            if (image && upload_texture(ui, texture, image))
            {
                texture->SetTexID((ImTextureID)image.value);
                texture->SetStatus(ImTextureStatus_OK);
            }
            break;
        }
        case ImTextureStatus_WantUpdates:
        {
            const R_ImageHandle image = R_ImageHandle((u64)texture->TexID);
            if (r_is_valid(ui->device, image) && upload_texture(ui, texture, image))
            {
                texture->SetStatus(ImTextureStatus_OK);
            }
            break;
        }
        case ImTextureStatus_WantDestroy:
            if (texture->UnusedFrames > R_MAX_FRAMES_IN_FLIGHT)
            {
                r_destroy_image(ui->device, R_ImageHandle((u64)texture->TexID));
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
            break;
        default:
            break;
        }
    }
}

static void destroy_textures (const UI_Context* ui)
{
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
    {
        if (texture->TexID != ImTextureID_Invalid)
        {
            r_destroy_image(ui->device, R_ImageHandle((u64)texture->TexID));
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// Edge-anchored panel of fixed width; panels on the same side stack top-down in call order
static b8 begin_panel (UI_Context* ui, const char* name, UI_Anchor anchor)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const f32            cursor_y = ui->panel_cursor_y[(u32)anchor];
    const f32            anchor_x = anchor == UI_Anchor::Right ? viewport->WorkPos.x + viewport->WorkSize.x - UI_PANEL_GAP : viewport->WorkPos.x + UI_PANEL_GAP;
    const ImVec2         pivot    = anchor == UI_Anchor::Right ? ImVec2(1.0f, 0.0f) : ImVec2(0.0f, 0.0f);
    const f32            max_h    = zm::max(ui->panel_bottom - UI_PANEL_GAP - cursor_y, ImGui::GetFrameHeight());

    ImGui::SetNextWindowPos(ImVec2(anchor_x, cursor_y), ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSizeConstraints(ImVec2(UI_PANEL_WIDTH, 0.0f), ImVec2(UI_PANEL_WIDTH, max_h));
    ImGui::SetNextWindowSize(ImVec2(UI_PANEL_WIDTH, 0.0f), ImGuiCond_Always);
    ui->panel_anchor = anchor;
    const b8 open = ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::PushItemWidth(-UI_PANEL_LABEL_WIDTH);
    return open;
}

static void end_panel (UI_Context* ui)
{
    ImGui::PopItemWidth();
    ui->panel_cursor_y[(u32)ui->panel_anchor] += ImGui::GetWindowSize().y + UI_PANEL_GAP;
    ImGui::End();
}

static R_FramebufferHandle target_framebuffer (UI_Context* ui, R_ImageHandle target)
{
    for (u32 i = 0; i < UI_MAX_TARGETS; i++)
    {
        if (ui->target_images[i] && !r_is_valid(ui->device, ui->target_images[i]))
        {
            r_destroy_framebuffer(ui->device, ui->target_framebuffers[i]);
            ui->target_images[i]       = {};
            ui->target_framebuffers[i] = {};
        }
    }
    u32 free_slot = UI_MAX_TARGETS;
    for (u32 i = 0; i < UI_MAX_TARGETS; i++)
    {
        if (ui->target_images[i] == target)
        {
            return ui->target_framebuffers[i];
        }
        if (!ui->target_images[i] && free_slot == UI_MAX_TARGETS)
        {
            free_slot = i;
        }
    }
    if (free_slot == UI_MAX_TARGETS)
    {
        LOG_ERROR("More UI targets than expected, dropping the cached framebuffers");
        for (u32 i = 0; i < UI_MAX_TARGETS; i++)
        {
            r_destroy_framebuffer(ui->device, ui->target_framebuffers[i]);
            ui->target_images[i]       = {};
            ui->target_framebuffers[i] = {};
        }
        free_slot = 0;
    }
    ui->target_images[free_slot]       = target;
    ui->target_framebuffers[free_slot] = r_create_framebuffer(ui->device, { .render_pass = ui->render_pass, .image = target, .layer = 0 });
    return ui->target_framebuffers[free_slot];
}

static void pose_editor (const char* label, Pose& pose, b8& changed)
{
    ImGui::PushID(label);
    ImGui::SeparatorText(label);
    changed |= ImGui::DragFloat3("position", &pose.position.x, 0.01f);
    Vec3 euler_degrees = zm::euler_from_quat(pose.orientation) * zm::RAD_TO_DEG;
    if (ImGui::DragFloat3("rotation", &euler_degrees.x, 0.5f))
    {
        pose.orientation = zm::quat_from_euler(euler_degrees * zm::DEG_TO_RAD);
        changed          = true;
    }
    ImGui::PopID();
}

static void device_tab (const UI_Context* ui, SIM_Sim* sim, SIM_Device device)
{
    ImGui::BeginDisabled(ui->status.replaying);
    SIM_DeviceState state  = sim_get_device_state(sim, device);
    b8              active = state.active;
    if (ImGui::Checkbox("active", &active))
    {
        sim_set_device_active(sim, device, active);
    }
    why_disabled(ui->status.replaying, "the replay drives the devices");
    b8 changed = false;
    pose_editor("Pose", state.pose, changed);
    if (changed)
    {
        sim_set_device_pose(sim, device, state.pose);
    }

    if (device != SIM_Device::Head)
    {
        ImGui::SeparatorText("Inputs");
        const char* source_names[(u32)SIM_HandSource::Count] = { "controller", "tracked hand" };
        i32         source                                   = (i32)sim_get_hand_source(sim, device);
        if (combo("source", &source, source_names, (i32)SIM_HandSource::Count))
        {
            sim_set_hand_source(sim, device, (SIM_HandSource)source);
        }
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            const SIM_Input input = (SIM_Input)i;
            if (!sim_hand_has_input(sim_get_hand_source(sim, device), sim_get_controller(sim), device, input))
            {
                continue;
            }
            SIM_InputValue value = sim_get_input(sim, device, input);
            b8              set   = false;
            switch (sim_input_type(input))
            {
            case SIM_InputType::Bool:
            {
                // Pressed while the mouse button is held, like the real button
                ImGui::Button(sim_input_name(input), ImVec2(-UI_PANEL_LABEL_WIDTH, 0.0f));
                const f32 pressed = ImGui::IsItemActive() ? 1.0f : 0.0f;
                if (pressed != value.value.x)
                {
                    value.value.x = pressed;
                    set           = true;
                }
                break;
            }
            case SIM_InputType::Float:
                set = ImGui::SliderFloat(sim_input_name(input), &value.value.x, 0.0f, 1.0f);
                break;
            case SIM_InputType::Vec2:
            {
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const f32    width = ImGui::CalcItemWidth();
                set = ImGui::SliderFloat2(sim_input_name(input), &value.value.x, -1.0f, 1.0f);
                label_group(sim_input_name(input), start, width);
                break;
            }
            }
            if (set)
            {
                sim_set_input(sim, device, input, value);
            }
        }
        ImGui::SeparatorText("Haptics");
        static const char* const outputs[(u32)SIM_HapticOutput::Count] = { "haptic", "trigger haptic", "thumb haptic" };
        for (u32 output = 0; output < (u32)SIM_HapticOutput::Count; output++)
        {
            const SIM_Haptic haptic = sim_get_haptic(sim, device, (SIM_HapticOutput)output);
            if (output == 0 || haptic.active)
            {
                char label[16];
                cstr_format(label, sizeof(label), haptic.active ? "%.2f" : "off", (f64)haptic.amplitude);
                ImGui::ProgressBar(haptic.active ? haptic.amplitude : 0.0f, ImVec2(-UI_PANEL_LABEL_WIDTH, 0.0f), label);
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::TextUnformatted(outputs[output]);
            }
        }
    }
    ImGui::EndDisabled();
}

//
// Body tracking
//

// The joints a person poses: not the hands past the wrists, whose fingers are the hands'
static b8 body_posable (u32 joint)
{
    return joint < (u32)SIM_BodyJoint::LeftHandPalm || joint >= (u32)SIM_BodyJoint::LeftUpperLeg || joint == (u32)SIM_BodyJoint::LeftHandWrist || joint == (u32)SIM_BodyJoint::RightHandWrist;
}

// The right side's joint of a left side one, else Count
static u32 body_mirror (u32 joint)
{
    if (joint >= (u32)SIM_BodyJoint::LeftShoulder && joint <= (u32)SIM_BodyJoint::LeftWristTwist)
    {
        return joint + 5;
    }
    if (joint >= (u32)SIM_BodyJoint::LeftUpperLeg && joint <= (u32)SIM_BodyJoint::LeftBall)
    {
        return joint + 7;
    }
    return SIM_BODY_JOINT_COUNT;
}

static b8 body_right_side (u32 joint)
{
    return (joint >= (u32)SIM_BodyJoint::RightShoulder && joint <= (u32)SIM_BodyJoint::RightWristTwist) || joint >= (u32)SIM_BodyJoint::RightUpperLeg;
}

static void body_now (const SIM_Sim* sim, SIM_BodyJoints& out_joints)
{
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    sim_body_joints(sim, sample, out_joints);
}

// Front and side views of the skeleton in the body's own frame, so posing shows whatever the
// head looks at
static void body_views (const SIM_Sim* sim)
{
    SIM_BodyJoints joints = {};
    body_now(sim, joints);
    SIM_BodySkeleton skeleton = {};
    sim_body_skeleton(sim, skeleton);
    const Pose   root   = zm::pose_inverse(joints.poses[(u32)SIM_BodyJoint::Root]);
    const ImVec2 start  = ImGui::GetCursorScreenPos();
    const f32    width  = ImGui::GetContentRegionAvail().x;
    const f32    height = 220.0f;
    ImGui::Dummy(ImVec2(width, height));
    label_text("skeleton front and side");
    ImDrawList* draw  = ImGui::GetWindowDrawList();
    const f32   half  = width * 0.25f;
    const f32   size  = zm::max(sim_get_body(sim).height, 1.0f);
    const f32   scale = zm::min(height / (size * 1.15f), half / (size * 0.55f)); // the T-pose's arm span is about the height
    draw->AddRect(start, ImVec2(start.x + width, start.y + height), ImGui::GetColorU32(UI_ACCENT_LO));
    draw->PushClipRect(start, ImVec2(start.x + width, start.y + height), true);
    for (u32 view = 0; view < 2; view++)
    {
        const ImVec2 origin = ImVec2(start.x + half * (view == 0 ? 1.0f : 3.0f), start.y + height - 8.0f);
        for (u32 j = 1; j < SIM_BODY_JOINT_COUNT; j++)
        {
            const Vec3   a    = zm::pose_mul(root, { joints.poses[j].position, zm::QUAT_IDENTITY }).position;
            const Vec3   b    = zm::pose_mul(root, { joints.poses[skeleton.parents[j]].position, zm::QUAT_IDENTITY }).position;
            const f32    ax   = view == 0 ? a.x : -a.z;
            const f32    bx   = view == 0 ? b.x : -b.z;
            const ImU32  tint = ImGui::GetColorU32(!joints.tracked[j] ? UI_WARNING : (body_right_side(j) ? UI_ACCENT_HI : UI_SOFT));
            draw->AddLine(ImVec2(origin.x + ax * scale, origin.y - a.y * scale), ImVec2(origin.x + bx * scale, origin.y - b.y * scale), tint, 1.5f);
        }
    }
    draw->PopClipRect();
}

static void body_tab (UI_Context* ui, SIM_Sim* sim)
{
    ImGui::BeginDisabled(ui->status.replaying);
    SIM_BodyState body = sim_get_body(sim);
    b8            edit = false;
    body_views(sim);
    ImGui::Checkbox("draw skeleton", &ui->body_overlay);
    ImGui::SetItemTooltip("the joints over the mirror view; screenshots stay without them");
    edit |= ImGui::SliderFloat("height", &body.height, SIM_BODY_MIN_HEIGHT, SIM_BODY_MAX_HEIGHT, "%.2f m");

    const char* poses[(u32)SIM_BodyPose::Count] = {};
    for (u32 p = 0; p < (u32)SIM_BodyPose::Count; p++)
    {
        poses[p] = sim_body_pose_name((SIM_BodyPose)p);
    }
    if (combo("pose", &ui->body_pose, poses, (i32)SIM_BodyPose::Count))
    {
        sim_set_body(sim, body);
        sim_set_body_pose(sim, (SIM_BodyPose)ui->body_pose);
        body = sim_get_body(sim);
    }
    static const char* const limb_labels[(u32)SIM_Limb::Count] = { "left arm posed", "right arm posed", "left leg posed", "right leg posed" };
    for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
    {
        b8 posed = body.limbs[l] == SIM_LimbMode::Posed;
        if (ImGui::Checkbox(limb_labels[l], &posed))
        {
            body.limbs[l] = posed ? SIM_LimbMode::Posed : SIM_LimbMode::Follow;
            edit          = true;
        }
        if (l % 2 == 0)
        {
            ImGui::SameLine();
        }
    }
    ImGui::SetItemTooltip("an arm follows its controller and a leg the floor unless posed");

    ImGui::SeparatorText("Joint");
    const char* names[SIM_BODY_JOINT_COUNT] = {};
    u32         indices[SIM_BODY_JOINT_COUNT] = {};
    i32         count    = 0;
    i32         selected = 0;
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        if (body_posable(j))
        {
            selected         = j == ui->body_joint ? count : selected;
            indices[count]   = j;
            names[count++]   = sim_body_joint_name((SIM_BodyJoint)j);
        }
    }
    if (combo("joint", &selected, names, count))
    {
        ui->body_joint = indices[selected];
    }
    const u32 joint = indices[selected];
    Vec3      euler = zm::euler_from_quat(body.rotations[joint]) * zm::RAD_TO_DEG;
    if (ImGui::DragFloat3("rotation", &euler.x, 0.5f))
    {
        sim_set_body(sim, body);
        sim_set_body_joint(sim, (SIM_BodyJoint)joint, zm::quat_from_euler(euler * zm::DEG_TO_RAD));
        body = sim_get_body(sim);
    }
    ImGui::SetItemTooltip("relative to the T-pose in the body's axes: pitch, yaw, roll in degrees");
    edit |= ImGui::Checkbox("tracked", &body.tracked[joint]);
    edit |= ImGui::SliderFloat("confidence", &body.confidences[joint], 0.0f, 1.0f, "%.2f");
    if (ImGui::Button("reset body"))
    {
        sim_reset_body(sim);
        body          = sim_get_body(sim);
        ui->body_pose = 0;
    }

    if (ImGui::CollapsingHeader("Bone scales"))
    {
        ImGui::Checkbox("link sides", &ui->body_link);
        for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
        {
            if (!sim_body_joint_scalable((SIM_BodyJoint)j) || (ui->body_link && body_right_side(j)))
            {
                continue;
            }
            if (ImGui::SliderFloat(sim_body_joint_name((SIM_BodyJoint)j), &body.bone_scales[j], SIM_BODY_MIN_SCALE, SIM_BODY_MAX_SCALE, "%.2f"))
            {
                const u32 mirror = body_mirror(j);
                if (ui->body_link && mirror < SIM_BODY_JOINT_COUNT)
                {
                    body.bone_scales[mirror] = body.bone_scales[j];
                }
                edit = true;
            }
        }
    }
    if (edit)
    {
        sim_set_body(sim, body);
    }
    why_disabled(ui->status.replaying, "the replay drives the body");
    ImGui::EndDisabled();
}

// The face of face and eye tracking (docs/face-tracking.md); the eye blendshapes follow the gaze,
// so they are not edited here
static void face_tab (UI_Context* ui, SIM_Sim* sim)
{
    ImGui::BeginDisabled(ui->status.replaying);
    SIM_FaceState face = sim_get_face(sim);
    b8            edit = false;
    const char*   presets[(u32)SIM_FacePreset::Count] = {};
    for (u32 p = 0; p < (u32)SIM_FacePreset::Count; p++)
    {
        presets[p] = sim_face_preset_name((SIM_FacePreset)p);
    }
    if (combo("preset", &ui->face_preset, presets, (i32)SIM_FacePreset::Count))
    {
        sim_set_face_preset(sim, (SIM_FacePreset)ui->face_preset);
        face = sim_get_face(sim);
    }
    edit |= ImGui::Checkbox("blink", &face.blink);
    ImGui::SetItemTooltip("a blink every 4 seconds");

    ImGui::SeparatorText("Gaze");
    const b8 yaw   = ImGui::SliderFloat("gaze yaw", &face.gaze_yaw, -SIM_FACE_MAX_GAZE_YAW, SIM_FACE_MAX_GAZE_YAW, "%.1f deg");
    const b8 pitch = ImGui::SliderFloat("gaze pitch", &face.gaze_pitch, -SIM_FACE_MAX_GAZE_PITCH, SIM_FACE_MAX_GAZE_PITCH, "%.1f deg");
    if (yaw || pitch)
    {
        face.look_at = false;
        edit         = true;
    }
    ImGui::SetItemTooltip("from the head, left and up positive; the eyes profile (4) turns them with the mouse");
    edit |= ImGui::Checkbox("look at a point", &face.look_at);
    if (face.look_at)
    {
        edit |= ImGui::DragFloat3("point", &face.target.x, 0.01f);
    }

    ImGui::SeparatorText("Confidence");
    edit |= ImGui::SliderFloat("lower face", &face.confidences[0], 0.0f, 1.0f, "%.2f");
    edit |= ImGui::SliderFloat("upper face", &face.confidences[1], 0.0f, 1.0f, "%.2f");
    if (ImGui::Button("reset face"))
    {
        sim_reset_face(sim);
        face            = sim_get_face(sim);
        ui->face_preset = 0;
        edit            = false;
    }

    if (ImGui::CollapsingHeader("Expressions"))
    {
        for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
        {
            const b8 follows_gaze = e >= (u32)SIM_FaceExpression::EyesLookDownL && e <= (u32)SIM_FaceExpression::EyesLookUpR;
            if (!follows_gaze)
            {
                edit |= ImGui::SliderFloat(sim_face_expression_name((SIM_FaceExpression)e), &face.weights[e], 0.0f, 1.0f, "%.2f");
            }
        }
    }
    if (ImGui::CollapsingHeader("Visemes"))
    {
        for (u32 v = 0; v < SIM_FACE_VISEME_COUNT; v++)
        {
            edit |= ImGui::SliderFloat(sim_face_viseme_name(v), &face.visemes[v], 0.0f, 1.0f, "%.2f");
        }
    }
    if (edit)
    {
        sim_set_face(sim, face);
    }
    why_disabled(ui->status.replaying, "the replay drives the face");
    ImGui::EndDisabled();
}

// The skeleton projected into each eye the mirror shows, behind the panels; the compositor
// stretches an eye over the window, or over its half with both
static void body_overlay (const UI_Context* ui, const SIM_Sim* sim, SIM_MirrorView mirror_view)
{
    if (!ui->body_overlay)
    {
        return;
    }
    SIM_BodyJoints joints = {};
    body_now(sim, joints);
    SIM_BodySkeleton skeleton = {};
    sim_body_skeleton(sim, skeleton);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList*          draw     = ImGui::GetBackgroundDrawList();
    const u32            first    = mirror_view == SIM_MirrorView::Right ? 1u : 0u;
    const u32            slots    = mirror_view == SIM_MirrorView::Both ? zm::min(sim_view_count(sim), 2u) : 1u;
    const f32            slot_w   = viewport->Size.x / (f32)slots;
    for (u32 s = 0; s < slots; s++)
    {
        Pose eye = {};
        Fov  fov = {};
        sim_get_view(sim, first + s, eye, fov);
        const Pose to_eye = zm::pose_inverse(eye);
        const f32  left   = zm::tan(fov.angle_left);
        const f32  right  = zm::tan(fov.angle_right);
        const f32  up     = zm::tan(fov.angle_up);
        const f32  down   = zm::tan(fov.angle_down);
        ImVec2     points[SIM_BODY_JOINT_COUNT] = {};
        b8         seen[SIM_BODY_JOINT_COUNT]   = {};
        for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
        {
            const Vec3 p = zm::pose_mul(to_eye, { joints.poses[j].position, zm::QUAT_IDENTITY }).position;
            seen[j]      = p.z < -0.05f;
            if (seen[j])
            {
                const f32 u = (p.x / -p.z - left) / (right - left);
                const f32 v = (up - p.y / -p.z) / (up - down);
                points[j]   = ImVec2(viewport->Pos.x + slot_w * ((f32)s + u), viewport->Pos.y + viewport->Size.y * v);
            }
        }
        for (u32 j = 1; j < SIM_BODY_JOINT_COUNT; j++)
        {
            const u32 parent = skeleton.parents[j];
            if (seen[j] && seen[parent])
            {
                const ImU32 tint = ImGui::GetColorU32(!joints.tracked[j] ? UI_WARNING : (body_right_side(j) ? UI_ACCENT_HI : UI_SOFT));
                draw->AddLine(points[parent], points[j], tint, 2.0f);
            }
        }
    }
}

// One tab per device and the body; the selected tab follows what the profile's mouse turns
// whenever the profile changes
static void devices_panel (UI_Context* ui, SIM_Sim* sim)
{
    if (!begin_panel(ui, "Devices", UI_Anchor::Right))
    {
        end_panel(ui);
        return;
    }
    const u32        profile = sim_get_profile(sim);
    const b8         follow  = profile != ui->devices_profile;
    const SIM_Device focus   = sim_profile_device(sim_get_config(sim).profiles[profile]);
    ui->devices_profile      = profile;

    struct Tab
    {
        const char* name;
        SIM_Device  device;
    };
    static const Tab tabs[] = {
        { "Head", SIM_Device::Head },
        { "Left", SIM_Device::LeftHand },
        { "Right", SIM_Device::RightHand },
    };
    if (ImGui::BeginTabBar("devices"))
    {
        for (const Tab& tab : tabs)
        {
            const b8 select = follow && tab.device == focus;
            if (ImGui::BeginTabItem(tab.name, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0))
            {
                ImGui::PushID(tab.name);
                device_tab(ui, sim, tab.device);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("Body"))
        {
            ImGui::PushID("Body");
            body_tab(ui, sim);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Face"))
        {
            ImGui::PushID("Face");
            face_tab(ui, sim);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    end_panel(ui);
}

static void menu_bar (UI_Context* ui, UI_Requests& requests)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }
    if (ImGui::BeginMenu("Simulator"))
    {
        if (ImGui::MenuItem("Screenshot"))
        {
            requests.screenshot = true;
        }
        if (ImGui::MenuItem("Recenter", nullptr, false, !ui->status.replaying))
        {
            requests.recenter = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ui->status.recording ? "Stop recording" : "Start recording"))
        {
            requests.record_toggle = true;
        }
        if (ImGui::MenuItem("Stop replay", nullptr, false, ui->status.replaying))
        {
            requests.replay_stop = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit app"))
        {
            requests.quit = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Simulator", nullptr, &ui->show_simulator);
        ImGui::MenuItem("Devices", nullptr, &ui->show_devices);
        ImGui::MenuItem("Log", nullptr, &ui->show_log);
        ImGui::MenuItem("Props", nullptr, &ui->show_props);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("Welcome and controls", "F1", ui->status.show_welcome))
        {
            requests.toggle_welcome = true;
        }
        if (ImGui::MenuItem("Show welcome at start", nullptr, !ui->status.welcome_dismissed))
        {
            requests.set_welcome_at_start = true;
            requests.welcome_at_start     = ui->status.welcome_dismissed;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// Full-width panel pinned to the bottom edge showing the runtime's log history
static void log_panel (UI_Context* ui)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + UI_PANEL_GAP, viewport->WorkPos.y + viewport->WorkSize.y - UI_PANEL_GAP), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - 2.0f * UI_PANEL_GAP, UI_LOG_HEIGHT), ImGuiCond_Always);
    if (ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::Checkbox("simulator", &ui->log_show_sim);
        ImGui::SameLine();
        ImGui::Checkbox("app", &ui->log_show_app);
        ImGui::Separator();
        ImGui::BeginChild("lines");
        const u32 count = os_log_history_count();
        for (u32 i = 0; i < count; i++)
        {
            const OS_LogLine* line = os_log_history(i);
            if ((line->source == OS_LogSource::Sim && !ui->log_show_sim) || (line->source == OS_LogSource::App && !ui->log_show_app))
            {
                continue;
            }
            ImVec4 color = line->source == OS_LogSource::App ? UI_APP_TEXT : UI_TEXT;
            if (line->level == OS_LogLevel::Warning)
            {
                color = UI_WARNING;
            }
            else if (line->level == OS_LogLevel::Error)
            {
                color = UI_ERROR;
            }
            ImGui::TextColored(color, "%8.3f  %s", (f64)line->time_ns * 1e-9, line->text);
        }
        if (os_log_sequence() != ui->log_seen_sequence)
        {
            ImGui::SetScrollHereY(1.0f);
            ui->log_seen_sequence = os_log_sequence();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

static void simulator_panel (UI_Context* ui, SIM_Sim* sim, UI_Requests& requests, SIM_MirrorView& mirror_view, const SIM_Config& config);
static void room_section    (const UI_Context* ui, UI_Requests& requests, b8 busy, const char* busy_reason);

// A new prop a metre in front of the head: a QR code at eye height facing it, a keyboard below
static void spawn_prop (SIM_Sim* sim, SIM_PropKind kind)
{
    const Pose  head    = sim_get_device_pose(sim, SIM_Device::Head);
    const Vec3  forward = zm::rotate(head.orientation, { 0.0f, 0.0f, -1.0f });
    const Quat  facing  = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, zm::atan2(-forward.x, -forward.z));
    const Vec3  ahead   = zm::rotate(facing, { 0.0f, 0.0f, -1.0f });
    SIM_Prop    prop    = { .kind = kind, .pose = { head.position + ahead, facing }, .size = 0.15f };
    if (kind == SIM_PropKind::Keyboard)
    {
        prop.pose.position.y = zm::max(head.position.y - 0.6f, 0.05f);
    }
    else
    {
        cstr_copy(prop.text, sizeof(prop.text), "https://example.com/puppetxr");
    }
    prop.target = prop.pose.position + zm::rotate(facing, { 0.5f, 0.0f, 0.0f });
    sim_add_prop(sim, prop);
}

// Props (docs/props.md): every change restarts the prop's motion from where it is defined
static void props_panel (UI_Context* ui, SIM_Sim* sim)
{
    if (!begin_panel(ui, "Props", UI_Anchor::Right))
    {
        end_panel(ui);
        return;
    }
    ImGui::BeginDisabled(ui->status.replaying);
    const SIM_Prop* props = nullptr;
    const u32       count = sim_props(sim, props);
    ImGui::BeginDisabled(count == SIM_MAX_PROPS);
    if (ImGui::Button("add QR code"))
    {
        spawn_prop(sim, SIM_PropKind::QrCode);
    }
    ImGui::SameLine();
    if (ImGui::Button("add keyboard"))
    {
        spawn_prop(sim, SIM_PropKind::Keyboard);
    }
    ImGui::EndDisabled();
    u32 remove = 0;
    for (u32 i = 0; i < count; i++)
    {
        SIM_Prop prop = props[i];
        ImGui::PushID((i32)prop.id);
        char header[32];
        cstr_format(header, sizeof(header), "%s %u", prop.kind == SIM_PropKind::QrCode ? "QR code" : "keyboard", prop.id);
        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
        {
            b8 changed = false;
            pose_editor("Pose", prop.pose, changed);
            if (prop.kind == SIM_PropKind::QrCode)
            {
                changed |= ImGui::InputText("text", prop.text, sizeof(prop.text));
                changed |= ImGui::DragFloat("size", &prop.size, 0.005f, 0.01f, 2.0f, "%.3f m");
            }
            const char* motions[(u32)SIM_PropMotion::Count] = { "still", "back and forth", "circle" };
            i32         motion                              = (i32)prop.motion;
            if (combo("motion", &motion, motions, (i32)SIM_PropMotion::Count))
            {
                prop.motion = (SIM_PropMotion)motion;
                prop.radius = prop.motion == SIM_PropMotion::Circle && prop.radius <= 0.0f ? 0.3f : prop.radius;
                prop.speed  = prop.motion != SIM_PropMotion::Still && prop.speed <= 0.0f ? 0.2f : prop.speed;
                changed     = true;
            }
            if (prop.motion == SIM_PropMotion::Linear)
            {
                changed |= ImGui::DragFloat3("to", &prop.target.x, 0.01f);
            }
            if (prop.motion == SIM_PropMotion::Circle)
            {
                changed |= ImGui::DragFloat("radius", &prop.radius, 0.01f, 0.01f, 10.0f, "%.2f m");
            }
            if (prop.motion != SIM_PropMotion::Still)
            {
                changed |= ImGui::DragFloat("speed", &prop.speed, 0.01f, 0.0f, 10.0f, "%.2f m/s");
            }
            if (changed)
            {
                sim_update_prop(sim, prop.id, prop);
            }
            if (ImGui::Button("remove"))
            {
                remove = prop.id;
            }
        }
        ImGui::PopID();
    }
    if (remove != 0)
    {
        sim_remove_prop(sim, remove);
    }
    ImGui::EndDisabled();
    end_panel(ui);
}

// Live typing: each character goes to the app's keyboard instead of into the field
static int type_character (ImGuiInputTextCallbackData* data) // NOLINT(misc-const-correctness): ImGuiInputTextCallback
{
    const u32 c        = data->EventChar;
    char      utf8[5]  = {};
    if (c < 0x80)
    {
        utf8[0] = (char)c;
    }
    else if (c < 0x800)
    {
        utf8[0] = (char)(0xC0 | (c >> 6));
        utf8[1] = (char)(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        utf8[0] = (char)(0xE0 | (c >> 12));
        utf8[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (c & 0x3F));
    }
    else
    {
        utf8[0] = (char)(0xF0 | (c >> 18));
        utf8[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (c & 0x3F));
    }
    sim_type_text((SIM_Sim*)data->UserData, utf8);
    return 1;
}

// XR_META_virtual_keyboard: text typed here reaches the app as if typed on its keyboard
static void keyboard_panel (UI_Context* ui, SIM_Sim* sim)
{
    const UI_Status& status = ui->status;
    if (!status.keyboard_exists)
    {
        return;
    }
    if (!begin_panel(ui, "Virtual keyboard", UI_Anchor::Right))
    {
        end_panel(ui);
        return;
    }
    ImGui::Text("%s%s", status.keyboard_visible ? "shown" : "hidden", status.keyboard_shifted ? ", shift" : "");
    ImGui::TextWrapped("before the cursor: %s", status.keyboard_context ? status.keyboard_context : "");
    ImGui::BeginDisabled(!status.keyboard_visible || status.replaying);
    ImGui::Checkbox("live typing", &ui->live_typing);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (ui->live_typing ? ImGuiInputTextFlags_CallbackCharFilter : 0);
    ImGui::SetNextItemWidth(-UI_PANEL_LABEL_WIDTH);
    const b8 entered = ImGui::InputText("text", ui->typing, sizeof(ui->typing), flags, type_character, sim);
    const b8 active  = ImGui::IsItemActive() || entered;
    if (ui->live_typing && active && ImGui::IsKeyPressed(ImGuiKey_Backspace))
    {
        sim_type_text(sim, "\b");
    }
    if (entered)
    {
        sim_type_text(sim, ui->live_typing ? "\n" : ui->typing);
        ui->typing[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (!ui->live_typing && ImGui::Button("type"))
    {
        sim_type_text(sim, ui->typing);
        ui->typing[0] = 0;
    }
    if (!ui->live_typing)
    {
        ImGui::SameLine();
    }
    if (ImGui::Button("backspace"))
    {
        sim_type_text(sim, "\b");
    }
    ImGui::SameLine();
    if (ImGui::Button("enter"))
    {
        sim_type_text(sim, "\n");
    }
    ImGui::EndDisabled();
    end_panel(ui);
}

static void build_panels (UI_Context* ui, SIM_Sim* sim, UI_Requests& requests, SIM_MirrorView& mirror_view)
{
    const SIM_Config&    config   = sim_get_config(sim);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    menu_bar(ui, requests);

    // The work area already excludes the main menu bar
    const f32 top = viewport->WorkPos.y + UI_PANEL_GAP;
    for (u32 i = 0; i < (u32)UI_Anchor::Count; i++)
    {
        ui->panel_cursor_y[i] = top;
    }
    ui->panel_bottom = viewport->WorkPos.y + viewport->WorkSize.y - (ui->show_log ? UI_LOG_HEIGHT + UI_PANEL_GAP : 0.0f);

    if (ui->show_log)
    {
        log_panel(ui);
    }
    if (ui->show_simulator)
    {
        simulator_panel(ui, sim, requests, mirror_view, config);
    }
    if (ui->show_devices)
    {
        devices_panel(ui, sim);
    }
    keyboard_panel(ui, sim);
    if (ui->show_props)
    {
        props_panel(ui, sim);
    }
}

static void record_section (const UI_Context* ui, UI_Requests& requests)
{
    const UI_Status& status = ui->status;
    if (ImGui::Button(status.recording ? "stop recording" : "record"))
    {
        requests.record_toggle = true;
    }
    if (status.recording)
    {
        ImGui::SameLine();
        ImGui::TextColored(UI_RECORDING, "%llu frames", (unsigned long long)status.recorded_frames);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_MUTED);
        ImGui::TextWrapped("%s", status.record_path ? status.record_path : "");
        ImGui::PopStyleColor();
    }
    else if (!status.replaying && !status.replay_finished)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("poses and inputs, bit-identical replay");
    }

    if (status.replaying || status.replay_finished)
    {
        const f32 progress = status.replay_total ? (f32)status.replay_frame / (f32)status.replay_total : 1.0f;
        char      label[64];
        cstr_format(label, sizeof(label), "%llu / %llu", (unsigned long long)status.replay_frame, (unsigned long long)status.replay_total);
        ImGui::ProgressBar(progress, ImVec2(-UI_PANEL_LABEL_WIDTH, 0.0f), label);
        ImGui::SameLine();
        ImGui::TextUnformatted(status.replaying ? "replay" : "replay done");
        if (status.replaying && ImGui::Button("stop replay"))
        {
            requests.replay_stop = true;
        }
        if (status.divergences > 0)
        {
            ImGui::TextColored(UI_WARNING, "%u frames diverged, first at %llu", status.divergences, (unsigned long long)status.first_divergence);
        }
    }
}

static void status_badge (const UI_Context* ui)
{
    const UI_Status&     status   = ui->status;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList*          draw     = ImGui::GetForegroundDrawList();
    if (status.mouse_captured)
    {
        const char*  hint = "Esc gives the mouse back";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f, viewport->Pos.y + viewport->Size.y - size.y - 2.0f * UI_PANEL_GAP), ImGui::GetColorU32(UI_SOFT), hint);
    }
    if (!status.recording && !status.replaying)
    {
        return;
    }
    const char*          text     = status.recording ? "REC" : "REPLAY";
    const ImU32          color    = ImGui::GetColorU32(status.recording ? UI_RECORDING : UI_ACCENT_HI);
    const ImVec2         size     = ImGui::CalcTextSize(text);
    const f32            radius   = size.y * 0.35f;
    const f32            right    = viewport->Pos.x + viewport->Size.x - 2.0f * UI_PANEL_GAP;
    const ImVec2         corner   = ImVec2(right - size.x - 3.0f * radius, viewport->Pos.y + (ImGui::GetFrameHeight() - size.y) * 0.5f);
    draw->AddCircleFilled(ImVec2(corner.x + radius, corner.y + size.y * 0.5f), radius, color);
    draw->AddText(ImVec2(corner.x + 3.0f * radius, corner.y), color, text);
}

static void append_key (KeyText& out, OS_Key key, const char* separator)
{
    char label[32];
    switch (key)
    {
    case OS_Key::MouseLeft:
        cstr_copy(label, sizeof(label), "left mouse");
        break;
    case OS_Key::MouseRight:
        cstr_copy(label, sizeof(label), "right mouse");
        break;
    case OS_Key::MouseMiddle:
        cstr_copy(label, sizeof(label), "middle mouse");
        break;
    default:
        os_key_label(key, label, sizeof(label));
        break;
    }
    const usize length = cstr_length(out.text);
    cstr_format(out.text + length, sizeof(out.text) - length, "%s%s", length ? separator : "", label);
}

static KeyText keys (const OS_Key* list, u32 count, const char* separator)
{
    KeyText out = {};
    for (u32 i = 0; i < count; i++)
    {
        append_key(out, list[i], separator);
    }
    return out;
}

// Every key bound to the slot without modifiers, mouse buttons last
static KeyText slot_keys (const SIM_Config& config, SIM_Slot slot)
{
    KeyText out = {};
    for (u32 pass = 0; pass < 2; pass++)
    {
        for (u32 i = 0; i < config.bindings_count; i++)
        {
            const SIM_Binding& binding = config.bindings[i];
            if (binding.is_slot && binding.slot == slot && binding.modifiers == 0 && (binding.key >= OS_Key::MouseLeft) == (pass == 1))
            {
                append_key(out, binding.key, " / ");
            }
        }
    }
    if (!out.text[0])
    {
        cstr_copy(out.text, sizeof(out.text), "unbound");
    }
    return out;
}

// What a drive does, by the source that drives it
static const char* drive_text (SIM_Drive drive, b8 keys)
{
    switch (drive)
    {
    case SIM_Drive::Walk:
        return keys ? "walk, Q/E down and up" : "walk";
    case SIM_Drive::MoveHead:
        return "move the head";
    case SIM_Drive::MoveLeft:
        return keys ? "move the left hand up, down and sideways, Q/E toward and away" : "move the left hand";
    case SIM_Drive::MoveRight:
        return keys ? "move the right hand up, down and sideways, Q/E toward and away" : "move the right hand";
    case SIM_Drive::TurnBody:
        return "look around";
    case SIM_Drive::TurnHead:
        return "turn the head";
    case SIM_Drive::TurnLeft:
        return "turn the left hand";
    case SIM_Drive::TurnRight:
        return "turn the right hand";
    case SIM_Drive::LeftStick:
        return "the left thumbstick";
    case SIM_Drive::RightStick:
        return "the right thumbstick";
    case SIM_Drive::PushLeft:
        return "push the left hand away and back";
    case SIM_Drive::PushRight:
        return "push the right hand away and back";
    case SIM_Drive::RollLeft:
        return "roll the left hand";
    case SIM_Drive::RollRight:
        return "roll the right hand";
    case SIM_Drive::TurnEyes:
        return "turn the eyes";
    case SIM_Drive::None:
    case SIM_Drive::Count:
        break;
    }
    return nullptr;
}

// The profile's sources that drive something, keys first
static u32 profile_lines (const SIM_Config& config, const SIM_Profile& profile, ProfileLine (&out)[6])
{
    const OS_Key move[] = { config.key_move_forward, config.key_move_left, config.key_move_back, config.key_move_right };
    const OS_Key turn[] = { config.key_turn_up, config.key_turn_left, config.key_turn_down, config.key_turn_right };
    KeyText      mouse  = {};
    KeyText      shift  = {};
    KeyText      wheel  = {};
    KeyText      roll   = {};
    cstr_copy(mouse.text, sizeof(mouse.text), "mouse");
    cstr_copy(shift.text, sizeof(shift.text), "Shift + mouse");
    cstr_copy(wheel.text, sizeof(wheel.text), "wheel");
    cstr_copy(roll.text, sizeof(roll.text), "Shift + wheel");
    KeyText arrows = keys(turn, ARRAY_SIZE(turn), " ");
    cstr_format(arrows.text + cstr_length(arrows.text), sizeof(arrows.text) - cstr_length(arrows.text), ", arrows");
    const ProfileLine lines[6] = {
        { keys(move, ARRAY_SIZE(move), " "), drive_text(profile.wasd, true) },
        { arrows, drive_text(profile.ijkl, false) },
        { mouse, drive_text(profile.mouse, false) },
        { shift, drive_text(profile.shift_mouse, false) },
        { wheel, drive_text(profile.wheel, false) },
        { roll, drive_text(profile.shift_wheel, false) },
    };
    u32 count = 0;
    for (const ProfileLine& line : lines)
    {
        if (line.action)
        {
            out[count++] = line;
        }
    }
    return count;
}

// Names the profile for a few seconds once it changed, not at start
static void profile_toast (UI_Context* ui, const SIM_Sim* sim)
{
    const SIM_Config& config  = sim_get_config(sim);
    const u32         profile = sim_get_profile(sim);
    const f64         now     = ImGui::GetTime();
    if (profile != ui->toast_profile)
    {
        ui->toast_until   = ui->toast_profile == UINT32_MAX ? 0.0 : now + UI_TOAST_SECONDS;
        ui->toast_profile = profile;
    }
    if (now >= ui->toast_until)
    {
        return;
    }
    ProfileLine lines[6] = {};
    const u32   count    = profile_lines(config, config.profiles[profile], lines);
    char        title[64];
    cstr_format(title, sizeof(title), "Profile %s", config.profiles[profile].name);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList*          draw     = ImGui::GetForegroundDrawList();
    const f32            line     = ImGui::GetTextLineHeightWithSpacing();
    f32                  key_w    = 0.0f;
    f32                  action_w = 0.0f;
    for (u32 i = 0; i < count; i++)
    {
        key_w    = zm::max(key_w, ImGui::CalcTextSize(lines[i].keys.text).x);
        action_w = zm::max(action_w, ImGui::CalcTextSize(lines[i].action).x);
    }
    const f32    pad    = 12.0f;
    const f32    width  = zm::max(ImGui::CalcTextSize(title).x, key_w + 16.0f + action_w) + 2.0f * pad;
    const ImVec2 corner = ImVec2(viewport->Pos.x + (viewport->Size.x - width) * 0.5f, viewport->Pos.y + 3.0f * line);
    draw->AddRectFilled(corner, ImVec2(corner.x + width, corner.y + (f32)(count + 1) * line + 2.0f * pad), ImGui::GetColorU32(ImGuiCol_WindowBg), 6.0f);
    draw->AddText(ImVec2(corner.x + pad, corner.y + pad), ImGui::GetColorU32(UI_ACCENT_HI), title);
    for (u32 i = 0; i < count; i++)
    {
        const f32 y = corner.y + pad + (f32)(i + 1) * line;
        draw->AddText(ImVec2(corner.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text), lines[i].keys.text);
        draw->AddText(ImVec2(corner.x + pad + key_w + 16.0f, y), ImGui::GetColorU32(UI_SOFT), lines[i].action);
    }
}

static void welcome_heading (const char* text)
{
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextColored(UI_ACCENT_HI, "%s", text);
}

static void welcome_row (const char* key, const char* action)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(key);
    label_text(key);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SOFT);
    ImGui::TextWrapped("%s", action);
    ImGui::PopStyleColor();
}

static b8 welcome_table (const char* id, f32 width)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit, ImVec2(width, 0.0f)))
    {
        return false;
    }
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, UI_WELCOME_KEY_WIDTH);
    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

static void welcome_controls (const SIM_Config& config, u32 profile, f32 width);
static void welcome_buttons  (const SIM_Config& config, u32 profile, f32 width);
static void welcome_footer   (b8 dismissed);

// Full-window, see-through and without inputs, so the sim can be driven while reading it
static void welcome_overlay (UI_Context* ui, const SIM_Config& config, u32 profile)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    // Blended in linear space on sRGB targets: 0.86 let a bright scene through at four tenths of its
    // brightness and the grey descriptions were hard to read
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    const b8               open  = ImGui::Begin("##welcome", nullptr, flags);
    ImGui::PopStyleVar(2);
    if (!open)
    {
        ImGui::End();
        return;
    }
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    const f32 margin = UI_WELCOME_MARGIN;
    const f32 room   = viewport->Size.x - 2.0f * margin;
    const b8  wide   = room >= 2.0f * UI_WELCOME_COLUMN + UI_WELCOME_GAP;
    const f32 width  = wide ? 2.0f * UI_WELCOME_COLUMN + UI_WELCOME_GAP : zm::min(UI_WELCOME_COLUMN * 1.4f, room);
    const f32 column = wide ? UI_WELCOME_COLUMN : width;
    const f32 left   = zm::max((viewport->Size.x - width) * 0.5f, margin);
    const f32 top    = zm::max((viewport->Size.y - ui->welcome_height) * 0.5f, margin);
    const f32 footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;

    // The body is clipped so the footer with the keys that close the overlay always shows
    ImGui::SetCursorPos(ImVec2(left, top));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, zm::max(viewport->Size.y - top - margin - footer, 0.0f)));
    const ImGuiWindowFlags body_flags = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    ImGui::BeginChild("body", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY, body_flags);
    ImGui::PushFont(nullptr, UI_FONT_SIZE * 1.7f);
    ImGui::TextUnformatted("Welcome to puppetxr");
    ImGui::PopFont();
    ImGui::TextWrapped("A simulated headset and two controllers, driven by keyboard and mouse. The app runs as it would on a real headset; this window mirrors what it renders.");
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(column);
    welcome_controls(config, profile, column);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (wide)
    {
        ImGui::SameLine(0.0f, UI_WELCOME_GAP);
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + column);
    welcome_buttons(config, profile, column);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const f32 body = ImGui::GetCursorPosY();
    ImGui::EndChild();

    const ImVec2 line = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(viewport->Pos.x + left, line.y), ImVec2(viewport->Pos.x + left + width, line.y), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
    ImGui::SetCursorPosX(left);
    welcome_footer(ui->status.welcome_dismissed);
    ui->welcome_height = body + footer;
    ImGui::End();
}

static void welcome_controls (const SIM_Config& config, u32 profile, f32 width)
{
    welcome_heading("Window");
    if (welcome_table("window", width))
    {
        const OS_Key ui_key[] = { config.key_ui };
        KeyText      ui_text  = keys(ui_key, 1, "");
        if (config.key_ui == OS_Key::Grave)
        {
            cstr_format(ui_text.text + cstr_length(ui_text.text), sizeof(ui_text.text) - cstr_length(ui_text.text), " (left of 1)");
        }
        welcome_row(ui_text.text, "show or hide the menu and the panels");
        welcome_row("click the view", "take the mouse: it looks around and its buttons are the controller's; Esc gives it back");
        welcome_row("F1", "show or hide this help");
        ImGui::EndTable();
    }

    char heading[64];
    cstr_format(heading, sizeof(heading), "Move and look: profile %s", config.profiles[profile].name);
    welcome_heading(heading);
    if (welcome_table("move", width))
    {
        ProfileLine lines[6] = {};
        const u32   count    = profile_lines(config, config.profiles[profile], lines);
        for (u32 i = 0; i < count; i++)
        {
            welcome_row(lines[i].keys.text, lines[i].action);
        }
        KeyText picks = {};
        for (u32 i = 0; i < config.profiles_count; i++)
        {
            if (config.profiles[i].key != OS_Key::None)
            {
                append_key(picks, config.profiles[i].key, " ");
            }
        }
        const OS_Key cycle[]  = { config.key_next_profile, config.key_previous_profile };
        const OS_Key resets[] = { config.key_reset, config.key_reset_hands };
        welcome_row(picks.text, "the profiles: body walks, stick drives the thumbsticks, hands moves the hands (a panel on the left, a pointer on the right), eyes turns the eyes with the mouse");
        welcome_row(keys(cycle, ARRAY_SIZE(cycle), " / ").text, "the next profile / the one before");
        welcome_row(keys(resets, ARRAY_SIZE(resets), " / ").text, "reset head and hands / only the hands");
        ImGui::EndTable();
    }
}

static void welcome_buttons (const SIM_Config& config, u32 profile, f32 width)
{
    welcome_heading("Controller buttons");
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SOFT);
    const SIM_BindHand hand = config.profiles[profile].hand;
    const b8           left = hand == SIM_BindHand::Left || (hand == SIM_BindHand::Active && config.primary_hand == SIM_Device::LeftHand);
    ImGui::TextWrapped("On the %s hand in this profile.", left ? "left" : "right");
    ImGui::PopStyleColor();
    if (welcome_table("buttons", width))
    {
        const OS_Key freeze[] = { config.key_freeze };
        welcome_row(slot_keys(config, SIM_Slot::Trigger).text, "trigger");
        welcome_row(slot_keys(config, SIM_Slot::Grip).text, "grip");
        welcome_row(slot_keys(config, SIM_Slot::Primary).text, "A / X");
        welcome_row(slot_keys(config, SIM_Slot::Secondary).text, "B / Y");
        welcome_row(slot_keys(config, SIM_Slot::StickClick).text, "thumbstick click");
        welcome_row(slot_keys(config, SIM_Slot::Menu).text, "menu");
        welcome_row("Shift / Ctrl + key", "the same on the left / right hand; with the turn keys, its thumbstick");
        welcome_row(keys(freeze, 1, "").text, "hold analog inputs where they are, again to release");
        const OS_Key hand_source[] = { config.key_hand_source };
        welcome_row(keys(hand_source, 1, "").text, "switch the hand between its controller and hand tracking");
        ImGui::EndTable();
    }

    welcome_heading("Help");
    if (welcome_table("help", width))
    {
        welcome_row("docs", "github.com/FlexW/puppetxr, the docs folder");
        welcome_row("Log panel", "messages of puppetxr and the app");
        welcome_row("puppetxrctl --help", "control a running session from a terminal");
        ImGui::EndTable();
    }
}

static void welcome_footer (b8 dismissed)
{
    if (dismissed)
    {
        ImGui::TextColored(UI_ACCENT_HI, "Esc / F1");
        ImGui::SameLine();
        ImGui::TextUnformatted("close");
    }
    else
    {
        ImGui::TextColored(UI_ACCENT_HI, "Enter");
        label_text("Enter");
        ImGui::SameLine();
        ImGui::TextUnformatted("got it, don't show again");
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(UI_ACCENT_HI, "Esc");
        ImGui::SameLine();
        ImGui::TextUnformatted("close for now");
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(UI_ACCENT_HI, "F1");
        ImGui::SameLine();
        ImGui::TextUnformatted("open again");
    }
}

static void simulator_tab (const UI_Context* ui, SIM_Sim* sim, UI_Requests& requests, SIM_MirrorView& mirror_view, const SIM_Config& config)
{
    const b8          busy        = ui->status.recording || ui->status.replaying;
    const char* const busy_reason = "fixed while recording or replaying";

    if (ImGui::CollapsingHeader("Session", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("frame %llu   time %.3f s", (unsigned long long)sim_frame_index(sim), (f64)sim_time(sim) * 1e-9);
        const char* profile_names[SIM_MAX_PROFILES] = {};
        for (u32 i = 0; i < config.profiles_count; i++)
        {
            profile_names[i] = config.profiles[i].name;
        }
        i32 profile = (i32)sim_get_profile(sim);
        if (combo("profile", &profile, profile_names, (i32)config.profiles_count))
        {
            sim_set_profile(sim, (u32)profile);
        }
        b8 controls = sim_controls_enabled(sim);
        if (ImGui::Checkbox("keyboard and mouse", &controls))
        {
            sim_set_controls_enabled(sim, controls);
        }
        b8 fixed = sim_clock_mode(sim) == SIM_ClockMode::FixedStep;
        if (ImGui::Checkbox("fixed step clock", &fixed))
        {
            sim_set_clock_mode(sim, fixed ? SIM_ClockMode::FixedStep : SIM_ClockMode::Realtime);
        }
        ImGui::BeginDisabled(ui->status.replaying);
        if (ImGui::Button("reset"))
        {
            sim_reset_poses(sim);
        }
        why_disabled(ui->status.replaying, "the replay drives the devices");
        ImGui::SameLine();
        if (ImGui::Button("reset hands"))
        {
            sim_reset_hands(sim);
        }
        why_disabled(ui->status.replaying, "the replay drives the devices");
        ImGui::SameLine();
        if (ImGui::Button("recenter"))
        {
            requests.recenter = true;
        }
        why_disabled(ui->status.replaying, "the replay drives the devices");
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* mirror_names[] = { "left eye", "right eye", "both eyes" };
        i32         mirror         = (i32)mirror_view;
        if (combo("mirror view", &mirror, mirror_names, 3))
        {
            mirror_view = (SIM_MirrorView)mirror;
        }
        if (ImGui::Button("screenshot"))
        {
            requests.screenshot = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s (%s), eye %ux%u @ %.0f Hz", config.system_name, config.device, config.view_width, config.view_height, (f64)config.refresh_rate_hz);
        b8 lens_mask = ui->status.lens_mask;
        if (ImGui::Checkbox("lens mask", &lens_mask))
        {
            requests.set_lens_mask = true;
            requests.lens_mask     = lens_mask;
        }
        ImGui::SetItemTooltip("black where the lens hides the display, and the hidden area for the app (XR_KHR_visibility_mask)");
    }

    if (ImGui::CollapsingHeader("Recording", ImGuiTreeNodeFlags_DefaultOpen))
    {
        record_section(ui, requests);
    }

    if (ImGui::CollapsingHeader("Controllers", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* controller_names[(u32)SIM_Controller::Count] = { "Oculus Touch", "KHR simple", "Meta Touch Pro", "Meta Touch Plus" };
        i32         controller                                   = (i32)sim_get_controller(sim);
        ImGui::BeginDisabled(busy);
        if (combo("type", &controller, controller_names, (i32)SIM_Controller::Count))
        {
            requests.set_controller = true;
            requests.controller     = (SIM_Controller)controller;
        }
        why_disabled(busy, busy_reason);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Room"))
    {
        room_section(ui, requests, busy, busy_reason);
    }
}

static void performance_row (const char* label)
{
    ImGui::TextUnformatted(label);
    label_text(label);
    ImGui::SameLine(UI_PANEL_LABEL_WIDTH);
}

// The app's frame rate and a graph of its last frames, the display's budget as a line, so a
// spike or an uneven rate shows at a glance
static void performance_tab (const UI_Context* ui, const SIM_Config& config)
{
    const UI_FrameTimes* times = ui->status.frame_times;
    if (!times || times->count == 0)
    {
        ImGui::TextDisabled("no frames yet");
        return;
    }
    const UI_FrameStats stats  = ui_frame_stats(*times);
    const f32           budget = config.refresh_rate_hz > 0.0f ? 1000.0f / config.refresh_rate_hz : 0.0f;
    performance_row("frame rate");
    ImGui::Text("%.1f fps (%.0f Hz display)", (f64)stats.fps, (f64)config.refresh_rate_hz);
    performance_row("frame time");
    ImGui::Text("%.1f ms, slowest %.1f ms", (f64)stats.mean_ms, (f64)stats.max_ms);
    performance_row("puppetxr");
    ImGui::Text("%.1f ms a frame (%.0f%%)", (f64)stats.puppetxr_mean_ms, stats.mean_ms > 0.0f ? (f64)(stats.puppetxr_mean_ms / stats.mean_ms * 100.0f) : 0.0);
    performance_row("mirror");
    ImGui::Text("%.0f presents a second", (f64)times->mirror_fps);

    f32 highest = budget * 2.0f;
    for (u32 i = 0; i < times->count; i++)
    {
        highest = zm::max(highest, times->frame_ms[i] * 1.1f);
    }
    if (ImPlot::BeginPlot("frame times", ImVec2(-1.0f, 170.0f), ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText))
    {
        label_text("frame times");
        ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines, 0);
        ImPlot::SetupAxesLimits(0.0, (f64)UI_FRAME_TIMES, 0.0, (f64)highest, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
        // The newest frame on the right; the ring starts at `next` once full
        const i32 offset = times->count == UI_FRAME_TIMES ? (i32)times->next : 0;
        const f64 start  = (f64)(UI_FRAME_TIMES - times->count);
        ImPlot::PlotLine("app", times->frame_ms, (i32)times->count, 1.0, start, { ImPlotProp_Offset, offset });
        ImPlot::PlotLine("puppetxr", times->puppetxr_ms, (i32)times->count, 1.0, start, { ImPlotProp_Offset, offset });
        if (budget > 0.0f)
        {
            const f64 line = (f64)budget;
            ImPlot::PlotInfLines("display", &line, 1, { ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal });
        }
        ImPlot::EndPlot();
    }
}

static void simulator_panel (UI_Context* ui, SIM_Sim* sim, UI_Requests& requests, SIM_MirrorView& mirror_view, const SIM_Config& config)
{
    if (!begin_panel(ui, "Simulator", UI_Anchor::Left))
    {
        end_panel(ui);
        return;
    }
    if (ImGui::BeginTabBar("simulator"))
    {
        if (ImGui::BeginTabItem("Simulator"))
        {
            simulator_tab(ui, sim, requests, mirror_view, config);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance"))
        {
            performance_tab(ui, config);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    const OS_Key ui_key[] = { config.key_ui };
    char         hint[64];
    cstr_format(hint, sizeof(hint), "F1 help   %s hide", keys(ui_key, 1, "").text);
    ImGui::TextDisabled("%s", hint);
    label_text(hint);
    end_panel(ui);
}

static void room_section (const UI_Context* ui, UI_Requests& requests, b8 busy, const char* busy_reason)
{
    ImGui::BeginDisabled(busy);
    const char* room_label = ui->status.room && ui->status.room[0] ? ui->status.room : "none";
    const ImGuiID room_id = ImGui::GetID("room");
    if (ImGui::BeginCombo("room", room_label))
    {
        if (ImGui::Selectable("none", !ui->status.room || !ui->status.room[0]))
        {
            requests.set_room = true;
            requests.room[0]  = 0;
        }
        for (u32 i = 0; i < ui->status.rooms_count; i++)
        {
            const b8 current = ui->status.room && cstr_equal(ui->status.room, ui->status.rooms[i]);
            if (ImGui::Selectable(ui->status.rooms[i], current))
            {
                requests.set_room = true;
                cstr_copy(requests.room, sizeof(requests.room), ui->status.rooms[i]);
            }
        }
        ImGui::EndCombo();
    }
    label_item(room_id, "room");
    why_disabled(busy, busy_reason);

    const Pose& origin  = ui->status.room_from_stage;
    const Vec3  forward = zm::rotate(origin.orientation, { 0.0f, 0.0f, -1.0f });
    ImGui::TextDisabled("stage origin (%.2f, %.2f) m, %.0f deg", (f64)origin.position.x, (f64)origin.position.z, (f64)(zm::atan2(-forward.x, -forward.z) * zm::RAD_TO_DEG));
    ImGui::TextDisabled("%u stored anchors", ui->status.anchors_stored);
    if (ImGui::Button("wipe anchors"))
    {
        requests.wipe_anchors = true;
    }
    why_disabled(busy, busy_reason);
    ImGui::SameLine();
    if (ImGui::Button("wipe anchors and cloud"))
    {
        requests.wipe_anchors = true;
        requests.wipe_cloud   = true;
    }
    why_disabled(busy, busy_reason);
    ImGui::EndDisabled();
}

static void record_draw_data (UI_Context* ui, R_CmdBufferHandle cmd, const ImDrawData* draw_data, u32 width, u32 height)
{
    if (!draw_data || draw_data->TotalVtxCount == 0)
    {
        return;
    }
    UI_FrameBuffers& frame = ui->frames[ui->frame_index];
    ui->frame_index        = (ui->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
    if (!ensure_buffers(ui, frame, (u64)draw_data->TotalVtxCount * sizeof(ImDrawVert), (u64)draw_data->TotalIdxCount * sizeof(ImDrawIdx)))
    {
        return;
    }

    ImDrawVert* vertices = (ImDrawVert*)r_get_buffer_mapped_data(ui->device, frame.vertices);
    ImDrawIdx*  indices  = (ImDrawIdx*)r_get_buffer_mapped_data(ui->device, frame.indices);
    for (i32 n = 0; n < draw_data->CmdLists.Size; n++)
    {
        const ImDrawList* list = draw_data->CmdLists[n];
        memory_copy(vertices, list->VtxBuffer.Data, (usize)list->VtxBuffer.Size * sizeof(ImDrawVert));
        memory_copy(indices, list->IdxBuffer.Data, (usize)list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vertices += list->VtxBuffer.Size;
        indices += list->IdxBuffer.Size;
    }
    r_flush_buffer(ui->device, frame.vertices, 0, (u64)draw_data->TotalVtxCount * sizeof(ImDrawVert));
    r_flush_buffer(ui->device, frame.indices, 0, (u64)draw_data->TotalIdxCount * sizeof(ImDrawIdx));

    r_cmd_bind_pipeline(ui->device, cmd, ui->pipeline);
    r_cmd_bind_vertex_buffer(ui->device, cmd, frame.vertices, 0);
    r_cmd_bind_index_buffer(ui->device, cmd, frame.indices, 0, sizeof(ImDrawIdx) == 2);
    r_cmd_set_viewport(ui->device, cmd, { 0.0f, 0.0f, (f32)width, (f32)height });

    const b8      srgb = ui->target_format == R_Format::R8G8B8A8_SRGB || ui->target_format == R_Format::B8G8R8A8_SRGB;
    const UI_Push push = {
        .scale       = { 2.0f / draw_data->DisplaySize.x, 2.0f / draw_data->DisplaySize.y },
        .translate   = { -1.0f - draw_data->DisplayPos.x * (2.0f / draw_data->DisplaySize.x), -1.0f - draw_data->DisplayPos.y * (2.0f / draw_data->DisplaySize.y) },
        .srgb_target = srgb ? 1u : 0u,
    };
    r_cmd_push_constants(ui->device, cmd, ui->pipeline, &push, sizeof(push));

    i32 vertex_offset = 0;
    u32 index_offset  = 0;
    for (i32 n = 0; n < draw_data->CmdLists.Size; n++)
    {
        const ImDrawList* list = draw_data->CmdLists[n];
        for (i32 c = 0; c < list->CmdBuffer.Size; c++)
        {
            const ImDrawCmd& draw = list->CmdBuffer[c];
            if (draw.UserCallback)
            {
                draw.UserCallback(list, &draw);
                continue;
            }
            // Clip rects are in points, the target in pixels
            const ImVec2 scale = draw_data->FramebufferScale;
            const i32    x0    = zm::max((i32)((draw.ClipRect.x - draw_data->DisplayPos.x) * scale.x), 0);
            const i32    y0    = zm::max((i32)((draw.ClipRect.y - draw_data->DisplayPos.y) * scale.y), 0);
            const i32    x1    = zm::min((i32)((draw.ClipRect.z - draw_data->DisplayPos.x) * scale.x), (i32)width);
            const i32    y1    = zm::min((i32)((draw.ClipRect.w - draw_data->DisplayPos.y) * scale.y), (i32)height);
            if (x1 <= x0 || y1 <= y0)
            {
                continue;
            }
            r_cmd_set_scissor(ui->device, cmd, { x0, y0, (u32)(x1 - x0), (u32)(y1 - y0) });
            r_cmd_bind_texture(ui->device, cmd, ui->pipeline, R_ImageHandle((u64)draw.GetTexID()), R_Filter::Linear);
            r_cmd_draw_indexed(ui->device, cmd, draw.ElemCount, index_offset + draw.IdxOffset, vertex_offset + (i32)draw.VtxOffset);
        }
        vertex_offset += list->VtxBuffer.Size;
        index_offset += (u32)list->IdxBuffer.Size;
    }
}

//
// Platform: ImGui's input, cursor, clipboard and text input over the platform layer
//

static ImGuiKey imgui_key (OS_Key key)
{
    if (key >= OS_Key::A && key <= OS_Key::Z)
    {
        return (ImGuiKey)(ImGuiKey_A + ((i32)key - (i32)OS_Key::A));
    }
    if (key >= OS_Key::Digit0 && key <= OS_Key::Digit9)
    {
        return (ImGuiKey)(ImGuiKey_0 + ((i32)key - (i32)OS_Key::Digit0));
    }
    if (key >= OS_Key::F1 && key <= OS_Key::F12)
    {
        return (ImGuiKey)(ImGuiKey_F1 + ((i32)key - (i32)OS_Key::F1));
    }
    if (key >= OS_Key::F13 && key <= OS_Key::F24)
    {
        return (ImGuiKey)(ImGuiKey_F13 + ((i32)key - (i32)OS_Key::F13));
    }
    if (key >= OS_Key::Keypad0 && key <= OS_Key::Keypad9)
    {
        return (ImGuiKey)(ImGuiKey_Keypad0 + ((i32)key - (i32)OS_Key::Keypad0));
    }
    switch (key)
    {
    case OS_Key::Space:
        return ImGuiKey_Space;
    case OS_Key::Escape:
        return ImGuiKey_Escape;
    case OS_Key::Enter:
        return ImGuiKey_Enter;
    case OS_Key::Tab:
        return ImGuiKey_Tab;
    case OS_Key::Backspace:
        return ImGuiKey_Backspace;
    case OS_Key::ArrowLeft:
        return ImGuiKey_LeftArrow;
    case OS_Key::ArrowRight:
        return ImGuiKey_RightArrow;
    case OS_Key::ArrowUp:
        return ImGuiKey_UpArrow;
    case OS_Key::ArrowDown:
        return ImGuiKey_DownArrow;
    case OS_Key::LeftShift:
        return ImGuiKey_LeftShift;
    case OS_Key::LeftCtrl:
        return ImGuiKey_LeftCtrl;
    case OS_Key::LeftAlt:
        return ImGuiKey_LeftAlt;
    case OS_Key::RightShift:
        return ImGuiKey_RightShift;
    case OS_Key::RightCtrl:
        return ImGuiKey_RightCtrl;
    case OS_Key::RightAlt:
        return ImGuiKey_RightAlt;
    case OS_Key::LeftSuper:
        return ImGuiKey_LeftSuper;
    case OS_Key::RightSuper:
        return ImGuiKey_RightSuper;
    case OS_Key::Grave:
        return ImGuiKey_GraveAccent;
    case OS_Key::LeftBracket:
        return ImGuiKey_LeftBracket;
    case OS_Key::RightBracket:
        return ImGuiKey_RightBracket;
    case OS_Key::Insert:
        return ImGuiKey_Insert;
    case OS_Key::Delete:
        return ImGuiKey_Delete;
    case OS_Key::Home:
        return ImGuiKey_Home;
    case OS_Key::End:
        return ImGuiKey_End;
    case OS_Key::PageUp:
        return ImGuiKey_PageUp;
    case OS_Key::PageDown:
        return ImGuiKey_PageDown;
    case OS_Key::Minus:
        return ImGuiKey_Minus;
    case OS_Key::Equals:
        return ImGuiKey_Equal;
    case OS_Key::Backslash:
        return ImGuiKey_Backslash;
    case OS_Key::Semicolon:
        return ImGuiKey_Semicolon;
    case OS_Key::Apostrophe:
        return ImGuiKey_Apostrophe;
    case OS_Key::Comma:
        return ImGuiKey_Comma;
    case OS_Key::Period:
        return ImGuiKey_Period;
    case OS_Key::Slash:
        return ImGuiKey_Slash;
    case OS_Key::NonUsBackslash:
        return ImGuiKey_Oem102;
    case OS_Key::CapsLock:
        return ImGuiKey_CapsLock;
    case OS_Key::ScrollLock:
        return ImGuiKey_ScrollLock;
    case OS_Key::NumLock:
        return ImGuiKey_NumLock;
    case OS_Key::PrintScreen:
        return ImGuiKey_PrintScreen;
    case OS_Key::Pause:
        return ImGuiKey_Pause;
    case OS_Key::Menu:
        return ImGuiKey_Menu;
    case OS_Key::KeypadDecimal:
        return ImGuiKey_KeypadDecimal;
    case OS_Key::KeypadDivide:
        return ImGuiKey_KeypadDivide;
    case OS_Key::KeypadMultiply:
        return ImGuiKey_KeypadMultiply;
    case OS_Key::KeypadMinus:
        return ImGuiKey_KeypadSubtract;
    case OS_Key::KeypadPlus:
        return ImGuiKey_KeypadAdd;
    case OS_Key::KeypadEnter:
        return ImGuiKey_KeypadEnter;
    case OS_Key::KeypadEquals:
        return ImGuiKey_KeypadEqual;
    default:
        return ImGuiKey_None;
    }
}

static i32 imgui_mouse_button (OS_Key key)
{
    switch (key)
    {
    case OS_Key::MouseLeft:
        return ImGuiMouseButton_Left;
    case OS_Key::MouseRight:
        return ImGuiMouseButton_Right;
    case OS_Key::MouseMiddle:
        return ImGuiMouseButton_Middle;
    case OS_Key::MouseX1:
        return 3;
    case OS_Key::MouseX2:
        return 4;
    default:
        return -1;
    }
}

static const char* get_clipboard (ImGuiContext* context)
{
    UI_Context* ui = (UI_Context*)ImGui::GetPlatformIO(context).Platform_ClipboardUserData;
    return os_clipboard_text(ui->window, ui->clipboard, sizeof(ui->clipboard)) ? ui->clipboard : nullptr;
}

static void set_clipboard (ImGuiContext* context, const char* text)
{
    const UI_Context* ui = (const UI_Context*)ImGui::GetPlatformIO(context).Platform_ClipboardUserData;
    os_set_clipboard_text(ui->window, text);
}

static void set_ime_data (ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data) // NOLINT(misc-const-correctness): ImGui's Platform_SetImeDataFn
{
    (void)viewport;
    const UI_Context* ui = (const UI_Context*)ImGui::GetPlatformIO(context).Platform_ImeUserData;
    os_window_text_input(ui->window, data->WantVisible || data->WantTextInput, data->InputPos.x, data->InputPos.y, data->InputLineHeight);
}

static void init_platform (UI_Context* ui)
{
    ImGuiIO& io            = ImGui::GetIO();
    io.BackendPlatformName = "puppetxr";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    ImGuiPlatformIO& platform           = ImGui::GetPlatformIO();
    platform.Platform_GetClipboardTextFn = get_clipboard;
    platform.Platform_SetClipboardTextFn = set_clipboard;
    platform.Platform_ClipboardUserData  = ui;
    platform.Platform_SetImeDataFn       = set_ime_data;
    platform.Platform_ImeUserData        = ui;
    ui->last_frame_ns                    = os_monotonic_ns();
}

// Window thread, before ImGui::NewFrame
static void update_platform (UI_Context* ui)
{
    ImGuiIO&  io      = ImGui::GetIO();
    u32       width   = 0;
    u32       height  = 0;
    const f32 density = os_window_display_density(ui->window);
    os_window_size(ui->window, width, height);
    io.DisplaySize             = ImVec2((f32)width / density, (f32)height / density);
    io.DisplayFramebufferScale = ImVec2(density, density);
    const u64 now              = os_monotonic_ns();
    io.DeltaTime               = now > ui->last_frame_ns ? (f32)((f64)(now - ui->last_frame_ns) * 1e-9) : 1.0f / 60.0f;
    ui->last_frame_ns          = now;
    if (!(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange))
    {
        const ImGuiMouseCursor cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
        os_window_set_cursor(ui->window, cursor == ImGuiMouseCursor_None ? OS_Cursor::Hidden : (OS_Cursor)cursor);
    }
}

static void build_frame (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    const BuildFrameCall& call = *(const BuildFrameCall*)user;
    update_platform(call.ui);
    // The cursor itself, as the window may get no moves (os_window_cursor)
    f32 cursor_x = 0.0f;
    f32 cursor_y = 0.0f;
    if (!call.ui->captured && os_window_cursor(call.ui->window, cursor_x, cursor_y))
    {
        ImGui::GetIO().AddMousePosEvent(cursor_x, cursor_y);
    }
    ImGui::NewFrame();
    ui_probing                                 = call.ui->probe ? call.ui : nullptr;
    call.ui->probe_count                       = 0;
    ImGui::GetCurrentContext()->TestEngineHookItems = call.ui->probe;
    if (call.show_panels)
    {
        build_panels(call.ui, call.sim, *call.requests, *call.mirror_view);
    }
    if (call.ui->status.show_welcome)
    {
        welcome_overlay(call.ui, sim_get_config(call.sim), sim_get_profile(call.sim));
    }
    body_overlay(call.ui, call.sim, *call.mirror_view);
    status_badge(call.ui);
    profile_toast(call.ui, call.sim);
    ImGui::Render();
    if (call.ui->probe)
    {
        memory_copy(call.ui->probed_items, call.ui->probe_items, sizeof(UI_ProbeItem) * call.ui->probe_count);
        call.ui->probed_count   = call.ui->probe_count;
        call.ui->probed_hovered = ImGui::GetCurrentContext()->HoveredId;
        call.ui->probed_mouse   = ImGui::GetIO().MousePos;
    }
    ui_probing = nullptr;
}

//
// ImGui's test engine hooks (IMGUI_ENABLE_TEST_ENGINE): ImGui calls them for every item while
// TestEngineHookItems is set, which only probing does
//

// Items without an area cannot be clicked; tabs add such before their own
void ImGuiTestEngineHook_ItemAdd (ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*)
{
    if (!ui_probing || ui_probing->probe_count == UI_MAX_PROBE_ITEMS || bb.GetArea() <= 0.0f)
    {
        return;
    }
    UI_ProbeItem& item = ui_probing->probe_items[ui_probing->probe_count++];
    item = { .id = id, .label = {}, .rect = bb };
    if (ui_probing->probe_pending.id == id)
    {
        cstr_copy(item.label, sizeof(item.label), ui_probing->probe_pending.label);
        ui_probing->probe_pending = {};
    }
}

void ImGuiTestEngineHook_ItemInfo (ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
    if (!ui_probing || !label)
    {
        return;
    }
    const char* hidden = cstr_find(label, "##");
    const usize length = hidden ? (usize)(hidden - label) : cstr_length(label);
    for (u32 i = ui_probing->probe_count; i > 0; i--)
    {
        UI_ProbeItem& item = ui_probing->probe_items[i - 1];
        if (item.id == id)
        {
            cstr_copy(item.label, zm::min(length + 1, sizeof(item.label)), label);
            return;
        }
    }
    ui_probing->probe_pending.id = id;
    cstr_copy(ui_probing->probe_pending.label, zm::min(length + 1, sizeof(ui_probing->probe_pending.label)), label);
}

void ImGuiTestEngineHook_Log (ImGuiContext*, const char*, ...)
{
}

const char* ImGuiTestEngine_FindItemDebugLabel (ImGuiContext*, ImGuiID)
{
    return nullptr;
}

//
// Public API
//

UI_Context* ui_create (const UI_CreateInfo& info)
{
    if (!info.device || !info.window)
    {
        LOG_ERROR("UI needs a device and a window");
        return nullptr;
    }

    UI_Context* ui    = (UI_Context*)os_memory_alloc(sizeof(UI_Context));
    ui->device         = info.device;
    ui->window         = info.window;
    ui->target_format  = info.target_format;
    ui->show_simulator = true;
    ui->body_link      = true;
    ui->body_joint     = (u32)SIM_BodyJoint::Neck;
    ui->show_devices   = true;
    ui->show_log       = true;
    ui->show_props     = true;
    ui->devices_profile = UINT32_MAX;
    ui->toast_profile   = UINT32_MAX;
    ui->log_show_sim   = true;
    ui->log_show_app   = true;

    b8 success = false;
    defer {
        if (!success)
        {
            ui_destroy(ui);
        }
    };

    IMGUI_CHECKVERSION();
    ui->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui->imgui);
    ui->implot = ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().Fonts->AddFontDefaultVector();
    apply_style();
    init_platform(ui);

    ui->render_pass = r_create_render_pass(ui->device, { .color_format = info.target_format, .load_op = R_LoadOp::Load, .initial_layout = R_ImageLayout::TransferDst, .final_layout = R_ImageLayout::Present });
    ui->pipeline    = r_create_pipeline(ui->device, {
        .vertex                  = &imgui_vert_shader,
        .fragment                = &imgui_frag_shader,
        .render_pass             = ui->render_pass,
        .textures_count          = 1,
        .texture_type            = R_TextureType::Type2D,
        .push_constants_size     = sizeof(UI_Push),
        .blend_enable            = true,
        .vertex_stride           = sizeof(ImDrawVert),
        .vertex_attributes       = { { 0, R_Format::R32G32_SFLOAT, (u32)offsetof(ImDrawVert, pos) }, { 1, R_Format::R32G32_SFLOAT, (u32)offsetof(ImDrawVert, uv) }, { 2, R_Format::R8G8B8A8_UNORM, (u32)offsetof(ImDrawVert, col) } },
        .vertex_attributes_count = 3,
    });
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    if (!ui->render_pass || !ui->pipeline)
    {
        LOG_ERROR("Failed to create UI render resources");
        return nullptr;
    }
    for (u32 i = 0; i < R_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!ensure_buffers(ui, ui->frames[i], UI_INITIAL_VERTEX_BYTES, UI_INITIAL_INDEX_BYTES))
        {
            return nullptr;
        }
    }

    success = true;
    return ui;
}

void ui_destroy (UI_Context* ui)
{
    if (!ui)
    {
        return;
    }
    r_wait_idle(ui->device);
    for (u32 i = 0; i < R_MAX_FRAMES_IN_FLIGHT; i++)
    {
        r_destroy_buffer(ui->device, ui->frames[i].vertices);
        r_destroy_buffer(ui->device, ui->frames[i].indices);
    }
    if (ui->imgui)
    {
        ImGui::SetCurrentContext(ui->imgui);
        destroy_textures(ui);
    }
    ui_release_targets(ui);
    r_destroy_pipeline(ui->device, ui->pipeline);
    r_destroy_render_pass(ui->device, ui->render_pass);
    if (ui->imgui)
    {
        ImGui::SetCurrentContext(ui->imgui);
        if (ui->implot)
        {
            ImPlot::DestroyContext(ui->implot);
        }
        ImGui::DestroyContext(ui->imgui);
    }
    os_memory_release(ui);
}

void ui_handle_raw_event (const UI_Context* ui, const OS_RawEvent& event)
{
    ASSERT(ui);
    if (event.window != os_window_id(ui->window))
    {
        return;
    }
    ImGui::SetCurrentContext(ui->imgui);
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type)
    {
    case OS_RawEventType::KeyDown:
    case OS_RawEventType::KeyUp:
    {
        const b8 down = event.type == OS_RawEventType::KeyDown;
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.modifiers & OS_MODIFIER_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.modifiers & OS_MODIFIER_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.modifiers & OS_MODIFIER_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (event.modifiers & OS_MODIFIER_SUPER) != 0);
        const ImGuiKey key = imgui_key(event.key);
        if (key != ImGuiKey_None)
        {
            io.AddKeyEvent(key, down);
        }
        break;
    }
    case OS_RawEventType::Text:
        io.AddInputCharactersUTF8(event.text);
        break;
    case OS_RawEventType::MouseMotion:
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(event.x, event.y);
        break;
    case OS_RawEventType::MouseButtonDown:
    case OS_RawEventType::MouseButtonUp:
    {
        const i32 button = imgui_mouse_button(event.key);
        if (button >= 0)
        {
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(button, event.type == OS_RawEventType::MouseButtonDown);
        }
        break;
    }
    case OS_RawEventType::MouseWheel:
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMouseWheelEvent(-event.dx, event.dy);
        break;
    case OS_RawEventType::FocusGained:
    case OS_RawEventType::FocusLost:
        io.AddFocusEvent(event.type == OS_RawEventType::FocusGained);
        break;
    default:
        break;
    }
}

// A click outside an open popup closes it
b8 ui_wants_click (const UI_Context* ui, f32 x, f32 y)
{
    ASSERT(ui);
    const ImGuiContext& g = *ui->imgui;
    if (g.OpenPopupStack.Size > 0)
    {
        return true;
    }
    for (const ImGuiWindow* window : g.Windows)
    {
        if (window->Active && !window->Hidden && !(window->Flags & ImGuiWindowFlags_NoMouseInputs) && window->Rect().Contains(ImVec2(x, y)))
        {
            return true;
        }
    }
    return false;
}

void ui_set_captured (UI_Context* ui, b8 captured)
{
    ASSERT(ui);
    ImGui::SetCurrentContext(ui->imgui);
    ImGuiIO& io  = ImGui::GetIO();
    ui->captured = captured;
    // The click that took the mouse is queued as a press whose release the panels never get; a
    // button left down swallowed the first click after Escape
    if (captured)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
        io.ClearEventsQueue();
        io.ClearInputMouse();
        io.ClearInputKeys();
        for (i32 button = 0; button < ImGuiMouseButton_COUNT; button++)
        {
            io.AddMouseButtonEvent(button, false);
        }
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    else
    {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    }
}

b8 ui_wants_mouse (const UI_Context* ui)
{
    ASSERT(ui);
    ImGui::SetCurrentContext(ui->imgui);
    return ImGui::GetIO().WantCaptureMouse;
}

b8 ui_wants_keyboard (const UI_Context* ui)
{
    ASSERT(ui);
    ImGui::SetCurrentContext(ui->imgui);
    return ImGui::GetIO().WantCaptureKeyboard;
}

UI_FrameStats ui_frame_stats (const UI_FrameTimes& times)
{
    UI_FrameStats stats = {};
    f32           total = 0.0f;
    f32           puppetxr = 0.0f;
    u32           count = 0;
    while (count < times.count && total < 1000.0f)
    {
        const u32 i = (times.next + UI_FRAME_TIMES - 1 - count) % UI_FRAME_TIMES;
        total        += times.frame_ms[i];
        puppetxr     += times.puppetxr_ms[i];
        stats.max_ms  = zm::max(stats.max_ms, times.frame_ms[i]);
        count++;
    }
    if (count > 0 && total > 0.0f)
    {
        stats.mean_ms       = total / (f32)count;
        stats.fps           = (f32)count * 1000.0f / total;
        stats.puppetxr_mean_ms = puppetxr / (f32)count;
    }
    return stats;
}

UI_Requests ui_build (UI_Context* ui, SIM_Sim* sim, b8 show_panels, SIM_MirrorView& mirror_view, const UI_Status& status)
{
    ASSERT(ui && sim);
    UI_Requests requests = {};
    ImGui::SetCurrentContext(ui->imgui);
    ImPlot::SetCurrentContext(ui->implot);
    ui->status = status;
    BuildFrameCall frame = { .ui = ui, .sim = sim, .show_panels = show_panels, .mirror_view = &mirror_view, .requests = &requests };
    os_run_on_window_thread(build_frame, &frame);
    return requests;
}

void ui_render (UI_Context* ui, R_CmdBufferHandle cmd, R_ImageHandle target)
{
    ASSERT(ui);
    u32 width = 0;
    u32 height = 0;
    u32 layers = 0;
    r_get_image_size(ui->device, target, width, height, layers);
    if (width == 0)
    {
        return;
    }
    ImGui::SetCurrentContext(ui->imgui);
    const ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        update_textures(ui, draw_data->Textures);
    }
    // The pass ends the target in Present layout, with no frame built yet too
    const R_FramebufferHandle framebuffer = target_framebuffer(ui, target);
    r_cmd_begin_render_pass(ui->device, cmd, ui->render_pass, framebuffer, {});
    if (draw_data)
    {
        record_draw_data(ui, cmd, draw_data, width, height);
    }
    r_cmd_end_render_pass(ui->device, cmd);
}

UI_Requests ui_draw (UI_Context* ui, SIM_Sim* sim, R_CmdBufferHandle cmd, R_ImageHandle target, b8 show_panels, SIM_MirrorView& mirror_view, const UI_Status& status)
{
    ASSERT(ui && sim);
    u32 width = 0;
    u32 height = 0;
    u32 layers = 0;
    r_get_image_size(ui->device, target, width, height, layers);
    if (width == 0)
    {
        return {};
    }
    const UI_Requests requests = ui_build(ui, sim, show_panels, mirror_view, status);
    ui_render(ui, cmd, target);
    return requests;
}

void ui_release_targets (UI_Context* ui)
{
    ASSERT(ui);
    for (u32 i = 0; i < UI_MAX_TARGETS; i++)
    {
        r_destroy_framebuffer(ui->device, ui->target_framebuffers[i]);
        ui->target_images[i]       = {};
        ui->target_framebuffers[i] = {};
    }
}

void ui_set_probe (UI_Context* ui, b8 enabled)
{
    ASSERT(ui);
    ui->probe        = enabled;
    ui->probed_count = 0;
}

void ui_probed_mouse (const UI_Context* ui, f32& out_x, f32& out_y)
{
    ASSERT(ui);
    out_x = ui->probed_mouse.x;
    out_y = ui->probed_mouse.y;
}

b8 ui_find_item_rect (const UI_Context* ui, const char* label, f32& out_x0, f32& out_y0, f32& out_x1, f32& out_y1)
{
    ASSERT(ui && label);
    for (u32 i = 0; i < ui->probed_count; i++)
    {
        const UI_ProbeItem& item = ui->probed_items[i];
        if (cstr_equal(item.label, label))
        {
            out_x0 = item.rect.Min.x;
            out_y0 = item.rect.Min.y;
            out_x1 = item.rect.Max.x;
            out_y1 = item.rect.Max.y;
            return true;
        }
    }
    return false;
}

b8 ui_find_item (const UI_Context* ui, const char* label, f32& out_x, f32& out_y, b8* out_hovered)
{
    ASSERT(ui && label);
    for (u32 i = 0; i < ui->probed_count; i++)
    {
        const UI_ProbeItem& item = ui->probed_items[i];
        if (cstr_equal(item.label, label))
        {
            out_x = (item.rect.Min.x + item.rect.Max.x) * 0.5f;
            out_y = (item.rect.Min.y + item.rect.Max.y) * 0.5f;
            if (out_hovered)
            {
                *out_hovered = item.id == ui->probed_hovered;
            }
            return true;
        }
    }
    return false;
}

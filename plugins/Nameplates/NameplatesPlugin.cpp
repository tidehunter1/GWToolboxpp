// NameplatesPlugin.cpp
//
// A GWToolbox++ plugin that draws a floating health bar ("nameplate") above
// every living agent in view, similar to MMO-style always-on nameplates,
// rather than only showing a bar for the hovered/targeted unit.
//
// This version is based on reading the actual GWToolboxpp 8.31 source
// (uploaded by the user):
//   - GWToolboxdll/Widgets/HealthWidget.cpp     -> confirms hp is a 0..1
//     float fraction, and GW::Agents::GetTargetAsAgentLiving() is real.
//   - GWToolboxdll/Utils/GameWorldCompositor.cpp -> confirms the EXACT
//     view/projection matrices GW's own 3D world camera uses (no
//     GW::Camera::WorldToScreen exists - toolbox rebuilds the matrices
//     itself every frame from GW::CameraMgr::GetCamera() +
//     GW::Render::GetFieldOfView()).
//   - plugins/Base/ToolboxPlugin.h, ToolboxUIPlugin.h/.cpp,
//     plugins/ExamplePlugin/ExamplePlugin.cpp,
//     cmake/gwtoolboxdll_plugins.cmake -> corrected THREE wrong
//     assumptions from earlier drafts:
//       1. The real entry point is `ToolboxPluginInstance()`, not
//          `GetPluginInstance()`.
//       2. There are no more "Clock"/"InstanceTimer" example plugins in
//          this release - the current example is `plugins/ExamplePlugin/`,
//          and it inherits plain `ToolboxPlugin`, not `ToolboxUIPlugin`.
//          `ToolboxUIPlugin` adds window position/size persistence and a
//          "/tb <name> show/hide/toggle" chat command intended for a single
//          bordered panel - overhead is unhelpful for a screen-wide overlay
//          drawing many bars, so this plugin uses plain `ToolboxPlugin`
//          instead, matching the example.
//       3. There's no per-plugin CMakeLists.txt to write. Plugins are wired
//          up centrally: drop a folder under plugins/<Name>/ with your
//          .h/.cpp, then add one line, `add_tb_plugin(<Name>)`, to
//          cmake/gwtoolboxdll_plugins.cmake. The macro globs your folder's
//          .h/.cpp files and links against the shared `plugin_base`
//          interface library (which already provides DllMain, base
//          classes, and a pre-initialized link to the shared gwca.dll -
//          no manual GW::Initialize() call needed in the plugin itself).
//
// BUILD FIX (from a real CI compile attempt): GWCA's own headers
// (Array.h, GamePos.h, Agent.h, etc.) use uint32_t/uintptr_t/etc. but never
// include <cstdint> themselves - they rely on whatever includes them first
// having already brought those types in. The main GWToolboxdll project gets
// this for free via its precompiled header; a standalone plugin does not.
// <cstdint> MUST be included before any GWCA header, or the compiler fails
// with a confusing cascade of "undeclared identifier"/"syntax error" spam
// inside Array.h/GamePos.h/Item.h that looks unrelated but really traces
// back to this one missing include.

// BUILD FIX #2 (from the clang-cl CI run): the <cstdint> fix above resolved
// the Array.h/GamePos.h cascade, but exposed a second, smaller batch of the
// exact same underlying issue - GWCA headers use DWORD/HWND/HMODULE (from
// <Windows.h>), memcmp (<cstring>), atan2 (<cmath>), _countof (a Windows/CRT
// macro), and std::function (<functional>), again without including the
// headers that declare them themselves. All of these are provided for free
// in the main project via its precompiled header; a standalone plugin needs
// them included explicitly, before any GWCA header.

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <GWCA/GWCA.h>
#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <ToolboxPlugin.h>
#include <imgui.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cwchar>
#include <optional>

// FEATURE UPDATE: bars now anchor to Agent::name_tag_x/y/z (the same point
// GW's own native name tag uses for that specific agent) instead of
// pos.x/y/z + a fixed head_offset_z. This makes bar height scale correctly
// per-creature automatically (a giant vs. a small NPC) rather than every
// agent using the same arbitrary offset. head_offset_z is kept as a small
// optional fine-tune layered on top, not the primary height source anymore.

// FEATURE UPDATE #2: priority name-list coloring. The user types
// semicolon-separated unit names into three text boxes (Priority 1/2/3);
// any agent whose decoded name matches an entry gets that tier's fixed
// color instead of its normal allegiance-based color.
//
// IMPORTANT: GW does not store agent names as plain text - names are
// "encoded" (GW::Agents::GetAgentEncName() returns an encoded wchar_t*),
// and must be asynchronously decoded via GW::UI::AsyncDecodeStr() before
// they're human-readable. This mirrors the exact pattern used by
// GWToolboxdll/Windows/SkillListingWindow.cpp's Skill::Name() (call
// AsyncDecodeStr ONCE into a persistent buffer, cache it, and just re-read
// the buffer on subsequent frames rather than re-issuing the decode call
// every frame - the buffer starts empty and gets filled in a later frame
// once GW's internal decode completes).
//
// Deliberately NOT using GWToolboxdll's own GuiUtils::EncString helper
// (used by GWToolboxdll/Windows/EnemyWindow.cpp) for this, even though it's
// a cleaner API - the plugin_base CMake target only links GWToolboxdll
// "for GetFont" per its own comment, meaning only a narrow, deliberately
// exported symbol set is guaranteed available to plugins. EncString's
// methods aren't confirmed to be in that exported set, so depending on it
// risks an unresolved-external-symbol link error. GW::UI::AsyncDecodeStr,
// by contrast, is GWCA_API - confirmed genuinely exported from gwca.dll,
// which every plugin already links against - so this plugin implements its
// own small, self-contained decode cache on top of that safer foundation
// instead.


// ---------------------------------------------------------------------------
// Decodes and caches agent display names, one entry per agent_id. See the
// FEATURE UPDATE #2 comment above for why this is hand-rolled on GWCA's
// exported GW::UI::AsyncDecodeStr rather than depending on GWToolboxdll's
// internal GuiUtils::EncString helper.
// ---------------------------------------------------------------------------
class AgentNameCache {
public:
    // Returns the best currently-known decoded name for this agent as a
    // lowercased std::wstring (ready for case-insensitive comparison). May
    // be empty for the first frame or two after an agent is first seen,
    // since decoding happens asynchronously - callers should treat "not
    // matched yet" as normal, not an error.
    const std::wstring& GetLower(uint32_t agent_id, const wchar_t* enc_name) {
        Entry& entry = cache_[agent_id];
        if (enc_name && wcsncmp(entry.last_enc, enc_name, kMaxEncLen - 1) != 0) {
            wcsncpy_s(entry.last_enc, enc_name, kMaxEncLen - 1);
            entry.buffer[0] = L'\0';
            GW::UI::AsyncDecodeStr(enc_name, entry.buffer, kBufferLen);
        }
        if (entry.buffer[0] != L'\0') {
            entry.decoded_lower = entry.buffer;
            for (auto& c : entry.decoded_lower) {
                c = static_cast<wchar_t>(towlower(c));
            }
        }
        return entry.decoded_lower;
    }

private:
    static constexpr size_t kBufferLen = 256;
    static constexpr size_t kMaxEncLen = 64;
    struct Entry {
        wchar_t last_enc[kMaxEncLen] = {};
        wchar_t buffer[kBufferLen] = {};
        std::wstring decoded_lower;
    };
    std::unordered_map<uint32_t, Entry> cache_;
};

// Converts UTF-8 (what ImGui::InputText buffers hold) to a wide string,
// via the real Win32 conversion API rather than a naive per-char cast, so
// non-ASCII characters in a typed name aren't silently mangled.
inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

// Splits "Angry Hog; Angry Bat" into ["angry hog", "angry bat"] - trimmed
// and lowercased, ready for direct comparison against AgentNameCache's
// already-lowercased decoded names.
inline std::vector<std::wstring> ParseSemicolonNameList(const std::string& raw) {
    std::vector<std::wstring> out;
    std::string current;
    auto flush = [&]() {
        // trim whitespace
        size_t start = current.find_first_not_of(" \t\r\n");
        size_t end = current.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) {
            std::wstring w = Utf8ToWide(current.substr(start, end - start + 1));
            for (auto& c : w) c = static_cast<wchar_t>(towlower(c));
            if (!w.empty()) out.push_back(std::move(w));
        }
        current.clear();
    };
    for (char c : raw) {
        if (c == ';') {
            flush();
        }
        else {
            current += c;
        }
    }
    flush();
    return out;
}

// ---------------------------------------------------------------------------
// Settings - persisted the same way other toolbox widgets do (Settings::...).
// Wire these into your Settings/Draw functions to save/load from the ini/json
// config the same way an existing widget in the repo does.
// ---------------------------------------------------------------------------
struct NameplateSettings {
    bool enabled = true;
    bool show_enemies = true;
    bool show_allies = true;
    bool show_neutrals = false;
    bool hide_own_bar = true;     // don't draw one over your own head
    bool hide_dead = true;
    float max_range = 5000.0f;    // in game units; tune to taste
    float bar_width = 40.0f;
    float bar_height = 5.0f;
    float head_offset_z = 0.0f;   // small additive fine-tune on top of the agent's native name-tag anchor (see WorldToScreen)

    // Priority name-list coloring - raw semicolon-separated text as typed.
    std::string priority1_raw;
    std::string priority2_raw;
    std::string priority3_raw;
};

class NameplatesPlugin : public ToolboxPlugin {
public:
    const char* Name() const override { return "Nameplates"; }

    // CRITICAL: PluginModule::Draw() (GWToolboxdll/Modules/PluginModule.cpp)
    // only calls a plugin's Draw() if GetVisiblePtr() returns a non-null
    // pointer to a `true` bool - base ToolboxPlugin::GetVisiblePtr() returns
    // nullptr by default, which silently means Draw() is NEVER called.
    // DrawSettings() has no such gate (it's called unconditionally whenever
    // the panel is expanded), which is why the settings UI worked fine while
    // the actual nameplate overlay never rendered. This override is what
    // makes the overlay actually run every frame.
    bool* GetVisiblePtr() override { return &visible_; }

    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override; // declared below, uses the pattern DrawSettingsInternal below expects

    void LoadSettings(const wchar_t* folder) override {
        ToolboxPlugin::LoadSettings(folder);
        LoadSetting("visible", visible_);
        LoadSetting("enabled", settings_.enabled);
        LoadSetting("show_enemies", settings_.show_enemies);
        LoadSetting("show_allies", settings_.show_allies);
        LoadSetting("show_neutrals", settings_.show_neutrals);
        LoadSetting("hide_own_bar", settings_.hide_own_bar);
        LoadSetting("hide_dead", settings_.hide_dead);
        LoadSetting("max_range", settings_.max_range);
        LoadSetting("bar_width", settings_.bar_width);
        LoadSetting("bar_height", settings_.bar_height);
        LoadSetting("head_offset_z", settings_.head_offset_z);
        LoadSetting("priority1_raw", settings_.priority1_raw);
        LoadSetting("priority2_raw", settings_.priority2_raw);
        LoadSetting("priority3_raw", settings_.priority3_raw);
        RefreshPriorityBuffersAndLists();
    }

    void SaveSettings(const wchar_t* folder) override {
        SaveSetting("visible", visible_);
        SaveSetting("enabled", settings_.enabled);
        SaveSetting("show_enemies", settings_.show_enemies);
        SaveSetting("show_allies", settings_.show_allies);
        SaveSetting("show_neutrals", settings_.show_neutrals);
        SaveSetting("hide_own_bar", settings_.hide_own_bar);
        SaveSetting("hide_dead", settings_.hide_dead);
        SaveSetting("max_range", settings_.max_range);
        SaveSetting("bar_width", settings_.bar_width);
        SaveSetting("bar_height", settings_.bar_height);
        SaveSetting("head_offset_z", settings_.head_offset_z);
        SaveSetting("priority1_raw", settings_.priority1_raw);
        SaveSetting("priority2_raw", settings_.priority2_raw);
        SaveSetting("priority3_raw", settings_.priority3_raw);
        ToolboxPlugin::SaveSettings(folder);
    }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override {
        ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll); // sets ImGui context/allocators - confirmed in ToolboxPlugin.cpp
        // No manual GW::Initialize() needed: this plugin links against the
        // same shared gwca.dll the main toolbox process already initialized
        // (confirmed via cmake/gwtoolboxdll_plugins.cmake's plugin_base target).
    }

    void SignalTerminate() override {
        ToolboxPlugin::SignalTerminate();
        // no persistent game-state changes to revert - this plugin only reads state and draws ImGui
    }

    bool CanTerminate() override { return true; }

    void Draw(IDirect3DDevice9* /*device*/) override {
        if (!settings_.enabled) return;
        DrawNameplates();
    }

private:
    NameplateSettings settings_;
    bool visible_ = true; // backs GetVisiblePtr() - also drives the "Visible" checkbox PluginModule draws automatically next to Load/Unload

    AgentNameCache name_cache_;
    std::vector<std::wstring> priority1_names_, priority2_names_, priority3_names_; // parsed+lowercased, derived from settings_.priorityN_raw

    // Fixed-size buffers for ImGui::InputText. Not using the project's
    // ImGuiAddons.h InputText(std::string&) overload here since it's an
    // IMGUI_API-marked symbol from GWToolboxdll itself - same
    // not-confirmed-exported-to-plugins concern as GuiUtils::EncString
    // above, so this sticks to core ImGui's classic char-buffer API, which
    // is safely linked directly against the imgui static library.
    static constexpr size_t kPriorityBufSize = 512;
    char priority1_buf_[kPriorityBufSize] = {};
    char priority2_buf_[kPriorityBufSize] = {};
    char priority3_buf_[kPriorityBufSize] = {};

    // Colors are fixed per tier by design (not user-configurable), per the
    // feature request: priority 1 = light blue, 2 = pink, 3 = purple.
    static constexpr ImU32 kPriority1Color = IM_COL32(135, 206, 250, 255); // light blue
    static constexpr ImU32 kPriority2Color = IM_COL32(255, 105, 180, 255); // pink
    static constexpr ImU32 kPriority3Color = IM_COL32(147, 112, 219, 255); // purple

    // Copies settings_.priorityN_raw into the ImGui text buffers and
    // reparses the lowercased match lists. Called on load, and whenever the
    // user edits a box in DrawSettingsInternal().
    void RefreshPriorityBuffersAndLists() {
        strncpy_s(priority1_buf_, settings_.priority1_raw.c_str(), kPriorityBufSize - 1);
        strncpy_s(priority2_buf_, settings_.priority2_raw.c_str(), kPriorityBufSize - 1);
        strncpy_s(priority3_buf_, settings_.priority3_raw.c_str(), kPriorityBufSize - 1);
        priority1_names_ = ParseSemicolonNameList(settings_.priority1_raw);
        priority2_names_ = ParseSemicolonNameList(settings_.priority2_raw);
        priority3_names_ = ParseSemicolonNameList(settings_.priority3_raw);
    }

    // Returns the tier color if name_lower matches an entry in any priority
    // list (priority 1 checked first, so it wins over 2/3 if somehow listed
    // in more than one box), or std::nullopt if it matches none.
    std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower) const {
        if (name_lower.empty()) return std::nullopt; // not decoded yet - fall back to allegiance color for now
        for (const auto& n : priority1_names_) if (n == name_lower) return kPriority1Color;
        for (const auto& n : priority2_names_) if (n == name_lower) return kPriority2Color;
        for (const auto& n : priority3_names_) if (n == name_lower) return kPriority3Color;
        return std::nullopt;
    }

    void DrawNameplates() {
        GW::AgentArray* agents = GW::Agents::GetAgentArray(); // confirmed: AgentMgr.h -> AgentArray* GetAgentArray()
        if (!agents || !agents->valid()) return;

        // GetControlledCharacter() returns AgentLiving* directly - no cast needed.
        GW::AgentLiving* me = GW::Agents::GetControlledCharacter();

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        for (GW::Agent* agent : *agents) { // GW::Array<Agent*> supports range-for via begin()/end()
            if (!agent) continue;
            if (!agent->GetIsLivingType()) continue; // confirmed: Agent::GetIsLivingType()

            GW::AgentLiving* living = agent->GetAsAgentLiving(); // confirmed: Agent::GetAsAgentLiving()
            if (!living) continue;

            if (settings_.hide_dead && living->GetIsDead()) continue; // confirmed: AgentLiving::GetIsDead()
            if (settings_.hide_own_bar && me && living->agent_id == me->agent_id) continue;

            if (!ShouldShowAllegiance(living->allegiance)) continue; // confirmed enum: GW::Constants::Allegiance

            if (!WithinRange(living)) continue;

            ImVec2 screen;
            if (!WorldToScreen(living, screen)) continue; // off-screen or behind camera

            // Look up (and, if needed, kick off async decoding of) this
            // agent's display name, for priority-list color matching.
            const std::wstring& name_lower = name_cache_.GetLower(
                living->agent_id, GW::Agents::GetAgentEncName(living->agent_id));

            DrawBar(draw_list, screen, living, name_lower);
        }
    }

    // Confirmed real enum (Constants.h):
    //   Ally_NonAttackable = 0x1, Neutral = 0x2, Enemy = 0x3,
    //   Spirit_Pet = 0x4, Minion = 0x5, Npc_Minipet = 0x6
    // Note this is meaningfully different from generic "Ally/Enemy/Neutral" -
    // Npc_Minipet and Spirit_Pet are their own buckets, not folded into Ally.
    bool ShouldShowAllegiance(GW::Constants::Allegiance allegiance) const {
        switch (allegiance) {
            case GW::Constants::Allegiance::Enemy:
                return settings_.show_enemies;
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Spirit_Pet:
            case GW::Constants::Allegiance::Minion:
                return settings_.show_allies;
            case GW::Constants::Allegiance::Neutral:
            case GW::Constants::Allegiance::Npc_Minipet:
                return settings_.show_neutrals;
            default:
                return false;
        }
    }

    bool WithinRange(const GW::AgentLiving* living) const {
        GW::Agent* me = GW::Agents::GetControlledCharacter();
        if (!me) return true; // no cull possible, just show it
        const float dx = living->pos.x - me->pos.x; // confirmed: Agent::pos is GamePos {x, y, zplane}
        const float dy = living->pos.y - me->pos.y;
        const float dist_sq = dx * dx + dy * dy;
        return dist_sq <= (settings_.max_range * settings_.max_range);
    }

    // Anchors the bar to the SAME point GW's own native name tag uses for
    // this specific agent (Agent::name_tag_x/y/z), rather than pos.x/y + an
    // arbitrary fixed offset. Confirmed real fields (Agent.h comments):
    //   name_tag_x - "Exactly the same as X above"
    //   name_tag_y - "Exactly the same as Y above"
    //   name_tag_z - separate Z coord in float
    // Using these means the bar sits at a sensible height automatically
    // across different creature sizes (a giant vs. a small NPC), instead of
    // everyone getting the same fixed head_offset_z regardless of height.
    // head_offset_z is kept as a small additive fine-tune on top of the
    // native anchor, not the sole source of height anymore - default 0.
    bool WorldToScreen(const GW::AgentLiving* living, ImVec2& out) const {
        const auto cam = GW::CameraMgr::GetCamera();
        if (!cam) return false;

        using namespace DirectX;

        const XMVECTOR eye_pos    = XMVectorSet(cam->position.x, cam->position.y, cam->position.z, 0.f);
        const XMVECTOR look_at    = XMVectorSet(cam->look_at_target.x, cam->look_at_target.y, cam->look_at_target.z, 0.f);
        const XMVECTOR up         = XMVectorSet(0.f, 0.f, -1.f, 0.f); // confirmed: GW's world up is -Z, not +Y

        const XMMATRIX view = XMMatrixLookAtLH(eye_pos, look_at, up);

        const float fov = GW::Render::GetFieldOfView();
        const float aspect = static_cast<float>(GW::Render::GetViewportWidth()) /
                              static_cast<float>(GW::Render::GetViewportHeight());
        // confirmed constants from GameWorldCompositor.h: kZNear = 46.875f, kZFar = 48000.f
        constexpr float kZNear = 46.875f;
        constexpr float kZFar  = 48000.f;
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, kZNear, kZFar);

        const XMVECTOR world_pos = XMVectorSet(
            living->name_tag_x, living->name_tag_y, living->name_tag_z + settings_.head_offset_z, 0.f);

        // Check view-space Z first: if the point is behind the camera,
        // TransformCoord's perspective divide would silently mirror it to a
        // bogus on-screen location instead of correctly reporting failure.
        const XMVECTOR view_pos = XMVector3TransformCoord(world_pos, view);
        float view_pos_arr[4];
        XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(view_pos_arr), view_pos);
        if (view_pos_arr[2] <= kZNear) return false; // behind or too close to camera

        const XMMATRIX view_proj = view * proj;
        const XMVECTOR clip_pos = XMVector3TransformCoord(world_pos, view_proj); // divides by w internally

        float ndc[4];
        XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(ndc), clip_pos);
        // ndc.x/y are now in [-1, 1] if on-screen (not clamped - caller may want to skip extreme values)

        const auto vw = static_cast<float>(GW::Render::GetViewportWidth());
        const auto vh = static_cast<float>(GW::Render::GetViewportHeight());

        out.x = (ndc[0] * 0.5f + 0.5f) * vw;
        out.y = (1.f - (ndc[1] * 0.5f + 0.5f)) * vh; // Y flip: +Y world/NDC = up = smaller screen Y

        return true;
    }

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living, const std::wstring& name_lower) {
        float hp_pct = living->hp; // confirmed: 0.0-1.0 fraction (Agent.h comment + HealthWidget.cpp usage)
        hp_pct = hp_pct < 0.f ? 0.f : (hp_pct > 1.f ? 1.f : hp_pct);

        const ImVec2 top_left(screen.x - settings_.bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + settings_.bar_width, top_left.y + settings_.bar_height);
        const ImVec2 fill_bottom_right(top_left.x + settings_.bar_width * hp_pct, bottom_right.y);

        const ImU32 bg_color = IM_COL32(40, 40, 40, 200);
        // Priority name-list match takes precedence over the normal
        // allegiance-based color; falls back to ColorFor() if unmatched
        // (or not yet decoded this frame).
        const auto priority_color = GetPriorityColor(name_lower);
        const ImU32 fill_color = priority_color.value_or(ColorFor(living->allegiance));

        draw_list->AddRectFilled(top_left, bottom_right, bg_color);
        draw_list->AddRectFilled(top_left, fill_bottom_right, fill_color);
        draw_list->AddRect(top_left, bottom_right, IM_COL32(0, 0, 0, 180));
    }

    ImU32 ColorFor(GW::Constants::Allegiance allegiance) const {
        switch (allegiance) {
            case GW::Constants::Allegiance::Enemy:
                return IM_COL32(220, 40, 40, 255);
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Spirit_Pet:
            case GW::Constants::Allegiance::Minion:
                return IM_COL32(40, 200, 60, 255);
            default:
                return IM_COL32(200, 200, 60, 255);
        }
    }

    void DrawSettingsInternal() {
        ImGui::Checkbox("Enabled", &settings_.enabled);
        ImGui::Checkbox("Show enemies", &settings_.show_enemies);
        ImGui::Checkbox("Show allies", &settings_.show_allies);
        ImGui::Checkbox("Show neutrals", &settings_.show_neutrals);
        ImGui::Checkbox("Hide own bar", &settings_.hide_own_bar);
        ImGui::Checkbox("Hide dead", &settings_.hide_dead);
        ImGui::SliderFloat("Max range", &settings_.max_range, 500.f, 10000.f);
        ImGui::SliderFloat("Bar width", &settings_.bar_width, 10.f, 100.f);
        ImGui::SliderFloat("Bar height", &settings_.bar_height, 2.f, 20.f);
        ImGui::SliderFloat("Head offset (fine-tune)", &settings_.head_offset_z, -100.f, 100.f);

        ImGui::Separator();
        ImGui::TextUnformatted("Priority name coloring (semicolon-separated, e.g. \"Angry Hog; Angry Bat\")");

        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kPriority1Color).Value);
        const bool p1_changed = ImGui::InputText("Priority 1 (light blue)", priority1_buf_, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (p1_changed) {
            settings_.priority1_raw = priority1_buf_;
            priority1_names_ = ParseSemicolonNameList(settings_.priority1_raw);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kPriority2Color).Value);
        const bool p2_changed = ImGui::InputText("Priority 2 (pink)", priority2_buf_, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (p2_changed) {
            settings_.priority2_raw = priority2_buf_;
            priority2_names_ = ParseSemicolonNameList(settings_.priority2_raw);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kPriority3Color).Value);
        const bool p3_changed = ImGui::InputText("Priority 3 (purple)", priority3_buf_, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (p3_changed) {
            settings_.priority3_raw = priority3_buf_;
            priority3_names_ = ParseSemicolonNameList(settings_.priority3_raw);
        }
    }
};

// DrawSettings() is rendered by toolbox inside its own Settings > Plugins
// panel (confirmed: ToolboxPlugin::DrawSettings is called from there, per
// ToolboxPlugin.h's doc comment "Will be drawn in the Settings/Plugins
// menu"), so there's no need for this plugin to open its own ImGui window
// for settings - unlike my earlier draft which incorrectly built a
// standalone floating settings window.
void NameplatesPlugin::DrawSettings()
{
    ToolboxPlugin::DrawSettings();
    DrawSettingsInternal();
}

// ---------------------------------------------------------------------------
// Plugin entry point - confirmed against plugins/ExamplePlugin/ExamplePlugin.cpp
// and the DLLAPI macro/declaration in plugins/Base/ToolboxPlugin.h. The real
// function name is ToolboxPluginInstance(), NOT GetPluginInstance() (an
// earlier, incorrect guess).
// ---------------------------------------------------------------------------
DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static NameplatesPlugin instance;
    return &instance;
}

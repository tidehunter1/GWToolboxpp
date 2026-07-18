#include <cstdint>
#include <cstring>
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
#include <GWCA/Constants/AgentIDs.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/GameEntities/NPC.h>
#include <GWCA/Managers/GameThreadMgr.h>

#include <ToolboxPlugin.h>
#include <Utils/FontLoader.h>
#include <imgui.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cwchar>
#include <optional>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <array>
#include <utility>

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::string TruncateWithEllipsis(ImFont* font, float font_size, const std::wstring& name, const std::string& full_utf8, float max_width) {
    const ImVec2 full_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, full_utf8.c_str());
    if (full_size.x <= max_width) return full_utf8;

    size_t lo = 0, hi = name.size();
    std::string best_utf8 = WideToUtf8(L"...");
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        const std::string candidate_utf8 = WideToUtf8(name.substr(0, mid) + L"...");
        const ImVec2 candidate_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, candidate_utf8.c_str());
        if (candidate_size.x <= max_width) {
            lo = mid;
            best_utf8 = candidate_utf8;
        }
        else {
            hi = mid - 1;
        }
    }
    return best_utf8;
}

inline void DrawStatusTriangles(ImDrawList* draw_list, float right_x, float center_y, const GW::AgentLiving* living) {
    static constexpr ImU32 kEnchantedColor = IM_COL32(224, 253, 94, 255);
    static constexpr ImU32 kHexedColor = IM_COL32(253, 113, 255, 255);
    static constexpr ImU32 kConditionedColor = IM_COL32(160, 117, 85, 255);
    static constexpr float kTriHeight = 8.f;
    static constexpr float kTriWidth = kTriHeight * 1.3f;
    static constexpr float kTriSpacing = kTriWidth + 2.f;
    static constexpr ImU32 kOutlineColor = IM_COL32(0, 0, 0, 255);
    static constexpr float kOutlinePx = 1.0f;

    int count = 0;
    auto make_triangle = [&](float w, float h, float ox, float oy, bool upsidedown, ImVec2& p1, ImVec2& p2, ImVec2& p3) {
        if (upsidedown) {
            p1 = ImVec2(ox, oy);
            p2 = ImVec2(ox + w, oy);
            p3 = ImVec2(ox + w / 2.f, oy + h);
        }
        else {
            p1 = ImVec2(ox, oy + h);
            p2 = ImVec2(ox + w, oy + h);
            p3 = ImVec2(ox + w / 2.f, oy);
        }
    };

    auto draw_tri = [&](ImU32 color, bool upsidedown) {
        const float x = (right_x - count * kTriSpacing) - kTriWidth;
        const float y = center_y - kTriHeight / 2.f;

        ImVec2 op1, op2, op3;
        make_triangle(kTriWidth + kOutlinePx * 2.f, kTriHeight + kOutlinePx * 2.f, x - kOutlinePx, y - kOutlinePx, upsidedown, op1, op2, op3);
        draw_list->AddTriangleFilled(op1, op2, op3, kOutlineColor);

        ImVec2 p1, p2, p3;
        make_triangle(kTriWidth, kTriHeight, x, y, upsidedown, p1, p2, p3);
        draw_list->AddTriangleFilled(p1, p2, p3, color);

        ++count;
    };

    if (living->GetIsEnchanted()) draw_tri(kEnchantedColor, false);
    if (living->GetIsHexed()) draw_tri(kHexedColor, true);
    if (living->GetIsConditioned()) draw_tri(kConditionedColor, true);
}

inline void DrawOutlinedText(ImDrawList* draw_list, ImFont* font, float font_size, const ImVec2& pos, ImU32 text_color, const std::string& text_utf8) {
    static constexpr ImU32 kOutlineColor = IM_COL32(0, 0, 0, 255);
    static constexpr float kOutlineOffset = 1.f;
    const char* text_begin = text_utf8.c_str();
    const char* text_end = text_begin + text_utf8.size();
    draw_list->AddText(font, font_size, ImVec2(pos.x - kOutlineOffset, pos.y), kOutlineColor, text_begin, text_end);
    draw_list->AddText(font, font_size, ImVec2(pos.x + kOutlineOffset, pos.y), kOutlineColor, text_begin, text_end);
    draw_list->AddText(font, font_size, ImVec2(pos.x, pos.y - kOutlineOffset), kOutlineColor, text_begin, text_end);
    draw_list->AddText(font, font_size, ImVec2(pos.x, pos.y + kOutlineOffset), kOutlineColor, text_begin, text_end);
    draw_list->AddText(font, font_size, pos, text_color, text_begin, text_end);
}

class StackYSmoother {
public:

    float Update(uint32_t agent_id, float target_y, float alpha) {
        Entry& e = cache_[agent_id];
        e.last_seen_tick = tick_;
        if (!e.initialized) {
            e.y = target_y;
            e.initialized = true;
        }
        else {
            e.y += (target_y - e.y) * alpha;
        }
        return e.y;
    }

    void Reset(uint32_t agent_id) {
        cache_.erase(agent_id);
    }

    void MaybePrune() {
        ++tick_;
        if (tick_ - last_prune_tick_ < kPruneIntervalTicks) return;
        last_prune_tick_ = tick_;

        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (tick_ - it->second.last_seen_tick > kPruneIntervalTicks) {
                it = cache_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

private:
    static constexpr uint64_t kPruneIntervalTicks = 1800;
    struct Entry {
        float y = 0.f;
        bool initialized = false;
        uint64_t last_seen_tick = 0;
    };
    std::unordered_map<uint32_t, Entry> cache_;
    uint64_t tick_ = 0;
    uint64_t last_prune_tick_ = 0;
};

class AgentNameCache {
public:

    struct NameLookup {
        const std::wstring& lower;
        const std::wstring& display;
        const std::string& display_utf8;
    };

    NameLookup Get(uint32_t agent_id, const wchar_t* enc_name) {
        Entry& entry = cache_[agent_id];
        entry.last_seen_tick = tick_;
        if (enc_name && wcsncmp(entry.last_enc, enc_name, kMaxEncLen - 1) != 0) {
            wcsncpy_s(entry.last_enc, enc_name, kMaxEncLen - 1);
            entry.buffer[0] = L'\0';
            entry.converted = false;
            GW::UI::AsyncDecodeStr(enc_name, entry.buffer, kBufferLen);
        }
        if (!entry.converted && entry.buffer[0] != L'\0') {
            entry.decoded_display = entry.buffer;
            entry.decoded_lower = entry.buffer;
            std::transform(entry.decoded_lower.begin(), entry.decoded_lower.end(), entry.decoded_lower.begin(), ::towlower);
            entry.decoded_display_utf8 = WideToUtf8(entry.decoded_display);
            entry.converted = true;
        }
        return { entry.decoded_lower, entry.decoded_display, entry.decoded_display_utf8 };
    }

    void MaybePrune() {
        ++tick_;
        if (tick_ - last_prune_tick_ < kPruneIntervalTicks) return;
        last_prune_tick_ = tick_;

        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (tick_ - it->second.last_seen_tick > kPruneIntervalTicks) {
                it = cache_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

private:
    static constexpr size_t kBufferLen = 256;
    static constexpr size_t kMaxEncLen = 64;
    static constexpr uint64_t kPruneIntervalTicks = 1800;
    struct Entry {
        wchar_t last_enc[kMaxEncLen] = {};
        wchar_t buffer[kBufferLen] = {};
        bool converted = false;
        std::wstring decoded_lower;
        std::wstring decoded_display;
        std::string decoded_display_utf8;
        uint64_t last_seen_tick = 0;
    };
    std::unordered_map<uint32_t, Entry> cache_;
    uint64_t tick_ = 0;
    uint64_t last_prune_tick_ = 0;
};

inline bool IsMinipet(uint16_t player_number) {

    static constexpr std::array<uint16_t, 129> ids = {
        230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
        240, 241, 242, 243, 244, 245, 246, 247, 248, 249,
        250, 251, 252, 253, 254, 255, 256, 257, 258, 259,
        260, 261, 262, 263, 264, 265, 266, 267, 268, 269,
        270, 271, 272, 273, 274, 275, 276, 277, 278, 279,
        280, 281, 282, 283, 284, 285, 286, 287, 288, 289,
        290, 291, 292, 293, 294, 295, 296, 297, 298, 299,
        300, 301, 302, 303, 304, 305, 306, 307, 309, 310,
        311, 312, 313, 314, 315, 316, 317, 318, 319, 320,
        321, 322, 323, 324, 325, 326, 327, 328, 329, 330,
        331, 332, 333, 334, 335, 336, 337, 338, 339, 340,
        341, 342, 343, 344, 345, 346, 347, 348, 349, 350,
        8035, 8344, 8349, 8350, 8351, 8352, 8354, 9038, 9039,
    };
    return std::binary_search(ids.begin(), ids.end(), player_number);
}

inline std::unordered_set<std::wstring> ParseSemicolonNameList(const std::string& raw) {
    std::unordered_set<std::wstring> out;
    std::istringstream stream(raw);
    std::string token;
    while (std::getline(stream, token, ';')) {
        const size_t start = token.find_first_not_of(" \t\r\n");
        const size_t end = token.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos) continue;

        std::wstring w = Utf8ToWide(token.substr(start, end - start + 1));
        std::transform(w.begin(), w.end(), w.begin(), ::towlower);
        if (!w.empty()) out.insert(std::move(w));
    }
    return out;
}

struct NameplateSettings {
    bool show_enemies = true;
    float max_range = 1500.0f;
    float bar_width = 200.0f;
    float bar_height = 20.0f;

    std::string priority1_raw;
    std::string priority2_raw;
    std::string priority3_raw;

    float npc_health_threshold = 100.0f;
    float allied_health_threshold = 100.0f;
    bool friendly_quest_only = false;
    bool name_only_mode = false;
    bool show_summoned_allies = false;

    uint32_t enemy_color = IM_COL32(220, 40, 40, 255);
    uint32_t quest_color = IM_COL32(255, 179, 71, 255);
    uint32_t priority1_color = IM_COL32(135, 206, 250, 255);
    uint32_t priority2_color = IM_COL32(255, 105, 180, 255);
    uint32_t priority3_color = IM_COL32(147, 112, 219, 255);
};

class NameplatesPlugin : public ToolboxPlugin {
public:
    const char* Name() const override { return "Nameplates"; }

    bool* GetVisiblePtr() override { return &visible_; }

    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override;

    void LoadSettings(const wchar_t* folder) override {
        ToolboxPlugin::LoadSettings(folder);
        LoadSetting("visible", visible_);
        LoadSetting("show_enemies", settings_.show_enemies);
        LoadSetting("max_range", settings_.max_range);
        LoadSetting("bar_width", settings_.bar_width);
        LoadSetting("bar_height", settings_.bar_height);
        LoadSetting("priority1_raw", settings_.priority1_raw);
        LoadSetting("priority2_raw", settings_.priority2_raw);
        LoadSetting("priority3_raw", settings_.priority3_raw);
        LoadSetting("npc_health_threshold", settings_.npc_health_threshold);
        LoadSetting("allied_health_threshold", settings_.allied_health_threshold);
        LoadSetting("friendly_quest_only", settings_.friendly_quest_only);
        LoadSetting("name_only_mode", settings_.name_only_mode);
        LoadSetting("show_summoned_allies", settings_.show_summoned_allies);
        LoadSetting("enemy_color", settings_.enemy_color);
        LoadSetting("quest_color", settings_.quest_color);
        LoadSetting("priority1_color", settings_.priority1_color);
        LoadSetting("priority2_color", settings_.priority2_color);
        LoadSetting("priority3_color", settings_.priority3_color);
        RefreshPriorityBuffersAndLists();
    }

    void SaveSettings(const wchar_t* folder) override {
        SaveSetting("visible", visible_);
        SaveSetting("show_enemies", settings_.show_enemies);
        SaveSetting("max_range", settings_.max_range);
        SaveSetting("bar_width", settings_.bar_width);
        SaveSetting("bar_height", settings_.bar_height);
        SaveSetting("priority1_raw", settings_.priority1_raw);
        SaveSetting("priority2_raw", settings_.priority2_raw);
        SaveSetting("priority3_raw", settings_.priority3_raw);
        SaveSetting("npc_health_threshold", settings_.npc_health_threshold);
        SaveSetting("allied_health_threshold", settings_.allied_health_threshold);
        SaveSetting("friendly_quest_only", settings_.friendly_quest_only);
        SaveSetting("name_only_mode", settings_.name_only_mode);
        SaveSetting("show_summoned_allies", settings_.show_summoned_allies);
        SaveSetting("enemy_color", settings_.enemy_color);
        SaveSetting("quest_color", settings_.quest_color);
        SaveSetting("priority1_color", settings_.priority1_color);
        SaveSetting("priority2_color", settings_.priority2_color);
        SaveSetting("priority3_color", settings_.priority3_color);
        ToolboxPlugin::SaveSettings(folder);
    }

    bool CanTerminate() override { return true; }

    void Draw(IDirect3DDevice9* ) override {
        DrawNameplates();
    }

private:
    NameplateSettings settings_;
    bool visible_ = true;

    AgentNameCache name_cache_;
    StackYSmoother stack_y_smoother_;
    std::unordered_set<std::wstring> priority1_names_, priority2_names_, priority3_names_;

    static constexpr size_t kPriorityBufSize = 512;
    char priority1_buf_[kPriorityBufSize] = {};
    char priority2_buf_[kPriorityBufSize] = {};
    char priority3_buf_[kPriorityBufSize] = {};

    static constexpr ImU32 kTargetColor    = IM_COL32(255, 220, 0, 255);
    static constexpr float kNameplateFontSize = static_cast<float>(FontLoader::FontSize::header2);
    static constexpr float kStackSmoothing = 0.05f;
    static constexpr float kBgTintAmount = 0.3f;
    static constexpr float kBgOpacity = 1.0f;

    static constexpr float kZNear = 46.875f;
    static constexpr float kZFar  = 48000.f;

    void RefreshOnePriorityBuffer(char* buf, const std::string& raw, std::unordered_set<std::wstring>& names) {
        strncpy_s(buf, kPriorityBufSize, raw.c_str(), kPriorityBufSize - 1);
        names = ParseSemicolonNameList(raw);
    }

    void RefreshPriorityBuffersAndLists() {
        RefreshOnePriorityBuffer(priority1_buf_, settings_.priority1_raw, priority1_names_);
        RefreshOnePriorityBuffer(priority2_buf_, settings_.priority2_raw, priority2_names_);
        RefreshOnePriorityBuffer(priority3_buf_, settings_.priority3_raw, priority3_names_);
    }

    std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower) const {
        if (name_lower.empty()) return std::nullopt;
        if (priority1_names_.count(name_lower)) return settings_.priority1_color;
        if (priority2_names_.count(name_lower)) return settings_.priority2_color;
        if (priority3_names_.count(name_lower)) return settings_.priority3_color;
        return std::nullopt;
    }

    struct PendingBar {
        GW::AgentLiving* living = nullptr;
        ImVec2 screen{};
        ImVec2 footprint{};
        std::wstring name_lower;
        std::wstring display;
        std::string display_utf8;
        bool is_targeted = false;
        bool is_name_only = false;
        bool stack_adjusted = false;
        float natural_y = 0.f;
        bool is_in_combat = false;
    };

    struct PlacedRect { float x_min, x_max, y_min, y_max; };

    std::vector<PendingBar> pending_;
    std::vector<PlacedRect> placed_;

    ImVec2 ComputeFootprint(bool is_name_only, const std::string& display_utf8) const {
        if (is_name_only) {
            if (display_utf8.empty()) return ImVec2(0.f, 0.f);
            ImFont* font = ImGui::GetFont();
            if (!font) return ImVec2(0.f, 0.f);
            const float font_size = kNameplateFontSize;
            return font->CalcTextSizeA(font_size, FLT_MAX, 0.f, display_utf8.c_str());
        }
        return ImVec2(settings_.bar_width, settings_.bar_height);
    }

    void ResolveStacking(std::vector<PendingBar>& items) {
        static constexpr float kGap = 2.f;

        static constexpr float kMaxPushMultiplier = 4.f;

        static constexpr float kSortEpsilon = 1.f;

        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < items.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const float ya = items[a].screen.y;
            const float yb = items[b].screen.y;
            if (std::fabs(ya - yb) > kSortEpsilon) return ya < yb;
            return items[a].living->agent_id < items[b].living->agent_id;
        });

        placed_.clear();
        placed_.reserve(items.size());

        for (size_t oi : order) {
            PendingBar& item = items[oi];
            if (item.footprint.x <= 0.f || item.footprint.y <= 0.f) continue;

            const float half_w = item.footprint.x / 2.f;
            const float x_min = item.screen.x - half_w;
            const float x_max = item.screen.x + half_w;
            const float natural_top = item.is_name_only ? item.screen.y - item.footprint.y / 2.f : item.screen.y;
            const float max_push = item.footprint.y * kMaxPushMultiplier;

            float cur_top = natural_top;
            bool moved = true;
            while (moved) {
                moved = false;
                for (const auto& p : placed_) {
                    const float y_min = cur_top;
                    const float y_max = cur_top + item.footprint.y;
                    const bool overlap_x = x_min < p.x_max && x_max > p.x_min;
                    const bool overlap_y = y_min < p.y_max && y_max > p.y_min;
                    if (overlap_x && overlap_y) {
                        const float candidate_top = p.y_min - item.footprint.y - kGap;
                        if (natural_top - candidate_top > max_push) {
                            moved = false;
                            break;
                        }
                        cur_top = candidate_top;
                        moved = true;
                    }
                }
            }

            item.stack_adjusted = (cur_top != natural_top);
            item.screen.y += (cur_top - natural_top);
            placed_.push_back({x_min, x_max, cur_top, cur_top + item.footprint.y});
        }
    }

    void DrawNameplates() {
        GW::AgentArray* agents = GW::Agents::GetAgentArray();
        if (!agents || !agents->valid()) return;

        GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
        GW::AgentLiving* target = GW::Agents::GetTargetAsAgentLiving();
        const bool in_outpost = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Outpost;
        const bool left_clicked_this_frame = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        DirectX::XMMATRIX view_proj;
        float viewport_width, viewport_height;
        if (!BuildFrameProjection(view_proj, viewport_width, viewport_height)) return;

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        pending_.clear();

        for (GW::Agent* agent : *agents) {
            if (!agent) continue;
            if (!agent->GetIsLivingType()) continue;

            GW::AgentLiving* living = agent->GetAsAgentLiving();
            if (!living) continue;

            if (living->GetIsDead()) continue;
            if (me && living->agent_id == me->agent_id) continue;
            if (IsMinipet(living->player_number)) continue;
            if (!settings_.show_summoned_allies
                && (living->allegiance == GW::Constants::Allegiance::Spirit_Pet
                    || living->allegiance == GW::Constants::Allegiance::Minion)) continue;

            if (!ShouldShowAllegiance(living->allegiance)) continue;

            const bool is_npc = living->allegiance == GW::Constants::Allegiance::Neutral
                || living->allegiance == GW::Constants::Allegiance::Npc_Minipet;
            const bool is_allied = living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable
                || living->allegiance == GW::Constants::Allegiance::Spirit_Pet
                || living->allegiance == GW::Constants::Allegiance::Minion;

            if (is_npc) {
                const bool passes_threshold = (living->hp * 100.f <= settings_.npc_health_threshold);
                const bool passes_quest = settings_.friendly_quest_only && living->GetHasQuest();
                if (!passes_threshold && !passes_quest) continue;
            }
            else if (is_allied) {
                const bool is_real_player = living->IsPlayer();
                const bool always_show_player_in_outpost = in_outpost && is_real_player;
                if (!always_show_player_in_outpost && living->hp * 100.f > settings_.allied_health_threshold) continue;
                if (in_outpost && !is_real_player) {
                    GW::NPC* npc = GW::Agents::GetNPCByID(living->player_number);
                    if (npc && npc->IsHenchman()) continue;
                }
            }

            if (!WithinRange(living, me)) continue;

            ImVec2 screen;
            if (!WorldToScreen(living, view_proj, viewport_width, viewport_height, screen)) continue;

            const auto name_lookup = name_cache_.Get(
                living->agent_id, GW::Agents::GetAgentEncName(living->agent_id));

            PendingBar pb;
            pb.living = living;
            pb.screen = screen;
            pb.name_lower = name_lookup.lower;
            pb.display = name_lookup.display;
            pb.display_utf8 = name_lookup.display_utf8;
            pb.is_targeted = target && living->agent_id == target->agent_id;
            pb.is_name_only = settings_.name_only_mode && in_outpost && living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable;

            if (living->GetInCombatStance()) {
                pb.is_in_combat = true;
            }
            else if (me && living->GetIsMoving()) {
                const float dx = living->pos.x - me->pos.x;
                const float dy = living->pos.y - me->pos.y;
                pb.is_in_combat = (dx * dx + dy * dy) <= GW::Constants::SqrRange::Earshot;
            }
            else {
                pb.is_in_combat = false;
            }
            pb.footprint = ComputeFootprint(pb.is_name_only, pb.display_utf8);

            pending_.push_back(std::move(pb));
        }

        for (auto& pb : pending_) {
            pb.natural_y = pb.screen.y;
        }

        ResolveStacking(pending_);

        for (auto& pb : pending_) {
            const float target_offset = pb.screen.y - pb.natural_y;
            const float smoothed_offset = stack_y_smoother_.Update(pb.living->agent_id, target_offset, kStackSmoothing);
            pb.screen.y = pb.natural_y + smoothed_offset;
        }

        for (const auto& pb : pending_) {
            if (pb.is_targeted) continue;
            DrawBar(draw_list, pb.screen, pb.living, pb.name_lower, pb.display, pb.display_utf8, pb.footprint, false, pb.is_name_only, left_clicked_this_frame, pb.is_in_combat);
        }
        for (const auto& pb : pending_) {
            if (!pb.is_targeted) continue;
            DrawBar(draw_list, pb.screen, pb.living, pb.name_lower, pb.display, pb.display_utf8, pb.footprint, true, pb.is_name_only, left_clicked_this_frame, pb.is_in_combat);
        }

        name_cache_.MaybePrune();
        stack_y_smoother_.MaybePrune();
    }

    bool ShouldShowAllegiance(GW::Constants::Allegiance allegiance) const {
        switch (allegiance) {
            case GW::Constants::Allegiance::Enemy:
                return settings_.show_enemies;
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Spirit_Pet:
            case GW::Constants::Allegiance::Minion:
                return true;
            case GW::Constants::Allegiance::Neutral:
            case GW::Constants::Allegiance::Npc_Minipet:
                return true;
            default:
                return false;
        }
    }

    bool WithinRange(const GW::AgentLiving* living, const GW::Agent* me) const {
        if (!me) return true;
        const float dx = living->pos.x - me->pos.x;
        const float dy = living->pos.y - me->pos.y;
        const float dist_sq = dx * dx + dy * dy;
        return dist_sq <= (settings_.max_range * settings_.max_range);
    }

    bool BuildFrameProjection(DirectX::XMMATRIX& out_view_proj,
                              float& out_viewport_width, float& out_viewport_height) const {
        const auto cam = GW::CameraMgr::GetCamera();
        if (!cam) return false;

        using namespace DirectX;

        const XMVECTOR eye_pos = XMVectorSet(cam->position.x, cam->position.y, cam->position.z, 0.f);
        const XMVECTOR look_at = XMVectorSet(cam->look_at_target.x, cam->look_at_target.y, cam->look_at_target.z, 0.f);
        const XMVECTOR up      = XMVectorSet(0.f, 0.f, -1.f, 0.f);

        const XMMATRIX view = XMMatrixLookAtLH(eye_pos, look_at, up);

        out_viewport_width  = static_cast<float>(GW::Render::GetViewportWidth());
        out_viewport_height = static_cast<float>(GW::Render::GetViewportHeight());
        if (out_viewport_width <= 0.f || out_viewport_height <= 0.f) return false;

        const float fov = GW::Render::GetFieldOfView();
        const float aspect = out_viewport_width / out_viewport_height;

        const XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, kZNear, kZFar);

        out_view_proj = view * proj;
        return true;
    }

    bool WorldToScreen(const GW::AgentLiving* living, const DirectX::XMMATRIX& view_proj,
                       float viewport_width, float viewport_height, ImVec2& out) const {
        using namespace DirectX;

        const XMVECTOR world_pos = XMVectorSet(
            living->pos.x, living->pos.y, living->name_tag_z, 1.f);

        const XMVECTOR clip_pos = XMVector4Transform(world_pos, view_proj);
        float clip_arr[4];
        XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(clip_arr), clip_pos);

        if (clip_arr[3] <= kZNear) return false;

        const float ndc_x = clip_arr[0] / clip_arr[3];
        const float ndc_y = clip_arr[1] / clip_arr[3];

        out.x = (ndc_x * 0.5f + 0.5f) * viewport_width;
        out.y = (1.f - (ndc_y * 0.5f + 0.5f)) * viewport_height;

        return true;
    }

    void CheckClickToTarget(const ImVec2& rect_min, const ImVec2& rect_max, const GW::AgentLiving* living, bool left_clicked_this_frame) const {
        if (!left_clicked_this_frame) return;
        if (ImGui::IsMouseHoveringRect(rect_min, rect_max, false)) {
            const uint32_t agent_id = living->agent_id;
            GW::GameThread::Enqueue([agent_id] {
                GW::Agents::ChangeTarget(agent_id);
            });
        }
    }

    void ShowHelpMarker(const char* help) const {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", help);
        }
    }

    void DrawNameOnly(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living, const std::string& display_utf8, const ImVec2& text_size, bool left_clicked_this_frame) {
        if (display_utf8.empty()) return;

        ImFont* font = ImGui::GetFont();
        if (!font) return;
        const float font_size = kNameplateFontSize;

        const float text_x = screen.x - text_size.x / 2.f;
        const float text_y = screen.y - text_size.y / 2.f;

        const ImU32 text_color = ProfessionColor(living->primary);
        DrawOutlinedText(draw_list, font, font_size, ImVec2(text_x, text_y), text_color, display_utf8);

        DrawStatusTriangles(draw_list, text_x + text_size.x, text_y - 6.f, living);

        CheckClickToTarget(ImVec2(text_x, text_y), ImVec2(text_x + text_size.x, text_y + text_size.y), living, left_clicked_this_frame);
    }

    void DrawBar(ImDrawList* draw_list, const ImVec2& screen, const GW::AgentLiving* living, const std::wstring& name_lower, const std::wstring& display_name, const std::string& display_utf8, const ImVec2& footprint, bool is_targeted, bool is_name_only, bool left_clicked_this_frame, bool is_in_combat) {
        const bool is_ally = living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable;

        if (is_name_only) {
            DrawNameOnly(draw_list, screen, living, display_utf8, footprint, left_clicked_this_frame);
            return;
        }

        float hp_pct = living->hp;
        hp_pct = hp_pct < 0.f ? 0.f : (hp_pct > 1.f ? 1.f : hp_pct);

        const float bar_width = settings_.bar_width;
        const float bar_height = settings_.bar_height;

        const ImVec2 top_left(screen.x - bar_width / 2.f, screen.y);
        const ImVec2 bottom_right(top_left.x + bar_width, top_left.y + bar_height);
        const ImVec2 fill_bottom_right(top_left.x + bar_width * hp_pct, bottom_right.y);

        ImU32 fill_color;
        if (const auto priority_color = GetPriorityColor(name_lower)) {
            fill_color = *priority_color;
        }
        else if (living->GetHasQuest()) {
            fill_color = settings_.quest_color;
        }
        else if (is_ally) {
            fill_color = ProfessionColor(living->primary);
        }
        else {
            fill_color = ColorFor(living->allegiance);
        }

        ImVec4 fill_col4 = ImGui::ColorConvertU32ToFloat4(fill_color);
        ImVec4 bg_col4;
        bg_col4.x = fill_col4.x * kBgTintAmount;
        bg_col4.y = fill_col4.y * kBgTintAmount;
        bg_col4.z = fill_col4.z * kBgTintAmount;
        bg_col4.w = kBgOpacity;
        const ImU32 bg_color = ImGui::ColorConvertFloat4ToU32(bg_col4);

        const ImU32 border_color = is_targeted ? kTargetColor : IM_COL32(0, 0, 0, 180);

        draw_list->AddRectFilled(top_left, bottom_right, bg_color);
        draw_list->AddRectFilled(top_left, fill_bottom_right, fill_color);
        draw_list->AddRect(top_left, bottom_right, border_color);

        DrawStatusTriangles(draw_list, bottom_right.x - 8.f, top_left.y + bar_height / 2.f, living);

        CheckClickToTarget(top_left, bottom_right, living, left_clicked_this_frame);

        if (!display_name.empty()) {
            ImFont* font = ImGui::GetFont();
            if (font) {
                const float font_size = kNameplateFontSize;

                constexpr float kPadding = 6.f;
                const float max_text_width = bar_width * 0.8f - kPadding;

                if (max_text_width > 0.f) {
                    const std::string clipped_utf8 = TruncateWithEllipsis(font, font_size, display_name, display_utf8, max_text_width);
                    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, clipped_utf8.c_str());

                    const float text_x = top_left.x + kPadding;
                    const float text_y = top_left.y + (bar_height - text_size.y) / 2.f;

                    static constexpr ImU32 kNormalTextColor = IM_COL32(255, 255, 255, 255);
                    static constexpr ImU32 kInCombatTextColor = IM_COL32(255, 190, 116, 255);
                    const bool is_enemy_in_combat = living->allegiance == GW::Constants::Allegiance::Enemy && is_in_combat;
                    const ImU32 name_text_color = is_enemy_in_combat ? kInCombatTextColor : kNormalTextColor;
                    DrawOutlinedText(draw_list, font, font_size, ImVec2(text_x, text_y), name_text_color, clipped_utf8);
                }
            }
        }
    }

    ImU32 ColorFor(GW::Constants::Allegiance allegiance) const {
        switch (allegiance) {
            case GW::Constants::Allegiance::Enemy:
                return settings_.enemy_color;
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Spirit_Pet:
            case GW::Constants::Allegiance::Minion:
                return IM_COL32(40, 200, 60, 255);
            default:
                return IM_COL32(0, 255, 152, 255);
        }
    }

    ImU32 ProfessionColor(GW::Constants::ProfessionByte prof) const {
        switch (prof) {
            case GW::Constants::ProfessionByte::Warrior:      return IM_COL32(255, 255, 136, 255);
            case GW::Constants::ProfessionByte::Ranger:       return IM_COL32(204, 255, 153, 255);
            case GW::Constants::ProfessionByte::Monk:         return IM_COL32(170, 204, 255, 255);
            case GW::Constants::ProfessionByte::Necromancer:  return IM_COL32(153, 255, 204, 255);
            case GW::Constants::ProfessionByte::Mesmer:       return IM_COL32(221, 170, 255, 255);
            case GW::Constants::ProfessionByte::Elementalist: return IM_COL32(255, 187, 187, 255);
            case GW::Constants::ProfessionByte::Assassin:     return IM_COL32(255, 204, 238, 255);
            case GW::Constants::ProfessionByte::Ritualist:    return IM_COL32(187, 255, 255, 255);
            case GW::Constants::ProfessionByte::Paragon:      return IM_COL32(255, 204, 153, 255);
            case GW::Constants::ProfessionByte::Dervish:      return IM_COL32(221, 221, 255, 255);
            default:                                          return IM_COL32(221, 221, 221, 255);
        }
    }

    void DrawPriorityInput(const char* label, uint32_t& color, char* buf, std::string& raw, std::unordered_set<std::wstring>& names) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(color).Value);
        const bool changed = ImGui::InputText(label, buf, kPriorityBufSize);
        ImGui::PopStyleColor();
        if (changed) {
            raw = buf;
            names = ParseSemicolonNameList(raw);
        }
        ImGui::SameLine();
        ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
        const std::string picker_id = std::string("##color_") + label;
        if (ImGui::ColorEdit3(picker_id.c_str(), &color_vec.x)) {
            color = ImGui::ColorConvertFloat4ToU32(color_vec);
        }
    }

    void DrawSettingsInternal() {
        ImGui::Checkbox("Show enemies", &settings_.show_enemies);
        ImGui::SameLine();
        ImVec4 enemy_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.enemy_color);
        if (ImGui::ColorEdit3("##color_show_enemies", &enemy_color_vec.x)) {
            settings_.enemy_color = ImGui::ColorConvertFloat4ToU32(enemy_color_vec);
        }

        ImGui::Checkbox("Ally name-only mode", &settings_.name_only_mode);
        ShowHelpMarker("Show Players/Heroes/Henchmen names only");
        ImGui::Checkbox("Show allied summoned creatures", &settings_.show_summoned_allies);
        ShowHelpMarker("Shows spirits and minions");

        int npc_display = static_cast<int>(std::lround(settings_.npc_health_threshold / 10.f));
        int allied_display = static_cast<int>(std::lround(settings_.allied_health_threshold / 10.f));
        const float threshold_half_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemInnerSpacing.x) / 2.f;
        ImGui::PushItemWidth(threshold_half_width);
        if (ImGui::SliderInt("##npc_threshold", &npc_display, 0, 10)) {
            settings_.npc_health_threshold = static_cast<float>(npc_display) * 10.f;
        }
        ImGui::SameLine();
        if (ImGui::SliderInt("##allied_threshold", &allied_display, 0, 10)) {
            settings_.allied_health_threshold = static_cast<float>(allied_display) * 10.f;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextUnformatted("NPC & Ally visibility threshold");
        ShowHelpMarker("10 = always show, 0 = off");

        ImGui::Checkbox("Quest-giver visibility override", &settings_.friendly_quest_only);
        ImGui::SameLine();
        ImVec4 quest_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.quest_color);
        if (ImGui::ColorEdit3("##color_quest", &quest_color_vec.x)) {
            settings_.quest_color = ImGui::ColorConvertFloat4ToU32(quest_color_vec);
        }
        ShowHelpMarker("Overrides the NPC visibility threshold slider");

        ImGui::SliderFloat("Max range", &settings_.max_range, 500.f, 5000.f);

        const float barsize_half_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemInnerSpacing.x) / 2.f;
        ImGui::PushItemWidth(barsize_half_width);
        if (ImGui::SliderFloat("##bar_width", &settings_.bar_width, 50.f, 200.f, "%.0f")) {
            settings_.bar_width = std::round(settings_.bar_width);
        }
        ImGui::SameLine();
        if (ImGui::SliderFloat("##bar_height", &settings_.bar_height, 15.f, 20.f, "%.0f")) {
            settings_.bar_height = std::round(settings_.bar_height);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextUnformatted("Bar width & height");

        ImGui::Separator();
        ImGui::TextUnformatted("Priority name coloring (semicolon-separated, e.g. \"Angry Hog; Angry Bat\")");

        DrawPriorityInput("Priority 1", settings_.priority1_color, priority1_buf_, settings_.priority1_raw, priority1_names_);
        DrawPriorityInput("Priority 2", settings_.priority2_color, priority2_buf_, settings_.priority2_raw, priority2_names_);
        DrawPriorityInput("Priority 3", settings_.priority3_color, priority3_buf_, settings_.priority3_raw, priority3_names_);
    }
};

void NameplatesPlugin::DrawSettings()
{
    ToolboxPlugin::DrawSettings();
    DrawSettingsInternal();
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static NameplatesPlugin instance;
    return &instance;
}

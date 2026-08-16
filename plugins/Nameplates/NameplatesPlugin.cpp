#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/NPC.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/QuestMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Packets/StoC.h>
#include <GWCA/Utilities/Scanner.h>

#include <ToolboxPlugin.h>
#include <imgui.h>

#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <cwchar>
#include <optional>
#include <algorithm>
#include <array>

inline std::wstring Utf8ToWide(const std::string& utf8) {
	if (utf8.empty()) return {};
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0) return {};
	std::wstring out(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
	return out;
}

template<typename CacheMap>
inline void PruneCache(CacheMap& cache, uint64_t& tick, uint64_t& last_prune, uint64_t interval) {
	++tick;
	if (tick - last_prune < interval) return;
	last_prune = tick;

	for (auto it = cache.begin(); it != cache.end(); ) {
		if (tick - it->second.last_seen_tick >= interval) {
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

inline std::vector<std::wstring> SplitWords(const std::wstring& text) {
	std::vector<std::wstring> out;
	size_t start = 0;
	while (start <= text.size()) {
		size_t pos = text.find(L' ', start);
		if (pos == std::wstring::npos) pos = text.size();
		if (pos > start) out.emplace_back(text.substr(start, pos - start));
		start = pos + 1;
	}
	return out;
}

inline GW::Constants::ProfessionByte GetAgentProfession(const GW::AgentLiving* living) {
	if (living->primary != GW::Constants::ProfessionByte::None) return living->primary;
	const GW::NPC* npc = GW::Agents::GetNPCByID(living->player_number);
	return npc ? static_cast<GW::Constants::ProfessionByte>(npc->primary) : GW::Constants::ProfessionByte::None;
}

class AgentNameCache {
public:
	struct NameLookup {
		const std::wstring* lower;
		const std::vector<std::wstring>* words;
		GW::Constants::ProfessionByte profession;
	};

	NameLookup Get(const GW::AgentLiving* living) {
		Entry& entry = cache_[living->agent_id];
		entry.last_seen_tick = tick_;
		const wchar_t* enc_name = GW::Agents::GetAgentEncName(living->agent_id);
		if (enc_name && wcsncmp(entry.last_enc, enc_name, kMaxEncLen - 1) != 0) {
			wcsncpy_s(entry.last_enc, enc_name, kMaxEncLen - 1);
			entry.buffer[0] = L'\0';
			entry.converted = false;
			entry.profession_resolved = false;
			GW::UI::AsyncDecodeStr(enc_name, entry.buffer, kBufferLen);
		}
		if (!entry.converted && entry.buffer[0] != L'\0') {
			entry.decoded_lower = entry.buffer;
			std::transform(entry.decoded_lower.begin(), entry.decoded_lower.end(), entry.decoded_lower.begin(), ::towlower);
			entry.decoded_words_lower = SplitWords(entry.decoded_lower);
			entry.converted = true;
		}
		if (!entry.profession_resolved) {
			entry.profession = GetAgentProfession(living);
			entry.profession_resolved = true;
		}
		return { &entry.decoded_lower, &entry.decoded_words_lower, entry.profession };
	}

	void MaybePrune() { PruneCache(cache_, tick_, last_prune_tick_, kPruneIntervalTicks); }

private:
	static constexpr size_t kBufferLen = 256;
	static constexpr size_t kMaxEncLen = 64;
	static constexpr uint64_t kPruneIntervalTicks = 1800;
	struct Entry {
		wchar_t last_enc[kMaxEncLen] = {};
		wchar_t buffer[kBufferLen] = {};
		bool converted = false;
		uint64_t last_seen_tick = 0;
		std::wstring decoded_lower;
		std::vector<std::wstring> decoded_words_lower;
		GW::Constants::ProfessionByte profession = GW::Constants::ProfessionByte::None;
		bool profession_resolved = false;
	};
	std::unordered_map<uint32_t, Entry> cache_;
	uint64_t tick_ = 0, last_prune_tick_ = 0;
};

inline std::vector<std::wstring> ParseSemicolonNameList(const std::string& raw) {
	std::vector<std::wstring> out;
	std::istringstream stream(raw);
	std::string token;
	while (std::getline(stream, token, ';')) {
		const size_t start = token.find_first_not_of(" \t\r\n");
		const size_t end = token.find_last_not_of(" \t\r\n");
		if (start == std::string::npos || end == std::string::npos) continue;

		std::wstring w = Utf8ToWide(token.substr(start, end - start + 1));
		std::transform(w.begin(), w.end(), w.begin(), ::towlower);
		if (!w.empty()) out.push_back(std::move(w));
	}
	std::sort(out.begin(), out.end());
	return out;
}

struct PriorityConfig {
	std::string raw;
	uint32_t color;
};

struct ProfessionColorConfig {
	bool enabled = true;
	uint32_t color = IM_COL32(221, 221, 221, 255);
};

struct NameplateSettings {
	bool recolor_quest_nametags = true, recolor_professions = false;
	bool recolor_enemy_nameplates_by_profession = false;
	uint32_t quest_color = IM_COL32(255, 179, 71, 255);

	bool color_by_boss = false;
	uint32_t boss_color = IM_COL32(255, 215, 0, 255);

	std::array<ProfessionColorConfig, 11> profession_colors = {{
		{false, IM_COL32(221, 221, 221, 255)},
		{true, IM_COL32(255, 255, 136, 255)},
		{true, IM_COL32(204, 255, 153, 255)},
		{true, IM_COL32(170, 204, 255, 255)},
		{true, IM_COL32(153, 255, 204, 255)},
		{true, IM_COL32(221, 170, 255, 255)},
		{true, IM_COL32(255, 187, 187, 255)},
		{true, IM_COL32(255, 204, 238, 255)},
		{true, IM_COL32(187, 255, 255, 255)},
		{true, IM_COL32(255, 204, 153, 255)},
		{true, IM_COL32(221, 221, 255, 255)}
	}};

	std::array<PriorityConfig, 2> priorities = {{
		{"", IM_COL32(135, 206, 250, 255)},
		{"", IM_COL32(255, 105, 180, 255)}
	}};
};

class NameplatesPlugin : public ToolboxPlugin {
public:
	NameplatesPlugin() {
		GW::UI::RegisterUIMessageCallback(&nametag_hook_entry_, GW::UI::UIMessage::kShowAgentNameTag, OnAgentNameTag);
		GW::UI::RegisterUIMessageCallback(&nametag_hook_entry_, GW::UI::UIMessage::kSetAgentNameTagAttribs, OnAgentNameTag);
		GW::UI::RegisterUIMessageCallback(&quest_hook_entry_, GW::UI::UIMessage::kQuestAdded, OnQuestUpdate);
		GW::UI::RegisterUIMessageCallback(&quest_hook_entry_, GW::UI::UIMessage::kQuestDetailsChanged, OnQuestUpdate);
		GW::UI::RegisterUIMessageCallback(&target_hook_entry_, GW::UI::UIMessage::kChangeTarget, OnTargetChanged);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_, OnAgentAllegianceChanged, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_, OnAgentMarkerChanged, 1);
	}

	const char* Name() const override { return "Nameplates"; }

	bool* GetVisiblePtr() override { return &visible_; }

	[[nodiscard]] bool HasSettings() const override { return true; }
	void DrawSettings() override;

	void LoadSettings(const wchar_t* folder) override {
		ToolboxPlugin::LoadSettings(folder);
		#define L_SET(var) LoadSetting(#var, settings_.var)
		L_SET(recolor_quest_nametags); L_SET(recolor_professions);
		L_SET(recolor_enemy_nameplates_by_profession);
		L_SET(quest_color);
		L_SET(color_by_boss); L_SET(boss_color);
		LoadSetting("visible", visible_);
		#undef L_SET

		for (size_t i = 0; i < 2; ++i) {
			const std::string prefix = "priority" + std::to_string(i + 1);
			LoadSetting((prefix + "_raw").c_str(), settings_.priorities[i].raw);
			LoadSetting((prefix + "_color").c_str(), settings_.priorities[i].color);
		}
		for (size_t i = 1; i < settings_.profession_colors.size(); ++i) {
			const std::string prefix = "profession" + std::to_string(i);
			LoadSetting((prefix + "_enabled").c_str(), settings_.profession_colors[i].enabled);
			LoadSetting((prefix + "_color").c_str(), settings_.profession_colors[i].color);
		}
		RefreshPriorityBuffersAndLists();
	}

	void SaveSettings(const wchar_t* folder) override {
		#define S_SET(var) SaveSetting(#var, settings_.var)
		S_SET(recolor_quest_nametags); S_SET(recolor_professions);
		S_SET(recolor_enemy_nameplates_by_profession);
		S_SET(quest_color);
		S_SET(color_by_boss); S_SET(boss_color);
		SaveSetting("visible", visible_);
		#undef S_SET

		for (size_t i = 0; i < 2; ++i) {
			const std::string prefix = "priority" + std::to_string(i + 1);
			SaveSetting((prefix + "_raw").c_str(), settings_.priorities[i].raw);
			SaveSetting((prefix + "_color").c_str(), settings_.priorities[i].color);
		}
		for (size_t i = 1; i < settings_.profession_colors.size(); ++i) {
			const std::string prefix = "profession" + std::to_string(i);
			SaveSetting((prefix + "_enabled").c_str(), settings_.profession_colors[i].enabled);
			SaveSetting((prefix + "_color").c_str(), settings_.profession_colors[i].color);
		}
		ToolboxPlugin::SaveSettings(folder);
	}

	bool CanTerminate() override { return true; }

	void Terminate() override {
		GW::UI::RemoveUIMessageCallback(&nametag_hook_entry_);
		GW::UI::RemoveUIMessageCallback(&quest_hook_entry_);
		GW::UI::RemoveUIMessageCallback(&target_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_);
	}

	void Draw(IDirect3DDevice9*) override {
		RefreshAllNametagsOnChange(last_recolor_professions_state_, settings_.recolor_professions);
		RefreshAllNametagsOnChange(last_recolor_quest_state_, settings_.recolor_quest_nametags, true);
		RefreshAllNametagsOnChange(last_recolor_enemy_profession_state_, settings_.recolor_enemy_nameplates_by_profession);
		RefreshAllNametagsOnChange(last_color_by_boss_state_, settings_.color_by_boss);

		if (const auto quest_log = GW::QuestMgr::GetQuestLog()) {
			const int quest_count = static_cast<int>(quest_log->size());
			if (last_quest_count_ != -1 && last_quest_count_ != quest_count) {
				RefreshAllNametags();
				RefreshTargetedNametagViaRetarget();
			}
			last_quest_count_ = quest_count;
		}

		name_cache_.MaybePrune();
	}

private:
	NameplateSettings settings_;
	bool visible_ = true;
	std::optional<bool> last_recolor_professions_state_;
	std::optional<bool> last_recolor_quest_state_;
	std::optional<bool> last_recolor_enemy_profession_state_;
	std::optional<bool> last_color_by_boss_state_;
	int last_quest_count_ = -1;
	GW::HookEntry nametag_hook_entry_;
	GW::HookEntry quest_hook_entry_;
	GW::HookEntry target_hook_entry_;
	GW::HookEntry allegiance_hook_entry_;
	GW::HookEntry marker_hook_entry_;

	AgentNameCache name_cache_;

	struct PriorityState {
		char buf[512] = {};
		std::vector<std::wstring> names;
	};
	std::array<PriorityState, 2> priority_states_;

	void RefreshPriorityBuffersAndLists() {
		for (size_t i = 0; i < 2; ++i) {
			strncpy_s(priority_states_[i].buf, 512, settings_.priorities[i].raw.c_str(), 511);
			priority_states_[i].names = ParseSemicolonNameList(settings_.priorities[i].raw);
		}
	}

	[[nodiscard]] std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower, const std::vector<std::wstring>& words) const {
		if (!name_lower.empty()
			&& std::binary_search(priority_states_[0].names.begin(), priority_states_[0].names.end(), name_lower)) {
			return settings_.priorities[0].color;
		}
		for (const auto& word : words) {
			if (std::binary_search(priority_states_[1].names.begin(), priority_states_[1].names.end(), word)) {
				return settings_.priorities[1].color;
			}
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<ImU32> TryGetProfessionColor(GW::Constants::ProfessionByte prof) const {
		const size_t index = static_cast<size_t>(prof);
		if (index == 0 || index >= settings_.profession_colors.size()) return std::nullopt;
		const auto& cfg = settings_.profession_colors[index];
		return cfg.enabled ? std::optional<ImU32>(cfg.color) : std::nullopt;
	}

	static void ShowHelpMarker(const char* help) {
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
	}

	static void RightAlignNextItem(float item_width) {
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - item_width);
	}

	static void DrawCheckboxWithColorRightAligned(const char* label, bool& toggle, uint32_t& color, const char* color_id) {
		ImGui::Checkbox(label, &toggle);
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorEdit3(color_id, &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
	}

	void DrawProfessionCell(size_t index) {
		ProfessionColorConfig& cfg = settings_.profession_colors[index];
		ImGui::PushID(static_cast<int>(index));
		const bool was_enabled = cfg.enabled;
		if (!was_enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
		ImGui::Checkbox("##enabled", &cfg.enabled);
		ImGui::SameLine();
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(cfg.color);
		if (ImGui::ColorEdit3("##color", &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			cfg.color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(GW::Constants::GetProfessionAcronym(static_cast<GW::Constants::Profession>(index)));
		if (!was_enabled) ImGui::PopStyleVar();
		ImGui::PopID();
	}

	static void RefreshAllNametags() {
		using SetGlobalNameTagVisibility_pt = void(__cdecl*)(uint32_t);
		static bool tried_resolve = false;
		static SetGlobalNameTagVisibility_pt set_func = nullptr;
		static uint32_t* flags_ptr = nullptr;
		if (!tried_resolve) {
			tried_resolve = true;
			uintptr_t address = GW::Scanner::Find("\x81\xce\xa0\x06\x00\x00", "xxxxxx");
			if (address) address = GW::Scanner::FunctionFromNearCall(GW::Scanner::FindInRange("\xe8", "x", 0, address, address + 0xff));
			if (address) {
				set_func = reinterpret_cast<SetGlobalNameTagVisibility_pt>(address);
				if (GW::Scanner::IsValidPtr(*reinterpret_cast<uintptr_t*>(address + 0xa))) {
					flags_ptr = *reinterpret_cast<uint32_t**>(address + 0xa);
				}
				else if (GW::Scanner::IsValidPtr(*reinterpret_cast<uintptr_t*>(address + 0xb))) {
					flags_ptr = *reinterpret_cast<uint32_t**>(address + 0xb);
				}
			}
		}
		if (!set_func || !flags_ptr) return;
		GW::GameThread::Enqueue([] {
			const uint32_t prev_flags = *flags_ptr;
			set_func(0);
			set_func(prev_flags);
		});
	}

	static void RefreshAllNametagsOnChange(std::optional<bool>& last_state, bool current_state, bool also_retarget = false) {
		if (last_state.has_value() && *last_state != current_state) {
			RefreshAllNametags();
			if (also_retarget) RefreshTargetedNametagViaRetarget();
		}
		last_state = current_state;
	}

	static void OnAgentNameTag(GW::HookStatus* status, GW::UI::UIMessage msgid, void* wParam, void*) {
		if (msgid != GW::UI::UIMessage::kShowAgentNameTag && msgid != GW::UI::UIMessage::kSetAgentNameTagAttribs) return;
		auto* self = static_cast<NameplatesPlugin*>(ToolboxPluginInstance());
		self->HandleAgentNameTag(status, static_cast<GW::UI::AgentNameTagInfo*>(wParam));
	}

	static void RefreshTargetedNametagViaRetarget() {
		GW::GameThread::Enqueue([] {
			const uint32_t target_id = GW::Agents::GetTargetId();
			if (target_id == 0) return;
			GW::Agent* target_agent = GW::Agents::GetAgentByID(target_id);
			GW::AgentLiving* target_living = target_agent ? target_agent->GetAsAgentLiving() : nullptr;
			if (!target_living || target_living->allegiance == GW::Constants::Allegiance::Enemy) return;
			GW::Agents::ChangeTarget(target_id);
		});
	}

	static void OnQuestUpdate(GW::HookStatus*, GW::UI::UIMessage msgid, void*, void*) {
		if (msgid != GW::UI::UIMessage::kQuestAdded
			&& msgid != GW::UI::UIMessage::kQuestDetailsChanged) return;
		RefreshAllNametags();
		RefreshTargetedNametagViaRetarget();
	}

	static void OnTargetChanged(GW::HookStatus*, GW::UI::UIMessage msgid, void* wParam, void*) {
		if (msgid != GW::UI::UIMessage::kChangeTarget) return;
		const auto* packet = static_cast<GW::UI::UIPacket::kChangeTarget*>(wParam);
		if (!packet) return;
		if (!packet->has_evaluated_target_changed && !packet->has_auto_target_changed && !packet->has_manual_target_changed) return;
		RefreshAllNametags();
	}

	static void OnAgentAllegianceChanged(GW::HookStatus*, GW::Packet::StoC::AgentUpdateAllegiance*) {
		RefreshAllNametags();
	}

	static void OnAgentMarkerChanged(GW::HookStatus*, GW::Packet::StoC::GenericValue* pak) {
		if (!pak) return;
		if (pak->value_id != GW::Packet::StoC::GenericValueID::apply_marker
			&& pak->value_id != GW::Packet::StoC::GenericValueID::remove_marker) return;
		RefreshAllNametags();
		RefreshTargetedNametagViaRetarget();
	}

	void HandleAgentNameTag(GW::HookStatus*, GW::UI::AgentNameTagInfo* tag) {
		if (!tag) return;

		GW::Agent* agent = GW::Agents::GetAgentByID(tag->agent_id);
		GW::AgentLiving* living = agent ? agent->GetAsAgentLiving() : nullptr;
		if (!living) return;

		const auto name_lookup = name_cache_.Get(living);
		if (const auto color = GetPriorityColor(*name_lookup.lower, *name_lookup.words)) {
			tag->text_color = *color;
			return;
		}

		const bool is_enemy = living->allegiance == GW::Constants::Allegiance::Enemy;

		if (is_enemy) {
			if (settings_.color_by_boss && living->GetHasBossGlow()) {
				tag->text_color = settings_.boss_color;
				return;
			}
			if (settings_.recolor_enemy_nameplates_by_profession) {
				if (const auto color = TryGetProfessionColor(name_lookup.profession)) {
					tag->text_color = *color;
				}
			}
			return;
		}

		if (settings_.recolor_quest_nametags && living->GetHasQuest()) {
			tag->text_color = settings_.quest_color;
			return;
		}

		if (settings_.recolor_professions
			&& living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable) {
			if (const auto color = TryGetProfessionColor(GetAgentProfession(living))) {
				tag->text_color = *color;
			}
		}
	}

	void DrawPriorityInput(const char* input_id, const char* hint, const char* color_id, uint32_t& color, char* buf, std::string& raw, std::vector<std::wstring>& names) {
		ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x));
		if (ImGui::InputTextWithHint(input_id, hint, buf, 512)) {
			raw = buf;
			names = ParseSemicolonNameList(raw);
		}
		ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorEdit3(color_id, &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
	}

	void DrawTargetProfessionDebug() {
		GW::AgentLiving* target = GW::Agents::GetTargetAsAgentLiving();
		const GW::Constants::ProfessionByte prof = target ? GetAgentProfession(target) : GW::Constants::ProfessionByte::None;
		char label[64];
		snprintf(label, sizeof(label), "Target profession: %s", GW::Constants::GetProfessionAcronym(static_cast<GW::Constants::Profession>(prof)));
		ImGui::TextUnformatted(label);
	}

	void DrawSettingsInternal() {
		ImGui::SeparatorText("Debug");
		DrawTargetProfessionDebug();

		ImGui::SeparatorText("Nametags");

		DrawCheckboxWithColorRightAligned("Color nametags by boss", settings_.color_by_boss, settings_.boss_color, "##color_by_boss");
		ShowHelpMarker("Overrides other nametag coloring (except Priority) for agents with the boss glow");

		DrawCheckboxWithColorRightAligned("Color quest-giver nametags", settings_.recolor_quest_nametags, settings_.quest_color, "##color_quest");

		ImGui::Checkbox("Color ally nametags by profession", &settings_.recolor_professions);

		ImGui::Checkbox("Color enemy nametags by profession", &settings_.recolor_enemy_nameplates_by_profession);
		ShowHelpMarker("Uses the profession colors below - if a monster's profession can't be determined, its normal color is used instead.");

		ImGui::Spacing();
		ImGui::TextUnformatted("Priority nametag coloring");
		ShowHelpMarker("Priority 1 matches full names exactly, semicolon-separated. \nPriority 2 matches whole words only, e.g. \"Monk\" matches \"Charr Monk\" but not \"Charrmonk\".");

		static constexpr std::array<const char*, 2> kPriorityHints = {
			"Keeper of Souls; Kournan Taskmaster",
			"monk; healer; priest; mender"
		};
		static constexpr std::array<const char*, 2> kPriorityInputIds = { "##priority_input_0", "##priority_input_1" };
		static constexpr std::array<const char*, 2> kPriorityColorIds = { "##priority_color_0", "##priority_color_1" };
		for (size_t i = 0; i < 2; ++i) {
			DrawPriorityInput(kPriorityInputIds[i], kPriorityHints[i], kPriorityColorIds[i], settings_.priorities[i].color, priority_states_[i].buf, settings_.priorities[i].raw, priority_states_[i].names);
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Profession colors");
		ShowHelpMarker("Used by 'Color ally nametags by profession' and 'Color enemy nametags by profession' above. Defaults match the classic ally-nametag profession colors.");

		if (ImGui::BeginTable("##profession_colors_table", 5)) {
			for (int c = 0; c < 5; ++c) {
				ImGui::TableSetupColumn("##pcol", ImGuiTableColumnFlags_WidthStretch);
			}
			for (size_t row = 0; row < 2; ++row) {
				ImGui::TableNextRow();
				for (size_t col = 0; col < 5; ++col) {
					ImGui::TableNextColumn();
					DrawProfessionCell(row * 5 + col + 1);
				}
			}
			ImGui::EndTable();
		}
	}
};

void NameplatesPlugin::DrawSettings() {
	ToolboxPlugin::DrawSettings();
	DrawSettingsInternal();
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance() {
	static NameplatesPlugin instance;
	return &instance;
}

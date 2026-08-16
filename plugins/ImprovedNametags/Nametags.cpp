#include <cstdint>
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
#include <PluginUtils.h>
#include <imgui.h>

#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <cwchar>
#include <optional>
#include <algorithm>
#include <array>

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

[[nodiscard]] inline std::vector<std::wstring> SplitWords(const std::wstring& text) {
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

[[nodiscard]] inline GW::Constants::ProfessionByte GetAgentProfession(const GW::AgentLiving* living) noexcept {
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

[[nodiscard]] inline std::vector<std::wstring> ParseSemicolonNameList(const std::string& raw) {
	std::vector<std::wstring> out;
	std::istringstream stream(raw);
	std::string token;
	while (std::getline(stream, token, ';')) {
		const size_t start = token.find_first_not_of(" \t\r\n");
		const size_t end = token.find_last_not_of(" \t\r\n");
		if (start == std::string::npos || end == std::string::npos) continue;

		std::wstring w = PluginUtils::StringToWString(token.substr(start, end - start + 1));
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

	bool priority_enabled = false;
	PriorityConfig priority = {"", IM_COL32(135, 206, 250, 255)};
};

class ImprovedNametagsPlugin : public ToolboxPlugin {
public:
	ImprovedNametagsPlugin() {
		GW::UI::RegisterUIMessageCallback(&nametag_hook_entry_, GW::UI::UIMessage::kShowAgentNameTag, OnAgentNameTag);
		GW::UI::RegisterUIMessageCallback(&nametag_hook_entry_, GW::UI::UIMessage::kSetAgentNameTagAttribs, OnAgentNameTag);
		GW::UI::RegisterUIMessageCallback(&quest_hook_entry_, GW::UI::UIMessage::kQuestAdded, OnQuestUpdate);
		GW::UI::RegisterUIMessageCallback(&quest_hook_entry_, GW::UI::UIMessage::kQuestDetailsChanged, OnQuestUpdate);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_, OnAgentAllegianceChanged, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_, OnAgentMarkerChanged, 1);
	}

	const char* Name() const override { return "ImprovedNametags"; }

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

		LoadSetting("priority_enabled", settings_.priority_enabled);
		LoadSetting("priority_raw", settings_.priority.raw);
		LoadSetting("priority_color", settings_.priority.color);
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

		SaveSetting("priority_enabled", settings_.priority_enabled);
		SaveSetting("priority_raw", settings_.priority.raw);
		SaveSetting("priority_color", settings_.priority.color);
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
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_);
	}

	void Draw(IDirect3DDevice9*) override {
		++frame_counter_;
		RefreshAllNametagsOnChange(last_recolor_professions_state_, settings_.recolor_professions, true);
		RefreshAllNametagsOnChange(last_recolor_quest_state_, settings_.recolor_quest_nametags, true);
		RefreshAllNametagsOnChange(last_recolor_enemy_profession_state_, settings_.recolor_enemy_nameplates_by_profession, true);
		RefreshAllNametagsOnChange(last_color_by_boss_state_, settings_.color_by_boss, true);
		RefreshAllNametagsOnChange(last_priority_enabled_state_, settings_.priority_enabled, true);

		if (const auto quest_log = GW::QuestMgr::GetQuestLog()) {
			const int quest_count = static_cast<int>(quest_log->size());
			if (last_quest_count_ != -1 && last_quest_count_ != quest_count) {
				RefreshAllNametags();
				RefreshTargetedNametagViaRetarget();
			}
			last_quest_count_ = quest_count;
		}

		name_cache_.MaybePrune();
		ProcessBossGlowRetries();
	}

private:
	NameplateSettings settings_;
	bool visible_ = true;
	std::optional<bool> last_recolor_professions_state_;
	std::optional<bool> last_recolor_quest_state_;
	std::optional<bool> last_recolor_enemy_profession_state_;
	std::optional<bool> last_color_by_boss_state_;
	std::optional<bool> last_priority_enabled_state_;
	int last_quest_count_ = -1;
	GW::HookEntry nametag_hook_entry_;
	GW::HookEntry quest_hook_entry_;
	GW::HookEntry allegiance_hook_entry_;
	GW::HookEntry marker_hook_entry_;

	AgentNameCache name_cache_;

	uint64_t frame_counter_ = 0;
	struct BossGlowRetry {
		uint32_t agent_id;
		uint64_t scheduled_frame;
	};
	std::vector<BossGlowRetry> boss_glow_retries_;

	void ScheduleBossGlowRetry(uint32_t agent_id) {
		for (const auto& r : boss_glow_retries_) {
			if (r.agent_id == agent_id) return;
		}
		boss_glow_retries_.push_back({agent_id, frame_counter_});
	}

	void ProcessBossGlowRetries() {
		for (auto it = boss_glow_retries_.begin(); it != boss_glow_retries_.end(); ) {
			if (frame_counter_ <= it->scheduled_frame) {
				++it;
				continue;
			}
			const uint32_t agent_id = it->agent_id;
			it = boss_glow_retries_.erase(it);

			GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
			GW::AgentLiving* living = agent ? agent->GetAsAgentLiving() : nullptr;
			if (living && living->GetHasBossGlow()) {
				RefreshAllNametags();
				RefreshTargetedNametagViaRetarget(true);
			}
		}
	}

	struct PriorityState {
		static constexpr size_t kBufSize = 1024 * 16;
		char buf[kBufSize] = {};
		std::vector<std::wstring> names;
		uint64_t pending_parse_at_ms = 0;
	};
	PriorityState priority_state_;
	static constexpr uint64_t kPriorityParseDelayMs = 150;

	static std::string SemicolonsToNewlines(const std::string& raw) {
		std::string out = raw;
		std::replace(out.begin(), out.end(), ';', '\n');
		return out;
	}

	static std::string NewlinesToSemicolons(const char* buf) {
		std::string out(buf);
		std::replace(out.begin(), out.end(), '\n', ';');
		return out;
	}

	void RefreshPriorityBuffersAndLists() {
		const std::string display = SemicolonsToNewlines(settings_.priority.raw);
		strncpy_s(priority_state_.buf, PriorityState::kBufSize, display.c_str(), PriorityState::kBufSize - 1);
		priority_state_.names = ParseSemicolonNameList(settings_.priority.raw);
		priority_state_.pending_parse_at_ms = 0;
	}

	[[nodiscard]] std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower, const std::vector<std::wstring>& words) const noexcept {
		if (!settings_.priority_enabled) return std::nullopt;
		if (!name_lower.empty()
			&& std::binary_search(priority_state_.names.begin(), priority_state_.names.end(), name_lower)) {
			return settings_.priority.color;
		}
		for (const auto& word : words) {
			if (std::binary_search(priority_state_.names.begin(), priority_state_.names.end(), word)) {
				return settings_.priority.color;
			}
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<ImU32> TryGetProfessionColor(GW::Constants::ProfessionByte prof) const noexcept {
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

	static void DrawCheckboxWithColorRightAligned(const char* label, bool& toggle, uint32_t& color, const char* color_id, const char* help = nullptr) {
		ImGui::Checkbox(label, &toggle);
		if (help) ShowHelpMarker(help);
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!toggle);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorEdit3(color_id, &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		ImGui::EndDisabled();
	}

	void DrawProfessionCell(size_t index) {
		ProfessionColorConfig& cfg = settings_.profession_colors[index];
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::Checkbox("##enabled", &cfg.enabled)) {
			RefreshAllNametags();
			RefreshTargetedNametagViaRetarget(true);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!cfg.enabled);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(cfg.color);
		if (ImGui::ColorEdit3("##color", &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			cfg.color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			RefreshAllNametags();
			RefreshTargetedNametagViaRetarget(true);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(GW::Constants::GetProfessionAcronym(static_cast<GW::Constants::Profession>(index)));
		ImGui::EndDisabled();
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
			if (also_retarget) RefreshTargetedNametagViaRetarget(true);
		}
		last_state = current_state;
	}

	static void OnAgentNameTag(GW::HookStatus* status, GW::UI::UIMessage msgid, void* wParam, void*) {
		if (msgid != GW::UI::UIMessage::kShowAgentNameTag && msgid != GW::UI::UIMessage::kSetAgentNameTagAttribs) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		self->HandleAgentNameTag(status, static_cast<GW::UI::AgentNameTagInfo*>(wParam));
	}

	static void RefreshTargetedNametagViaRetarget(bool allow_enemy = false) {
		GW::GameThread::Enqueue([allow_enemy] {
			const uint32_t target_id = GW::Agents::GetTargetId();
			if (target_id == 0) return;
			GW::Agent* target_agent = GW::Agents::GetAgentByID(target_id);
			GW::AgentLiving* target_living = target_agent ? target_agent->GetAsAgentLiving() : nullptr;
			if (!target_living) return;
			if (!allow_enemy && target_living->allegiance == GW::Constants::Allegiance::Enemy) return;
			GW::Agents::ChangeTarget(0u);
			GW::Agents::ChangeTarget(target_id);
		});
	}

	static void OnQuestUpdate(GW::HookStatus*, GW::UI::UIMessage msgid, void*, void*) {
		if (msgid != GW::UI::UIMessage::kQuestAdded
			&& msgid != GW::UI::UIMessage::kQuestDetailsChanged) return;
		RefreshAllNametags();
		RefreshTargetedNametagViaRetarget();
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
			if (settings_.color_by_boss) {
				if (living->GetHasBossGlow()) {
					tag->text_color = settings_.boss_color;
					return;
				}
				ScheduleBossGlowRetry(living->agent_id);
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
			if (const auto color = TryGetProfessionColor(name_lookup.profession)) {
				tag->text_color = *color;
			}
		}
	}

	void DrawPriorityInput(const char* input_id, PriorityState& state, std::string& raw) {
		if (ImGui::InputTextMultiline(input_id, state.buf, PriorityState::kBufSize, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.f))) {
			state.pending_parse_at_ms = GetTickCount64() + kPriorityParseDelayMs;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			raw = NewlinesToSemicolons(state.buf);
			state.names = ParseSemicolonNameList(raw);
			state.pending_parse_at_ms = 0;
			RefreshAllNametags();
			RefreshTargetedNametagViaRetarget(true);
		}
		else if (state.pending_parse_at_ms != 0 && GetTickCount64() >= state.pending_parse_at_ms) {
			raw = NewlinesToSemicolons(state.buf);
			state.names = ParseSemicolonNameList(raw);
			state.pending_parse_at_ms = 0;
		}
	}

	void DrawSettingsInternal() {
		ImGui::SeparatorText("Nametags");

		DrawCheckboxWithColorRightAligned("Color by boss", settings_.color_by_boss, settings_.boss_color, "##color_by_boss", "Overrides other nametag coloring (except Priority) for agents with the boss glow");

		DrawCheckboxWithColorRightAligned("Color by quest", settings_.recolor_quest_nametags, settings_.quest_color, "##color_quest");

		ImGui::Checkbox("##priority_enabled", &settings_.priority_enabled);
		ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextUnformatted("Priority coloring");
		ShowHelpMarker("One name per line. A single word (e.g. \"Monk\") matches any name containing that word. A full name (e.g. \"Keeper of Souls\") matches only that exact name.");
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!settings_.priority_enabled);
		ImVec4 priority_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.priority.color);
		if (ImGui::ColorEdit3("##priority_color", &priority_color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			settings_.priority.color = ImGui::ColorConvertFloat4ToU32(priority_color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			RefreshAllNametags();
			RefreshTargetedNametagViaRetarget(true);
		}

		DrawPriorityInput("##priority_input", priority_state_, settings_.priority.raw);
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::Checkbox("Color allies by profession", &settings_.recolor_professions);

		ImGui::Checkbox("Color foes by profession", &settings_.recolor_enemy_nameplates_by_profession);
		ShowHelpMarker("Uses the profession colors below - if a monster's profession can't be determined, its normal color is used instead.");

		ImGui::BeginDisabled(!settings_.recolor_professions && !settings_.recolor_enemy_nameplates_by_profession);
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
		ImGui::EndDisabled();
	}
};

void ImprovedNametagsPlugin::DrawSettings() {
	ToolboxPlugin::DrawSettings();
	DrawSettingsInternal();
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance() {
	static ImprovedNametagsPlugin instance;
	return &instance;
}

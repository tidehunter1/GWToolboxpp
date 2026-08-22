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
#include <GWCA/Managers/MapMgr.h>
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

	bool escape_to_embark = false;
	int escape_to_embark_threshold_pct = 10;

	bool show_healthbar_all_agents = false;
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
		L_SET(escape_to_embark); L_SET(escape_to_embark_threshold_pct);
		L_SET(show_healthbar_all_agents);
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
		S_SET(escape_to_embark); S_SET(escape_to_embark_threshold_pct);
		S_SET(show_healthbar_all_agents);
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
		UninstallPassthroughHook();
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

		UpdateEscapeToEmbark();

		if (last_show_healthbar_all_agents_state_.has_value() && *last_show_healthbar_all_agents_state_ && !settings_.show_healthbar_all_agents) {
			RevertHealthbarFlags();
		}
		last_show_healthbar_all_agents_state_ = settings_.show_healthbar_all_agents;
		UpdateHealthbarAllAgents();
		PruneCache(healthbar_flag_cache_, healthbar_flag_tick_, healthbar_flag_last_prune_tick_, kHealthbarFlagPruneIntervalTicks);

		if (const auto quest_log = GW::QuestMgr::GetQuestLog()) {
			const int quest_count = static_cast<int>(quest_log->size());
			if (last_quest_count_ != -1 && last_quest_count_ != quest_count) {
				RefreshAllNametags();
				RefreshTargetedNametag();
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

	bool embark_escape_armed_ = true;
	static constexpr float kEmbarkRearmHysteresisPct = 5.f;

	struct HealthbarFlagState {
		uint64_t last_seen_tick = 0;
	};
	std::unordered_map<uint32_t, HealthbarFlagState> healthbar_flag_cache_;
	uint64_t healthbar_flag_tick_ = 0, healthbar_flag_last_prune_tick_ = 0;
	static constexpr uint64_t kHealthbarFlagPruneIntervalTicks = 1800;
	uint64_t healthbar_last_discovery_ms_ = 0;
	static constexpr uint64_t kHealthbarDiscoveryIntervalMs = 200;
	std::optional<bool> last_show_healthbar_all_agents_state_;
	bool allegiance_color_test_performed_ = false;
	uintptr_t allegiance_color_test_func_addr_ = 0;
	bool allegiance_color_test_ok_[6] = {};
	uint32_t allegiance_color_test_result_[6] = {};
	std::string live_test_subject_name_;
	bool live_test_performed_ = false;
	int live_test_raw_allegiance_ = 0;
	uint32_t live_test_computed_color_ = 0;
	bool live_test_ok_ = false;
	bool hook_install_ok_ = true;

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
				RefreshTargetedNametag();
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

	[[nodiscard]] std::optional<ImU32> GetPriorityColor(const std::vector<std::wstring>& words) const noexcept {
		if (!settings_.priority_enabled) return std::nullopt;
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
			RefreshTargetedNametag();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!cfg.enabled);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(cfg.color);
		if (ImGui::ColorEdit3("##color", &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			cfg.color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			RefreshAllNametags();
			RefreshTargetedNametag();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(GW::Constants::GetProfessionAcronym(static_cast<GW::Constants::Profession>(index)));
		ImGui::EndDisabled();
		ImGui::PopID();
	}

	void UpdateEscapeToEmbark() {
		if (!settings_.escape_to_embark) {
			embark_escape_armed_ = true;
			return;
		}

		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (!me || me->GetIsDead()) return;
		if (!GW::Map::GetIsMapLoaded()) return;
		if (GW::Map::GetMapID() == GW::Constants::MapID::Embark_Beach) return;

		const float hp_pct = std::clamp(me->hp, 0.f, 1.f) * 100.f;
		const float threshold = static_cast<float>(settings_.escape_to_embark_threshold_pct);

		if (embark_escape_armed_ && hp_pct <= threshold) {
			embark_escape_armed_ = false;
			GW::GameThread::Enqueue([] {
				GW::Map::Travel(GW::Constants::MapID::Embark_Beach);
			});
		}
		else if (!embark_escape_armed_ && hp_pct > threshold + kEmbarkRearmHysteresisPct) {
			embark_escape_armed_ = true;
		}
	}

	using ManagerFindAgent_pt = void*(__cdecl*)(uint32_t agent_id);
	using SetFlagBit_pt = void(__thiscall*)(void* view_obj, uint32_t bitmask, int32_t on_off);

	static uintptr_t GetManagerFindAgentAddress() {
		static bool tried_resolve = false;
		static uintptr_t address = 0;
		if (!tried_resolve) {
			tried_resolve = true;
			address = GW::Scanner::Find(
				"\x55\x8b\xec\x8b\x4d\x08\x3b\x0d\x00\x00\x00\x00\x72\x04\x33\xc0\x5d\xc3\xa1\x00\x00\x00\x00\x8b\x04\x88\x5d\xc3\xcc\xcc\xcc\xcc\x55\x8b\xec\x8b\x4d\x08\x3b\x0d\x00\x00\x00\x00\x73\x20\xa1\x00\x00\x00\x00\x8b\x0c\x88\x85\xc9\x74\x14\x33\xc0\x81\xb9\x9c\x00\x00\x00",
				"xxxxxxxx????xxxxxxx????xxxxxxxxxxxxxxxxx????xxx????xxxxxxxxxxxxxxx"
			);
		}
		return address;
	}

	static uintptr_t GetSetFlagBitAddress() {
		static bool tried_resolve = false;
		static uintptr_t address = 0;
		if (!tried_resolve) {
			tried_resolve = true;
			address = GW::Scanner::Find(
				"\x55\x8b\xec\x83\xec\x64\x83\x7d\x0c\x00\x53\x57\x8b\xf9\x8b\x57\x58\x74\x07\x8b\xda\x0b\x5d\x08\xeb\x07\x8b\x5d\x08\xf7\xd3\x23\xda\x89\x5f\x58\x3b\xd3\x0f\x84\x36\x01\x00\x00\x8d\x45\xf4\x50\x8d\x45\x0c\x50\x52\xe8\x00\x00\x00\x00",
				"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????"
			);
		}
		return address;
	}

	using ComputeAllegianceColor_pt = uint32_t*(__thiscall*)(void* fake_context, uint32_t* out_color, int32_t param3);

	static uintptr_t GetComputeAllegianceColorAddress() {
		static bool tried_resolve = false;
		static uintptr_t address = 0;
		if (!tried_resolve) {
			tried_resolve = true;
			address = GW::Scanner::Find(
				"\x55\x8b\xec\x51\x56\x57\x8b\xf9\xf6\x87\x5c\x01\x00\x00\x08\x74\x09\xc7\x45\xfc\xa0\xa0\xa0\xff\xeb\x25\x8a\x87\xb5\x01\x00\x00\x3c\x03\x75\x09\xc7\x45\xfc\x00\x00\xff\xff\xeb\x12\xc7\x45\xfc\x00\xff\xa0\xff\x3c\x06\x74\x07\xc7\x45\xfc\x00\xff\x00\xff\x83",
				"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
			);
		}
		return address;
	}

	struct PassthroughHookState {
		bool installed = false;
		uintptr_t target_addr = 0;
		uint8_t original_bytes[6] = {};
		uint8_t* trampoline = nullptr;
	};
	PassthroughHookState hook_state_;

	bool InstallPassthroughHook() {
		if (hook_state_.installed) return true;
		const uintptr_t target = GetComputeAllegianceColorAddress();
		if (!target) return false;

		__try {
			memcpy(hook_state_.original_bytes, reinterpret_cast<void*>(target), 6);

			hook_state_.trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!hook_state_.trampoline) return false;

			memcpy(hook_state_.trampoline, hook_state_.original_bytes, 6);
			hook_state_.trampoline[6] = 0xE9;
			const uintptr_t jmp_back_target = target + 6;
			const uintptr_t jmp_instr_end = reinterpret_cast<uintptr_t>(hook_state_.trampoline) + 6 + 5;
			const int32_t rel_offset = static_cast<int32_t>(jmp_back_target - jmp_instr_end);
			memcpy(hook_state_.trampoline + 7, &rel_offset, 4);

			DWORD old_protect;
			if (!VirtualProtect(reinterpret_cast<void*>(target), 6, PAGE_EXECUTE_READWRITE, &old_protect)) {
				VirtualFree(hook_state_.trampoline, 0, MEM_RELEASE);
				hook_state_.trampoline = nullptr;
				return false;
			}

			uint8_t patch[6];
			patch[0] = 0xE9;
			const uintptr_t patch_jmp_target = reinterpret_cast<uintptr_t>(hook_state_.trampoline);
			const uintptr_t patch_instr_end = target + 5;
			const int32_t patch_rel_offset = static_cast<int32_t>(patch_jmp_target - patch_instr_end);
			memcpy(patch + 1, &patch_rel_offset, 4);
			patch[5] = 0x90;

			memcpy(reinterpret_cast<void*>(target), patch, 6);

			DWORD dummy;
			VirtualProtect(reinterpret_cast<void*>(target), 6, old_protect, &dummy);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 6);

			hook_state_.target_addr = target;
			hook_state_.installed = true;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			if (hook_state_.trampoline) {
				VirtualFree(hook_state_.trampoline, 0, MEM_RELEASE);
				hook_state_.trampoline = nullptr;
			}
			return false;
		}
	}

	void UninstallPassthroughHook() {
		if (!hook_state_.installed) return;
		__try {
			DWORD old_protect;
			if (VirtualProtect(reinterpret_cast<void*>(hook_state_.target_addr), 6, PAGE_EXECUTE_READWRITE, &old_protect)) {
				memcpy(reinterpret_cast<void*>(hook_state_.target_addr), hook_state_.original_bytes, 6);
				DWORD dummy;
				VirtualProtect(reinterpret_cast<void*>(hook_state_.target_addr), 6, old_protect, &dummy);
				FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(hook_state_.target_addr), 6);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		if (hook_state_.trampoline) {
			VirtualFree(hook_state_.trampoline, 0, MEM_RELEASE);
			hook_state_.trampoline = nullptr;
		}
		hook_state_.installed = false;
	}

	static bool TryComputeAllegianceColor(uintptr_t func_addr, uint8_t allegiance_byte, uint32_t& out_color) {
		if (!func_addr) return false;
		static uint8_t fake_obj[512];
		__try {
			memset(fake_obj, 0, sizeof(fake_obj));
			fake_obj[0x1b5] = allegiance_byte;
			uint32_t color = 0;
			reinterpret_cast<ComputeAllegianceColor_pt>(func_addr)(fake_obj, &color, 0);
			out_color = color;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static std::string WideToNarrow(const std::wstring& wide) {
		if (wide.empty()) return {};
		const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		if (len <= 0) return {};
		std::string out(static_cast<size_t>(len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
		return out;
	}

	GW::AgentLiving* FindNearestOtherLiving(std::string& out_name) {
		GW::AgentArray* agents = GW::Agents::GetAgentArray();
		if (!agents || !agents->valid()) return nullptr;
		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (!me) return nullptr;
		float best_dist_sq = -1.f;
		GW::AgentLiving* best_living = nullptr;
		for (GW::Agent* agent : *agents) {
			if (!agent || !agent->GetIsLivingType()) continue;
			GW::AgentLiving* living = agent->GetAsAgentLiving();
			if (!living || living->GetIsDead()) continue;
			if (living->agent_id == me->agent_id) continue;
			const float dx = living->pos.x - me->pos.x;
			const float dy = living->pos.y - me->pos.y;
			const float dist_sq = dx * dx + dy * dy;
			if (best_dist_sq < 0.f || dist_sq < best_dist_sq) {
				best_dist_sq = dist_sq;
				best_living = living;
			}
		}
		if (best_living) out_name = WideToNarrow(*name_cache_.Get(best_living).lower);
		return best_living;
	}

	static bool ApplyHealthbarFlag(uint32_t agent_id, bool on) {
		const uintptr_t manager_addr = GetManagerFindAgentAddress();
		const uintptr_t setflag_addr = GetSetFlagBitAddress();
		if (!manager_addr || !setflag_addr) return false;
		void* view_obj = reinterpret_cast<ManagerFindAgent_pt>(manager_addr)(agent_id);
		if (!view_obj) return false;
		const auto setflag_func = reinterpret_cast<SetFlagBit_pt>(setflag_addr);
		GW::GameThread::Enqueue([setflag_func, view_obj, on] { setflag_func(view_obj, 0x100, on ? 1 : 0); });
		return true;
	}

	void UpdateHealthbarAllAgents() {
		if (!settings_.show_healthbar_all_agents) return;
		const uint64_t now = GetTickCount64();
		if (now - healthbar_last_discovery_ms_ < kHealthbarDiscoveryIntervalMs) return;
		healthbar_last_discovery_ms_ = now;

		GW::AgentArray* agents = GW::Agents::GetAgentArray();
		if (!agents || !agents->valid()) return;
		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();

		for (GW::Agent* agent : *agents) {
			if (!agent || !agent->GetIsLivingType()) continue;
			GW::AgentLiving* living = agent->GetAsAgentLiving();
			if (!living || living->GetIsDead()) continue;
			if (me && living->agent_id == me->agent_id) continue;
			if (healthbar_flag_cache_.count(living->agent_id)) continue;
			if (ApplyHealthbarFlag(living->agent_id, true)) {
				healthbar_flag_cache_[living->agent_id] = { healthbar_flag_tick_ };
			}
		}
	}

	void RevertHealthbarFlags() {
		for (auto& pair : healthbar_flag_cache_) ApplyHealthbarFlag(pair.first, false);
		healthbar_flag_cache_.clear();
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
			if (also_retarget) RefreshTargetedNametag();
		}
		last_state = current_state;
	}

	static void OnAgentNameTag(GW::HookStatus* status, GW::UI::UIMessage msgid, void* wParam, void*) {
		if (msgid != GW::UI::UIMessage::kShowAgentNameTag && msgid != GW::UI::UIMessage::kSetAgentNameTagAttribs) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		self->HandleAgentNameTag(status, static_cast<GW::UI::AgentNameTagInfo*>(wParam));
	}

	static void RefreshTargetedNametag() {
		GW::GameThread::Enqueue([] {
			const uint32_t target_id = GW::Agents::GetTargetId();
			if (target_id == 0) return;
			GW::Agent* target_agent = GW::Agents::GetAgentByID(target_id);
			GW::AgentLiving* target_living = target_agent ? target_agent->GetAsAgentLiving() : nullptr;
			if (!target_living) return;
			GW::Agents::RefreshAgentNameTag(target_agent);
		});
	}

	static void OnQuestUpdate(GW::HookStatus*, GW::UI::UIMessage msgid, void*, void*) {
		if (msgid != GW::UI::UIMessage::kQuestAdded
			&& msgid != GW::UI::UIMessage::kQuestDetailsChanged) return;
		RefreshAllNametags();
		RefreshTargetedNametag();
	}

	static void OnAgentAllegianceChanged(GW::HookStatus*, GW::Packet::StoC::AgentUpdateAllegiance*) {
		RefreshAllNametags();
	}

	static void OnAgentMarkerChanged(GW::HookStatus*, GW::Packet::StoC::GenericValue* pak) {
		if (!pak) return;
		if (pak->value_id != GW::Packet::StoC::GenericValueID::apply_marker
			&& pak->value_id != GW::Packet::StoC::GenericValueID::remove_marker) return;
		RefreshAllNametags();
		RefreshTargetedNametag();
	}

	void HandleAgentNameTag(GW::HookStatus*, GW::UI::AgentNameTagInfo* tag) {
		if (!tag) return;

		GW::Agent* agent = GW::Agents::GetAgentByID(tag->agent_id);
		GW::AgentLiving* living = agent ? agent->GetAsAgentLiving() : nullptr;
		if (!living) return;

		const auto name_lookup = name_cache_.Get(living);
		if (const auto color = GetPriorityColor(*name_lookup.words)) {
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
			RefreshTargetedNametag();
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
		ShowHelpMarker("One name per line. Any single word (e.g. \"Monk\") matches any name containing that exact word.");
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!settings_.priority_enabled);
		ImVec4 priority_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.priority.color);
		if (ImGui::ColorEdit3("##priority_color", &priority_color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			settings_.priority.color = ImGui::ColorConvertFloat4ToU32(priority_color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			RefreshAllNametags();
			RefreshTargetedNametag();
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

		ImGui::Spacing();
		ImGui::SeparatorText("Safety");

		ImGui::Checkbox("Escape to Embark Beach", &settings_.escape_to_embark);
		ShowHelpMarker("Teleports you to Embark Beach based on health % threshold");

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::SliderInt("##embark_threshold", &settings_.escape_to_embark_threshold_pct, 1, 100, "%d%%");

		ImGui::Spacing();
		ImGui::SeparatorText("Health Bars");
		ImGui::Checkbox("Show health bar on all agents", &settings_.show_healthbar_all_agents);
		ShowHelpMarker("Shows the same floating health bar you get from hovering over a unit, on all nearby agents at once.");

		ImGui::Spacing();
		ImGui::SeparatorText("Experimental: Allegiance Color Test");
		ImGui::TextWrapped("Pure computation only, no real agent touched. Safe to test.");
		if (ImGui::Button("Test: compute colors for all 6 allegiance values")) {
			allegiance_color_test_performed_ = true;
			const uintptr_t func_addr = GetComputeAllegianceColorAddress();
			allegiance_color_test_func_addr_ = func_addr;
			static const uint8_t kAllegianceValues[6] = { 1, 2, 3, 4, 5, 6 };
			for (int i = 0; i < 6; ++i) {
				uint32_t color = 0;
				allegiance_color_test_ok_[i] = TryComputeAllegianceColor(func_addr, kAllegianceValues[i], color);
				allegiance_color_test_result_[i] = color;
			}
		}
		if (allegiance_color_test_performed_) {
			ImGui::Text("Function address: 0x%08X", static_cast<unsigned>(allegiance_color_test_func_addr_));
			static const char* kLabels[6] = { "1=Ally", "2=Neutral", "3=Enemy", "4=Spirit/Pet", "5=Minion", "6=NPC/Minipet" };
			for (int i = 0; i < 6; ++i) {
				if (allegiance_color_test_ok_[i]) {
					ImGui::Text("%s -> 0x%08X", kLabels[i], allegiance_color_test_result_[i]);
				} else {
					ImGui::Text("%s -> (call failed)", kLabels[i]);
				}
			}
		}

		ImGui::Spacing();
		if (ImGui::Button("Test: compute color for nearest other agent")) {
			live_test_performed_ = false;
			GW::AgentLiving* living = FindNearestOtherLiving(live_test_subject_name_);
			if (living) {
				live_test_raw_allegiance_ = static_cast<int>(living->allegiance);
				const uintptr_t func_addr = GetComputeAllegianceColorAddress();
				live_test_ok_ = TryComputeAllegianceColor(func_addr, static_cast<uint8_t>(live_test_raw_allegiance_), live_test_computed_color_);
				live_test_performed_ = true;
			}
		}
		if (live_test_performed_) {
			ImGui::Text("Subject: \"%s\"", live_test_subject_name_.c_str());
			ImGui::Text("Real allegiance value: %d", live_test_raw_allegiance_);
			if (live_test_ok_) {
				ImGui::Text("Computed color: 0x%08X", live_test_computed_color_);
				ImGui::TextWrapped("Compare this against what you actually see on this agent in-game (red ring = enemy, green = ally/neutral).");
			} else {
				ImGui::Text("Computation failed.");
			}
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Experimental: Passthrough Hook Test");
		ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Rewrites live game code. Highest risk test today.");
		ImGui::TextWrapped("Installs a hook that changes nothing - proves the mechanism is safe before any real override logic gets added.");
		bool hook_installed = hook_state_.installed;
		if (ImGui::Checkbox("Install passthrough hook", &hook_installed)) {
			if (hook_installed) {
				hook_install_ok_ = InstallPassthroughHook();
			} else {
				UninstallPassthroughHook();
			}
		}
		ImGui::Text("Status: %s", hook_state_.installed ? "installed" : "not installed");
		if (hook_state_.installed) {
			ImGui::TextWrapped("Now click the 'compute colors for all 6 allegiance values' button above again. Results must be IDENTICAL to before installing - that proves the hook round-trips correctly through the original function.");
		}
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

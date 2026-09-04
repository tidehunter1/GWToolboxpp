#include <cstdint>
#include <cstring>
#include <cstddef>
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
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Packets/StoC.h>

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
#include <cmath>
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
	if (living->IsPlayer() || living->primary != GW::Constants::ProfessionByte::None) return living->primary;
	const GW::NPC* npc = GW::Agents::GetNPCByID(living->player_number);
	return npc ? static_cast<GW::Constants::ProfessionByte>(npc->primary) : GW::Constants::ProfessionByte::None;
}

template<typename FuncPtr>
inline FuncPtr ResolveScannedFunc(FuncPtr& cached, const char* pattern, const char* mask) {
	if (!cached) {
		const uintptr_t addr = GW::Scanner::Find(pattern, mask);
		if (addr) cached = reinterpret_cast<FuncPtr>(addr);
	}
	return cached;
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

[[nodiscard]] inline std::vector<std::wstring> ParseNameList(const std::string& raw) {
	std::vector<std::wstring> out;
	std::istringstream stream(raw);
	std::string token;
	while (std::getline(stream, token, '\n')) {
		const size_t start = token.find_first_not_of(" \t\r\n");
		const size_t end = token.find_last_not_of(" \t\r\n");
		if (start == std::string::npos || end == std::string::npos) continue;

		std::wstring w = PluginUtils::StringToWString(token.substr(start, end - start + 1));
		std::transform(w.begin(), w.end(), w.begin(), ::towlower);
		if (!w.empty()) out.push_back(std::move(w));
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
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

struct NametagSettings {
	bool recolor_quest_nametags = true, recolor_professions = false;
	bool recolor_enemy_nametags_by_profession = false;
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
		GW::UI::RegisterUIMessageCallback(&target_change_hook_entry_, GW::UI::UIMessage::kChangeTarget, OnTargetChanged);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_, OnAgentAllegianceChanged, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentAdd>(&agent_add_hook_entry_, OnAgentAdd, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentRemove>(&agent_remove_hook_entry_, OnAgentRemove, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_, OnAgentMarkerChanged, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::MapLoaded>(&map_loaded_hook_entry_, OnMapLoaded, 1);
		GW::UI::RegisterUIMessageCallback(&chat_suppress_hook_entry_, GW::UI::UIMessage::kWriteToChatLog, OnChatLogWrite);
		GW::UI::RegisterUIMessageCallback(&chat_suppress_hook_entry_, GW::UI::UIMessage::kWriteToChatLogWithSender, OnChatLogWriteWithSender);
		GW::UI::RegisterKeydownCallback(&reveal_hotkey_hook_entry_, OnRevealHotkeyDown);
		GW::UI::RegisterKeyupCallback(&reveal_hotkey_hook_entry_, OnRevealHotkeyUp);
	}

	const char* Name() const override { return "ImprovedNametags"; }

	bool* GetVisiblePtr() override { return &visible_; }

	[[nodiscard]] bool HasSettings() const override { return true; }
	void DrawSettings() override;

	void LoadSettings(const wchar_t* folder) override {
		ToolboxPlugin::LoadSettings(folder);
		#define L_SET(var) LoadSetting(#var, settings_.var)
		L_SET(recolor_quest_nametags); L_SET(recolor_professions);
		L_SET(recolor_enemy_nametags_by_profession);
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
		S_SET(recolor_enemy_nametags_by_profession);
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
		RemoveAllegianceColorHook();
		GW::UI::RemoveUIMessageCallback(&chat_suppress_hook_entry_);
		GW::UI::RemoveUIMessageCallback(&target_change_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentAdd>(&agent_add_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentRemove>(&agent_remove_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::MapLoaded>(&map_loaded_hook_entry_);
		GW::UI::RemoveKeydownCallback(&reveal_hotkey_hook_entry_);
		GW::UI::RemoveKeyupCallback(&reveal_hotkey_hook_entry_);
	}

	void Draw(IDirect3DDevice9*) override {
		++frame_counter_;
		EnsureAllegianceColorHookInstalled();

		if (dirty_rescan_) {
			dirty_rescan_ = false;
			RescanAllAgentsForHealthbar();
		}

		UpdateEscapeToEmbark();

		name_cache_.MaybePrune();
		ProcessBossGlowRetries();
	}

private:
	NametagSettings settings_;
	bool visible_ = true;
	bool dirty_rescan_ = true;
	bool ctrl_reveal_down_ = false;
	bool alt_reveal_down_ = false;
	bool hide_hotkey_active_ = false;
	bool hotkey_saved_show_healthbar_all_ = false;
	GW::HookEntry allegiance_hook_entry_;
	GW::HookEntry agent_add_hook_entry_;
	GW::HookEntry agent_remove_hook_entry_;
	GW::HookEntry marker_hook_entry_;
	GW::HookEntry target_change_hook_entry_;
	GW::HookEntry map_loaded_hook_entry_;
	GW::HookEntry chat_suppress_hook_entry_;
	GW::HookEntry reveal_hotkey_hook_entry_;

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
				RefreshHealthbarForAgent(agent);
			}
		}
	}

	bool embark_escape_armed_ = true;
	static constexpr float kEmbarkRearmHysteresisPct = 5.f;

	struct AgentState {
		bool we_applied_flag = false;
		bool has_quest_marker = false;
	};
	std::vector<AgentState> agent_state_;

	struct PriorityState {
		static constexpr size_t kBufSize = 1024 * 16;
		char buf[kBufSize] = {};
		std::vector<std::wstring> names;
		uint64_t pending_parse_at_ms = 0;
	};
	PriorityState priority_state_;
	static constexpr uint64_t kPriorityParseDelayMs = 150;

	void RefreshPriorityBuffersAndLists() {
		strncpy_s(priority_state_.buf, PriorityState::kBufSize, settings_.priority.raw.c_str(), _TRUNCATE);
		priority_state_.names = ParseNameList(settings_.priority.raw);
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

	[[nodiscard]] static bool DrawCheckboxWithColorRightAligned(const char* label, bool& toggle, uint32_t& color, const char* color_id, const char* help = nullptr) {
		bool changed = ImGui::Checkbox(label, &toggle);
		if (help) ShowHelpMarker(help);
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!toggle);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorEdit3(color_id, &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
		ImGui::EndDisabled();
		return changed;
	}

	void DrawProfessionCell(size_t index) {
		ProfessionColorConfig& cfg = settings_.profession_colors[index];
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::Checkbox("##enabled", &cfg.enabled)) {
			dirty_rescan_ = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!cfg.enabled);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(cfg.color);
		if (ImGui::ColorEdit3("##color", &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			cfg.color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			dirty_rescan_ = true;
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

	using AllegianceColorFn_pt = uint32_t*(__thiscall*)(void*, uint32_t*, int32_t);
	static inline AllegianceColorFn_pt AllegianceColor_Func = nullptr;
	static inline AllegianceColorFn_pt AllegianceColor_Ret = nullptr;

	static uint32_t* __thiscall OnAllegianceColor(void* ctx, uint32_t* out_color, int32_t flag) {
		GW::Hook::EnterHook();
		uint32_t* result = AllegianceColor_Ret(ctx, out_color, flag);
		if (result) {
			auto* agent = static_cast<GW::Agent*>(ctx);
			GW::AgentLiving* living = agent->GetAsAgentLiving();
			if (living) {
				auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
				if (const auto color = self->DecideAgentColor(living)) {
					*result = static_cast<uint32_t>(*color);
				}
			}
		}
		GW::Hook::LeaveHook();
		return result;
	}

	void EnsureAllegianceColorHookInstalled() {
		if (AllegianceColor_Func) return;
		if (!ResolveScannedFunc(AllegianceColor_Func,
			"\x55\x8b\xec\x51\x56\x57\x8b\xf9\xf6\x87\x5c\x01\x00\x00\x08\x74\x09\xc7\x45\xfc\xa0\xa0\xa0\xff\xeb\x25\x8a\x87\xb5\x01\x00\x00\x3c\x03\x75\x09\xc7\x45\xfc\x00\x00\xff\xff\xeb\x12\xc7\x45\xfc\x00\xff\xa0\xff\x3c\x06\x74\x07",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")) return;
		GW::Hook::CreateHook(&AllegianceColor_Func, OnAllegianceColor, &AllegianceColor_Ret);
		GW::Hook::EnableHooks(AllegianceColor_Func);
	}

	void RemoveAllegianceColorHook() {
		if (AllegianceColor_Func) {
			GW::Hook::DisableHooks(AllegianceColor_Func);
			GW::Hook::RemoveHook(AllegianceColor_Func);
			AllegianceColor_Func = nullptr;
			AllegianceColor_Ret = nullptr;
		}
	}

	using EvaluatedTargetWrapper_pt = void(__thiscall*)(void*, uint32_t);
	static inline EvaluatedTargetWrapper_pt EvaluatedTargetWrapper_Func = nullptr;

	static bool EnsureEvaluatedTargetWrapperScanned() {
		if (EvaluatedTargetWrapper_Func) return true;
		if (!ResolveScannedFunc(EvaluatedTargetWrapper_Func,
			"\x55\x8b\xec\x8b\x55\x08\x56\x8b\xf1\x8b\x4e\x58\x8b\xc1\xc1\xe8\x07\x83\xe0\x01\x3b\xd0\x74\x7c\x85\xd2\x74\x3e\xf6\xc1\x10\x74\x1d\x8b\x86\x98\x00\x00\x00\x85",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")) return false;
		return true;
	}

	static void ApplyRingRefreshIfTarget(GW::Agent* agent, uint32_t agent_id) {
		if (EvaluatedTargetWrapper_Func && GW::Agents::GetTargetId() == agent_id) {
			EvaluatedTargetWrapper_Func(agent, 0);
			EvaluatedTargetWrapper_Func(agent, 1);
		}
	}

	static void RefreshTargetedRing(uint32_t agent_id) {
		if (!EnsureEvaluatedTargetWrapperScanned()) return;
		GW::GameThread::Enqueue([agent_id] {
			GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
			if (!agent) return;
			ApplyRingRefreshIfTarget(agent, agent_id);
		});
	}

	using SetNameTagBit_pt = void(__thiscall*)(void*, uint32_t, int);
	static inline SetNameTagBit_pt SetNameTagBit_Func = nullptr;

	static bool EnsureSetNameTagBitScanned() {
		if (SetNameTagBit_Func) return true;
		if (!ResolveScannedFunc(SetNameTagBit_Func,
			"\x55\x8b\xec\x83\xec\x64\x83\x7d\x0c\x00\x53\x57\x8b\xf9\x8b\x57\x58\x74\x07\x8b\xda\x0b\x5d\x08\xeb\x07\x8b\x5d\x08\xf7\xd3\x23\xda\x89\x5f\x58\x3b\xd3\x0f\x84\x36\x01\x00\x00\x8d\x45\xf4\x50\x8d\x45\x0c\x50\x52",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")) return false;
		return true;
	}

	using QueueEventAllocator_pt = void*(__thiscall*)(void*, uint32_t);
	static inline QueueEventAllocator_pt QueueEventAllocator_Func = nullptr;

	static bool EnsureQueueEventAllocatorScanned() {
		if (QueueEventAllocator_Func) return true;
		if (!ResolveScannedFunc(QueueEventAllocator_Func,
			"\x55\x8b\xec\x53\x56\x57\x8b\xf9\xe8\x23\x3b\xff\xff\x8b\x55\x08\x8b\xd8\x89\x13\x8b\x57\x2c\x89\x53\x04\xc7\x43\x08\x00\x00\x00\x00\x81\xbf\x40\x01\x00\x00\xdd\xdd\xdd\xdd\x75\x14\x68\x87\x01\x00\x00\xba\x38\xdf\x93\x00\xb9\xbc\xdf\x93\x00\xe8\xcf\x23\xc9\xff\x8b\xb7\x40\x01\x00\x00\x03\xf3\x8b\x16\x8b\x4e\x04\x8b\x06\x83\xe1\xfe\x8b\x40\x04\x83\xe0\xfe\x2b\xc8\x89\x14\x31\x8b\x4e\x04\x8b\x06\x89\x48\x04\x8b\x87\x44\x01\x00\x00\x89\x06\x8b\x06\x8b\x40\x04\x89\x46\x04\x8b\x87\x44\x01\x00\x00\x89\x58\x04\x89\xb7\x44\x01\x00\x00\xa1\x38\xa8\x08\x01\x85\xc0\x0f\x84\xc0\x00\x00\x00\x50",
			"xxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????x????x????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxx")) return false;
		return true;
	}

	static void TriggerAllegianceRecolor(GW::Agent* agent, uint32_t allegiance_value) {
		if (!QueueEventAllocator_Func) return;
		void* node = QueueEventAllocator_Func(agent, 8);
		if (!node) return;
		*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(node) + 0x1c) = allegiance_value;
	}

	void OnRevealHotkeyStateChanged() {
		const bool now_active = ctrl_reveal_down_ || alt_reveal_down_;
		if (now_active == hide_hotkey_active_) return;
		hide_hotkey_active_ = now_active;
		if (now_active) {
			hotkey_saved_show_healthbar_all_ = settings_.show_healthbar_all_agents;
			settings_.show_healthbar_all_agents = false;
		} else {
			settings_.show_healthbar_all_agents = hotkey_saved_show_healthbar_all_;
		}
		dirty_rescan_ = true;
	}

	void OnRevealHotkeyKeyEvent(uint32_t key, bool down) {
		if (key == GW::UI::ControlAction_ShowOthers) ctrl_reveal_down_ = down;
		else if (key == GW::UI::ControlAction_ShowTargets) alt_reveal_down_ = down;
		else return;
		OnRevealHotkeyStateChanged();
	}

	static void OnRevealHotkeyDown(GW::HookStatus*, uint32_t key) {
		static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance())->OnRevealHotkeyKeyEvent(key, true);
	}

	static void OnRevealHotkeyUp(GW::HookStatus*, uint32_t key) {
		static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance())->OnRevealHotkeyKeyEvent(key, false);
	}

	void UpdateManualTargetFlag(GW::AgentLiving* living, AgentState& state) {
		const uint32_t agent_id = living->agent_id;
		const bool want_flag = settings_.show_healthbar_all_agents;
		if (want_flag == state.we_applied_flag) return;

		EnsureSetNameTagBitScanned();
		state.we_applied_flag = want_flag;
		GW::GameThread::Enqueue([agent_id, want_flag] {
			GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
			if (!agent) return;
			if (SetNameTagBit_Func) {
				SetNameTagBit_Func(agent, GW::NameTagFlags_ManualTarget, want_flag ? 1 : 0);
			}
		});
	}

	void TriggerAllegianceRecolorForAgentId(uint32_t agent_id) {
		EnsureQueueEventAllocatorScanned();
		GW::GameThread::Enqueue([agent_id] {
			GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
			if (!agent) return;
			GW::AgentLiving* fresh_living = agent->GetAsAgentLiving();
			if (!fresh_living) return;
			const uint32_t allegiance_value = static_cast<uint32_t>(fresh_living->allegiance);
			TriggerAllegianceRecolor(agent, allegiance_value);
			GW::Agents::RefreshAgentNameTag(agent);
		});
	}

	void UpdateAgentHealthbarState(GW::AgentLiving* living, AgentState& state) {
		UpdateManualTargetFlag(living, state);
		TriggerAllegianceRecolorForAgentId(living->agent_id);
	}

	void RefreshHealthbarForAgent(GW::Agent* agent) {
		if (!agent || !agent->GetIsLivingType()) return;
		GW::AgentLiving* living = agent->GetAsAgentLiving();
		if (!living || living->GetIsDead()) return;
		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (me && living->agent_id == me->agent_id) return;

		if (living->agent_id >= agent_state_.size()) {
			agent_state_.resize(living->agent_id + 128);
		}
		UpdateAgentHealthbarState(living, agent_state_[living->agent_id]);
	}

	void RefreshManualTargetFlagForAgentId(uint32_t agent_id) {
		GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
		if (!agent || !agent->GetIsLivingType()) return;
		GW::AgentLiving* living = agent->GetAsAgentLiving();
		if (!living || living->GetIsDead()) return;
		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (me && living->agent_id == me->agent_id) return;

		if (living->agent_id >= agent_state_.size()) {
			agent_state_.resize(living->agent_id + 128);
		}
		UpdateManualTargetFlag(living, agent_state_[living->agent_id]);
	}

	void RefreshHealthbarForAgentId(uint32_t agent_id) {
		RefreshHealthbarForAgent(GW::Agents::GetAgentByID(agent_id));
	}

	[[nodiscard]] bool HasQuestMarker(uint32_t agent_id) const {
		return agent_id < agent_state_.size() && agent_state_[agent_id].has_quest_marker;
	}

	void RescanAllAgentsForHealthbar() {
		GW::AgentArray* agents = GW::Agents::GetAgentArray();
		if (!agents || !agents->valid()) return;
		for (GW::Agent* agent : *agents) {
			RefreshHealthbarForAgent(agent);
		}
	}

	static void OnTargetChanged(GW::HookStatus*, GW::UI::UIMessage, void* wParam, void*) {
		auto* pak = static_cast<GW::UI::UIPacket::kChangeTarget*>(wParam);
		if (!pak || !pak->has_evaluated_target_changed || pak->evaluated_target_id == 0) return;
		RefreshTargetedRing(pak->evaluated_target_id);
	}

	static void OnAgentAllegianceChanged(GW::HookStatus*, GW::Packet::StoC::AgentUpdateAllegiance* pak) {
		if (!pak) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		self->RefreshManualTargetFlagForAgentId(pak->agent_id);
		const uint32_t agent_id = pak->agent_id;
		GW::GameThread::Enqueue([agent_id] {
			GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
			if (agent) GW::Agents::RefreshAgentNameTag(agent);
		});
	}

	static void OnAgentAdd(GW::HookStatus*, GW::Packet::StoC::AgentAdd* pak) {
		if (!pak) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		self->RefreshHealthbarForAgentId(pak->agent_id);
	}

	static void OnMapLoaded(GW::HookStatus*, GW::Packet::StoC::MapLoaded*) {
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		self->agent_state_.clear();
		self->boss_glow_retries_.clear();
		self->dirty_rescan_ = true;
	}

	static void OnAgentRemove(GW::HookStatus*, GW::Packet::StoC::AgentRemove* pak) {
		if (!pak) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		if (pak->agent_id < self->agent_state_.size()) {
			self->agent_state_[pak->agent_id] = AgentState{};
		}
	}

	static void OnAgentMarkerChanged(GW::HookStatus*, GW::Packet::StoC::GenericValue* pak) {
		if (!pak) return;
		if (pak->value_id != GW::Packet::StoC::GenericValueID::apply_marker
			&& pak->value_id != GW::Packet::StoC::GenericValueID::remove_marker) return;

		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		const bool applying = pak->value_id == GW::Packet::StoC::GenericValueID::apply_marker;
		if (pak->agent_id >= self->agent_state_.size()) {
			self->agent_state_.resize(pak->agent_id + 128);
		}
		self->agent_state_[pak->agent_id].has_quest_marker = applying;
		self->RefreshHealthbarForAgentId(pak->agent_id);
	}

	static constexpr int kExpectedWarningMessages = 3;
	int warnings_blocked_ = 0;

	[[nodiscard]] bool ShouldSuppressWarning(uint32_t channel) {
		if (warnings_blocked_ >= kExpectedWarningMessages) return false;
		if (channel != GW::Chat::Channel::CHANNEL_GWCA2 && channel != GW::Chat::Channel::CHANNEL_WARNING) return false;
		++warnings_blocked_;
		return true;
	}

	static void OnChatLogWrite(GW::HookStatus* status, GW::UI::UIMessage, void* wParam, void*) {
		auto* msg = static_cast<GW::UI::UIPacket::kWriteToChatLog*>(wParam);
		if (!msg) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		if (self->ShouldSuppressWarning(static_cast<uint32_t>(msg->channel))) {
			status->blocked = true;
		}
	}

	static void OnChatLogWriteWithSender(GW::HookStatus* status, GW::UI::UIMessage, void* wParam, void*) {
		auto* msg = static_cast<GW::UI::UIPacket::kWriteToChatLogWithSender*>(wParam);
		if (!msg) return;
		auto* self = static_cast<ImprovedNametagsPlugin*>(ToolboxPluginInstance());
		if (self->ShouldSuppressWarning(msg->channel)) {
			status->blocked = true;
		}
	}

	[[nodiscard]] std::optional<ImU32> DecideAgentColor(const GW::AgentLiving* living) {
		if (!living) return std::nullopt;

		if (settings_.priority_enabled) {
			if (const auto color = GetPriorityColor(*name_cache_.Get(living).words)) {
				return color;
			}
		}

		const bool is_enemy = living->allegiance == GW::Constants::Allegiance::Enemy;

		if (is_enemy) {
			if (settings_.color_by_boss) {
				if (living->GetHasBossGlow()) {
					return settings_.boss_color;
				}
				ScheduleBossGlowRetry(living->agent_id);
			}
			if (settings_.recolor_enemy_nametags_by_profession) {
				if (const auto color = TryGetProfessionColor(name_cache_.Get(living).profession)) {
					return color;
				}
			}
			return std::nullopt;
		}

		if (settings_.recolor_quest_nametags
			&& (living->GetHasQuest() || HasQuestMarker(living->agent_id))) {
			return settings_.quest_color;
		}

		if (settings_.recolor_professions
			&& living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable) {
			if (const auto color = TryGetProfessionColor(name_cache_.Get(living).profession)) {
				return color;
			}
		}

		return std::nullopt;
	}

	void DrawPriorityInput(const char* input_id, PriorityState& state, std::string& raw) {
		static constexpr int kMaxNameLength = 40;
		const float box_width = (ImGui::CalcTextSize("M").x * kMaxNameLength + ImGui::GetStyle().FramePadding.x * 2.0f) * 0.5f;
		if (ImGui::InputTextMultiline(input_id, state.buf, PriorityState::kBufSize, ImVec2(box_width, ImGui::GetTextLineHeight() * 8.f))) {
			state.pending_parse_at_ms = GetTickCount64() + kPriorityParseDelayMs;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			raw = state.buf;
			state.names = ParseNameList(raw);
			state.pending_parse_at_ms = 0;
			dirty_rescan_ = true;
		}
		else if (state.pending_parse_at_ms != 0 && GetTickCount64() >= state.pending_parse_at_ms) {
			raw = state.buf;
			state.names = ParseNameList(raw);
			state.pending_parse_at_ms = 0;
		}
	}

	void DrawSettingsInternal() {
		ImGui::SeparatorText("Nametags");

		if (DrawCheckboxWithColorRightAligned("Color by boss", settings_.color_by_boss, settings_.boss_color, "##color_by_boss", "Overrides other nametag coloring (except Priority) for agents with the boss glow")) {
			dirty_rescan_ = true;
		}

		if (DrawCheckboxWithColorRightAligned("Color by quest", settings_.recolor_quest_nametags, settings_.quest_color, "##color_quest")) {
			dirty_rescan_ = true;
		}

		if (ImGui::Checkbox("##priority_enabled", &settings_.priority_enabled)) {
			dirty_rescan_ = true;
		}
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
			dirty_rescan_ = true;
		}

		char priority_header_label[48];
		snprintf(priority_header_label, sizeof(priority_header_label), "Priority Names (%zu)###priority_names", priority_state_.names.size());
		if (ImGui::TreeNodeEx(priority_header_label, ImGuiTreeNodeFlags_FramePadding)) {
			DrawPriorityInput("##priority_input", priority_state_, settings_.priority.raw);
			ImGui::TreePop();
		}
		ImGui::EndDisabled();

		ImGui::Spacing();
		if (ImGui::Checkbox("Color allies by profession", &settings_.recolor_professions)) {
			dirty_rescan_ = true;
		}

		if (ImGui::Checkbox("Color foes by profession", &settings_.recolor_enemy_nametags_by_profession)) {
			dirty_rescan_ = true;
		}
		ShowHelpMarker("Uses the profession colors below - if a monster's profession can't be determined, its normal color is used instead.");

		ImGui::BeginDisabled(!settings_.recolor_professions && !settings_.recolor_enemy_nametags_by_profession);
		size_t enabled_profession_count = 0;
		for (size_t i = 1; i < settings_.profession_colors.size(); ++i) {
			if (settings_.profession_colors[i].enabled) ++enabled_profession_count;
		}
		char profession_header_label[48];
		snprintf(profession_header_label, sizeof(profession_header_label), "Profession Colors (%zu)###profession_colors", enabled_profession_count);
		if (ImGui::TreeNodeEx(profession_header_label, ImGuiTreeNodeFlags_FramePadding)) {
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
			ImGui::TreePop();
		}
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::SeparatorText("Safety");

		ImGui::Checkbox("Escape to Embark Beach", &settings_.escape_to_embark);
		ShowHelpMarker("Teleports you to Embark Beach based on health % threshold");

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::DragInt("##embark_threshold", &settings_.escape_to_embark_threshold_pct, 1.0f, 1, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);

		ImGui::Spacing();
		ImGui::SeparatorText("Health Bars");
		if (ImGui::Checkbox("Show health bar on all agents", &settings_.show_healthbar_all_agents)) {
			dirty_rescan_ = true;
		}
		ShowHelpMarker("Shows the same floating health bar you get from hovering over a unit, on all nearby agents at once.");

		ImGui::Spacing();
		ImGui::SeparatorText("Debug: allegiance inspector (read-only)");
		if (ImGui::CollapsingHeader("Nearest agent allegiance")) {
			GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
			GW::AgentLiving* nearest_living = nullptr;
			float nearest_dist_sq = 0.f;
			if (me) {
				GW::AgentArray* agents = GW::Agents::GetAgentArray();
				if (agents && agents->valid()) {
					for (GW::Agent* agent : *agents) {
						if (!agent || !agent->GetIsLivingType()) continue;
						GW::AgentLiving* living = agent->GetAsAgentLiving();
						if (!living || living->GetIsDead()) continue;
						if (living->agent_id == me->agent_id) continue;
						const float dx = agent->x - me->x;
						const float dy = agent->y - me->y;
						const float dist_sq = dx * dx + dy * dy;
						if (!nearest_living || dist_sq < nearest_dist_sq) {
							nearest_living = living;
							nearest_dist_sq = dist_sq;
						}
					}
				}
			}

			if (nearest_living) {
				ImGui::Text("Nearest agent: ID %u, distance %.0f", nearest_living->agent_id, std::sqrt(nearest_dist_sq));
				ImGui::Text("IsPlayer: %s", nearest_living->IsPlayer() ? "yes" : "no");
				ImGui::Text("allegiance (raw): %u", static_cast<uint32_t>(nearest_living->allegiance));
			} else {
				ImGui::TextDisabled("No nearby agent found");
			}
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

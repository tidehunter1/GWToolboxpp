#include <cstdint>
#include <cstring>
#include <cstdio>
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

		UpdateEscapeToEmbark();

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
	bool embark_escape_armed_ = true;
	static constexpr float kEmbarkRearmHysteresisPct = 5.f;

	bool test_lookup_performed_ = false;
	uintptr_t test_lookup_func_addr_ = 0;
	uint32_t test_lookup_target_id_ = 0;
	uintptr_t test_lookup_result_ = 0;
	std::string test_bytes_at_signature_;
	std::string test_bytes_at_knownworking_;
	bool stage2_enabled_ = false;
	bool stage2_performed_ = false;
	bool stage2_skipped_existing_ = false;
	std::string stage2_subject_name_;
	uint32_t stage2_subject_id_ = 0;
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

	static bool TryReadBytes(uintptr_t addr, uint8_t* out, size_t count) {
		if (!addr) return false;
		__try {
			memcpy(out, reinterpret_cast<void*>(addr), count);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static std::string FormatBytesHex(uintptr_t addr, size_t count) {
		std::vector<uint8_t> buf(count, 0);
		if (!TryReadBytes(addr, buf.data(), count)) {
			return "(unreadable)";
		}
		std::ostringstream oss;
		for (size_t i = 0; i < count; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
			oss << tmp;
		}
		return oss.str();
	}

	static std::string WideToNarrow(const std::wstring& wide) {
		if (wide.empty()) return {};
		const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		if (len <= 0) return {};
		std::string out(static_cast<size_t>(len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
		return out;
	}

	bool FindNearestOtherAgent(uint32_t& out_agent_id, std::string& out_name) {
		GW::AgentArray* agents = GW::Agents::GetAgentArray();
		if (!agents || !agents->valid()) return false;
		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (!me) return false;
		const uint32_t target_id = GW::Agents::GetTargetId();
		float best_dist_sq = -1.f;
		GW::AgentLiving* best_living = nullptr;
		for (GW::Agent* agent : *agents) {
			if (!agent || !agent->GetIsLivingType()) continue;
			GW::AgentLiving* living = agent->GetAsAgentLiving();
			if (!living || living->GetIsDead()) continue;
			if (living->agent_id == me->agent_id) continue;
			if (living->agent_id == target_id) continue;
			const float dx = living->pos.x - me->pos.x;
			const float dy = living->pos.y - me->pos.y;
			const float dist_sq = dx * dx + dy * dy;
			if (best_dist_sq < 0.f || dist_sq < best_dist_sq) {
				best_dist_sq = dist_sq;
				best_living = living;
			}
		}
		if (!best_living) return false;
		out_agent_id = best_living->agent_id;
		out_name = WideToNarrow(*name_cache_.Get(best_living).lower);
		return true;
	}

	using ManagerFindAgent_pt = void*(__cdecl*)(uint32_t agent_id);

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

	using CreateOverlay_pt = void(__thiscall*)(void* view_obj, uint32_t param2);

	static uintptr_t GetCreateOverlayAddress() {
		static bool tried_resolve = false;
		static uintptr_t address = 0;
		if (!tried_resolve) {
			tried_resolve = true;
			address = GW::Scanner::Find(
				"\x55\x8b\xec\x83\xec\x20\x56\x8b\xf1\xe8\x00\x00\x00\x00\x85\xc0\x74\x75\x8b\x06\x8d\x4d\xf0\x51\xff\x75\x08\x8b\xce\xff\x10\xd9\x46\x3c\xff\x75\xf4\xd9\x5d\x08\xd9\x45\x08\x51\xd9\x1c\x24\xe8\x00\x00\x00\x00\xd9\x86\x84\x00\x00\x00\x8b\xc8",
				"xxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxx"
			);
		}
		return address;
	}

	using SetGlobalNameTagVisibility_pt = void(__cdecl*)(uint32_t);

	static SetGlobalNameTagVisibility_pt GetSetGlobalNameTagVisibilityFunc(uint32_t** out_flags_ptr) {
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
		if (out_flags_ptr) *out_flags_ptr = flags_ptr;
		return set_func;
	}

	static void RefreshAllNametags() {
		uint32_t* flags_ptr = nullptr;
		SetGlobalNameTagVisibility_pt set_func = GetSetGlobalNameTagVisibilityFunc(&flags_ptr);
		if (!set_func || !flags_ptr) return;
		GW::GameThread::Enqueue([set_func, flags_ptr] {
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
		ImGui::SeparatorText("Experimental");
		ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "BUILD MARKER: TEST-REV-7");
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Stage 1: read-only lookup, no state changes. Target a unit first.");
		if (ImGui::Button("Test: resolve agent view object")) {
			test_lookup_func_addr_ = GetManagerFindAgentAddress();
			test_lookup_target_id_ = GW::Agents::GetTargetId();
			test_lookup_result_ = 0;
			if (test_lookup_func_addr_ && test_lookup_target_id_ != 0) {
				const auto func = reinterpret_cast<ManagerFindAgent_pt>(test_lookup_func_addr_);
				test_lookup_result_ = reinterpret_cast<uintptr_t>(func(test_lookup_target_id_));
			}
			test_bytes_at_signature_ = FormatBytesHex(test_lookup_func_addr_, 24);
			test_bytes_at_knownworking_ = FormatBytesHex(reinterpret_cast<uintptr_t>(GetSetGlobalNameTagVisibilityFunc(nullptr)), 24);
			test_lookup_performed_ = true;
		}
		if (test_lookup_performed_) {
			ImGui::Text("Signature address: 0x%08X", static_cast<unsigned>(test_lookup_func_addr_));
			ImGui::Text("Target agent_id: %u", test_lookup_target_id_);
			ImGui::Text("Resolved pointer: 0x%08X", static_cast<unsigned>(test_lookup_result_));
			ImGui::TextWrapped("Bytes at signature address: %s", test_bytes_at_signature_.c_str());
			ImGui::TextWrapped("Bytes at known-working address: %s", test_bytes_at_knownworking_.c_str());
		}
		ImGui::Text("Known-working scan (RefreshAllNametags target): 0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(GetSetGlobalNameTagVisibilityFunc(nullptr))));

		ImGui::Spacing();
		ImGui::SeparatorText("Stage 2 - REAL RISK");
		ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Creates an engine object. Real crash risk.");
		ImGui::TextWrapped("Tests on the nearest OTHER unit (not your target), so a real target/hover trigger can't fake a positive result. Leave your target empty or on something else, and don't hover the named unit below.");
		ImGui::Checkbox("Enable Stage 2", &stage2_enabled_);
		if (stage2_enabled_) {
			if (ImGui::Button("Test: force-create overlay on nearest other unit")) {
				stage2_performed_ = false;
				stage2_skipped_existing_ = false;
				stage2_subject_id_ = 0;
				stage2_subject_name_.clear();
				if (FindNearestOtherAgent(stage2_subject_id_, stage2_subject_name_)) {
					const uintptr_t manager_addr = GetManagerFindAgentAddress();
					const uintptr_t create_addr = GetCreateOverlayAddress();
					if (manager_addr && create_addr) {
						const auto find_func = reinterpret_cast<ManagerFindAgent_pt>(manager_addr);
						void* view_obj = find_func(stage2_subject_id_);
						if (view_obj) {
							uint32_t overlay_ptr = 0;
							const bool read_ok = TryReadBytes(reinterpret_cast<uintptr_t>(view_obj) + 0x98, reinterpret_cast<uint8_t*>(&overlay_ptr), sizeof(overlay_ptr));
							if (read_ok && overlay_ptr != 0) {
								stage2_skipped_existing_ = true;
							}
							else if (read_ok) {
								const auto create_func = reinterpret_cast<CreateOverlay_pt>(create_addr);
								GW::GameThread::Enqueue([create_func, view_obj] {
									create_func(view_obj, 1);
								});
								stage2_performed_ = true;
							}
						}
					}
				}
			}
			if (!stage2_subject_name_.empty()) {
				ImGui::Text("Test subject: \"%s\" (agent_id %u)", stage2_subject_name_.c_str(), stage2_subject_id_);
			}
			if (stage2_skipped_existing_) {
				ImGui::TextColored(ImVec4(1.f, 1.f, 0.4f, 1.f), "Skipped: overlay already exists on this agent (it may already be targeted/hovered).");
			}
			if (stage2_performed_) {
				ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "Call enqueued. Look for the named unit above WITHOUT targeting or hovering it.");
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

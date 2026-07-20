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

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Utilities/Hooker.h>

#include <ToolboxPlugin.h>
#include <imgui.h>

#include <d3d9.h>

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <string_view>
#include <sstream>
#include <unordered_map>
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

inline void WideToUtf8Into(const std::wstring& wide, std::string& out) {
	if (wide.empty()) { out.clear(); return; }
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0) { out.clear(); return; }
	out.resize(static_cast<size_t>(len));
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
}

inline std::string WideToUtf8(const std::wstring& wide) {
	std::string out;
	WideToUtf8Into(wide, out);
	return out;
}

inline std::string TruncateWithEllipsis(ImFont* font, float font_size, const std::wstring& name, std::string_view full_utf8, float max_width) {
	const char* full_begin = full_utf8.data();
	const char* full_end = full_begin + full_utf8.size();
	const ImVec2 full_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, full_begin, full_end);
	if (full_size.x <= max_width) return std::string(full_utf8);

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

inline ImU32 ScaleAlpha(ImU32 color, float mult) {
	if (mult >= 1.f) return color;
	ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
	c.w *= mult;
	return ImGui::ColorConvertFloat4ToU32(c);
}

inline void DrawStatusTriangles(ImDrawList* draw_list, float right_x, float center_y, const GW::AgentLiving* living, float opacity_mult = 1.f) {
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
		draw_list->AddTriangleFilled(op1, op2, op3, ScaleAlpha(kOutlineColor, opacity_mult));

		ImVec2 p1, p2, p3;
		make_triangle(kTriWidth, kTriHeight, x, y, upsidedown, p1, p2, p3);
		draw_list->AddTriangleFilled(p1, p2, p3, ScaleAlpha(color, opacity_mult));

		++count;
	};

	if (living->GetIsEnchanted()) draw_tri(kEnchantedColor, false);
	if (living->GetIsHexed()) draw_tri(kHexedColor, true);
	if (living->GetIsConditioned()) draw_tri(kConditionedColor, true);
}

inline void DrawOutlinedText(ImDrawList* draw_list, ImFont* font, float font_size, const ImVec2& pos, ImU32 text_color, std::string_view text_utf8, float opacity_mult = 1.f) {
	static constexpr ImU32 kOutlineColor = IM_COL32(0, 0, 0, 255);
	static constexpr float kOutlineOffset = 1.f;
	const ImU32 outline_color = ScaleAlpha(kOutlineColor, opacity_mult);
	text_color = ScaleAlpha(text_color, opacity_mult);
	const char* text_begin = text_utf8.data();
	const char* text_end = text_begin + text_utf8.size();
	draw_list->AddText(font, font_size, ImVec2(pos.x - kOutlineOffset, pos.y), outline_color, text_begin, text_end);
	draw_list->AddText(font, font_size, ImVec2(pos.x + kOutlineOffset, pos.y), outline_color, text_begin, text_end);
	draw_list->AddText(font, font_size, ImVec2(pos.x, pos.y - kOutlineOffset), outline_color, text_begin, text_end);
	draw_list->AddText(font, font_size, ImVec2(pos.x, pos.y + kOutlineOffset), outline_color, text_begin, text_end);
	draw_list->AddText(font, font_size, pos, text_color, text_begin, text_end);
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

	void MaybePrune() { PruneCache(cache_, tick_, last_prune_tick_, kPruneIntervalTicks); }

private:
	static constexpr uint64_t kPruneIntervalTicks = 1800;
	struct Entry {
		float y = 0.f;
		bool initialized = false;
		uint64_t last_seen_tick = 0;
	};
	std::unordered_map<uint32_t, Entry> cache_;
	uint64_t tick_ = 0, last_prune_tick_ = 0;
};

class AgentNameCache {
public:
	struct NameLookup {
		const std::wstring* lower;
		const std::wstring* display;
	};

	NameLookup Get(uint32_t agent_id, const wchar_t* enc_name) {
		Entry& entry = cache_[agent_id];
		entry.last_seen_tick = tick_;
		if (enc_name && wcsncmp(entry.last_enc, enc_name, kMaxEncLen - 1) != 0) {
			wcsncpy_s(entry.last_enc, enc_name, kMaxEncLen - 1);
			entry.buffer[0] = L'\0';
			entry.converted = false;
			entry.truncated_for_width = -1.f;
			GW::UI::AsyncDecodeStr(enc_name, entry.buffer, kBufferLen);
		}
		if (!entry.converted && entry.buffer[0] != L'\0') {
			entry.decoded_display = entry.buffer;
			entry.decoded_lower = entry.buffer;
			std::transform(entry.decoded_lower.begin(), entry.decoded_lower.end(), entry.decoded_lower.begin(), ::towlower);
			WideToUtf8Into(entry.decoded_display, entry.decoded_display_utf8);
			entry.converted = true;
		}
		return { &entry.decoded_lower, &entry.decoded_display };
	}

	const std::string& GetTruncated(uint32_t agent_id, ImFont* font, float font_size, float max_width) {
		Entry& entry = cache_[agent_id];
		if (entry.truncated_for_width != max_width) {
			entry.truncated_utf8 = TruncateWithEllipsis(font, font_size, entry.decoded_display, entry.decoded_display_utf8, max_width);
			entry.truncated_for_width = max_width;
		}
		return entry.truncated_utf8;
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
		float truncated_for_width = -1.f;
		uint64_t last_seen_tick = 0;
		std::wstring decoded_lower, decoded_display;
		std::string decoded_display_utf8, truncated_utf8;
	};
	std::unordered_map<uint32_t, Entry> cache_;
	uint64_t tick_ = 0, last_prune_tick_ = 0;
};

class CastStateCache {
public:
	struct CastState {
		GW::Constants::SkillID skill_id = GW::Constants::SkillID::No_Skill;
		ULONGLONG cast_start_ms = 0;
		float cast_time_ms = 0.f;
		bool casting = false;
		bool was_cancelled = false;
		ULONGLONG ended_at_ms = 0;
	};

	const CastState* Find(uint32_t agent_id) {
		auto it = cache_.find(agent_id);
		if (it == cache_.end()) return nullptr;
		it->second.last_seen_tick = tick_;
		return &it->second.state;
	}

	void OnStartedCast(uint32_t agent_id, GW::Constants::SkillID skill_id, float duration_ms) {
		Entry& e = cache_[agent_id];
		e.last_seen_tick = tick_;
		e.state.skill_id = skill_id;
		e.state.cast_start_ms = GetTickCount64();
		e.state.cast_time_ms = duration_ms;
		e.state.casting = true;
		e.state.was_cancelled = false;
		e.state.ended_at_ms = 0;
	}

	void OnCompleted(uint32_t agent_id, GW::Constants::SkillID skill_id) {
		auto it = cache_.find(agent_id);
		if (it == cache_.end() || it->second.state.skill_id != skill_id) return;
		it->second.state.casting = false;
		it->second.state.was_cancelled = false;
		it->second.state.ended_at_ms = GetTickCount64();
	}

	void OnCancelled(uint32_t agent_id, GW::Constants::SkillID skill_id) {
		auto it = cache_.find(agent_id);
		if (it == cache_.end() || it->second.state.skill_id != skill_id) return;
		it->second.state.casting = false;
		it->second.state.was_cancelled = true;
		it->second.state.ended_at_ms = GetTickCount64();
	}

	void MaybePrune() { PruneCache(cache_, tick_, last_prune_tick_, kPruneIntervalTicks); }

private:
	static constexpr uint64_t kPruneIntervalTicks = 1800;
	struct Entry {
		CastState state;
		uint64_t last_seen_tick = 0;
	};
	std::unordered_map<uint32_t, Entry> cache_;
	uint64_t tick_ = 0, last_prune_tick_ = 0;
};

class SkillNameCache {
public:
	const std::string& Get(GW::Constants::SkillID skill_id) {
		Entry& entry = cache_[skill_id];
		if (!entry.requested) {
			entry.requested = true;
			const GW::Skill* skill = GW::SkillbarMgr::GetSkillConstantData(skill_id);
			if (skill && skill->name) {
				wchar_t enc_buf[16] = {};
				if (GW::UI::UInt32ToEncStr(skill->name, enc_buf, 16)) {
					GW::UI::AsyncDecodeStr(enc_buf, entry.buffer, kBufferLen);
				}
			}
		}
		if (!entry.converted && entry.buffer[0] != L'\0') {
			WideToUtf8Into(std::wstring(entry.buffer), entry.utf8);
			entry.converted = true;
		}
		return entry.utf8;
	}

private:
	static constexpr size_t kBufferLen = 128;
	struct Entry {
		bool requested = false;
		bool converted = false;
		wchar_t buffer[kBufferLen] = {};
		std::string utf8;
	};
	std::unordered_map<GW::Constants::SkillID, Entry> cache_;
};

inline uint32_t TexmodCrc32(const void* data, size_t bytes) {
	static uint32_t table[256];
	static bool table_ready = false;
	if (!table_ready) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int bit = 0; bit < 8; ++bit) {
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			}
			table[i] = c;
		}
		table_ready = true;
	}
	uint32_t crc = 0xFFFFFFFFu;
	const auto* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < bytes; ++i) {
		crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
	}
	return crc;
}

inline UINT TexmodDxtBlockBytes(D3DFORMAT format) {
	switch (format) {
		case D3DFMT_DXT1: return 8;
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
		case D3DFMT_DXT4:
		case D3DFMT_DXT5: return 16;
		default: return 0;
	}
}

inline int TexmodBitsPerPixel(D3DFORMAT format) {
	switch (format) {
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
		case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10:
			return 32;
		case D3DFMT_R8G8B8:
			return 24;
		case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
		case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_L16:
		case D3DFMT_D16_LOCKABLE: case D3DFMT_D16: case D3DFMT_D15S1:
			return 16;
		case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4:
			return 8;
		case D3DFMT_DXT1:
			return 4;
		case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
			return 8;
		default:
			return 32;
	}
}

inline uint32_t ComputeTexmodHash(IDirect3DTexture9* texture) {
	if (!texture) return 0;

	D3DSURFACE_DESC desc;
	if (FAILED(texture->GetLevelDesc(0, &desc))) return 0;

	D3DLOCKED_RECT locked;
	if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)) || !locked.pBits || locked.Pitch <= 0) {
		return 0;
	}
	const auto* bits = static_cast<const uint8_t*>(locked.pBits);
	const size_t pitch = static_cast<size_t>(locked.Pitch);

	uint32_t hash = 0;
	if (const UINT block = TexmodDxtBlockBytes(desc.Format)) {
		const size_t blocks_wide = (desc.Width + 3) / 4;
		const size_t blocks_high = (desc.Height + 3) / 4;
		const size_t total_size = blocks_wide * blocks_high * block;
		if (total_size && total_size <= pitch * blocks_high) {
			std::vector<uint8_t> compact(total_size);
			memcpy(compact.data(), bits, total_size);
			hash = TexmodCrc32(compact.data(), compact.size());
		}
	}
	else {
		const int bpp = TexmodBitsPerPixel(desc.Format);
		const size_t row_size = bpp ? static_cast<size_t>(desc.Width) * (bpp / 8) : 0;
		if (row_size && desc.Height && row_size <= pitch) {
			std::vector<uint8_t> compact(static_cast<size_t>(desc.Height) * row_size);
			for (UINT row = 0; row < desc.Height; ++row) {
				memcpy(compact.data() + row * row_size, bits + row * pitch, row_size);
			}
			hash = TexmodCrc32(compact.data(), compact.size());
		}
	}

	texture->UnlockRect(0);
	return hash;
}

inline bool IsMinipet(uint16_t player_number) {
	static constexpr std::array<uint16_t, 129> ids = {
		230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259,
		260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289,
		290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320,
		321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350,
		8035, 8344, 8349, 8350, 8351, 8352, 8354, 9038, 9039
	};
	return std::binary_search(ids.begin(), ids.end(), player_number);
}

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

struct NameplateSettings {
	bool show_enemies = true, show_summoned_allies = false, show_friendlies = true, auto_toggle_show_names = true;
	bool recolor_quest_nametags = true, recolor_professions = false, fade_enemies_by_range = true, color_nameplate_text_by_combat = true;
	bool hide_enemy_native_nametags = true;
	bool hide_enemy_native_healthbar = true;
	uint32_t combat_text_color = IM_COL32(255, 255, 0, 255);
	float max_range = 3500.0f, bar_width = 200.0f, bar_height = 20.0f, npc_health_threshold = 60.0f, allied_health_threshold = 60.0f;
	float border_thickness = 1.0f;
	uint32_t enemy_color = IM_COL32(220, 40, 40, 255), quest_color = IM_COL32(255, 179, 71, 255), friendly_color = IM_COL32(0, 255, 152, 255);
	uint32_t target_border_color = IM_COL32(255, 255, 0, 255);
	uint32_t border_color = IM_COL32(0, 0, 0, 180);

	bool show_priority_castbars = true;
	float castbar_height = 16.0f;
	uint32_t castbar_fill_color = IM_COL32(255, 140, 0, 255);
	uint32_t castbar_cancelled_color = IM_COL32(255, 0, 0, 255);

	std::array<PriorityConfig, 3> priorities = {{
		{"", IM_COL32(135, 206, 250, 255)},
		{"", IM_COL32(255, 105, 180, 255)},
		{"", IM_COL32(147, 112, 219, 255)}
	}};
};

class NameplatesPlugin : public ToolboxPlugin {
public:
	NameplatesPlugin() {
		pending_.reserve(256);
		placed_.reserve(256);
		order_.reserve(256);
		GW::UI::RegisterUIMessageCallback(&nametag_hook_entry_, GW::UI::UIMessage::kShowAgentNameTag, OnAgentNameTag);
		GW::UI::RegisterUIMessageCallback(&nametag_hook_entry_, GW::UI::UIMessage::kSetAgentNameTagAttribs, OnAgentNameTag);
		GW::UI::RegisterUIMessageCallback(&cast_hook_entry_, GW::UI::UIMessage::kAgentSkillStartedCast, OnSkillCastMessage);
		GW::UI::RegisterUIMessageCallback(&cast_hook_entry_, GW::UI::UIMessage::kAgentSkillActivated, OnSkillCastMessage);
		GW::UI::RegisterUIMessageCallback(&cast_hook_entry_, GW::UI::UIMessage::kAgentSkillActivatedInstantly, OnSkillCastMessage);
		GW::UI::RegisterUIMessageCallback(&cast_hook_entry_, GW::UI::UIMessage::kAgentSkillCancelled, OnSkillCastMessage);
	}

	void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override {
		ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
		get_skill_image_ = reinterpret_cast<GetSkillImageFn>(GetProcAddress(toolbox_handle, "GetSkillImage"));
	}

	const char* Name() const override { return "Nameplates"; }

	bool* GetVisiblePtr() override { return &visible_; }

	[[nodiscard]] bool HasSettings() const override { return true; }
	void DrawSettings() override;

	void LoadSettings(const wchar_t* folder) override {
		ToolboxPlugin::LoadSettings(folder);
		#define L_SET(var) LoadSetting(#var, settings_.var)
		L_SET(show_enemies); L_SET(max_range); L_SET(bar_width); L_SET(bar_height); L_SET(border_thickness);
		L_SET(npc_health_threshold); L_SET(allied_health_threshold);
		L_SET(show_summoned_allies); L_SET(auto_toggle_show_names);
		L_SET(recolor_quest_nametags); L_SET(recolor_professions); L_SET(hide_enemy_native_nametags); L_SET(hide_enemy_native_healthbar);
		L_SET(show_friendlies); L_SET(friendly_color); L_SET(enemy_color); L_SET(quest_color); L_SET(target_border_color); L_SET(border_color);
		L_SET(show_priority_castbars); L_SET(castbar_height); L_SET(castbar_fill_color); L_SET(castbar_cancelled_color);
		L_SET(fade_enemies_by_range); L_SET(color_nameplate_text_by_combat); L_SET(combat_text_color);
		LoadSetting("visible", visible_);
		#undef L_SET

		for (size_t i = 0; i < 3; ++i) {
			const std::string prefix = "priority" + std::to_string(i + 1);
			LoadSetting((prefix + "_raw").c_str(), settings_.priorities[i].raw);
			LoadSetting((prefix + "_color").c_str(), settings_.priorities[i].color);
		}
		RefreshPriorityBuffersAndLists();
	}

	void SaveSettings(const wchar_t* folder) override {
		#define S_SET(var) SaveSetting(#var, settings_.var)
		S_SET(show_enemies); S_SET(max_range); S_SET(bar_width); S_SET(bar_height); S_SET(border_thickness);
		S_SET(npc_health_threshold); S_SET(allied_health_threshold);
		S_SET(show_summoned_allies); S_SET(auto_toggle_show_names);
		S_SET(recolor_quest_nametags); S_SET(recolor_professions); S_SET(hide_enemy_native_nametags); S_SET(hide_enemy_native_healthbar);
		S_SET(show_friendlies); S_SET(friendly_color); S_SET(enemy_color); S_SET(quest_color); S_SET(target_border_color); S_SET(border_color);
		S_SET(show_priority_castbars); S_SET(castbar_height); S_SET(castbar_fill_color); S_SET(castbar_cancelled_color);
		S_SET(fade_enemies_by_range); S_SET(color_nameplate_text_by_combat); S_SET(combat_text_color);
		SaveSetting("visible", visible_);
		#undef S_SET

		for (size_t i = 0; i < 3; ++i) {
			const std::string prefix = "priority" + std::to_string(i + 1);
			SaveSetting((prefix + "_raw").c_str(), settings_.priorities[i].raw);
			SaveSetting((prefix + "_color").c_str(), settings_.priorities[i].color);
		}
		ToolboxPlugin::SaveSettings(folder);
	}

	bool CanTerminate() override { return true; }

	void Terminate() override {
		GW::UI::RemoveUIMessageCallback(&nametag_hook_entry_);
		GW::UI::RemoveUIMessageCallback(&cast_hook_entry_);
		if (create_texture_func_) {
			GW::Hook::RemoveHook(reinterpret_cast<void*>(create_texture_func_));
		}
	}

	void Draw(IDirect3DDevice9* ) override { DrawNameplates(); }

private:
	NameplateSettings settings_;
	bool visible_ = true;
	std::optional<bool> last_outpost_pref_state_;
	std::optional<bool> last_recolor_professions_state_;
	std::optional<bool> last_recolor_quest_state_;
	std::optional<bool> last_show_enemies_state_;
	GW::HookEntry nametag_hook_entry_;
	GW::HookEntry cast_hook_entry_;

	using GetSkillImageFn = IDirect3DTexture9** (__cdecl*)(GW::Constants::SkillID);
	GetSkillImageFn get_skill_image_ = nullptr;

	static constexpr uint32_t kTargetHealthbarHash = 0x0B19B995u;
	static constexpr uint32_t kTargetIndicatorHash = 0xD9B07004u;

	using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
	CreateTextureFn create_texture_func_ = nullptr;
	CreateTextureFn create_texture_ret_ = nullptr;
	IDirect3DTexture9* last_created_texture_ = nullptr;

	AgentNameCache name_cache_;
	StackYSmoother stack_y_smoother_;
	CastStateCache cast_cache_;
	SkillNameCache skill_name_cache_;

	struct PriorityState {
		char buf[512] = {};
		std::vector<std::wstring> names;
	};
	std::array<PriorityState, 3> priority_states_;

	static constexpr float kNameplateFontSize = 18.f;
	static constexpr float kStackSmoothing = 0.05f;
	static constexpr float kBgTintAmount = 0.3f;
	static constexpr float kBgOpacity = 1.0f;

	[[nodiscard]] static ImU32 TintedBackground(ImU32 color) {
		const ImVec4 c4 = ImGui::ColorConvertU32ToFloat4(color);
		const ImVec4 bg4(c4.x * kBgTintAmount, c4.y * kBgTintAmount, c4.z * kBgTintAmount, kBgOpacity);
		return ImGui::ColorConvertFloat4ToU32(bg4);
	}
	static constexpr float kZNear = 46.875f;
	static constexpr float kZFar  = 48000.f;

	static constexpr ULONGLONG kCastLingerMs = 2000;

	static constexpr float kFadeRange1Sq = 1500.f * 1500.f;
	static constexpr float kFadeRange2Sq = 2500.f * 2500.f;

	[[nodiscard]] static float GetRangeOpacityMultiplier(float dist_sq) {
		if (dist_sq <= kFadeRange1Sq)   return 1.00f;
		if (dist_sq <= kFadeRange2Sq)   return 0.75f;
		return 0.50f;
	}

	void RefreshPriorityBuffersAndLists() {
		for (size_t i = 0; i < 3; ++i) {
			strncpy_s(priority_states_[i].buf, 512, settings_.priorities[i].raw.c_str(), 511);
			priority_states_[i].names = ParseSemicolonNameList(settings_.priorities[i].raw);
		}
	}

	[[nodiscard]] std::optional<ImU32> GetPriorityColor(const std::wstring& name_lower) const {
		if (name_lower.empty()) return std::nullopt;
		for (size_t i = 0; i < 3; ++i) {
			if (std::binary_search(priority_states_[i].names.begin(), priority_states_[i].names.end(), name_lower)) {
				return settings_.priorities[i].color;
			}
		}
		return std::nullopt;
	}

	struct PendingBar {
		GW::AgentLiving* living = nullptr;
		ImVec2 screen{}, footprint{};
		const std::wstring* name_lower = nullptr;
		const std::wstring* display = nullptr;
		bool is_targeted = false, is_in_combat = false;
		float natural_y = 0.f;
		float dist_sq_from_me = -1.f;
	};

	struct PlacedRect { float x_min, x_max, y_min, y_max; };

	std::vector<PendingBar> pending_;
	std::vector<PlacedRect> placed_;
	std::vector<size_t> order_;

	void ResolveStacking(std::vector<PendingBar>& items) {
		static constexpr float kGap = 2.f;
		static constexpr float kMaxPushMultiplier = 4.f;
		static constexpr float kSortEpsilon = 1.f;

		order_.resize(items.size());
		for (size_t i = 0; i < items.size(); ++i) order_[i] = i;
		std::sort(order_.begin(), order_.end(), [&](size_t a, size_t b) {
			const float ya = items[a].screen.y;
			const float yb = items[b].screen.y;
			if (std::fabs(ya - yb) > kSortEpsilon) return ya < yb;
			return items[a].living->agent_id < items[b].living->agent_id;
		});

		placed_.clear();
		placed_.reserve(items.size());

		for (size_t oi : order_) {
			PendingBar& item = items[oi];
			if (item.footprint.x <= 0.f || item.footprint.y <= 0.f) continue;

			const float half_w = item.footprint.x / 2.f;
			const float x_min = item.screen.x - half_w;
			const float x_max = item.screen.x + half_w;
			const float natural_top = item.screen.y;
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

			item.screen.y += (cur_top - natural_top);
			placed_.push_back({x_min, x_max, cur_top, cur_top + item.footprint.y});
		}
	}

	void DrawNameplates() {
		if (settings_.hide_enemy_native_healthbar) {
			EnsureTextureHookInstalled();
		}

		GW::AgentArray* agents = GW::Agents::GetAgentArray();
		if (!agents || !agents->valid()) return;

		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		GW::AgentLiving* target = GW::Agents::GetTargetAsAgentLiving();
		const bool in_outpost = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Outpost;
		const bool left_clicked_this_frame = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

		if (settings_.auto_toggle_show_names
			&& (!last_outpost_pref_state_.has_value() || *last_outpost_pref_state_ != in_outpost)) {
			last_outpost_pref_state_ = in_outpost;
			const bool show = in_outpost;
			GW::GameThread::Enqueue([show] {
				GW::UI::SetPreference(GW::UI::FlagPreference::AlwaysShowFoeNames, show);
			});
		}

		if (last_show_enemies_state_.has_value() && *last_show_enemies_state_ && !settings_.show_enemies) {
			GW::GameThread::Enqueue([] {
				GW::UI::SetPreference(GW::UI::FlagPreference::AlwaysShowFoeNames, true);
			});
		}
		last_show_enemies_state_ = settings_.show_enemies;

		FlashOnChange(last_recolor_professions_state_, settings_.recolor_professions, [] {
			FlashPreference(GW::UI::FlagPreference::AlwaysShowAllyNames);
		});

		FlashOnChange(last_recolor_quest_state_, settings_.recolor_quest_nametags, [] {
			FlashPreference(GW::UI::FlagPreference::AlwaysShowAllyNames);
		});

		DirectX::XMMATRIX view_proj;
		float viewport_width, viewport_height;
		if (!BuildFrameProjection(view_proj, viewport_width, viewport_height)) return;

		ImFont* font = ImGui::GetFont();

		GatherPendingBars(agents, me, target, in_outpost, view_proj, viewport_width, viewport_height);
		ResolveStacking(pending_);
		ApplyStackSmoothing();

		ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
		const PendingBar* target_pb = nullptr;
		
		for (const auto& pb : pending_) {
			if (pb.is_targeted) {
				target_pb = &pb;
				continue;
			}
			DrawBar(draw_list, pb, font, left_clicked_this_frame);
		}
		
		if (target_pb) {
			DrawBar(draw_list, *target_pb, font, left_clicked_this_frame);
		}

		name_cache_.MaybePrune();
		stack_y_smoother_.MaybePrune();
		cast_cache_.MaybePrune();
	}

	void GatherPendingBars(GW::AgentArray* agents, GW::AgentLiving* me, GW::AgentLiving* target,
							bool in_outpost, const DirectX::XMMATRIX& view_proj,
							float viewport_width, float viewport_height) {
		pending_.clear();
		const float max_range_sq = settings_.max_range * settings_.max_range;

		for (GW::Agent* agent : *agents) {
			if (!agent) continue;
			if (!agent->GetIsLivingType()) continue;

			GW::AgentLiving* living = agent->GetAsAgentLiving();
			if (!living) continue;

			if (living->GetIsDead()) continue;
			if (me && living->agent_id == me->agent_id) continue;
			float dist_sq = -1.f;
			if (!WithinRange(living, me, max_range_sq, dist_sq)) continue;
			if (IsMinipet(living->player_number)) continue;
			if (in_outpost) continue;

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
				if (living->hp * 100.f > settings_.npc_health_threshold) continue;
			}
			else if (is_allied) {
				if (living->hp * 100.f > settings_.allied_health_threshold) continue;
			}

			ImVec2 screen;
			if (!WorldToScreen(living, view_proj, viewport_width, viewport_height, screen)) continue;

			const auto name_lookup = name_cache_.Get(living->agent_id, GW::Agents::GetAgentEncName(living->agent_id));

			PendingBar pb;
			pb.living = living;
			pb.screen = screen;
			pb.natural_y = screen.y;
			pb.name_lower = name_lookup.lower;
			pb.display = name_lookup.display;
			pb.is_targeted = target && living->agent_id == target->agent_id;
			pb.dist_sq_from_me = dist_sq;

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
			pb.footprint = ImVec2(settings_.bar_width, settings_.bar_height);
			if (settings_.show_priority_castbars && CastBarIsVisible(living->agent_id)) {
				pb.footprint.y += settings_.castbar_height;
			}

			pending_.push_back(std::move(pb));
		}
	}

	void ApplyStackSmoothing() {
		for (auto& pb : pending_) {
			const float target_offset = pb.screen.y - pb.natural_y;
			const float smoothed_offset = stack_y_smoother_.Update(pb.living->agent_id, target_offset, kStackSmoothing);
			pb.screen.y = pb.natural_y + smoothed_offset;
		}
	}

	[[nodiscard]] bool ShouldShowAllegiance(GW::Constants::Allegiance allegiance) const {
		switch (allegiance) {
			case GW::Constants::Allegiance::Enemy:
				return settings_.show_enemies;
			case GW::Constants::Allegiance::Ally_NonAttackable:
			case GW::Constants::Allegiance::Spirit_Pet:
			case GW::Constants::Allegiance::Minion:
			case GW::Constants::Allegiance::Neutral:
			case GW::Constants::Allegiance::Npc_Minipet:
				return settings_.show_friendlies;
			default:
				return false;
		}
	}

	[[nodiscard]] bool WithinRange(const GW::AgentLiving* living, const GW::Agent* me, float max_range_sq, float& out_dist_sq) const {
		if (!me) { out_dist_sq = -1.f; return true; }
		const float dx = living->pos.x - me->pos.x;
		const float dy = living->pos.y - me->pos.y;
		out_dist_sq = dx * dx + dy * dy;
		return out_dist_sq <= max_range_sq;
	}

	[[nodiscard]] bool BuildFrameProjection(DirectX::XMMATRIX& out_view_proj,
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

	[[nodiscard]] bool WorldToScreen(const GW::AgentLiving* living, const DirectX::XMMATRIX& view_proj,
					   float viewport_width, float viewport_height, ImVec2& out) const {
		using namespace DirectX;

		const XMVECTOR world_pos = XMVectorSet(living->pos.x, living->pos.y, living->name_tag_z, 1.f);
		const XMVECTOR clip_pos = XMVector4Transform(world_pos, view_proj);
		float clip_arr[4];
		XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(clip_arr), clip_pos);

		if (clip_arr[3] <= kZNear) return false;

		const float inv_w = 1.f / clip_arr[3];
		out.x = ((clip_arr[0] * inv_w) * 0.5f + 0.5f) * viewport_width;
		out.y = (1.f - ((clip_arr[1] * inv_w) * 0.5f + 0.5f)) * viewport_height;
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
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
	}

	static void DrawCheckboxWithColor(const char* label, bool& toggle, uint32_t& color, const char* color_id) {
		ImGui::Checkbox(label, &toggle);
		ImGui::SameLine();
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorEdit3(color_id, &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
	}

	void DrawBar(ImDrawList* draw_list, const PendingBar& pb, ImFont* font, bool left_clicked_this_frame) {
		const GW::AgentLiving* living = pb.living;

		const float hp_pct = std::clamp(living->hp, 0.f, 1.f);
		const float bar_width = settings_.bar_width;
		const float bar_height = settings_.bar_height;

		const ImVec2 top_left(pb.screen.x - bar_width / 2.f, pb.screen.y);
		const ImVec2 bottom_right(top_left.x + bar_width, top_left.y + bar_height);
		const ImVec2 fill_bottom_right(top_left.x + bar_width * hp_pct, bottom_right.y);

		const auto priority_color = GetPriorityColor(*pb.name_lower);
		ImU32 fill_color;
		if (priority_color) fill_color = *priority_color;
		else fill_color = ColorFor(living->allegiance);

		const ImU32 bg_color = TintedBackground(fill_color);
		const ImU32 border_color = pb.is_targeted ? settings_.target_border_color : settings_.border_color;

		float opacity_mult = 1.f;
		if (settings_.fade_enemies_by_range && living->allegiance == GW::Constants::Allegiance::Enemy && !pb.is_targeted) {
			opacity_mult = GetRangeOpacityMultiplier(pb.dist_sq_from_me);
		}

		draw_list->AddRectFilled(top_left, bottom_right, ScaleAlpha(bg_color, opacity_mult));
		draw_list->AddRectFilled(top_left, fill_bottom_right, ScaleAlpha(fill_color, opacity_mult));
		draw_list->AddRect(top_left, bottom_right, ScaleAlpha(border_color, opacity_mult), 0.f, 0, settings_.border_thickness);
		DrawStatusTriangles(draw_list, bottom_right.x - 8.f, top_left.y + bar_height / 2.f, living, opacity_mult);
		CheckClickToTarget(top_left, bottom_right, living, left_clicked_this_frame);

		if (!pb.display->empty() && font) {
			const float font_size = kNameplateFontSize;
			constexpr float kPadding = 6.f;
			const float max_text_width = bar_width * 0.8f - kPadding;

			if (max_text_width > 0.f) {
				const std::string& clipped_utf8 = name_cache_.GetTruncated(living->agent_id, font, font_size, max_text_width);
				const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, clipped_utf8.c_str());

				const float text_x = top_left.x + kPadding;
				const float text_y = top_left.y + (bar_height - text_size.y) / 2.f;

				static constexpr ImU32 kNormalTextColor = IM_COL32(255, 255, 255, 255);
				const bool is_enemy_in_combat = settings_.color_nameplate_text_by_combat
					&& living->allegiance == GW::Constants::Allegiance::Enemy && pb.is_in_combat;
				const ImU32 name_text_color = is_enemy_in_combat ? settings_.combat_text_color : kNormalTextColor;
				DrawOutlinedText(draw_list, font, font_size, ImVec2(text_x, text_y), name_text_color, clipped_utf8, opacity_mult);
			}
		}

		if (settings_.show_priority_castbars && priority_color) {
			DrawCastBar(draw_list, top_left, bar_width, bottom_right.y, living->agent_id, opacity_mult, font);
		}
	}

	[[nodiscard]] static bool IsCastBarVisible(const CastStateCache::CastState* cast, ULONGLONG now) {
		if (!cast) return false;
		if (cast->casting) return true;
		return cast->ended_at_ms != 0 && (now - cast->ended_at_ms) < kCastLingerMs;
	}

	[[nodiscard]] bool CastBarIsVisible(uint32_t agent_id) {
		return IsCastBarVisible(cast_cache_.Find(agent_id), GetTickCount64());
	}

	void DrawCastBar(ImDrawList* draw_list, const ImVec2& nameplate_top_left, float bar_width, float nameplate_bottom_y, uint32_t agent_id, float opacity_mult, ImFont* font) {
		const CastStateCache::CastState* cast = cast_cache_.Find(agent_id);
		const ULONGLONG now = GetTickCount64();
		if (!IsCastBarVisible(cast, now)) return;
		const bool lingering = !cast->casting;
		const bool flashing = lingering && cast->was_cancelled;

		const float height = settings_.castbar_height;
		const ImVec2 top_left(nameplate_top_left.x, nameplate_bottom_y);
		const ImVec2 icon_bottom_right(top_left.x + height, top_left.y + height);
		const ImVec2 bar_bottom_right(top_left.x + bar_width, top_left.y + height);
		const ImVec2 bar_top_left(icon_bottom_right.x, top_left.y);

		const ImU32 castbar_bg_color = TintedBackground(settings_.castbar_fill_color);

		draw_list->AddRectFilled(top_left, bar_bottom_right, ScaleAlpha(castbar_bg_color, opacity_mult));

		if (get_skill_image_) {
			IDirect3DTexture9** texture = get_skill_image_(cast->skill_id);
			if (texture && *texture) {
				draw_list->AddImage(*texture, top_left, icon_bottom_right);
			}
		}

		if (cast->cast_time_ms > 0.f) {
			const ULONGLONG reference_time = cast->casting ? now : cast->ended_at_ms;
			const float elapsed = static_cast<float>(reference_time - cast->cast_start_ms);
			const float pct = std::clamp(elapsed / cast->cast_time_ms, 0.f, 1.f);
			const ImVec2 fill_bottom_right(bar_top_left.x + (bar_width - height) * pct, top_left.y + height);
			draw_list->AddRectFilled(bar_top_left, fill_bottom_right, ScaleAlpha(settings_.castbar_fill_color, opacity_mult));
		}

		if (font) {
			const std::string& skill_name = skill_name_cache_.Get(cast->skill_id);
			if (!skill_name.empty()) {
				const float text_font_size = height * 0.8f;
				draw_list->PushClipRect(bar_top_left, bar_bottom_right, true);
				DrawOutlinedText(draw_list, font, text_font_size, ImVec2(bar_top_left.x + 3.f, top_left.y + (height - text_font_size) / 2.f), IM_COL32(255, 255, 255, 255), skill_name, opacity_mult);
				draw_list->PopClipRect();
			}
		}

		if (flashing) {
			draw_list->AddRectFilled(bar_top_left, bar_bottom_right, ScaleAlpha(settings_.castbar_cancelled_color, opacity_mult * 0.25f));
		}

		draw_list->AddRect(top_left, bar_bottom_right, ScaleAlpha(IM_COL32(0, 0, 0, 180), opacity_mult), 0.f, 0, 1.f);
	}

	[[nodiscard]] ImU32 ColorFor(GW::Constants::Allegiance allegiance) const {
		switch (allegiance) {
			case GW::Constants::Allegiance::Enemy:
				return settings_.enemy_color;
			case GW::Constants::Allegiance::Spirit_Pet:
			case GW::Constants::Allegiance::Minion:
				return IM_COL32(40, 200, 60, 255);
			default:
				return settings_.friendly_color;
		}
	}

	[[nodiscard]] ImU32 ProfessionColor(GW::Constants::ProfessionByte prof) const {
		static constexpr std::array<ImU32, 11> kColors = {
			IM_COL32(221, 221, 221, 255), IM_COL32(255, 255, 136, 255),
			IM_COL32(204, 255, 153, 255), IM_COL32(170, 204, 255, 255),
			IM_COL32(153, 255, 204, 255), IM_COL32(221, 170, 255, 255),
			IM_COL32(255, 187, 187, 255), IM_COL32(255, 204, 238, 255),
			IM_COL32(187, 255, 255, 255), IM_COL32(255, 204, 153, 255),
			IM_COL32(221, 221, 255, 255)
		};
		const size_t index = static_cast<size_t>(prof);
		return (index < kColors.size()) ? kColors[index] : kColors[0];
	}

	static void FlashPreference(GW::UI::FlagPreference pref) {
		GW::GameThread::Enqueue([pref] {
			const bool current = GW::UI::GetPreference(pref);
			GW::UI::SetPreference(pref, !current);
			GW::UI::SetPreference(pref, current);
		});
	}

	template<typename Func>
	static void FlashOnChange(std::optional<bool>& last_state, bool current_state, Func&& flash) {
		if (last_state.has_value() && *last_state != current_state) {
			flash();
		}
		last_state = current_state;
	}

	static void OnAgentNameTag(GW::HookStatus* status, GW::UI::UIMessage msgid, void* wParam, void*) {
		if (msgid != GW::UI::UIMessage::kShowAgentNameTag && msgid != GW::UI::UIMessage::kSetAgentNameTagAttribs) return;
		auto* self = static_cast<NameplatesPlugin*>(ToolboxPluginInstance());
		self->HandleAgentNameTag(status, static_cast<GW::UI::AgentNameTagInfo*>(wParam));
	}

	void HandleAgentNameTag(GW::HookStatus* status, GW::UI::AgentNameTagInfo* tag) const {
		if (!tag) return;

		GW::Agent* agent = GW::Agents::GetAgentByID(tag->agent_id);
		GW::AgentLiving* living = agent ? agent->GetAsAgentLiving() : nullptr;

		if (settings_.hide_enemy_native_nametags && status && living
			&& living->allegiance == GW::Constants::Allegiance::Enemy) {
			status->blocked = true;
			return;
		}

		if (!settings_.recolor_quest_nametags && !settings_.recolor_professions) return;
		if (!living) return;

		if (settings_.recolor_professions
			&& living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable) {
			tag->text_color = ProfessionColor(living->primary);
			return;
		}

		if (settings_.recolor_quest_nametags && living->GetHasQuest()) {
			tag->text_color = settings_.quest_color;
		}
	}

	static void OnSkillCastMessage(GW::HookStatus*, GW::UI::UIMessage msgid, void* wParam, void*) {
		auto* self = static_cast<NameplatesPlugin*>(ToolboxPluginInstance());
		switch (msgid) {
			case GW::UI::UIMessage::kAgentSkillStartedCast: {
				const auto packet = static_cast<GW::UI::UIPacket::kAgentSkillStartedCast*>(wParam);
				self->HandleSkillStartedCast(packet->agent_id, packet->skill_id, packet->duration);
			} break;
			case GW::UI::UIMessage::kAgentSkillActivated:
			case GW::UI::UIMessage::kAgentSkillActivatedInstantly: {
				const auto packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wParam);
				self->cast_cache_.OnCompleted(packet->agent_id, packet->skill_id);
			} break;
			case GW::UI::UIMessage::kAgentSkillCancelled: {
				const auto packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wParam);
				self->cast_cache_.OnCancelled(packet->agent_id, packet->skill_id);
			} break;
			default: break;
		}
	}

	void HandleSkillStartedCast(uint32_t agent_id, GW::Constants::SkillID skill_id, float duration) {
		if (!settings_.show_priority_castbars) return;
		const auto name_lookup = name_cache_.Get(agent_id, GW::Agents::GetAgentEncName(agent_id));
		if (!GetPriorityColor(*name_lookup.lower)) return;
		cast_cache_.OnStartedCast(agent_id, skill_id, duration * 1000.f);
	}

	void EnsureTextureHookInstalled() {
		if (create_texture_func_) return;
		IDirect3DDevice9* device = GW::Render::GetDevice();
		if (!device) return;
		uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(device);
		constexpr int kCreateTextureIndex = 23;
		create_texture_func_ = reinterpret_cast<CreateTextureFn>(vtable[kCreateTextureIndex]);
		GW::Hook::CreateHook(reinterpret_cast<void**>(&create_texture_func_), OnD3D9CreateTexture, reinterpret_cast<void**>(&create_texture_ret_));
		GW::Hook::EnableHooks(reinterpret_cast<void*>(create_texture_func_));
	}

	static void NukeTextureInPlace(IDirect3DTexture9* texture) {
		D3DSURFACE_DESC desc;
		if (FAILED(texture->GetLevelDesc(0, &desc))) return;
		D3DLOCKED_RECT locked;
		if (FAILED(texture->LockRect(0, &locked, nullptr, 0)) || !locked.pBits || locked.Pitch <= 0) return;
		auto* bits = static_cast<uint8_t*>(locked.pBits);
		for (UINT row = 0; row < desc.Height; ++row) {
			memset(bits + row * static_cast<size_t>(locked.Pitch), 0, static_cast<size_t>(locked.Pitch));
		}
		texture->UnlockRect(0);
	}

	static HRESULT WINAPI OnD3D9CreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** out_texture, HANDLE* shared_handle) {
		auto* self = static_cast<NameplatesPlugin*>(ToolboxPluginInstance());
		GW::Hook::EnterHook();
		const HRESULT result = self->create_texture_ret_(device, width, height, levels, usage, format, pool, out_texture, shared_handle);

		if (auto tex = self->last_created_texture_) {
			self->last_created_texture_ = nullptr;
			if (self->settings_.hide_enemy_native_healthbar) {
				const uint32_t hash = ComputeTexmodHash(tex);
				if (hash == kTargetHealthbarHash || hash == kTargetIndicatorHash) {
					NukeTextureInPlace(tex);
				}
			}
			tex->Release();
		}

		if (SUCCEEDED(result) && out_texture && *out_texture && pool != D3DPOOL_DEFAULT) {
			self->last_created_texture_ = *out_texture;
			self->last_created_texture_->AddRef();
		}

		GW::Hook::LeaveHook();
		return result;
	}

	void DrawPriorityInput(const char* label, uint32_t& color, char* buf, std::string& raw, std::vector<std::wstring>& names) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImColor(color).Value);
		const bool changed = ImGui::InputText(label, buf, 512);
		ImGui::PopStyleColor();
		if (changed) {
			raw = buf;
			names = ParseSemicolonNameList(raw);
		}
		ImGui::SameLine();
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		const std::string picker_id = std::string("##color_") + label;
		if (ImGui::ColorEdit3(picker_id.c_str(), &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
	}

	void DrawSettingsInternal() {
		ImGui::SeparatorText("Explorable Areas");

		DrawCheckboxWithColor("Show enemy nameplates", settings_.show_enemies, settings_.enemy_color, "##color_show_enemies");
		DrawCheckboxWithColor("Show friendly nameplates", settings_.show_friendlies, settings_.friendly_color, "##color_friendly");

		ImGui::Checkbox("Show summoned friendly nameplates", &settings_.show_summoned_allies);
		ShowHelpMarker("Show spirits, minions & summoning stones, minipets are always hidden");

		ImGui::Checkbox("Use nameplate alpha", &settings_.fade_enemies_by_range);
		ShowHelpMarker("Nameplates fade in steps: \n0-1500 range, 100% opaque \n1500-2500 range, 75% transparency \n2500 range and above, 50% transparency");

		DrawCheckboxWithColor("Color nameplate text by combat status", settings_.color_nameplate_text_by_combat, settings_.combat_text_color, "##color_combat_text");
		ShowHelpMarker("Enemies that are in-combat stance regardless of distance have their name colored, \nenemies within earshot and are moving are also colored this way");

		const float border_thickness_width = (ImGui::CalcItemWidth() - ImGui::GetStyle().ItemInnerSpacing.x) / 2.f;
		ImGui::SetNextItemWidth(border_thickness_width);
		ImGui::SliderFloat("##border_thickness", &settings_.border_thickness, 1.0f, 3.0f, "%.1f");
		ImGui::SameLine();
		ImGui::TextUnformatted("Border thickness");
		ImGui::SameLine();
		ImVec4 border_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.border_color);
		if (ImGui::ColorEdit3("##color_border", &border_color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			settings_.border_color = ImGui::ColorConvertFloat4ToU32(border_color_vec);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted("Target border color");
		ImGui::SameLine();
		ImVec4 target_border_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.target_border_color);
		if (ImGui::ColorEdit3("##color_target_border", &target_border_color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			settings_.target_border_color = ImGui::ColorConvertFloat4ToU32(target_border_color_vec);
		}

		int thresholds[2] = {
			static_cast<int>(std::lround(settings_.npc_health_threshold)),
			static_cast<int>(std::lround(settings_.allied_health_threshold))
		};
		if (ImGui::SliderInt2("NPC & ally visibility threshold", thresholds, 0, 100)) {
			settings_.npc_health_threshold = static_cast<float>(thresholds[0]);
			settings_.allied_health_threshold = static_cast<float>(thresholds[1]);
		}
		ShowHelpMarker("0 = off, 100 = on");

		if (ImGui::SliderFloat("Max range", &settings_.max_range, 500.f, 5000.f, "%.0f")) {
			settings_.max_range = std::round(settings_.max_range);
		}

		const float half_width = (ImGui::CalcItemWidth() - ImGui::GetStyle().ItemInnerSpacing.x) / 2.f;
		ImGui::PushItemWidth(half_width);
		if (ImGui::SliderFloat("##bar_width", &settings_.bar_width, 50.f, 300.f, "%.0f")) {
			settings_.bar_width = std::round(settings_.bar_width);
		}
		ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
		if (ImGui::SliderFloat("##bar_height", &settings_.bar_height, 15.f, 30.f, "%.0f")) {
			settings_.bar_height = std::round(settings_.bar_height);
		}
		ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::TextUnformatted("Bar width & height");

		ImGui::Separator();
		ImGui::TextUnformatted("Priority nameplate coloring");
		ShowHelpMarker("Semicolon-separated. e.g. \"Charr Shaman; Keeper of Souls\"");

		for (size_t i = 0; i < 3; ++i) {
			const std::string label = "Priority " + std::to_string(i + 1);
			DrawPriorityInput(label.c_str(), settings_.priorities[i].color, priority_states_[i].buf, settings_.priorities[i].raw, priority_states_[i].names);
		}

		ImGui::Checkbox("Show cast bars for priority targets", &settings_.show_priority_castbars);
		ShowHelpMarker("Only agents matching a priority slot above get a cast bar, to avoid clutter when many enemies cast at once");

		if (settings_.show_priority_castbars) {
			if (ImGui::SliderFloat("##castbar_height", &settings_.castbar_height, 10.f, 24.f, "%.0f")) {
				settings_.castbar_height = std::round(settings_.castbar_height);
			}
			ImGui::SameLine();
			ImGui::TextUnformatted("Cast bar height");

			ImVec4 castbar_fill_vec = ImGui::ColorConvertU32ToFloat4(settings_.castbar_fill_color);
			if (ImGui::ColorEdit3("##color_castbar_fill", &castbar_fill_vec.x, ImGuiColorEditFlags_NoInputs)) {
				settings_.castbar_fill_color = ImGui::ColorConvertFloat4ToU32(castbar_fill_vec);
			}
			ImGui::SameLine();
			ImGui::TextUnformatted("Cast bar fill color");

			ImVec4 castbar_cancelled_vec = ImGui::ColorConvertU32ToFloat4(settings_.castbar_cancelled_color);
			if (ImGui::ColorEdit3("##color_castbar_cancelled", &castbar_cancelled_vec.x, ImGuiColorEditFlags_NoInputs)) {
				settings_.castbar_cancelled_color = ImGui::ColorConvertFloat4ToU32(castbar_cancelled_vec);
			}
			ImGui::SameLine();
			ImGui::TextUnformatted("Cast bar cancelled flash color");
		}

		ImGui::SeparatorText("All Areas");

		DrawCheckboxWithColor("Color quest-giver nametags", settings_.recolor_quest_nametags, settings_.quest_color, "##color_quest");

		ImGui::Checkbox("Color ally nametags by profession", &settings_.recolor_professions);
		ShowHelpMarker("Works on Players/Heroes/Henchmen");

		ImGui::Checkbox("Manage foe/player nametag game setting", &settings_.auto_toggle_show_names);
		ShowHelpMarker("Manages the 'Menu > Options > General' setting 'Show foe names...', \nOFF in explorable areas, ON in outposts");

		ImGui::Checkbox("Hide enemy native nametag", &settings_.hide_enemy_native_nametags);
		ShowHelpMarker("Experimental: blocks the game's own nametag on enemies, since 'Show foe names' has no effect on your current target. Enemies only, never friendlies or NPCs.");

		ImGui::Checkbox("Hide enemy overhead health bar & target indicator", &settings_.hide_enemy_native_healthbar);
		ShowHelpMarker("Experimental: identifies the two textures the game uses for the overhead health bar and the animated target arrow by content hash, and blanks them in place. Costs a small amount of FPS while new textures are loading in, since every loaded texture has to be hashed to find them.");
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

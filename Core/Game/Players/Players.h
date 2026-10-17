#pragma once

namespace Players
{
	auto PlayersRender() -> void
	{
		// Snapshot the camera once per frame so WorldToScreen / GetPosition /
		// GetCameraForward / GetCameraYaw read from memory instead of round-
		// tripping the driver on every call. Saves ~120 syscalls/frame.
		SDK::BeginFrame();

		// Snapshot ActorEntries under the cache mutex so the cache thread's
		// refresh-move can't pull the vector out from under our iteration. The
		// lock is held only for the copy (microseconds); render iterates the
		// stable local copy after release. Without this, the process hard-faults
		// once the entity count gets large enough that refresh-iteration windows
		// start overlapping (i.e. when you load into a real round).
		std::vector<ActorEntry> actor_snapshot;
		{
			std::lock_guard<std::mutex> lk(EntityState::g_cache_mtx);
			actor_snapshot = Cached.ActorEntries;
		}

		Vector3 fwd = SDK::GetCameraForward();

		if (Gun.Enabled && GetAsyncKeyState(VK_LBUTTON))
		{
			Recoil::Compensate(fwd);
		}
		else
		{
			Recoil::lastForward = fwd;
			Recoil::pendingX = Recoil::pendingY = 0;
			Recoil::initialized = false;
		}

		ImGui::PushFont(DefaultFont);

		if (Aimbot.FovEnable) FovCutsom(Aimbot.fov, false, false, false, 5);

		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		Vector3 CamPosition = SDK::GetPosition();
		ImVec2 screen_center(Monitor.Width * 0.5f, Monitor.Height);
		float target_dist = FLT_MAX;

		constexpr float GROUP_RADIUS_XY    = 1.5f;
		constexpr float GROUP_RADIUS_Z     = 2.0f;
		constexpr float GROUP_RADIUS_XY_SQ = GROUP_RADIUS_XY * GROUP_RADIUS_XY;
		constexpr float GROUP_RADIUS_Z_SQ  = GROUP_RADIUS_Z * GROUP_RADIUS_Z;
		const float HEAD_OFFSET      = 1.70f;
		const float HEAD_Z_EXTRA     = 0.2f;
		const float HEAD_MAX_Z_ABOVE = 1.3f;

		struct PlayerGroup {
			Vector3  base_pos;
			Vector3  best_head_pos;
			float    best_head_diff;
			uint64_t actor_handle;
			float    head_sum_z;
			int      head_count;
		};

		std::vector<PlayerGroup> player_groups;
		player_groups.reserve(16);

		// ── Debug: show filter byte for every actor within 5m ────────────────
		if (Visuals.DebugFilterByte)
		{
			struct DebugEntry { Vector2 screen; uint64_t fb; };
			std::vector<DebugEntry> entries;

			for (auto& e : actor_snapshot)
			{
				bool v50 = SDK::IsValidWorldPos(e.pos50);
				bool v60 = SDK::IsValidWorldPos(e.pos60);
				if (!v50 && !v60) continue;
				Vector3 pos = (v60 && !v50) ? e.pos60 : (v50 && !v60) ? e.pos50
				            : (fabsf(e.pos60.z) > fabsf(e.pos50.z)) ? e.pos60 : e.pos50;
				Vector2 screen = SDK::WorldToScreen(pos);
				if (screen.x == 0.f) continue;

				entries.push_back({ screen, e.fb });
			}

			// Group entries by screen proximity and render as stacked list
			std::vector<bool> rendered(entries.size(), false);
			for (size_t i = 0; i < entries.size(); i++)
			{
				if (rendered[i]) continue;

				std::vector<uint64_t> group_fbs;
				group_fbs.push_back(entries[i].fb);
				Vector2 anchor = entries[i].screen;

				for (size_t j = i + 1; j < entries.size(); j++)
				{
					if (rendered[j]) continue;
					float dx = entries[j].screen.x - anchor.x;
					float dy = entries[j].screen.y - anchor.y;
					if (dx*dx + dy*dy < 100.f) // 10px radius
					{
						group_fbs.push_back(entries[j].fb);
						rendered[j] = true;
					}
				}
				rendered[i] = true;

				float line_h = ImGui::GetTextLineHeight();
				for (size_t k = 0; k < group_fbs.size(); k++)
				{
					uint64_t fb = group_fbs[k];
					uint8_t left  = (uint8_t)((fb >> 16) & 0xFF);
					uint8_t right = (uint8_t)((fb >> 32) & 0xFF);
					char buf[32];
					sprintf_s(buf, sizeof(buf), "L:%02X R:%02X", left, right);
					float y = anchor.y + k * line_h;
					draw->AddText(ImVec2(anchor.x + 1, y + 1), IM_COL32(0, 0, 0, 255), buf);
					draw->AddText(ImVec2(anchor.x,     y),     IM_COL32(255, 255, 0, 255), buf);
				}
			}
		}

		// ── Pass 1: group unique players by proximity ──────────────────────────
		for (auto& e : actor_snapshot) {
			if (!SDK::IsActorPlayer(e.fb)) continue;
			bool should_render = Visuals.TeamCheck
				? SDK::ShouldRenderActor(e.fb)
				: SDK::ShouldRenderActorNoTeamCheck(e.fb);
			if (!should_render) continue;
			if (Visuals.DeadCheck && SDK::IsPlayerDead(e.fb)) continue;
			if (e.pos.empty()) continue;
			bool already_grouped = false;
			for (auto& g : player_groups) {
				float dx = e.pos.x - g.base_pos.x;
				float dy = e.pos.y - g.base_pos.y;
				float dz = e.pos.z - g.base_pos.z;
				if ((dx * dx + dy * dy) < GROUP_RADIUS_XY_SQ && (dz * dz) < GROUP_RADIUS_Z_SQ) {
					already_grouped = true; break;
				}
			}
			if (already_grouped) continue;
			PlayerGroup pg;
			pg.base_pos       = e.pos;
			pg.best_head_pos  = { e.pos.x, e.pos.y, e.pos.z + HEAD_OFFSET + HEAD_Z_EXTRA };
			pg.best_head_diff = FLT_MAX;
			pg.actor_handle   = e.ptr;
			pg.head_sum_z     = 0.f;
			pg.head_count     = 0;
			player_groups.push_back(pg);
		}

		// ── Pass 2: find best head position for each group ─────────────────────
		for (auto& e : actor_snapshot) {
			if (!SDK::IsActorPlayer(e.fb)) continue;
			const Vector3& pos = e.pos;
			if (pos.empty()) continue;
			for (auto& g : player_groups) {
				float dx = pos.x - g.base_pos.x;
				float dy = pos.y - g.base_pos.y;
				float dz = pos.z - g.base_pos.z;
				if ((dx * dx + dy * dy) < GROUP_RADIUS_XY_SQ && (dz * dz) < GROUP_RADIUS_Z_SQ) {
					float z_above = pos.z - g.base_pos.z;
					if (z_above > 0.05f && z_above <= HEAD_MAX_Z_ABOVE) {
						float z_diff = fabsf(z_above - HEAD_OFFSET);
						if (z_diff < g.best_head_diff) {
							g.best_head_diff = z_diff;
							g.head_sum_z     = pos.z;
							g.head_count     = 1;
						}
					}
					break;
				}
			}
		}

		// ── Resolve heads ──────────────────────────────────────────────────────
		for (auto& g : player_groups) {
			if (g.head_count > 0) {
				float z_ceil  = g.base_pos.z + HEAD_MAX_Z_ABOVE;
				float final_z = g.head_sum_z < z_ceil ? g.head_sum_z : z_ceil;
				g.best_head_pos = { g.base_pos.x, g.base_pos.y, final_z + HEAD_Z_EXTRA };
			}
			else {
				float off = HEAD_OFFSET < HEAD_MAX_Z_ABOVE ? HEAD_OFFSET : HEAD_MAX_Z_ABOVE;
				g.best_head_pos = { g.base_pos.x, g.base_pos.y, g.base_pos.z + off + HEAD_Z_EXTRA };
			}
		}

		// Player count display
		{
			char player_count_text[32];
			sprintf_s(player_count_text, sizeof(player_count_text), "Players: %d", (int)player_groups.size());
			draw->AddText(ImVec2(10.f, 10.f), IM_COL32(255, 255, 255, 255), player_count_text);
		}

		// ── Pass 3: aimbot target selection ────────────────────────────────────
		uint64_t target_actor  = 0;
		Vector2  target_screen = { 0, 0 };

		for (auto& g : player_groups) {
			float dx = g.base_pos.x - CamPosition.x;
			float dy = g.base_pos.y - CamPosition.y;
			float dz = g.base_pos.z - CamPosition.z;
			if (std::sqrt(dx * dx + dy * dy + dz * dz) > Visuals.MaxDistance) continue;

			Vector2 aim_screen = SDK::WorldToScreen(g.best_head_pos);
			if (aim_screen.x == 0.f) continue;

			if (Aimbot.Enabled) {
				float ax   = aim_screen.x - (Monitor.Width  * 0.5f);
				float ay   = aim_screen.y - (Monitor.Height * 0.5f);
				float dist = sqrtf(ax * ax + ay * ay);
				if (dist < Aimbot.fov && dist < target_dist) {
					target_dist   = dist;
					target_screen = aim_screen;
					target_actor  = g.actor_handle;
				}
			}
		}

		// ── Pass 4: draw visuals ───────────────────────────────────────────────
		for (auto& g : player_groups) {
			// Find cached entry for this actor (no kernel reads)
			const ActorEntry* entry = nullptr;
			for (auto& e : actor_snapshot)
				if (e.ptr == g.actor_handle) { entry = &e; break; }
			uint64_t FilterByte = entry ? entry->fb : 0;

			float dx = g.base_pos.x - CamPosition.x;
			float dy = g.base_pos.y - CamPosition.y;
			float dz = g.base_pos.z - CamPosition.z;
			float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (distance > Visuals.MaxDistance) continue;

			auto [bounds_min, bounds_max] = SDK::GetActorBounds();

			Vector2 feet_screen = SDK::WorldToScreen(g.base_pos);
			Vector2 head_screen = SDK::WorldToScreen(g.best_head_pos);

			if (feet_screen.x == 0.f || head_screen.x == 0.f) continue;

			bool  is_targeted  = (Aimbot.Enabled && g.actor_handle == target_actor);
			ImU32 color        = is_targeted ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(Visuals.BoxColor);
			ImU32 arrowsColor  = ImGui::GetColorU32(Visuals.ArrowColor);
			ImU32 border_color = IM_COL32(0, 0, 0, 255);

			float box_height  = feet_screen.y - head_screen.y;
			float box_width   = box_height * 0.72f;  // 20% breiter als original 0.6f (2D/Corner)
			float left        = feet_screen.x - box_width / 2.0f;
			float head_radius = std::clamp(box_height * 0.12f, 3.0f, 20.0f);
			float box_top     = Visuals.HeadCircle ? head_screen.y - head_radius : head_screen.y;

			if (Visuals.Snapline) {
				draw->AddLine(
					screen_center,
					ImVec2(feet_screen.x, feet_screen.y),
					IM_COL32(255, 255, 255, 120),
					2.0f
				);
			}

			if (Visuals.Distances) {
				char distance_text[48];
				sprintf_s(distance_text, sizeof(distance_text), "[%.0fm]", distance);

				float scale = std::clamp(box_height / 200.0f * 1.5f, 0.9f, 2.25f);  // 50% größer

				ImFont* font = ImGui::GetFont();
				float originalScale = font->Scale;
				font->Scale = scale;
				ImGui::PushFont(font);

				ImVec2 dist_text_size = ImGui::CalcTextSize(distance_text);
				float padding = 3.0f * scale;

				// Centered below the box (under feet)
				ImVec2 text_pos(
					feet_screen.x - dist_text_size.x * 0.5f,
					feet_screen.y + padding
				);

				// White text with black outline, no background
				ImVec2 outline_offsets[] = { {-1,0},{1,0},{0,-1},{0,1} };
				for (auto& off : outline_offsets)
					draw->AddText(ImVec2(text_pos.x + off.x, text_pos.y + off.y), IM_COL32(0, 0, 0, 255), distance_text);
				draw->AddText(text_pos, IM_COL32(255, 255, 255, 255), distance_text);

				ImGui::PopFont();
				font->Scale = originalScale;
			}

			if (Visuals.Arrow) {
				float direction = entry ? entry->direction : 0.f;

				static std::map<uint64_t, bool>    actor_is_south;
				static std::map<uint64_t, Vector3> actor_last_pos;

				uint64_t addr = g.actor_handle;
				float val = std::clamp(direction, -1.0f, 1.0f);

				Vector3 last_pos = actor_last_pos.count(addr) ? actor_last_pos[addr] : g.base_pos;
				actor_last_pos[addr] = g.base_pos;

				float move_x = g.base_pos.x - last_pos.x;
				float move_y = g.base_pos.y - last_pos.y;
				if ((move_x * move_x + move_y * move_y) > 0.0001f)
					actor_is_south[addr] = (move_y < 0);

				float ax = val;
				float ay = std::sqrt(1.0f - (ax * ax));
				if (actor_is_south[addr]) ay = -ay;

				Vector3 end_point = g.base_pos;
				end_point.x += ax * 1.0f;
				end_point.y += ay * 1.0f;

				Vector2 screen_pos = SDK::WorldToScreen(end_point);
				if (screen_pos.x != 0.f && feet_screen.x != 0.f) {
					draw->AddLine(ImVec2(feet_screen.x, feet_screen.y), ImVec2(screen_pos.x, screen_pos.y), arrowsColor, 2.0f);
					draw->AddCircleFilled(ImVec2(screen_pos.x, screen_pos.y), 4.0f, arrowsColor);
				}
			}

			if (Visuals.Box) {
				switch (Visuals.BoxType) {
				case 0:
				{
					draw->AddRect(
						ImVec2(left, box_top),
						ImVec2(left + box_width, feet_screen.y),
						border_color, 0.0f, 0, 3.0f
					);
					draw->AddRect(
						ImVec2(left, box_top),
						ImVec2(left + box_width, feet_screen.y),
						color, 0.0f, 0, 1.0f
					);
				}
				break;
				case 1:
				{
					float right       = feet_screen.x + box_width / 2;
					float top         = box_top;
					float bottom      = feet_screen.y;
					float corner_size = box_width * 0.25f;

					draw->AddLine(ImVec2(left, top),                  ImVec2(left + corner_size, top),          border_color, 3.0f);
					draw->AddLine(ImVec2(left, top),                  ImVec2(left, top + corner_size),          border_color, 3.0f);
					draw->AddLine(ImVec2(left, top),                  ImVec2(left + corner_size, top),          color, 1.0f);
					draw->AddLine(ImVec2(left, top),                  ImVec2(left, top + corner_size),          color, 1.0f);

					draw->AddLine(ImVec2(right - corner_size, top),   ImVec2(right, top),                       border_color, 3.0f);
					draw->AddLine(ImVec2(right, top),                 ImVec2(right, top + corner_size),         border_color, 3.0f);
					draw->AddLine(ImVec2(right - corner_size, top),   ImVec2(right, top),                       color, 1.0f);
					draw->AddLine(ImVec2(right, top),                 ImVec2(right, top + corner_size),         color, 1.0f);

					draw->AddLine(ImVec2(left, bottom - corner_size), ImVec2(left, bottom),                     border_color, 3.0f);
					draw->AddLine(ImVec2(left, bottom),               ImVec2(left + corner_size, bottom),       border_color, 3.0f);
					draw->AddLine(ImVec2(left, bottom - corner_size), ImVec2(left, bottom),                     color, 1.0f);
					draw->AddLine(ImVec2(left, bottom),               ImVec2(left + corner_size, bottom),       color, 1.0f);

					draw->AddLine(ImVec2(right, bottom - corner_size), ImVec2(right, bottom),                   border_color, 3.0f);
					draw->AddLine(ImVec2(right - corner_size, bottom), ImVec2(right, bottom),                   border_color, 3.0f);
					draw->AddLine(ImVec2(right, bottom - corner_size), ImVec2(right, bottom),                   color, 1.0f);
					draw->AddLine(ImVec2(right - corner_size, bottom), ImVec2(right, bottom),                   color, 1.0f);
				}
				break;
				case 2:
					if (!bounds_min.empty() && !bounds_max.empty()) {
						const float max_extent = 1.2f;  // 20% schmaler als original 1.5f
						bounds_min.x = std::clamp(bounds_min.x, -max_extent, max_extent);
						bounds_min.y = std::clamp(bounds_min.y, -max_extent, max_extent);
						bounds_max.x = std::clamp(bounds_max.x, -max_extent, max_extent);
						bounds_max.y = std::clamp(bounds_max.y, -max_extent, max_extent);

						Vector3 corners[8] = {
							g.base_pos + Vector3(bounds_min.x, bounds_min.y, bounds_min.z),
							g.base_pos + Vector3(bounds_min.x, bounds_max.y, bounds_min.z),
							g.base_pos + Vector3(bounds_max.x, bounds_max.y, bounds_min.z),
							g.base_pos + Vector3(bounds_max.x, bounds_min.y, bounds_min.z),
							g.base_pos + Vector3(bounds_min.x, bounds_min.y, bounds_max.z),
							g.base_pos + Vector3(bounds_min.x, bounds_max.y, bounds_max.z),
							g.base_pos + Vector3(bounds_max.x, bounds_max.y, bounds_max.z),
							g.base_pos + Vector3(bounds_max.x, bounds_min.y, bounds_max.z),
						};

						Vector2 screen[8];
						bool    valid[8];
						for (int i = 0; i < 8; ++i) {
							screen[i] = SDK::WorldToScreen(corners[i]);
							valid[i]  = (screen[i].x != 0.f && screen[i].y != 0.f);
						}

						for (int i = 0; i < 4; ++i) {
							if (valid[i] && valid[(i + 1) % 4])
								draw->AddLine(ImVec2(screen[i].x, screen[i].y), ImVec2(screen[(i + 1) % 4].x, screen[(i + 1) % 4].y), border_color, 3.0f);
							if (valid[i + 4] && valid[((i + 1) % 4) + 4])
								draw->AddLine(ImVec2(screen[i + 4].x, screen[i + 4].y), ImVec2(screen[((i + 1) % 4) + 4].x, screen[((i + 1) % 4) + 4].y), border_color, 3.0f);
							if (valid[i] && valid[i + 4])
								draw->AddLine(ImVec2(screen[i].x, screen[i].y), ImVec2(screen[i + 4].x, screen[i + 4].y), border_color, 3.0f);
						}
						for (int i = 0; i < 4; ++i) {
							if (valid[i] && valid[(i + 1) % 4])
								draw->AddLine(ImVec2(screen[i].x, screen[i].y), ImVec2(screen[(i + 1) % 4].x, screen[(i + 1) % 4].y), color, 1.0f);
							if (valid[i + 4] && valid[((i + 1) % 4) + 4])
								draw->AddLine(ImVec2(screen[i + 4].x, screen[i + 4].y), ImVec2(screen[((i + 1) % 4) + 4].x, screen[((i + 1) % 4) + 4].y), color, 1.0f);
							if (valid[i] && valid[i + 4])
								draw->AddLine(ImVec2(screen[i].x, screen[i].y), ImVec2(screen[i + 4].x, screen[i + 4].y), color, 1.0f);
						}
					}
					break;
				}
			}

			if (Visuals.HeadCircle) {
				ImU32 headColor = ImGui::GetColorU32(Visuals.HeadColor);
				draw->AddCircle(
					ImVec2(head_screen.x, head_screen.y),
					head_radius,
					headColor,
					0,
					1.5f
				);
			}
		}

		if (Aimbot.Enabled && target_screen.x != 0 && target_screen.y != 0)
		{
			if (Aimbot.line)
			{
				ImVec2 origin(Monitor.Width / 2.f, Monitor.Height / 2.f);
				ImVec2 target(target_screen.x, target_screen.y);
				// Black outline
				ImGui::GetBackgroundDrawList()->AddLine(origin, target, IM_COL32(0, 0, 0, 255), 3.f);
				// White line on top
				ImGui::GetBackgroundDrawList()->AddLine(origin, target, IM_COL32(255, 255, 255, 255), 1.f);
			}
			if (GetAsyncKeyState(Aimbot.AimKey)) {
				perform(target_screen);
			}
		}
		ImGui::PopFont();

		// ── Gadget ESP ────────────────────────────────────────────────────────
		if (Gadget.ESP)
		{
			ImU32 gadget_color = ImGui::GetColorU32(Gadget.GadgetColor);

			for (auto& e : actor_snapshot)
			{
				if (!SDK::IsGadget(e.fb)) continue;
				if (SDK::IsGadgetDestroyed(e.fb)) continue;
				if (e.pos.empty()) continue;

				Vector2 screen = SDK::WorldToScreen(e.pos);
				if (screen.x == 0.f) continue;

				if (Gadget.Dot)
					draw->AddCircleFilled(ImVec2(screen.x, screen.y), 4.0f, gadget_color);

				if (Gadget.Box3D)
				{
					auto [bmin, bmax] = SDK::GetGadgetBounds();
					Vector3 corners[8] = {
						e.pos + Vector3(bmin.x, bmin.y, bmin.z),
						e.pos + Vector3(bmin.x, bmax.y, bmin.z),
						e.pos + Vector3(bmax.x, bmax.y, bmin.z),
						e.pos + Vector3(bmax.x, bmin.y, bmin.z),
						e.pos + Vector3(bmin.x, bmin.y, bmax.z),
						e.pos + Vector3(bmin.x, bmax.y, bmax.z),
						e.pos + Vector3(bmax.x, bmax.y, bmax.z),
						e.pos + Vector3(bmax.x, bmin.y, bmax.z),
					};
					Vector2 sc[8];
					bool    valid[8];
					for (int i = 0; i < 8; i++) {
						sc[i]    = SDK::WorldToScreen(corners[i]);
						valid[i] = (sc[i].x != 0.f);
					}
					ImU32 border = IM_COL32(0, 0, 0, 200);
					for (int i = 0; i < 4; i++) {
						if (valid[i] && valid[(i+1)%4])       { draw->AddLine(ImVec2(sc[i].x,sc[i].y), ImVec2(sc[(i+1)%4].x,sc[(i+1)%4].y), border, 3.f); draw->AddLine(ImVec2(sc[i].x,sc[i].y), ImVec2(sc[(i+1)%4].x,sc[(i+1)%4].y), gadget_color, 1.f); }
						if (valid[i+4] && valid[((i+1)%4)+4]) { draw->AddLine(ImVec2(sc[i+4].x,sc[i+4].y), ImVec2(sc[((i+1)%4)+4].x,sc[((i+1)%4)+4].y), border, 3.f); draw->AddLine(ImVec2(sc[i+4].x,sc[i+4].y), ImVec2(sc[((i+1)%4)+4].x,sc[((i+1)%4)+4].y), gadget_color, 1.f); }
						if (valid[i] && valid[i+4])            { draw->AddLine(ImVec2(sc[i].x,sc[i].y), ImVec2(sc[i+4].x,sc[i+4].y), border, 3.f); draw->AddLine(ImVec2(sc[i].x,sc[i].y), ImVec2(sc[i+4].x,sc[i+4].y), gadget_color, 1.f); }
					}
				}
			}
		}
	}
}

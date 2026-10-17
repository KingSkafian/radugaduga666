#pragma once
#include <map>

namespace SDK
{
    // Camera

    // Per-frame camera snapshot. Populated once per render frame by
    // BeginFrame() and consumed by GetMatrix / GetPosition / WorldToScreen /
    // GetCameraForward / GetCameraYaw. Without this, every WorldToScreen
    // round-trips two driver reads (matrix + cam_pos), and PlayersRender
    // calls WorldToScreen 30-60x per frame — 120 syscalls/frame just for
    // the camera, all serialized through the comms mutex against the
    // CachePlayers thread. The snapshot is consistent within a frame and
    // the values don't change mid-frame anyway.
    inline Matrix4 g_frame_matrix{};
    inline Vector3 g_frame_cam_pos{};

    inline void BeginFrame()
    {
        if (!Cached.CameraPointer) return;
        g_frame_matrix  = g_backend->read<Matrix4>(Cached.CameraPointer + 0x240);
        g_frame_cam_pos = g_backend->read<Vector3>(Cached.CameraPointer + 0x390);
    }

    Matrix4 GetMatrix()
    {
        return g_frame_matrix;
    }

    Vector2 GetResolution()
    {
        Vector2 res;
        res.x = GetSystemMetrics(SM_CXSCREEN);
        res.y = GetSystemMetrics(SM_CYSCREEN);
        return res;
    }

    Vector3 GetPosition()
    {
        return g_frame_cam_pos;
    }

    Vector2 WorldToScreen(const Vector3& world_position)
    {
        const Matrix4& matrix = g_frame_matrix;
        Vector2 resolution = GetResolution();
        const Vector3& cam_pos = g_frame_cam_pos;

        Vector3 relative_pos;
        relative_pos.x = world_position.x - cam_pos.x;
        relative_pos.y = world_position.y - cam_pos.y;
        relative_pos.z = world_position.z - cam_pos.z;

        float clip_x = matrix.m[0] * relative_pos.x + matrix.m[4] * relative_pos.y + matrix.m[8] * relative_pos.z + matrix.m[12];
        float clip_y = matrix.m[1] * relative_pos.x + matrix.m[5] * relative_pos.y + matrix.m[9] * relative_pos.z + matrix.m[13];
        float clip_w = matrix.m[3] * relative_pos.x + matrix.m[7] * relative_pos.y + matrix.m[11] * relative_pos.z + matrix.m[15];

        if (clip_w <= 0.01f)
            return { 0, 0 };

        float ndc_x = clip_x / clip_w;
        float ndc_y = clip_y / clip_w;

        Vector2 screen_position;
        screen_position.x = (ndc_x + 1.0f) * 0.5f * resolution.x;
        screen_position.y = (1.0f - ndc_y) * 0.5f * resolution.y;

        return screen_position;
    }

    float GetCameraYaw()
    {
        const Matrix4& matrix = g_frame_matrix;
        float forward_x = matrix.m[2];
        float forward_y = matrix.m[6];
        float yaw = atan2f(forward_y, forward_x);
        return yaw += 1.57079632679f;
    }

    Vector2 RotatePoint2D(float x, float y, float angle)
    {
        float cos_angle = cosf(angle);
        float sin_angle = sinf(angle);

        Vector2 rotated;
        rotated.x = x * cos_angle - y * sin_angle;
        rotated.y = x * sin_angle + y * cos_angle;

        return rotated;
    }

    Vector2 WorldToRadar(const Vector3& world_position, const Vector3& cam_position, float radar_range = 100.0f)
    {
        float rel_x = world_position.x - cam_position.x;
        float rel_y = world_position.y - cam_position.y;

        float camera_yaw = GetCameraYaw();
        Vector2 rotated = RotatePoint2D(rel_x, rel_y, -camera_yaw);

        rotated.x = std::clamp(rotated.x / radar_range, -1.0f, 1.0f);
        rotated.y = std::clamp(rotated.y / radar_range, -1.0f, 1.0f);

        return rotated;
    }

    Vector3 GetCameraForward()
    {
        const Matrix4& matrix = g_frame_matrix;

        Vector3 forward;
        forward.x = matrix.m[2];
        forward.y = matrix.m[6];
        forward.z = matrix.m[10];

        float length = sqrtf(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        if (length > 0.0f) {
            forward.x /= length;
            forward.y /= length;
            forward.z /= length;
        }

        return forward;
    }

    // Actor

    // ─── IsValidPtr2 ─────────────────────────────────────────────────────────
    // Looser check: no alignment requirement, just address range.
    inline bool IsValidPtr2(uintptr_t ptr)
    {
        return ptr != 0 && ptr > 0x0 && ptr < 0x7FFFFFFFFFFF;
    }

    uint64_t ReadFilterByte()
    {
        return g_backend->read<uint64_t>(Cached.ActorPointer + 0xB8);
    }

    uint64_t ReadFilterByteDirect(uint64_t actor)
    {
        if (!IsValidPtr2(actor)) return 0;
        return g_backend->read<uint64_t>(actor + 0xB8);
    }

    bool ShouldRenderActor(uint64_t filterByte)
    {
        uint8_t byte4 = static_cast<uint8_t>(filterByte >> 32) & 0xFF;
        if (byte4 == 0x02 || byte4 == 0x00 || byte4 == 0x80 || byte4 == 0x82)
            return false;
        return true;
    }

    bool IsPlayerAlly(uint64_t filterByte)
    {
        uint8_t byte4 = static_cast<uint8_t>(filterByte >> 32);
        return byte4 == 0x02;
    }

    bool ShouldRenderActorNoTeamCheck(uint64_t filterByte)
    {
        uint8_t byte4 = static_cast<uint8_t>(filterByte >> 32) & 0xFF;
        if (byte4 == 0x80 || byte4 == 0x82)
            return false;
        return true;
    }

    bool IsGadgetDestroyed(uint64_t filterByte)
    {
        uint8_t byte4 = static_cast<uint8_t>(filterByte >> 32);
        return (byte4 == 0x80);
    }    inline bool IsValidPtr(uintptr_t ptr)
    {
        return ptr != 0 && ptr > 0x10000 && ptr < 0x7FFFFFFFFFFF;
    }
    static bool IsValidWorldPos(const Vector3& v)
    {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
        if (fabsf(v.x) >= 500000.f || fabsf(v.y) >= 500000.f || fabsf(v.z) >= 500000.f) return false;
        // Rotation matrix rows and unit vectors have at most one component > 2.0;
        // real world positions need at least two.
        int sig = (fabsf(v.x) > 2.f) + (fabsf(v.y) > 2.f) + (fabsf(v.z) > 2.f);
        return sig >= 2;
    }
    static bool IsValidQuat(const Vector4& q)
    {
        float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        return std::isfinite(len2) && len2 > 0.9f && len2 < 1.1f;
    }
    uint8_t GetPlayerByte(uint64_t filterByte)
    {
        return static_cast<uint8_t>(filterByte >> 56);
    }

    inline bool IsActorPlayer(uint64_t fb)
    {
        return ((fb >> 52) & 0xFFF) == 0x2C8;
    }

    // ─── Component Array / Position ───────────────────────────────────────────
    // Cached component list offset — same for every entity, found once.
    inline uint64_t s_comp_array_offset = 0;

    // Cached slot index inside the components list where the position component
    // (ID 0x0c63) lives. Same for every player entity because they share a
    // component layout, so once we find it for one we can read every other
    // player's position without re-walking 400 entries. UINT32_MAX = unknown.
    inline uint32_t s_comp_pos_slot = UINT32_MAX;

    // Dynamic position offset — calibrated once against the stable 0xb00 anchor.
    // Default 0x1f0 (matches R6SNexus observation); overridden on first valid
    // entity. The seller's update: 0xb00 is the interpolated/lerped position
    // (~300-500 ms behind), while the calibrated offset (~0x1f0) is the live
    // position. Rappel detection falls back to 0xb00 because the live offset
    // doesn't track entities on ropes correctly.
    inline uint32_t s_pos_offset       = 0x1f0;
    inline bool     s_pos_offset_found = false;

    // Scans posComp offsets 0x80–0x2F0 (step 4) and picks the Vector3 closest
    // to the known-stable anchor at posComp+0xb00.
    static uint32_t FindPosOffset(uint64_t posComp)
    {
        Vector3 ref = g_backend->read<Vector3>(posComp + 0xb00);
        if (!IsValidWorldPos(ref)) return 0;

        constexpr uint32_t kLo = 0x80;
        constexpr uint32_t kHi = 0x2F0;

        uint32_t bestOff  = 0;
        float    bestDist = FLT_MAX;
        for (uint32_t off = kLo; off + 12 <= kHi; off += 4)
        {
            Vector3 v = g_backend->read<Vector3>(posComp + off);
            if (!IsValidWorldPos(v)) continue;
            float dx = v.x - ref.x, dy = v.y - ref.y, dz = v.z - ref.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < bestDist) { bestDist = d2; bestOff = off; }
        }
        return bestOff;
    }

    // Scans 0xA0–0x1FF (step 8) on the entity, walks up to 400 entries of each
    // candidate list looking for the position component (ID 0x0c63).
    static uint64_t FindComponentsArray(uint64_t ent)
    {
        if (!IsValidPtr2(ent)) return 0;

        if (s_comp_array_offset)
        {
            uint64_t list_ptr = g_backend->read<uint64_t>(ent + s_comp_array_offset);
            if (IsValidPtr2(list_ptr)) return list_ptr;
            s_comp_array_offset = 0;
        }

        for (uint64_t offset = 0xA0; offset <= 0x1FF; offset += 8)
        {
            uint64_t list_ptr = g_backend->read<uint64_t>(ent + offset);
            if (!IsValidPtr2(list_ptr)) continue;

            for (uint64_t i = 0; i < 400; i++)
            {
                uint64_t comp = g_backend->read<uint64_t>(list_ptr + i * 8);
                if (!IsValidPtr2(comp)) continue;

                uint16_t id = g_backend->read<uint16_t>(comp - 0x08);
                if (id == 0x0c63)
                {
                    s_comp_array_offset = offset;
                    s_comp_pos_slot     = static_cast<uint32_t>(i);
                    printf("[FindComponentsArray] offset=0x%llX slot=%u\n", offset, s_comp_pos_slot);
                    return list_ptr;
                }
            }
        }

        return 0;
    }

    // Returns the world position for a player entity via component ID 0x0c63.
    // Uses a dynamically-calibrated offset (live pos at ~0x1f0). Rappel detection
    // reads the rotation quaternion at posComp+0x660; if pitch/roll > 0.05 the
    // entity is tilted (on a rope), so fall back to the stable anchor at 0xb00.
    // The comp slot index is cached globally to avoid re-walking 400 entries
    // on every call.
    static Vector3 GetPositionFromComponents(uint64_t ent)
    {
        if (!IsValidPtr2(ent)) return {};

        uint64_t list_ptr = FindComponentsArray(ent);
        if (!list_ptr) return {};

        // Fast path: comp slot is known. Verify ID still matches in case the
        // game shuffled components — drop through to full scan if it doesn't.
        if (s_comp_pos_slot != UINT32_MAX)
        {
            uint64_t comp = g_backend->read<uint64_t>(list_ptr + s_comp_pos_slot * 8);
            if (IsValidPtr2(comp))
            {
                uint16_t id = g_backend->read<uint16_t>(comp - 0x08);
                if (id == 0x0c63)
                {
                    if (!s_pos_offset_found)
                    {
                        uint32_t found = FindPosOffset(comp);
                        if (found) {
                            s_pos_offset       = found;
                            s_pos_offset_found = true;
                            printf("[PosOffset] Dynamic offset found: 0x%X\n", found);
                        }
                    }
                    Vector4 rot = g_backend->read<Vector4>(comp + 0x660);
                    bool rappelling = (fabsf(rot.x) > 0.05f || fabsf(rot.y) > 0.05f);
                    Vector3 v = g_backend->read<Vector3>(comp + (rappelling ? 0xb00u : s_pos_offset));
                    if (!IsValidWorldPos(v)) return {};
                    return v;
                }
            }
            s_comp_pos_slot = UINT32_MAX; // stale
        }

        // Slow path: walk the components list to find the position component.
        for (uint64_t i = 0; i < 400; i++)
        {
            uint64_t comp = g_backend->read<uint64_t>(list_ptr + i * 8);
            if (!IsValidPtr2(comp)) continue;

            uint16_t id = g_backend->read<uint16_t>(comp - 0x08);
            if (id != 0x0c63) continue;

            s_comp_pos_slot = static_cast<uint32_t>(i);

            if (!s_pos_offset_found)
            {
                uint32_t found = FindPosOffset(comp);
                if (found) {
                    s_pos_offset       = found;
                    s_pos_offset_found = true;
                    printf("[PosOffset] Dynamic offset found: 0x%X\n", found);
                }
            }

            Vector4 rot = g_backend->read<Vector4>(comp + 0x660);
            bool rappelling = (fabsf(rot.x) > 0.05f || fabsf(rot.y) > 0.05f);
            Vector3 v = g_backend->read<Vector3>(comp + (rappelling ? 0xb00u : s_pos_offset));
            if (!IsValidWorldPos(v)) return {};
            return v;
        }

        return {};
    }

    // Players  → component array (ID 0x0c63, pos at +0xb00)
    // Gadgets / weapons → actor+0x50 or actor+0x60, whichever is a valid world pos
    Vector3 GetActorPosition(uint64_t ActorPointer = 0)
    {
        uint64_t ptr = ActorPointer ? ActorPointer : Cached.ActorPointer;
        if (!IsValidPtr2(ptr)) return {};

        uint64_t fb = ReadFilterByteDirect(ptr);

        if (IsActorPlayer(fb))
            return GetPositionFromComponents(ptr);

        // Non-player: try +0x50 and +0x60, pick the valid one
        Vector3 pos50 = g_backend->read<Vector3>(ptr + 0x50);
        Vector3 pos60 = g_backend->read<Vector3>(ptr + 0x60);
        bool v50 = IsValidWorldPos(pos50);
        bool v60 = IsValidWorldPos(pos60);

        if (v50 && v60)
        {
            // Both valid — prefer whichever has greater magnitude (further from origin = world pos)
            float mag50 = pos50.x * pos50.x + pos50.y * pos50.y + pos50.z * pos50.z;
            float mag60 = pos60.x * pos60.x + pos60.y * pos60.y + pos60.z * pos60.z;
            return mag50 >= mag60 ? pos50 : pos60;
        }
        if (v50) return pos50;
        if (v60) return pos60;
        return {};
    }

    float GetActorDirection()
    {
        return g_backend->read<float>(Cached.ActorPointer + 0x30);
    }

    std::pair<Vector3, Vector3> GetActorBounds()
    {
        Vector3 min = { -0.55f, -0.55f, 0.0f };
        Vector3 max = { 0.55f, 0.55f, 1.8f };
        return { min, max };
    }

    std::pair<Vector3, Vector3> GetGadgetBounds()
    {
        Vector3 min = { -0.1f, -0.1f, -0.1f };
        Vector3 max = {  0.1f,  0.1f,  0.1f };
        return { min, max };
    }




    bool IsPlayerDead(uint64_t filterByte)
    {
        uint8_t byte4 = static_cast<uint8_t>(filterByte >> 32) & 0xFF;
        if (byte4 == 0x00) return false;
        return (byte4 == 0x84 || byte4 == 0x82);
    }

    bool IsGadget(uint64_t filterByte)
    {
        if (IsActorPlayer(filterByte))
            return false;
        uint8_t byte2 = static_cast<uint8_t>(filterByte >> 16);
        return byte2 == 0x19 || byte2 == 0x11 || byte2 == 0x18;
    }




    static uintptr_t g_round_ptr = 0;
    static bool      g_round_ptr_searched = false;

    uintptr_t FindRoundPointer()
    {
        if (g_round_ptr_searched) return g_round_ptr;
        g_round_ptr_searched = true;

        printf("[FindRoundPointer] Starting pattern scan...\n");

        auto text_section = pe_parser::get_text_section(g_backend->m_base_address);
        if (text_section.size == 0) {
            printf("[FindRoundPointer] Failed: Could not find .text section\n");
            return 0;
        }

        static const struct { const char* name; const char* pattern; int disp; int rip; } sigs[] = {
            { "primary", "E8 ? ? ? ? ? ? ? ? ? ? ? 4? ? ?", 8, 12 },
        };

        auto TryReadRoundState = [&](uintptr_t candidate) -> int
            {
                if (!candidate) return -1;

                uint64_t base = g_backend->read<uint64_t>(candidate);
                if (!base) return -1;

                // CE chain: +0x40->+0x48->+0x78->+0x18->+0x90->+0x38->+0x348
                uint64_t p1 = g_backend->read<uint64_t>(base + 0x40);
                if (p1) {
                    uint64_t p2 = g_backend->read<uint64_t>(p1 + 0x48);
                    if (p2) {
                        uint64_t p3 = g_backend->read<uint64_t>(p2 + 0x78);
                        if (p3) {
                            uint64_t p4 = g_backend->read<uint64_t>(p3 + 0x18);
                            if (p4) {
                                uint64_t p5 = g_backend->read<uint64_t>(p4 + 0x90);
                                if (p5) {
                                    uint64_t p6 = g_backend->read<uint64_t>(p5 + 0x38);
                                    if (p6) {
                                        int state = g_backend->read<int>(p6 + 0x348);
                                        if (state >= 0 && state <= 5) return state;
                                    }
                                }
                            }
                        }
                    }
                }

                return -1;
            };

        for (const auto& sig : sigs)
        {
            auto results = signature_scanner::scan_pattern(
                signature_scanner::parse_pattern(sig.pattern),
                text_section.start, text_section.size);

            if (results.empty()) {
                printf("[FindRoundPointer] '%s' — no hits\n", sig.name);
                continue;
            }

            printf("[FindRoundPointer] '%s' — %zu hit(s), validating...\n", sig.name, results.size());

            for (size_t i = 0; i < results.size(); ++i)
            {
                uintptr_t match = results[i];

                int32_t rel = 0;
                g_backend->read_memory(match + sig.disp, &rel, sizeof(int32_t));

                uintptr_t candidate = (match + sig.rip) + rel;

                int state = TryReadRoundState(candidate);

                if (state == 5)
                {
                    printf("[FindRoundPointer] [%zu] VALIDATED — round state 5, ptr=0x%llX\n", i, candidate);
                    g_round_ptr = candidate;
                    return g_round_ptr;
                }
            }

            printf("[FindRoundPointer] '%s' — no hit passed validation, make sure you're in main menu\n", sig.name);
        }

        printf("[FindRoundPointer] All signatures failed\n");
        return 0;
    }

    int GetRoundState()
    {
        if (!g_round_ptr && !g_round_ptr_searched)
            FindRoundPointer();

        if (!g_round_ptr) return -1;

        uint64_t base = g_backend->read<uint64_t>(g_round_ptr);
        if (!base) return -1;

        // CE chain: +0x40->+0x48->+0x78->+0x18->+0x90->+0x38->+0x348
        uint64_t p1 = g_backend->read<uint64_t>(base + 0x40);
        if (p1) {
            uint64_t p2 = g_backend->read<uint64_t>(p1 + 0x48);
            if (p2) {
                uint64_t p3 = g_backend->read<uint64_t>(p2 + 0x78);
                if (p3) {
                    uint64_t p4 = g_backend->read<uint64_t>(p3 + 0x18);
                    if (p4) {
                        uint64_t p5 = g_backend->read<uint64_t>(p4 + 0x90);
                        if (p5) {
                            uint64_t p6 = g_backend->read<uint64_t>(p5 + 0x38);
                            if (p6) {
                                int state = g_backend->read<int>(p6 + 0x348);
                                if (state >= 0 && state <= 5) return state;
                            }
                        }
                    }
                }
            }
        }

        return -1;
    }
    // Debug Menu

    void DrawDebugMenu()
    {
        Matrix4 matrix = GetMatrix();
        Vector3 cam_pos = GetPosition();

        ImGui::SetNextWindowSize(ImVec2(370, 260), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_Always);
        ImGui::Begin("##debug", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar);

        ImGui::TextColored(ImVec4(0.0f, 0.67f, 0.78f, 1.0f), "[ Camera Debug ]");
        ImGui::Separator();

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Position");
        ImGui::Text("X: %-12.4f Y: %-12.4f Z: %-12.4f", cam_pos.x, cam_pos.y, cam_pos.z);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "View Matrix");
        ImGui::Text("         [0]         [1]         [2]         [3]");
        for (int row = 0; row < 4; row++) {
            ImGui::Text("[%d]  %9.4f  %9.4f  %9.4f  %9.4f",
                row,
                matrix.m[row + 0],
                matrix.m[row + 4],
                matrix.m[row + 8],
                matrix.m[row + 12]
            );
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "CameraPointer: 0x%llX", Cached.CameraPointer);
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Actors Cached: %zu", Cached.Actors.size());

        ImGui::End();
    }
   
}
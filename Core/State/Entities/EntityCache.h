#pragma once
#include "../../../Source/Bootstrap.h"
#include <algorithm>
#include <deque>
#include <mutex>
namespace EntityState
{
    // Protects Cached.ActorEntries against vector reallocation while the render
    // thread is iterating it. Held briefly: during the refresh's move-assign in
    // the cache thread, and during the render thread's snapshot copy at the top
    // of PlayersRender. Fast-loop per-entry field writes (e.fb / e.pos / etc.)
    // are not locked — they can produce torn reads on the render side but not
    // crashes; only the vector reallocation can fault the iterator.
    inline std::mutex g_cache_mtx;

    void CachePlayers()
    {
        printf("[DEBUG] CachePlayers() started\n");
        if (!signature_scanner::auto_find_offsets(
            &offsets::ActorPatch,
            &offsets::CameraPatch,
            &offsets::CodeCaveOne,
            &offsets::CodeCaveTwo,
            offsets::ActorMov))
        {
            printf("[ERROR] Failed to find offsets automatically!\n");
            return;
        }

        unsigned char cam_orig[8]    = { 0 };
        unsigned char actor_orig[16] = { 0 };
        g_backend->read_memory(offsets::CameraPatch, cam_orig,   sizeof(cam_orig));
        g_backend->read_memory(offsets::ActorPatch,  actor_orig, sizeof(actor_orig));

        int actor_ps = PatchEngine::CalcPatchSize(actor_orig, 7);
        PatchEngine::InitBackups(cam_orig, actor_orig, actor_ps);

        //PatchEngine::SetupCamera();
        printf("[DEBUG] SetupCamera() completed\n");
        //PatchEngine::SetupActor();
        printf("[DEBUG] SetupActor() completed\n");

        // BoneEmu init — read PE for module size, find the bone-transform
        // function via Zydis. Both are one-shots; if either fails the
        // emulator silently no-ops on later GetBonePosition calls so the
        // rest of the cheat keeps working.
        if (!g_BoneEmulatorV3.InitFromBase(g_backend->m_base_address))
            printf("[BoneEmu] InitFromBase FAILED\n");
        else if (!g_BoneEmulatorV3.FindTargetFunction())
            printf("[BoneEmu] FindTargetFunction FAILED\n");
        else
            printf("[BoneEmu] ready\n");

        DWORD last_bone_probe = GetTickCount();

        DWORD last_print     = GetTickCount();
        DWORD last_refresh   = GetTickCount();
        DWORD last_fast      = GetTickCount();
        bool  patches_restored = false;

        for (;;)
        {
            uint64_t CameraAddress = g_backend->read<uint64_t>(offsets::CodeCaveOne);
            if (CameraAddress && CameraAddress != Cached.CameraPointer)
            {
                Cached.CameraPointer = CameraAddress;
                printf("[Camera] CameraPointer: 0x%llX\n", Cached.CameraPointer);
            }

            uint64_t EntityListPtr = g_backend->read<uint64_t>(offsets::CodeCaveTwo);

            if (GetTickCount() - last_print >= 1000)
            {
                printf("[EntityList] CodeCaveTwo: 0x%llX\n", EntityListPtr);
                last_print = GetTickCount();
            }

            if (EntityListPtr > 0x1000 && EntityListPtr < 0x7FFFFFFFFFFF)
            {
                if (!patches_restored)
                {
                    PatchEngine::RestoreActor();
                    PatchEngine::RestoreCamera();
                    patches_restored = true;
                    printf("[EntityList] Pointer valid — patches restored\n");
                }

                if (GetTickCount() - last_refresh >= 2000)
                {
                    last_refresh = GetTickCount();

                    std::vector<uint64_t>   new_actors;
                    std::vector<ActorEntry> new_entries;
                    new_actors.reserve(256);
                    new_entries.reserve(256);

                    int null_streak = 0;
                    for (uint64_t i = 0; i < 15000; i++)
                    {
                        uint64_t entholderzoidisthegoat = g_backend->read<uint64_t>(EntityListPtr + i * 8);
                        if (!SDK::IsValidPtr2(entholderzoidisthegoat)) {
                            if (++null_streak > 64) break;
                            continue;
                        }
                        null_streak = 0;

                        uint64_t ent = g_backend->read<uint64_t>(entholderzoidisthegoat);
                        if (!SDK::IsValidPtr2(ent)) continue;

                        ActorEntry e;
                        e.ptr  = ent;
                        e.fb   = g_backend->read<uint64_t>(ent + 0xB8);
                        e.pos50 = g_backend->read<Vector3>(ent + 0x50);
                        e.pos60 = g_backend->read<Vector3>(ent + 0x60);
                        e.direction = g_backend->read<float>(ent + 0x30);

                        if (SDK::IsActorPlayer(e.fb)) {
                            Cached.ActorPointer = ent;
                            e.pos = SDK::GetActorPosition(ent);
                            printf("[Player] [%llu] 0x%llX  fb=0x%llX\n", i, ent, e.fb);
                        } else {
                            bool v50 = SDK::IsValidWorldPos(e.pos50);
                            bool v60 = SDK::IsValidWorldPos(e.pos60);
                            if (v60 && !v50)       e.pos = e.pos60;
                            else if (v50 && !v60)  e.pos = e.pos50;
                            else if (v50 && v60)   e.pos = (fabsf(e.pos60.z) > fabsf(e.pos50.z)) ? e.pos60 : e.pos50;
                        }

                        new_actors.push_back(ent);
                        new_entries.push_back(e);
                    }

                    {
                        std::lock_guard<std::mutex> lk(EntityState::g_cache_mtx);
                        Cached.Actors      = std::move(new_actors);
                        Cached.ActorEntries = std::move(new_entries);
                    }
                    printf("[EntityList] Refreshed — %zu actors\n", Cached.Actors.size());
                }

                // Fast per-frame update.
                //
                // Players: re-read fb (death check) + position every tick. Direction
                // only if the arrow visual is enabled. ~4-5 reads/player/tick.
                //
                // Non-players (gadgets, lights, props): positions don't change after
                // they spawn, so we skip pos reads entirely on the fast path —
                // pos50/pos60 are stamped once by the slow refresh and stay valid.
                // fb still needs occasional refresh to detect destroyed-gadget state,
                // but every-5-ticks (~80 ms) is plenty for that. This is the change
                // that lets the loop actually hit ~60 Hz for player tracking instead
                // of stalling at 3-5 Hz under the per-tick non-player read load.
                if (GetTickCount() - last_fast >= 16)
                {
                    last_fast = GetTickCount();
                    static int fast_tick = 0;
                    fast_tick++;
                    bool slow_tick = (fast_tick % 5 == 0);

                    for (auto& e : Cached.ActorEntries)
                    {
                        bool was_player = SDK::IsActorPlayer(e.fb);

                        if (was_player)
                        {
                            e.fb = g_backend->read<uint64_t>(e.ptr + 0xB8);
                            if (Visuals.Arrow)
                                e.direction = g_backend->read<float>(e.ptr + 0x30);
                            e.pos = SDK::GetPositionFromComponents(e.ptr);
                        }
                        else if (slow_tick)
                        {
                            e.fb = g_backend->read<uint64_t>(e.ptr + 0xB8);
                        }
                    }
                }
            }
            else
            {
                patches_restored = false;
            }

            // BoneEmu smoke test: every 5 seconds, if we have a player
            // pointer cached, solve one bone and dump the full diagnostic
            // bundle so we can see exactly where emulation exits.
            if (GetTickCount() - last_bone_probe >= 5000 && Cached.ActorPointer)
            {
                last_bone_probe = GetTickCount();
                DWORD t0 = GetTickCount();
                Vector3 chest = g_BoneEmulatorV3.GetBonePosition(Cached.ActorPointer, 4);
                DWORD t1 = GetTickCount();

                uint64_t base    = g_BoneEmulatorV3.GetModuleBase();
                uint64_t target  = g_BoneEmulatorV3.GetTargetFunc();
                uint64_t exitRip = g_BoneEmulatorV3.GetLastExitRip();
                printf("[BoneEmu] pawn=0x%llX chest=(%.2f,%.2f,%.2f) ms=%lu\n",
                    (unsigned long long)Cached.ActorPointer,
                    chest.x, chest.y, chest.z, (unsigned long)(t1 - t0));
                printf("[BoneEmu]   reason=%s instr=%d exitRVA=0x%llX foundValid=%d\n",
                    g_BoneEmulatorV3.GetLastExitReason(),
                    g_BoneEmulatorV3.GetLastInstrCount(),
                    (unsigned long long)(exitRip >= base ? exitRip - base : exitRip),
                    g_BoneEmulatorV3.GetFoundValidPosition() ? 1 : 0);
                printf("[BoneEmu]   unhandled=%d firstMnemonic=%s firstRVA=0x%llX externCalls=%d boneMulHits=%d\n",
                    g_BoneEmulatorV3.GetUnhandledCount(),
                    ZydisMnemonicGetString(g_BoneEmulatorV3.GetFirstUnhandled()),
                    (unsigned long long)(g_BoneEmulatorV3.GetFirstUnhandledRip() >= base
                                         ? g_BoneEmulatorV3.GetFirstUnhandledRip() - base
                                         : g_BoneEmulatorV3.GetFirstUnhandledRip()),
                    g_BoneEmulatorV3.GetCallExternalCount(),
                    g_BoneEmulatorV3.GetIsBoneIndexMulHits());
                printf("[BoneEmu]   targetRVA=0x%llX\n",
                    (unsigned long long)(target >= base ? target - base : target));
            }

            Sleep(1);
        }
    }

    void GetGameState()
    {
        int last_round_state = -1;

        for (;;)
        {
            int round_state = SDK::GetRoundState();

            if (round_state != last_round_state)
            {
                // Clear all actor/camera state on every state transition
                Cached.Actors.clear();
                Cached.CameraPointer = 0;
                SDK::s_comp_array_offset = 0;
                SDK::s_comp_pos_slot     = UINT32_MAX;
                SDK::s_pos_offset_found  = false;
                SDK::s_pos_offset        = 0x1f0;
                printf("[GameState] State %d -> %d — cleared actors, camera\n",
                       last_round_state, round_state);

                switch (round_state)
                {
                case 0:
                    printf("[GameState] RoundSwap\n");
                    Sleep(1000);
                    break;

                case 1:
                    printf("[GameState] OperatorSelection — applying patches\n");
                    break;

                case 2:
                    // PreparationPhase. Auto-patch DISABLED — ShieldGuard
                    // scans .text on round entry and the trampolines are
                    // the textbook "Corrupted memory" signature. Press F5
                    // manually to pulse-patch (install for 500 ms, then
                    // restore; cave pointers persist).
                    printf("[GameState] PreparationPhase — press F5 to capture pointers\n");
                    break;

                case 3:
                    // ActionPhase. Same as case 2 — F5 only.
                    printf("[GameState] ActionPhase — press F5 to capture pointers\n");
                    break;

                case 4:
                    // RoundEnd. Don't touch the caves automatically either —
                    // they're harmless data and zeroing them here would just
                    // force another F5 pulse next round. Leave them alone.
                    printf("[GameState] RoundEnd\n");
                    break;

                case 5:
                    printf("[GameState] MainMenu\n");
                    break;

                default:
                    printf("[GameState] Unknown state: %d\n", round_state);
                    Sleep(1000);
                    break;
                }

                last_round_state = round_state;
            }

            Cached.GameState = round_state;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

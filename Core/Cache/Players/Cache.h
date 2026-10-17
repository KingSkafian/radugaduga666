#pragma once
#include "../../../Source/Bootstrap.h"
namespace Cache
{
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
            printf("[ERROR] The cheat cannot continue without valid offsets.\n");
            return;
        }

        // Read original bytes BEFORE patching so CalcPatchSize gets real instructions
        unsigned char cam_orig[8] = { 0 };
        unsigned char actor_orig[16] = { 0 };
        g_backend->read_memory(offsets::CameraPatch, cam_orig, sizeof(cam_orig));
        g_backend->read_memory(offsets::ActorPatch, actor_orig, sizeof(actor_orig));

        int actor_ps = Decryptions::CalcPatchSize(actor_orig, 7);
        printf("[DEBUG] actor_original bytes: ");
        for (int i = 0; i < 16; i++) printf("%02X ", actor_orig[i]);
        printf("\n[DEBUG] Calculated actor patch_size = %d\n", actor_ps);

        Decryptions::InitBackups(cam_orig, actor_orig, actor_ps);

        Decryptions::SetupCamera();
        printf("[DEBUG] SetupCamera() completed\n");

        Decryptions::SetupActor();
        printf("[DEBUG] SetupActor() completed\n");

        std::vector<uint64_t> temp_list;
        int loop_count = 0;

        for (;;)
        {
            uint64_t CameraAddress = g_backend->read<uint64_t>(offsets::CodeCaveOne);

            Cached.CameraPointer = CameraAddress;
            if (CameraAddress)
            {
                Cached.CameraPointer = CameraAddress;
            }

            uint64_t EntityAddress = g_backend->read<uint64_t>(offsets::CodeCaveTwo);

            if (!EntityAddress)
            {
                loop_count++;
                continue;
            }

            if (std::find(temp_list.begin(), temp_list.end(), EntityAddress) != temp_list.end())
            {
                loop_count++;
                continue;
            }

            temp_list.push_back(EntityAddress);
            Cached.Actors = temp_list;

            loop_count++;
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
                switch (round_state)
                {
                case 0:
                    printf("[GameState] RoundSwap\n");
                    Sleep(1000);
                    break;

                case 1:
                    printf("[GameState] OperatorSelection\n");
                    break;

                case 2:
                    printf("[GameState] PreparationPhase — applying patches\n");

                    Decryptions::SetupCamera();
                    Decryptions::SetupActor();
                    Sleep(5000);

                    Decryptions::RestoreCamera();
                    Decryptions::RestoreActor();
                    break;

                case 3:
                    printf("[GameState] ActionPhase\n");
                    Decryptions::SetupCamera();
                    Decryptions::SetupActor();
                    Sleep(50);

                    Decryptions::RestoreCamera();
                    Decryptions::RestoreActor();

                    if (last_round_state == 1)
                    {
                        printf("[GameState] Skipped PreparationPhase — applying patches now\n");
                        Decryptions::SetupCamera();
                        Decryptions::SetupActor();
                    }
                    break;

                case 4:
                    printf("[GameState] RoundEnd — clearing code caves\n");

                    Decryptions::ClearCodeCaves();
                    //Patches::ClearCodeCaves();

                    break;

                case 5:
                    printf("[GameState] MainMenu\n");

                    break;

                default:
                    printf("[GameState] Unknown state: %d\n", round_state);
                    break;
                }

                last_round_state = round_state;
            }


            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
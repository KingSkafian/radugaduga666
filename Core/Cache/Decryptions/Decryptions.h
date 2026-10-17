#pragma once
namespace Decryptions {
    struct PatchBackup {
        unsigned char camera_original[8];
        unsigned char actor_original[16];
        int actor_patch_size = 0;
        bool camera_backed_up = false;
        bool actor_backed_up = false;
    };
    static PatchBackup backup;

    void InitBackups(
        const unsigned char* cam_orig,
        const unsigned char* actor_orig,
        int actor_patch_sz)
    {
        memcpy(backup.camera_original, cam_orig, 8);
        memcpy(backup.actor_original, actor_orig, 16);
        backup.actor_patch_size = actor_patch_sz;
        backup.camera_backed_up = true;
        backup.actor_backed_up = true;
    }

    void SetupCamera()
    {
        const auto patch_address = offsets::CameraPatch;
        const auto codecave_address = offsets::CodeCaveOne;

        if (patch_address == 0 || codecave_address == 0)
        {
            printf("[-] Invalid camera patch or codecave address\n");
            return;
        }

        int64_t rip_next = patch_address + 7;
        int64_t rel = codecave_address - rip_next;

        if (rel > INT32_MAX || rel < INT32_MIN)
        {
            printf("[-] Camera relative offset too large: 0x%llX\n", (long long)rel);
            return;
        }

        int32_t rel_offset = static_cast<int32_t>(rel);

        unsigned char patch[8] = {
            0x48, 0x89, 0x15,
            static_cast<unsigned char>(rel_offset & 0xFF),
            static_cast<unsigned char>((rel_offset >> 8) & 0xFF),
            static_cast<unsigned char>((rel_offset >> 16) & 0xFF),
            static_cast<unsigned char>((rel_offset >> 24) & 0xFF),
            0x90
        };

        if (!write_buffer(patch_address, patch, sizeof(patch)))
        {
            printf("[-] Failed to write camera patch\n");
            return;
        }

        printf("[+] Wrote camera patch!\n");
    }

    void RestoreCamera()
    {
        if (!backup.camera_backed_up) return;
        const auto patch_address = offsets::CameraPatch;
        write_buffer((uint64_t)patch_address, backup.camera_original, sizeof(backup.camera_original));
    }

    int CalcPatchSize(const unsigned char* original_bytes, int hook_size) {
        int total = 0;
        while (total < hook_size) {
            hde64s hs;
            int len = hde64_disasm(original_bytes + total, &hs);
            if (hs.flags & F_ERROR || len <= 0) break;
            total += len;
        }
        return total;
    }

    void SetupActor() {
        const auto patch_address = offsets::ActorPatch;
        const auto codecave_address = offsets::CodeCaveTwo;

        int patch_size = CalcPatchSize(backup.actor_original, 7);
        backup.actor_patch_size = patch_size;
        printf("patch_size = %d\n", patch_size);

        int rel_offset = static_cast<int>(codecave_address - (patch_address + 7));
        unsigned char patch[16] = { 0 };
        patch[0] = offsets::ActorMov[0];
        patch[1] = offsets::ActorMov[1];
        patch[2] = offsets::ActorMov[2];
        patch[3] = static_cast<unsigned char>(rel_offset & 0xFF);
        patch[4] = static_cast<unsigned char>((rel_offset >> 8) & 0xFF);
        patch[5] = static_cast<unsigned char>((rel_offset >> 16) & 0xFF);
        patch[6] = static_cast<unsigned char>((rel_offset >> 24) & 0xFF);
        for (int i = 7; i < patch_size; i++) {
            patch[i] = 0x90;
        }

        if (!write_buffer(patch_address, patch, patch_size)) {
            std::cout << "Failed to write patch." << std::endl;
            //return codecave_address;
        }
    }

    void RestoreActor()
    {
        const auto patch_address = offsets::ActorPatch;
        int size = backup.actor_patch_size > 0 ? backup.actor_patch_size : 10;

        write_buffer((uint64_t)patch_address, backup.actor_original, size);
    }

    void ClearCodeCaves()
    {
        unsigned long long zero = 0;
        write_buffer((uint64_t)offsets::CodeCaveOne, &zero, sizeof(zero));
        write_buffer((uint64_t)offsets::CodeCaveTwo, &zero, sizeof(zero));
        Cached.Actors.clear();
    }
}
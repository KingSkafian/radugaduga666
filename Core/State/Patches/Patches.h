#pragma once
namespace PatchEngine {
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
        // Lazily find a .text CC cave for the trampoline
        if (offsets::ActorTrampolineEmpty == 0) {
            CcCave my_cave = find_largest_text_cave();
            if (my_cave.address == 0) {
                printf("[-] SetupActor: no .text CC cave found, cannot install trampoline\n");
                return;
            }
            offsets::ActorTrampolineEmpty = my_cave.address;
            offsets::ActorTrampolineSize  = (int)my_cave.size;
            printf("[+] Trampoline cave found: 0x%llx (%d bytes)\n",
                my_cave.address, (int)my_cave.size);
        }

        const auto patch_address      = offsets::ActorPatch;
        const auto codecave_address   = offsets::CodeCaveTwo;
        const auto trampoline_address = offsets::ActorTrampolineEmpty;

        if (patch_address == 0 || codecave_address == 0 || trampoline_address == 0) {
            printf("[-] SetupActor: invalid actor patch/codecave/trampoline address\n");
            return;
        }

        if (offsets::ActorMov[0] == 0 && offsets::ActorMov[1] == 0 && offsets::ActorMov[2] == 0) {
            printf("[-] SetupActor: ActorMov bytes not set\n");
            return;
        }

        int patch_size = CalcPatchSize(backup.actor_original, 7);
        backup.actor_patch_size = patch_size;

        if (patch_size > 46) {
            printf("[-] SetupActor: patch_size %d exceeds trampoline buffer\n", patch_size);
            return;
        }

        // Derive MOV [rip+rel32], Rsrc from ActorMov bytes
        uint8_t orig_rex   = offsets::ActorMov[0];
        uint8_t orig_modrm = offsets::ActorMov[2];

        uint8_t rex_W    = (orig_rex >> 3) & 1;
        uint8_t rex_R    = (orig_rex >> 2) & 1;
        uint8_t new_rex  = (uint8_t)(0x40 | (rex_W << 3) | (rex_R << 2));
        uint8_t reg_fld  = (orig_modrm >> 3) & 0x7;
        uint8_t new_modrm = (uint8_t)((reg_fld << 3) | 0x05);

        unsigned char trampoline[128] = { 0 };
        int t = 0;

        // MOV [rip+rel32], Rsrc  →  store actor pointer into codecave
        unsigned long long mov_rip_next = trampoline_address + 7;
        int64_t mov_rel = (int64_t)codecave_address - (int64_t)mov_rip_next;
        if (mov_rel > INT32_MAX || mov_rel < INT32_MIN) {
            printf("[-] SetupActor: trampoline->codecave offset too large\n");
            return;
        }
        int32_t mov_rel32 = (int32_t)mov_rel;

        trampoline[t++] = new_rex;
        trampoline[t++] = 0x89;
        trampoline[t++] = new_modrm;
        trampoline[t++] = (mov_rel32 >> 0) & 0xFF;
        trampoline[t++] = (mov_rel32 >> 8) & 0xFF;
        trampoline[t++] = (mov_rel32 >> 16) & 0xFF;
        trampoline[t++] = (mov_rel32 >> 24) & 0xFF;

        // Copy original bytes and fix up RIP-relative displacements / branches
        int copy_offset = t;
        memcpy(trampoline + t, backup.actor_original, patch_size);
        t += patch_size;

        {
            int i = 0, t_off = 0;
            while (i < patch_size) {
                hde64s hs;
                int scan_pos = copy_offset + i + t_off;
                int len = hde64_disasm(trampoline + scan_pos, &hs);
                if (hs.flags & F_ERROR || len <= 0) break;

                uint64_t old_rip = patch_address + i + len;
                uint64_t new_rip = trampoline_address + scan_pos + len;

                bool is_rel32_branch = (hs.opcode == 0xE9) || (hs.opcode == 0xE8) ||
                    ((hs.flags & F_IMM32) && (hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F));
                bool is_rel8_branch  = (hs.opcode == 0xEB) ||
                    (hs.opcode >= 0x70 && hs.opcode <= 0x7F);
                bool is_rip_relative = (hs.flags & F_MODRM) && ((hs.modrm & 0xC7) == 0x05);

                if (is_rel32_branch && (hs.flags & F_IMM32)) {
                    int imm_pos = scan_pos + len - 4;
                    int32_t old_rel32;
                    memcpy(&old_rel32, trampoline + imm_pos, 4);
                    uint64_t target = old_rip + old_rel32;
                    int64_t  new_rel64 = (int64_t)target - (int64_t)new_rip;
                    if (new_rel64 <= INT32_MAX && new_rel64 >= INT32_MIN) {
                        int32_t new_rel32 = (int32_t)new_rel64;
                        memcpy(trampoline + imm_pos, &new_rel32, 4);
                    }
                }
                else if (is_rel8_branch && (hs.flags & F_IMM8)) {
                    int8_t   old_rel8 = (int8_t)trampoline[scan_pos + len - 1];
                    uint64_t target   = old_rip + old_rel8;
                    int64_t  fit      = (int64_t)target - (int64_t)new_rip;
                    if (fit > 127 || fit < -128) {
                        // Expand rel8 → rel32 (6 bytes)
                        int tail_start = scan_pos + 2;
                        int tail_len   = t - tail_start;
                        if (t + 4 <= (int)sizeof(trampoline)) {
                            memmove(trampoline + tail_start + 4, trampoline + tail_start, tail_len);
                            t += 4; t_off += 4;
                            uint64_t exp_new_rip = trampoline_address + scan_pos + 6;
                            int64_t  exp_rel64   = (int64_t)target - (int64_t)exp_new_rip;
                            if (exp_rel64 <= INT32_MAX && exp_rel64 >= INT32_MIN) {
                                int32_t  nr32    = (int32_t)exp_rel64;
                                uint8_t  short_op = trampoline[scan_pos];
                                if (short_op == 0xEB) {
                                    trampoline[scan_pos+0] = 0xE9;
                                    memcpy(trampoline + scan_pos + 1, &nr32, 4);
                                    trampoline[scan_pos+5] = 0x90;
                                } else {
                                    trampoline[scan_pos+0] = 0x0F;
                                    trampoline[scan_pos+1] = (uint8_t)(0x80 + (short_op & 0x0F));
                                    memcpy(trampoline + scan_pos + 2, &nr32, 4);
                                }
                            }
                        }
                    } else {
                        trampoline[scan_pos + len - 1] = (uint8_t)((int8_t)fit);
                    }
                }
                else if (is_rip_relative && (hs.flags & F_DISP32)) {
                    int disp_pos = scan_pos + len - 4;
                    int32_t old_disp32;
                    memcpy(&old_disp32, trampoline + disp_pos, 4);
                    uint64_t target    = old_rip + old_disp32;
                    int64_t  new_disp64 = (int64_t)target - (int64_t)new_rip;
                    if (new_disp64 <= INT32_MAX && new_disp64 >= INT32_MIN) {
                        int32_t new_disp32 = (int32_t)new_disp64;
                        memcpy(trampoline + disp_pos, &new_disp32, 4);
                        printf("[DEBUG] SetupActor: fixed rip-rel disp at i=%d (target=0x%llx)\n",
                            i, (unsigned long long)target);
                    } else {
                        printf("[-] SetupActor: rip-rel fixup too far at i=%d (target=0x%llx)\n",
                            i, (unsigned long long)target);
                    }
                }

                i += len;
            }
        }

        // Absolute jmp back to patch_address + patch_size
        unsigned long long jmp_back = patch_address + patch_size;
        trampoline[t++] = 0x48; trampoline[t++] = 0xB8;
        memcpy(trampoline + t, &jmp_back, 8); t += 8;
        trampoline[t++] = 0xFF; trampoline[t++] = 0xE0;

        offsets::ActorTrampolineSize = t;

        if (!write_buffer(trampoline_address, trampoline, t)) {
            printf("[-] SetupActor: failed to write trampoline\n");
            return;
        }

        // Write jmp at patch site
        int64_t jmp_rel = (int64_t)trampoline_address - (int64_t)(patch_address + 5);
        unsigned char site_patch[16];
        memset(site_patch, 0x90, sizeof(site_patch));

        if (jmp_rel > INT32_MAX || jmp_rel < INT32_MIN) {
            if (patch_size < 12) {
                printf("[-] SetupActor: patch_size too small for absolute jmp\n");
                return;
            }
            site_patch[0] = 0x48; site_patch[1] = 0xB8;
            memcpy(site_patch + 2, &trampoline_address, 8);
            site_patch[10] = 0xFF; site_patch[11] = 0xE0;
        } else {
            int32_t jmp_rel32 = (int32_t)jmp_rel;
            site_patch[0] = 0xE9;
            site_patch[1] = (jmp_rel32 >> 0) & 0xFF;
            site_patch[2] = (jmp_rel32 >> 8) & 0xFF;
            site_patch[3] = (jmp_rel32 >> 16) & 0xFF;
            site_patch[4] = (jmp_rel32 >> 24) & 0xFF;
        }

        if (!write_buffer(patch_address, site_patch, patch_size)) {
            printf("[-] SetupActor: failed to write jmp at patch site\n");
            return;
        }

        printf("[+] Actor trampoline installed! REX=%02X ModRM=%02X trampoline=0x%llx\n",
            new_rex, new_modrm, trampoline_address);
    }

    void RestoreActor()
    {
        const auto patch_address = offsets::ActorPatch;
        int size = backup.actor_patch_size > 0 ? backup.actor_patch_size : 10;

        write_buffer((uint64_t)patch_address, backup.actor_original, size);

        // Clear the trampoline cave back to CC so it can be reused next round
        if (offsets::ActorTrampolineEmpty != 0 && offsets::ActorTrampolineSize > 0) {
            unsigned char cc_fill[64];
            memset(cc_fill, 0xCC, sizeof(cc_fill));
            write_buffer(offsets::ActorTrampolineEmpty, cc_fill,
                (offsets::ActorTrampolineSize < 64 ? offsets::ActorTrampolineSize : 64));
            offsets::ActorTrampolineEmpty = 0;
            offsets::ActorTrampolineSize  = 0;
        }
    }

    void ClearCodeCaves()
    {
        unsigned long long zero = 0;
        write_buffer((uint64_t)offsets::CodeCaveOne, &zero, sizeof(zero));
        write_buffer((uint64_t)offsets::CodeCaveTwo, &zero, sizeof(zero));
        Cached.Actors.clear();
    }
}

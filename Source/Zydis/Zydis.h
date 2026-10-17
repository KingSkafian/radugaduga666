#pragma once
#include "../../Dependencies/Zydis/Zydis.h"
#include "../Bootstrap.h"

namespace pe_parser
{
    struct SectionInfo {
        unsigned long long start;
        unsigned long long size;
        std::string name;
    };

    bool read_dos_header(unsigned long long base, IMAGE_DOS_HEADER* dos_header) {
        *dos_header = g_backend->read<IMAGE_DOS_HEADER>(base);
        return dos_header->e_magic == IMAGE_DOS_SIGNATURE;
    }

    bool read_nt_headers(unsigned long long base, IMAGE_NT_HEADERS64* nt_headers) {
        IMAGE_DOS_HEADER dos_header;
        if (!read_dos_header(base, &dos_header)) {
            return false;
        }

        *nt_headers = g_backend->read<IMAGE_NT_HEADERS64>(base + dos_header.e_lfanew);
        return nt_headers->Signature == IMAGE_NT_SIGNATURE;
    }

    std::vector<SectionInfo> get_sections(unsigned long long base) {
        std::vector<SectionInfo> sections;

        IMAGE_DOS_HEADER dos_header;
        IMAGE_NT_HEADERS64 nt_headers;

        if (!read_dos_header(base, &dos_header)) {
            return sections;
        }

        if (!read_nt_headers(base, &nt_headers)) {
            return sections;
        }

        if (nt_headers.Signature != IMAGE_NT_SIGNATURE) {
            return sections;
        }

        unsigned long long section_header_addr = base + dos_header.e_lfanew + sizeof(IMAGE_NT_HEADERS64);

        for (int i = 0; i < nt_headers.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER section_header = g_backend->read<IMAGE_SECTION_HEADER>(
                section_header_addr + i * sizeof(IMAGE_SECTION_HEADER));

            SectionInfo info;
            info.start = base + section_header.VirtualAddress;
            info.size = section_header.Misc.VirtualSize;
            info.name = std::string((char*)section_header.Name,
                strnlen((char*)section_header.Name, IMAGE_SIZEOF_SHORT_NAME));

            sections.push_back(info);
        }

        return sections;
    }

    SectionInfo find_section(unsigned long long base, const std::string& section_name) {
        auto sections = get_sections(base);

        for (const auto& section : sections) {
            if (section.name == section_name) {
                return section;
            }
        }

        SectionInfo empty = { 0, 0, "" };
        return empty;
    }

    SectionInfo get_text_section(unsigned long long base) {
        return find_section(base, ".text");
    }

    SectionInfo get_data_section(unsigned long long base) {
        return find_section(base, ".data");
    }

    SectionInfo get_rdata_section(unsigned long long base) {
        return find_section(base, ".rdata");
    }
}

namespace signature_scanner
{
    // Chunked read helper - kernel driver may not handle large single reads
    bool read_memory_chunked(uintptr_t address, void* buffer, size_t total_size) {
        const size_t page_size = 0x1000;
        uint8_t* dst = (uint8_t*)buffer;
        size_t remaining = total_size;
        size_t offset = 0;

        while (remaining > 0) {
            size_t chunk = (remaining < page_size) ? remaining : page_size;
            if (!g_backend->read_memory(address + offset, dst + offset, chunk)) {
                return false;
            }
            offset += chunk;
            remaining -= chunk;
        }
        return true;
    }

    struct Pattern {
        std::vector<int> bytes;
        std::string mask;
    };

    Pattern parse_pattern(const std::string& pattern_str) {
        Pattern pattern;
        std::istringstream iss(pattern_str);
        std::string byte_str;

        while (iss >> byte_str) {
            if (byte_str == "?") {
                pattern.bytes.push_back(-1);
                pattern.mask += "?";
            }
            else if (byte_str.size() == 2 && byte_str[1] == '?') {
                int high = std::stoi(byte_str.substr(0, 1), nullptr, 16);
                pattern.bytes.push_back(0x100 | high);
                pattern.mask += "n";
            }
            else if (byte_str.size() == 2 && byte_str[0] == '?') {
                int low = std::stoi(byte_str.substr(1, 1), nullptr, 16);
                pattern.bytes.push_back(0x200 | low);
                pattern.mask += "n";
            }
            else {
                pattern.bytes.push_back(std::stoi(byte_str, nullptr, 16));
                pattern.mask += "x";
            }
        }
        return pattern;
    }

    std::vector<unsigned long long> scan_pattern(const Pattern& pattern, unsigned long long start, unsigned long long size) {
        std::vector<unsigned long long> results;
        const unsigned long long chunk_size = 0x100000;

        for (unsigned long long offset = 0; offset < size; offset += chunk_size) {
            unsigned long long current_chunk_size = (chunk_size < (size - offset)) ? chunk_size : (size - offset);
            std::vector<unsigned char> buffer((size_t)current_chunk_size);

            read_memory_chunked((uintptr_t)(start + offset), buffer.data(), (size_t)current_chunk_size);

            for (size_t i = 0; i < current_chunk_size - pattern.bytes.size(); i++) {
                bool found = true;
                for (size_t j = 0; j < pattern.bytes.size(); j++) {
                    int p = pattern.bytes[j];
                    unsigned char b = buffer[i + j];

                    if (p == -1) {
                        continue;
                    }
                    else if ((p & 0x300) == 0x100) {
                        if ((b >> 4) != (p & 0xF)) { found = false; break; }
                    }
                    else if ((p & 0x300) == 0x200) {
                        if ((b & 0xF) != (p & 0xF)) { found = false; break; }
                    }
                    else {
                        if (b != (unsigned char)p) { found = false; break; }
                    }
                }
                if (found) {
                    results.push_back(start + offset + i);
                }
            }
        }

        return results;
    }

    bool decode_instruction(unsigned long long addr,
        ZydisDecodedInstruction* instr,
        ZydisDecodedOperand* operands)
    {
        uint8_t raw[ZYDIS_MAX_INSTRUCTION_LENGTH] = {};
        if (!g_backend->read_memory(addr, raw, sizeof(raw))) {
            printf("[ZYDIS] read_memory failed @ 0x%llX\n", addr);
            return false;
        }

        ZydisDecoder decoder;
        ZyanStatus init_status = ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        if (!ZYAN_SUCCESS(init_status)) {
            printf("[ZYDIS] ZydisDecoderInit FAILED status=0x%X\n", init_status);
            return false;
        }

        ZydisDecoderContext ctx;
        ZyanStatus decode_status = ZydisDecoderDecodeInstruction(&decoder, &ctx, raw, sizeof(raw), instr);
        if (!ZYAN_SUCCESS(decode_status)) {
            printf("[ZYDIS] DecodeInstruction FAILED @ 0x%llX status=0x%X bytes: %02X %02X %02X %02X\n",
                addr, decode_status, raw[0], raw[1], raw[2], raw[3]);
            return false;
        }

        ZyanStatus op_status = ZydisDecoderDecodeOperands(&decoder, &ctx, instr, operands, ZYDIS_MAX_OPERAND_COUNT);
        if (!ZYAN_SUCCESS(op_status)) {
            printf("[ZYDIS] DecodeOperands FAILED @ 0x%llX status=0x%X\n", addr, op_status);
            return false;
        }

        return true;
    }

    // Returns true if mnemonic is any kind of jump
    static bool is_jump(ZydisMnemonic m) {
        return (m == ZYDIS_MNEMONIC_JMP ||
            m == ZYDIS_MNEMONIC_JZ || m == ZYDIS_MNEMONIC_JNZ ||
            m == ZYDIS_MNEMONIC_JB || m == ZYDIS_MNEMONIC_JNB ||
            m == ZYDIS_MNEMONIC_JNBE || m == ZYDIS_MNEMONIC_JBE ||
            m == ZYDIS_MNEMONIC_JL || m == ZYDIS_MNEMONIC_JNL ||
            m == ZYDIS_MNEMONIC_JNLE || m == ZYDIS_MNEMONIC_JLE ||
            m == ZYDIS_MNEMONIC_JS || m == ZYDIS_MNEMONIC_JNS ||
            m == ZYDIS_MNEMONIC_JO || m == ZYDIS_MNEMONIC_JNO ||
            m == ZYDIS_MNEMONIC_JP || m == ZYDIS_MNEMONIC_JNP ||
            m == ZYDIS_MNEMONIC_JCXZ || m == ZYDIS_MNEMONIC_JECXZ ||
            m == ZYDIS_MNEMONIC_JRCXZ);
    }

    // Walk forward up to max_insns, return address of first matching mnemonic (or 0)
    static unsigned long long scan_forwards(unsigned long long start, ZydisMnemonic target, int max_insns = 20) {
        unsigned long long cur = start;
        for (int i = 0; i < max_insns; i++) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) break;
            if (instr.mnemonic == target) return cur;
            cur += instr.length;
        }
        return 0;
    }

    // Walk backwards up to max_bytes decoding instructions, return addresses in ascending order
    static std::vector<unsigned long long> collect_backwards(unsigned long long from, int max_bytes) {
        std::vector<unsigned long long> addrs;
        unsigned long long start = (from > (unsigned long long)max_bytes) ? from - max_bytes : 0;
        unsigned long long cur = start;
        while (cur < from) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (decode_instruction(cur, &instr, &ops[0])) {
                addrs.push_back(cur);
                cur += instr.length;
            }
            else {
                cur++;
            }
        }
        return addrs;
    }

    // Walk backwards, return address of last occurrence of target mnemonic within max_insns from end
    static unsigned long long scan_backwards(unsigned long long from, ZydisMnemonic target, int max_insns) {
        auto addrs = collect_backwards(from, max_insns * 15);
        for (int i = (int)addrs.size() - 1;
            i >= 0 && i >= (int)addrs.size() - max_insns;
            i--)
        {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (decode_instruction(addrs[i], &instr, &ops[0])) {
                if (instr.mnemonic == target) return addrs[i];
            }
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // FindFunctionStart
    // -------------------------------------------------------------------------
    static unsigned long long FindFunctionStart(unsigned long long sigAddr, unsigned long long moduleBase) {
        if (!sigAddr || !moduleBase) return 0;

        IMAGE_NT_HEADERS64 nt = {};
        if (!pe_parser::read_nt_headers(moduleBase, &nt)) goto fallback;

        {
            auto& excDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (excDir.VirtualAddress == 0 || excDir.Size == 0) goto fallback;

            uint64_t excTableAddr = moduleBase + excDir.VirtualAddress;
            uint32_t count = excDir.Size / sizeof(RUNTIME_FUNCTION);
            uint32_t targetRVA = static_cast<uint32_t>(sigAddr - moduleBase);

            uint32_t lo = 0, hi = count - 1;
            bool found = false;
            RUNTIME_FUNCTION best = {};
            uint32_t bestSize = UINT32_MAX;

            while (lo <= hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                RUNTIME_FUNCTION rf = {};
                if (!g_backend->read_memory(excTableAddr + mid * sizeof(RUNTIME_FUNCTION), &rf, sizeof(rf)))
                    break;

                if (targetRVA < rf.BeginAddress) {
                    if (mid == 0) break;
                    hi = mid - 1;
                }
                else if (targetRVA >= rf.EndAddress) {
                    lo = mid + 1;
                }
                else {
                    best = rf;
                    bestSize = rf.EndAddress - rf.BeginAddress;
                    found = true;

                    for (int k = (int)mid - 1; k >= 0 && k >= (int)mid - 32; k--) {
                        RUNTIME_FUNCTION r2 = {};
                        if (!g_backend->read_memory(excTableAddr + k * sizeof(RUNTIME_FUNCTION), &r2, sizeof(r2))) break;
                        if (r2.BeginAddress > targetRVA || targetRVA >= r2.EndAddress) continue;
                        uint32_t sz = r2.EndAddress - r2.BeginAddress;
                        if (sz < bestSize) { best = r2; bestSize = sz; }
                    }
                    for (uint32_t k = mid + 1; k < count && k <= mid + 32; k++) {
                        RUNTIME_FUNCTION r2 = {};
                        if (!g_backend->read_memory(excTableAddr + k * sizeof(RUNTIME_FUNCTION), &r2, sizeof(r2))) break;
                        if (r2.BeginAddress > targetRVA || targetRVA >= r2.EndAddress) continue;
                        uint32_t sz = r2.EndAddress - r2.BeginAddress;
                        if (sz < bestSize) { best = r2; bestSize = sz; }
                    }
                    break;
                }
            }

            if (!found) goto fallback;

            uint32_t resolvedBegin = best.BeginAddress;
            RUNTIME_FUNCTION current = best;

            for (int depth = 0; depth < 8; depth++) {
                uint64_t unwindAddr = moduleBase + current.UnwindData;
                uint8_t  unwindHdr[4] = {};
                if (!g_backend->read_memory(unwindAddr, unwindHdr, sizeof(unwindHdr))) break;

                uint8_t flags = (unwindHdr[0] >> 3) & 0x1F;
                uint8_t countOfCodes = unwindHdr[2];

                if (!(flags & 0x4)) break;

                uint32_t chainOffset = 4 + (((countOfCodes + 1) & ~1) * 2);
                RUNTIME_FUNCTION chained = {};
                if (!g_backend->read_memory(unwindAddr + chainOffset, &chained, sizeof(chained))) break;
                if (chained.BeginAddress == 0) break;

                resolvedBegin = chained.BeginAddress;
                current = chained;
            }

            return moduleBase + resolvedBegin;
        }

    fallback:
        {
            const int BACK = 0x500;
            std::vector<uint8_t> raw(BACK);
            unsigned long long scan_from = (sigAddr > (unsigned long long)BACK)
                ? sigAddr - BACK : 0;
            size_t actual = (size_t)(sigAddr - scan_from);
            read_memory_chunked((uintptr_t)scan_from, raw.data(), actual);
            for (int i = (int)actual - 1; i >= 0; i--) {
                if (raw[i] == 0xCC || raw[i] == 0xC3 || raw[i] == 0xC2) {
                    return scan_from + i + 1;
                }
            }
            return sigAddr - 0x100;
        }
    }

    // -------------------------------------------------------------------------
    // validate_actor_prologue_tail
    //   Helper: runs the post-JMP portion (MOV r,RDX / MOV r,RCX, then
    //   CMP/JMP/CALL chain) starting from `cur`. Returns true on success.
    // -------------------------------------------------------------------------
    static bool validate_actor_prologue_tail(unsigned long long cur)
    {
        // --- MOV r??, RDX  then  MOV r??, RCX  (in either order) ---
        bool saw_mov_rdx = false, saw_mov_rcx = false;
        for (int i = 0; i < 4 && !(saw_mov_rdx && saw_mov_rcx); i++) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) break;
            if (instr.mnemonic == ZYDIS_MNEMONIC_MOV
                && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
            {
                if (ops[1].reg.value == ZYDIS_REGISTER_RDX) saw_mov_rdx = true;
                if (ops[1].reg.value == ZYDIS_REGISTER_RCX) saw_mov_rcx = true;
            }
            cur += instr.length;
        }

        if (!saw_mov_rdx || !saw_mov_rcx) {
            return false;
        }

        bool saw_cmp = false, saw_jmp2 = false, saw_call = false;
        for (int i = 0; i < 30 && !saw_call; i++) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) break;

            if (instr.mnemonic == ZYDIS_MNEMONIC_CMP)  saw_cmp = true;
            if (saw_cmp && is_jump(instr.mnemonic))     saw_jmp2 = true;
            if (saw_jmp2 && (instr.mnemonic == ZYDIS_MNEMONIC_CALL ||
                instr.mnemonic == ZYDIS_MNEMONIC_MOV))  saw_call = true;

            cur += instr.length;
        }

        return saw_cmp && saw_jmp2 && saw_call;
    }

    // -------------------------------------------------------------------------
    // validate_actor_prologue
    //   Two-pass: first tries the original "fall-through after JMP" path
    //   (works for game version A). If that fails, retries with the JMP
    //   target followed (works for game version B).
    // -------------------------------------------------------------------------
    static bool validate_actor_prologue(unsigned long long fn_start) {
        if (!fn_start) return false;

        unsigned long long cur = fn_start;
        int push_count = 0;

        for (int i = 0; i < 12; i++) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            bool ok = decode_instruction(cur, &instr, &ops[0]);

            printf("[ACTOR]   prologue[%d] @ 0x%llX decode=%s mnemonic=%s len=%d\n",
                i, cur,
                ok ? "OK" : "FAIL",
                ok ? ZydisMnemonicGetString(instr.mnemonic) : "?",
                ok ? instr.length : 0);

            if (!ok) return false;

            if (instr.mnemonic == ZYDIS_MNEMONIC_PUSH) {
                push_count++;
                cur += instr.length;
            }
            else {
                break;
            }
        }

        if (push_count < 5) {
            printf("[ACTOR] prologue: only %d pushes (need 5+)\n", push_count);
            return false;
        }
        // --- SUB RSP, imm ---
        {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) return false;
            if (instr.mnemonic != ZYDIS_MNEMONIC_SUB) return false;
            if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
            if (ops[0].reg.value != ZYDIS_REGISTER_RSP)    return false;
            cur += instr.length;
        }

        // --- MOV reg, [RDX] ---
        ZydisRegister mov_dst = ZYDIS_REGISTER_NONE;
        {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) return false;
            if (instr.mnemonic != ZYDIS_MNEMONIC_MOV) return false;
            if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER)  return false;
            if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY)    return false;
            if (ops[1].mem.base != ZYDIS_REGISTER_RDX)       return false;
            mov_dst = ops[0].reg.value;
            cur += instr.length;
        }

        // --- TEST mov_dst, mov_dst ---
        {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) return false;
            if (instr.mnemonic != ZYDIS_MNEMONIC_TEST) return false;
            if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
            if (ZydisRegisterGetId(ops[0].reg.value) != ZydisRegisterGetId(mov_dst)) return false;
            cur += instr.length;
        }

        // --- any JMP --- compute BOTH the fall-through and the taken path,
        //                 then try the post-JMP tail on each.
        unsigned long long fall_through_cur = 0;
        unsigned long long taken_target_cur = 0;
        {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0])) return false;
            if (!is_jump(instr.mnemonic)) return false;

            fall_through_cur = cur + instr.length;

            uint64_t target = 0;
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &ops[0], cur, &target)))
            {
                taken_target_cur = (unsigned long long)target;
            }
        }

        // Pass 1: file-2 behavior — fall-through after JMP (game version A)
        if (validate_actor_prologue_tail(fall_through_cur)) {
            return true;
        }
        printf("[ACTOR] prologue: fall-through tail failed, trying jump target\n");

        // Pass 2: file-1 behavior — follow JMP target (game version B)
        if (taken_target_cur != 0 && validate_actor_prologue_tail(taken_target_cur)) {
            return true;
        }

        printf("[ACTOR] prologue: both fall-through and taken-path tails failed\n");
        return false;
    }

    static bool try_match_patch_pattern_at(
        unsigned long long cur,
        bool                allow_mov_reg_reg,
        unsigned long long* out_test_addr,
        unsigned long long* out_mov_addr,
        ZydisRegister* out_entity_reg,
        const char** out_sentinel_name)
    {
        ZydisDecodedInstruction instr0;
        ZydisDecodedOperand     ops0[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_instruction(cur, &instr0, &ops0[0])) return false;

        // ---- Step 1: MOV [reg1 + reg2*1], src ----
        if (!(instr0.mnemonic == ZYDIS_MNEMONIC_MOV
            && ops0[0].type == ZYDIS_OPERAND_TYPE_MEMORY
            && ops0[0].mem.base != ZYDIS_REGISTER_NONE
            && ops0[0].mem.index != ZYDIS_REGISTER_NONE
            && ops0[0].mem.scale == 1))
            return false;

        unsigned long long mov_addr = cur;
        unsigned long long after_mov = cur + instr0.length;

        // ---- Step 2: TEST <reg>, <reg> immediately after ----
        ZydisDecodedInstruction instr1;
        ZydisDecodedOperand     ops1[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_instruction(after_mov, &instr1, &ops1[0])) return false;
        if (instr1.mnemonic != ZYDIS_MNEMONIC_TEST
            || ops1[0].type != ZYDIS_OPERAND_TYPE_REGISTER
            || ops1[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
            return false;

        unsigned long long test_addr = after_mov;
        unsigned long long after_test = test_addr + instr1.length;

        // ---- Step 3: any JMP immediately after TEST ----
        ZydisDecodedInstruction instr2;
        ZydisDecodedOperand     ops2[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_instruction(after_test, &instr2, &ops2[0])) return false;
        if (!is_jump(instr2.mnemonic)) return false;

        unsigned long long after_jmp = after_test + instr2.length;

        // ---- Step 4: sentinel ----
        //   strict pass  : LOCK INC only
        //   loose pass   : LOCK INC, OR a MOV r,(R|E)SP stack-pointer-save idiom
        //                  (e.g. `mov r10d, esp` / `mov r10, rsp`).
        ZydisDecodedInstruction instr3;
        ZydisDecodedOperand     ops3[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_instruction(after_jmp, &instr3, &ops3[0])) return false;

        bool step4_lock_inc = (instr3.attributes & ZYDIS_ATTRIB_HAS_LOCK)
            && instr3.mnemonic == ZYDIS_MNEMONIC_INC;

        // Loose sentinel: MOV <gpr32|gpr64>, (E|R)SP — the stack-pointer save
        // idiom the compiler emits right after the non-taken branch. We don't
        // pin the destination register because the compiler is free to pick
        // any caller-saved reg across builds, but the *source* being the stack
        // pointer is highly distinctive.
        bool step4_mov_sp_save = false;
        if (allow_mov_reg_reg
            && instr3.mnemonic == ZYDIS_MNEMONIC_MOV
            && ops3[0].type == ZYDIS_OPERAND_TYPE_REGISTER
            && ops3[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
        {
            ZydisRegister      dst = ops3[0].reg.value;
            ZydisRegister      src = ops3[1].reg.value;
            ZydisRegisterClass dstC = ZydisRegisterGetClass(dst);
            ZydisRegisterClass srcC = ZydisRegisterGetClass(src);

            bool dst_is_gpr =
                (dstC == ZYDIS_REGCLASS_GPR64 || dstC == ZYDIS_REGCLASS_GPR32);
            bool src_is_sp =
                (src == ZYDIS_REGISTER_RSP || src == ZYDIS_REGISTER_ESP);

            // Sanity: dst and src must reference different physical regs.
            if (dst_is_gpr
                && src_is_sp
                && (srcC == ZYDIS_REGCLASS_GPR64 || srcC == ZYDIS_REGCLASS_GPR32)
                && ZydisRegisterGetId(dst) != ZydisRegisterGetId(src))
            {
                step4_mov_sp_save = true;
            }
        }

        if (!step4_lock_inc && !step4_mov_sp_save) return false;

        *out_test_addr = test_addr;
        *out_mov_addr = mov_addr;
        *out_entity_reg = ops0[0].mem.base;
        *out_sentinel_name = step4_lock_inc ? "LOCK INC" : "MOV r,(R|E)SP";
        return true;
    }

    // -------------------------------------------------------------------------
    // find_patch_pattern_in_function
    //   Two-pass scan inside the function:
    //     Pass A: strict — sentinel must be LOCK INC (game version A)
    //     Pass B: loose  — sentinel may be LOCK INC OR any MOV r64,r64 (version B+)
    //   Strict pass first so a binary that matches both lands on the same
    //   address it always did.
    // -------------------------------------------------------------------------
    static bool find_patch_pattern_in_function(
        unsigned long long fn_start,
        unsigned long long* patch_addr,
        unsigned char       mov_instruction[3])
    {
        const int MAX_SCAN_BYTES = 0x10000;
        const unsigned long long limit = fn_start + MAX_SCAN_BYTES;

        auto run_pass = [&](bool allow_mov_reg_reg) -> bool {
            printf("[ACTOR] Scanning for MOV/TEST/JMP/%s pattern from 0x%llX\n",
                allow_mov_reg_reg ? "(LOCK-INC|MOV r64,r64)" : "LOCK-INC",
                fn_start);

            unsigned long long cur = fn_start;
            while (cur < limit) {
                ZydisDecodedInstruction instr0;
                ZydisDecodedOperand     ops0[ZYDIS_MAX_OPERAND_COUNT];
                if (!decode_instruction(cur, &instr0, &ops0[0])) {
                    cur++;
                    continue;
                }

                unsigned long long test_addr = 0;
                unsigned long long mov_addr = 0;
                ZydisRegister      entity_reg = ZYDIS_REGISTER_NONE;
                const char* sentinel = nullptr;

                if (try_match_patch_pattern_at(cur, allow_mov_reg_reg,
                    &test_addr, &mov_addr, &entity_reg, &sentinel))
                {
                    printf("[ACTOR] Pattern found: MOV@0x%llX  TEST@0x%llX  sentinel=%s\n",
                        mov_addr, test_addr, sentinel);
                    printf("[ACTOR] entity list register: %s\n",
                        ZydisRegisterGetString(entity_reg));

                    int reg_id = ZydisRegisterGetId(entity_reg);
                    if (reg_id < 0) {
                        printf("[ACTOR] could not get reg id\n");
                        return false;
                    }

                    uint8_t rex_R = (reg_id >= 8) ? 1 : 0;
                    uint8_t new_rex = (uint8_t)(0x40 | (1 << 3) | (rex_R << 2)); // REX.W | REX.R?
                    uint8_t reg_fld = (uint8_t)(reg_id & 0x7);
                    uint8_t new_modrm = (uint8_t)((reg_fld << 3) | 0x05);         // mod=00, rm=101 rip-rel

                    *patch_addr = test_addr;
                    mov_instruction[0] = new_rex;
                    mov_instruction[1] = 0x89;
                    mov_instruction[2] = new_modrm;
                    return true;
                }

                cur += instr0.length;
            }

            return false;
            };

        // Pass A: strict (LOCK INC only) — game version A
        if (run_pass(/*allow_mov_reg_reg=*/false)) return true;

        printf("[ACTOR] strict LOCK-INC pass failed in 0x%llX, retrying with MOV r64,r64 allowed\n",
            fn_start);

        // Pass B: loose (LOCK INC or any MOV r64,r64) — game version B and forward-compat
        if (run_pass(/*allow_mov_reg_reg=*/true)) return true;

        printf("[ACTOR] find_patch_pattern_in_function: pattern not found within 0x%X bytes of 0x%llX\n",
            MAX_SCAN_BYTES, fn_start);
        return false;
    }

    // -------------------------------------------------------------------------
    // resolve_call_target
    //   Decodes a CALL instruction at call_addr and returns the absolute target.
    //   Handles E8 (rel32) and FF /2 (indirect RIP-relative) forms.
    //   Returns 0 on failure.
    // -------------------------------------------------------------------------
    static unsigned long long resolve_call_target(unsigned long long call_addr)
    {
        ZydisDecodedInstruction instr;
        ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_instruction(call_addr, &instr, &ops[0])) return 0;
        if (instr.mnemonic != ZYDIS_MNEMONIC_CALL)          return 0;

        // Immediate (relative) call: E8 rel32
        if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
        {
            uint64_t target = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &ops[0], call_addr, &target)))
                return (unsigned long long)target;
        }

        // Memory (indirect) call: FF /2  [RIP+disp]
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY
            && ops[0].mem.base == ZYDIS_REGISTER_RIP)
        {
            uint64_t ptr_addr = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &ops[0], call_addr, &ptr_addr)))
            {
                unsigned long long target = 0;
                if (g_backend->read_memory(ptr_addr, &target, sizeof(target)))
                    return target;
            }
        }

        return 0;
    }

    // -------------------------------------------------------------------------
    // find_actor_via_caller
    //   Given a hit from the caller signature, scans forward for the first
    //   N_CALLS_TO_CHECK direct CALL instructions, follows each one, checks
    //   validate_actor_prologue on the callee, then searches for the
    //   MOV/TEST/JMP/sentinel patch pattern inside that callee.
    // -------------------------------------------------------------------------
    static const int N_CALLS_TO_CHECK = 5;

    static bool find_actor_via_caller(
        unsigned long long caller_sig_hit,
        unsigned long long* patch_addr,
        unsigned char       mov_instruction[3])
    {
        printf("[ACTOR] Processing caller sig hit @ 0x%llX\n", caller_sig_hit);

        unsigned long long cur = caller_sig_hit;
        unsigned long long scan_limit = caller_sig_hit + 0x200;
        int calls_found = 0;

        while (cur < scan_limit && calls_found < N_CALLS_TO_CHECK)
        {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode_instruction(cur, &instr, &ops[0]))
            {
                printf("[ACTOR]   decode failed @ 0x%llX, stepping 1 byte\n", cur);
                cur++;
                continue;
            }

            if (instr.mnemonic == ZYDIS_MNEMONIC_CALL)
            {
                unsigned long long callee = resolve_call_target(cur);
                calls_found++;

                printf("[ACTOR]   call[%d] @ 0x%llX -> callee 0x%llX\n",
                    calls_found, cur, callee);

                // BUG FIX: always advance past this instruction before continuing
                cur += instr.length;

                if (callee != 0)
                {
                    unsigned long long fn_start = FindFunctionStart(callee, g_backend->m_base_address);
                    if (!fn_start) fn_start = callee;

                    printf("[ACTOR]   fn_start = 0x%llX\n", fn_start);

                    // Debug: dump first 16 raw bytes at fn_start so we can see what Zydis sees
                    {
                        uint8_t raw[16] = {};
                        bool read_ok = g_backend->read_memory(fn_start, raw, sizeof(raw));
                        printf("[ACTOR]   read_memory(%llX, 16) = %s | bytes: ", fn_start, read_ok ? "OK" : "FAIL");
                        for (int b = 0; b < 16; b++) printf("%02X ", raw[b]);
                        printf("\n");

                        ZydisDecodedInstruction test_instr;
                        ZydisDecodedOperand     test_ops[ZYDIS_MAX_OPERAND_COUNT];
                        bool decode_ok = decode_instruction(fn_start, &test_instr, &test_ops[0]);
                        printf("[ACTOR]   first insn decode = %s", decode_ok ? "OK" : "FAIL");
                        if (decode_ok)
                            printf(" | mnemonic=%s len=%d", ZydisMnemonicGetString(test_instr.mnemonic), test_instr.length);
                        printf("\n");
                    }

                    if (validate_actor_prologue(fn_start))
                    {
                        printf("[ACTOR]   prologue PASSED for callee 0x%llX\n", fn_start);

                        if (find_patch_pattern_in_function(fn_start, patch_addr, mov_instruction))
                        {
                            printf("[ACTOR]   patch pattern FOUND — patch=0x%llX  mov=%02X %02X %02X\n",
                                *patch_addr,
                                mov_instruction[0], mov_instruction[1], mov_instruction[2]);
                            return true;
                        }
                        else
                        {
                            printf("[ACTOR]   patch pattern NOT found in fn 0x%llX, continuing\n", fn_start);
                        }
                    }
                    else
                    {
                        printf("[ACTOR]   prologue FAILED for callee 0x%llX\n", fn_start);
                    }
                }
                continue;
            }

            cur += instr.length;
        }

        printf("[ACTOR] find_actor_via_caller: no valid actor found from hit @ 0x%llX\n", caller_sig_hit);
        return false;
    }

    // -------------------------------------------------------------------------
    // Camera + code cave helpers (unchanged)
    // -------------------------------------------------------------------------
    bool validate_camera_function(unsigned long long sig_addr) {
        unsigned long long movzx_addr = scan_forwards(sig_addr, ZYDIS_MNEMONIC_MOVZX, 20);
        if (!movzx_addr) return false;

        unsigned long long xor_addr = scan_forwards(movzx_addr, ZYDIS_MNEMONIC_XOR, 15);
        if (!xor_addr) return false;

        unsigned long long mov_addr = scan_backwards(xor_addr, ZYDIS_MNEMONIC_MOV, 5);
        if (!mov_addr) return false;

        unsigned long long call_addr = scan_forwards(xor_addr, ZYDIS_MNEMONIC_CALL, 3);
        if (!call_addr) return false;

        return true;
    }

    bool find_camera_patch(unsigned long long sig_addr, unsigned long long* patch_addr) {
        if (!validate_camera_function(sig_addr)) return false;

        unsigned long long movzx_addr = scan_forwards(sig_addr, ZYDIS_MNEMONIC_MOVZX, 20);
        unsigned long long xor_addr = scan_forwards(movzx_addr, ZYDIS_MNEMONIC_XOR, 15);
        if (!xor_addr) return false;

        *patch_addr = xor_addr;
        return true;
    }

    bool find_code_caves(unsigned long long* cave_one, unsigned long long* cave_two,
        unsigned long long* cave_three = nullptr,
        unsigned long long min_size = 0x1000,
        unsigned long long separation = 0x2000,
        unsigned long long safety_offset = 0x50)
    {
        auto data_section = pe_parser::get_data_section(g_backend->m_base_address);
        if (data_section.size == 0) return false;

        std::vector<unsigned long long> caves;
        unsigned long long current_cave_start = 0;
        unsigned long long current_cave_size = 0;
        const unsigned long long chunk_size = 0x10000;

        for (unsigned long long offset = 0; offset < data_section.size; offset += chunk_size) {
            unsigned long long current_chunk_size =
                (chunk_size < (data_section.size - offset))
                ? chunk_size : (data_section.size - offset);
            std::vector<unsigned char> buffer((size_t)current_chunk_size);
            read_memory_chunked((uintptr_t)(data_section.start + offset), buffer.data(), (size_t)current_chunk_size);

            for (size_t i = 0; i < current_chunk_size; i++) {
                if (buffer[i] == 0) {
                    if (current_cave_size == 0)
                        current_cave_start = data_section.start + offset + i;
                    current_cave_size++;
                }
                else {
                    if (current_cave_size >= (min_size + safety_offset)) {
                        caves.push_back(current_cave_start + safety_offset);
                    }
                    current_cave_size = 0;
                }
            }
        }

        if (current_cave_size >= (min_size + safety_offset))
            caves.push_back(current_cave_start + safety_offset);

        if (caves.size() < 2) return false;

        if (!cave_three) {
            for (size_t i = 0; i < caves.size(); i++) {
                for (size_t j = i + 1; j < caves.size(); j++) {
                    if (caves[j] - caves[i] >= separation) {
                        *cave_one = caves[i];
                        *cave_two = caves[j];
                        return true;
                    }
                }
            }
            return false;
        }

        // Three-cave search: all pairs must be at least `separation` apart
        if (caves.size() < 3) return false;
        for (size_t i = 0; i < caves.size(); i++) {
            for (size_t j = i + 1; j < caves.size(); j++) {
                if (caves[j] - caves[i] < separation) continue;
                for (size_t k = j + 1; k < caves.size(); k++) {
                    if (caves[k] - caves[j] >= separation) {
                        *cave_one   = caves[i];
                        *cave_two   = caves[j];
                        *cave_three = caves[k];
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // find_world_patch
    // Find the TtWorldCastRay function via its .rdata string, locate the LEA
    // that references it in .text, then look 5 bytes ABOVE the LEA for:
    //   48 85 DB  (test rbx,rbx)
    //   74 ??     (je rel8)
    // This 5-byte site is replaced with a near JMP to a trampoline that
    // captures RBP (the Havok world pointer, = first arg to TtWorldCastRay).
    // -------------------------------------------------------------------------
    bool find_world_patch(unsigned long long* patch_addr)
    {
        uint64_t base = g_backend->m_base_address;

        // 1. Find "TtWorldCastRay" in .rdata
        auto rdata = pe_parser::get_rdata_section(base);
        if (!rdata.size) {
            printf("[SCANNER] WorldPatch: .rdata not found\n");
            return false;
        }
        // "TtWorldCastRay" = 54 74 57 6F 72 6C 64 43 61 73 74 52 61 79
        auto str_pat = parse_pattern("54 74 57 6F 72 6C 64 43 61 73 74 52 61 79");
        auto str_res = scan_pattern(str_pat, rdata.start, rdata.size);
        if (str_res.empty()) {
            printf("[SCANNER] WorldPatch: TtWorldCastRay not found in .rdata\n");
            return false;
        }
        uint64_t wcr_addr = str_res[0];
        printf("[SCANNER] WorldPatch: TtWorldCastRay string @ 0x%llx\n", wcr_addr);

        // 2. Find LEA in .text whose RIP-relative target == wcr_addr
        auto text = pe_parser::get_text_section(base);
        if (!text.size) return false;

        uint64_t lea_addr = 0;
        const uint64_t CHUNK = 0x10000;
        for (uint64_t off = 0; off < text.size && !lea_addr; off += CHUNK) {
            uint64_t csz = (CHUNK < text.size - off) ? CHUNK : (text.size - off);
            std::vector<uint8_t> buf((size_t)csz);
            read_memory_chunked(text.start + off, buf.data(), (size_t)csz);
            for (uint64_t i = 0; i + 7 <= csz; i++) {
                // REX.W LEA: 48 8D <modrm> where modrm & 0xC7 == 0x05 (RIP-relative)
                if (buf[i] != 0x48 || buf[i+1] != 0x8D) continue;
                if ((buf[i+2] & 0xC7) != 0x05) continue;
                int32_t rel32; memcpy(&rel32, &buf[i+3], 4);
                uint64_t insn_va = text.start + off + i;
                uint64_t target  = insn_va + 7 + (int64_t)rel32;
                if (target == wcr_addr) { lea_addr = insn_va; break; }
            }
        }
        if (!lea_addr) {
            printf("[SCANNER] WorldPatch: LEA for TtWorldCastRay not found in .text\n");
            return false;
        }
        printf("[SCANNER] WorldPatch: LEA @ 0x%llx\n", lea_addr);

        // 3. The patch site is exactly 5 bytes ABOVE the LEA:
        //    48 85 DB  = test rbx,rbx
        //    74 ??     = je rel8
        // These 5 bytes are replaced with a near JMP to the trampoline.
        uint64_t site = lea_addr - 5;
        uint8_t site_bytes[5] = {};
        read_memory_chunked(site, site_bytes, 5);
        if (site_bytes[0] != 0x48 || site_bytes[1] != 0x85 || site_bytes[2] != 0xDB ||
            site_bytes[3] != 0x74) {
            printf("[SCANNER] WorldPatch: expected 'test rbx,rbx + je' at LEA-5 (got %02X %02X %02X %02X)\n",
                site_bytes[0], site_bytes[1], site_bytes[2], site_bytes[3]);
            return false;
        }
        *patch_addr = site;
        printf("[SCANNER] WorldPatch: patch site @ 0x%llx (test+je, 5 bytes)\n", site);
        return true;
    }

    // -------------------------------------------------------------------------
    // auto_find_offsets
    // -------------------------------------------------------------------------
    bool auto_find_offsets(unsigned long long* actor_patch, unsigned long long* camera_patch,
        unsigned long long* cave_one, unsigned long long* cave_two,
        unsigned char       actor_mov[3],
        unsigned long long* world_patch = nullptr,
        unsigned long long* cave_three  = nullptr)
    {
        printf("[SCANNER] Starting automatic offset finder...\n");

        auto text_section = pe_parser::get_text_section(g_backend->m_base_address);
        if (text_section.size == 0) {
            printf("[SCANNER] Failed: Could not find .text section\n");
            return false;
        }

        std::vector<std::string> actor_caller_sigs = {
            "65 ? 8B ? 25 58 00 00 00 ? 8B ? ? ? 8D ? ? ? ? 00 ? C1 ? 03"//"65 ? 8B ? 25 58 00 00 00 ? 8B ? ? ? 8D ? ? ? ? 00 ? 89 ? 24 ? ? C1 ? 03"
        };

        std::vector<std::string> camera_sigs = {
            "C7 44 24 28 00 08 00 00 4C 89",
            "C7 44 24 28 00 08 00 00",
        };

        // ---- Actor scan via caller sigs ----
        printf("[SCANNER] Searching for actor function via caller signatures...\n");
        bool actor_found = false;
        int  total_caller_matches = 0;

        for (const auto& sig_str : actor_caller_sigs)
        {
            Pattern pattern = parse_pattern(sig_str);
            auto    results = scan_pattern(pattern, text_section.start, text_section.size);
            total_caller_matches += (int)results.size();
            printf("[SCANNER] caller sig '%s' -> %zu hits\n", sig_str.c_str(), results.size());

            for (size_t idx = 0; idx < results.size(); idx++)
            {
                printf("[SCANNER] trying caller hit[%zu] @ 0x%llX\n", idx, results[idx]);

                unsigned long long found_patch = 0;
                unsigned char      found_mov[3] = {};

                if (find_actor_via_caller(results[idx], &found_patch, found_mov))
                {
                    *actor_patch = found_patch;
                    actor_mov[0] = found_mov[0];
                    actor_mov[1] = found_mov[1];
                    actor_mov[2] = found_mov[2];

                    offsets::ActorPatch = *actor_patch;
                    offsets::ActorMov[0] = actor_mov[0];
                    offsets::ActorMov[1] = actor_mov[1];
                    offsets::ActorMov[2] = actor_mov[2];

                    actor_found = true;
                    break;
                }
            }

            if (actor_found) break;
        }

        if (actor_found) {
            printf("[SCANNER] Actor function found via caller (%d caller matches scanned)\n", total_caller_matches);
            printf("[SCANNER] patch=0x%llX  mov=%02X %02X %02X\n",
                *actor_patch, actor_mov[0], actor_mov[1], actor_mov[2]);
        }
        else {
            printf("[SCANNER] Failed: Actor function not found (%d caller matches scanned)\n", total_caller_matches);
            return false;
        }

        // ---- Camera scan ----
        printf("[SCANNER] Searching for camera function...\n");
        bool camera_found = false;
        int  total_camera_matches = 0;

        for (const auto& sig_str : camera_sigs) {
            Pattern pattern = parse_pattern(sig_str);
            auto    results = scan_pattern(pattern, text_section.start, text_section.size);
            total_camera_matches += (int)results.size();

            for (size_t idx = 0; idx < results.size(); idx++) {
                if (find_camera_patch(results[idx], camera_patch)) {
                    camera_found = true;
                    break;
                }
            }

            if (camera_found) break;
        }

        if (camera_found) {
            printf("[SCANNER] Camera function found (%d potential matches scanned)\n", total_camera_matches);
        }
        else {
            printf("[SCANNER] Failed: Camera function not found (%d matches scanned)\n", total_camera_matches);
            return false;
        }

        // ---- Code caves ----
        printf("[SCANNER] Searching for code caves...\n");
        if (!find_code_caves(cave_one, cave_two, cave_three)) {
            printf("[SCANNER] Failed: Could not find suitable code caves\n");
            return false;
        }
        // if (cave_three) offsets::CodeCaveThree = *cave_three;
        printf("[SCANNER] Code caves found\n");

        // ---- World patch (optional) ----
        if (world_patch) {
            if (find_world_patch(world_patch)) {
                // offsets::WorldPatch = *world_patch;
                printf("[SCANNER]   World patch:  0x%llx\n", *world_patch);
            } else {
                printf("[SCANNER] WorldPatch not found (non-fatal)\n");
            }
        }
        if (cave_three) {
            printf("[SCANNER]   Code cave 3:  0x%llx\n", *cave_three);
        }

        printf("[SCANNER] Success! All offsets located\n");
        printf("[SCANNER]   Actor patch:  0x%llx\n", *actor_patch);
        printf("[SCANNER]   Camera patch: 0x%llx\n", *camera_patch);
        printf("[SCANNER]   Code cave 1:  0x%llx\n", *cave_one);
        printf("[SCANNER]   Code cave 2:  0x%llx\n", *cave_two);
        return true;
    }

} // namespace signature_scanner


// ---------------------------------------------------------------------------
// find_largest_text_cave  (unchanged)
// ---------------------------------------------------------------------------
struct CcCave {
    unsigned long long address;
    unsigned long long size;
};

CcCave find_largest_text_cave(unsigned long long safety_offset = 0x10)
{
    CcCave best = { 0, 0 };

    auto text_section = pe_parser::get_text_section(g_backend->m_base_address);
    if (text_section.size == 0) {
        printf("[SCANNER] find_largest_text_cave: .text section not found\n");
        return best;
    }

    unsigned long long current_cave_start = 0;
    unsigned long long current_cave_size = 0;
    const unsigned long long chunk_size = 0x100000;

    for (unsigned long long offset = 0; offset < text_section.size; offset += chunk_size) {
        unsigned long long current_chunk_size =
            (chunk_size < (text_section.size - offset))
            ? chunk_size : (text_section.size - offset);

        std::vector<unsigned char> buffer((size_t)current_chunk_size);
        signature_scanner::read_memory_chunked(
            (uintptr_t)(text_section.start + offset), buffer.data(), (size_t)current_chunk_size);

        for (size_t i = 0; i < current_chunk_size; i++) {
            if (buffer[i] == 0xCC) {
                if (current_cave_size == 0)
                    current_cave_start = text_section.start + offset + i;
                current_cave_size++;
            }
            else {
                if (current_cave_size > best.size) {
                    best.size = current_cave_size;
                    best.address = current_cave_start;
                }
                current_cave_size = 0;
            }
        }
    }

    if (current_cave_size > best.size) {
        best.size = current_cave_size;
        best.address = current_cave_start;
    }

    if (best.size <= safety_offset) {
        printf("[SCANNER] find_largest_text_cave: no usable cave found\n");
        return { 0, 0 };
    }

    best.address += safety_offset;
    best.size -= safety_offset;

    printf("[SCANNER] Largest .text CC cave: 0x%llx  size: 0x%llx bytes\n", best.address, best.size);
    return best;
}


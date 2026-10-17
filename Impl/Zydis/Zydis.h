#pragma once
#include <Zydis/Zydis.h>

namespace pe_parser
{
    struct SectionInfo {
        unsigned long long start;
        unsigned long long size;
        std::string name;
    };

    bool read_dos_header(unsigned long long base, IMAGE_DOS_HEADER* dos_header) {
        *dos_header = g_driver->read<IMAGE_DOS_HEADER>(base);
        return dos_header->e_magic == IMAGE_DOS_SIGNATURE;
    }

    bool read_nt_headers(unsigned long long base, IMAGE_NT_HEADERS64* nt_headers) {
        IMAGE_DOS_HEADER dos_header;
        if (!read_dos_header(base, &dos_header)) {
            return false;
        }

        *nt_headers = g_driver->read<IMAGE_NT_HEADERS64>(base + dos_header.e_lfanew);
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
            IMAGE_SECTION_HEADER section_header = g_driver->read<IMAGE_SECTION_HEADER>(
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
            if (!g_driver->read_memory(address + offset, dst + offset, chunk)) {
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
                // Full wildcard
                pattern.bytes.push_back(-1);
                pattern.mask += "?";
            }
            else if (byte_str.size() == 2 && byte_str[1] == '?') {
                // High nibble fixed, low nibble wildcard (e.g. "7?")
                // Store as a special value: 0x100 | high_nibble
                int high = std::stoi(byte_str.substr(0, 1), nullptr, 16);
                pattern.bytes.push_back(0x100 | high);
                pattern.mask += "n"; // n = nibble wildcard
            }
            else if (byte_str.size() == 2 && byte_str[0] == '?') {
                // Low nibble fixed, high nibble wildcard (e.g. "?4")
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
                        // Full wildcard, always matches
                        continue;
                    }
                    else if ((p & 0x300) == 0x100) {
                        // High nibble fixed (e.g. 7?): check upper 4 bits
                        if ((b >> 4) != (p & 0xF)) { found = false; break; }
                    }
                    else if ((p & 0x300) == 0x200) {
                        // Low nibble fixed (e.g. ?4): check lower 4 bits
                        if ((b & 0xF) != (p & 0xF)) { found = false; break; }
                    }
                    else {
                        // Exact match
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

    bool read_instruction(unsigned long long addr, ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operands) {
        unsigned char buffer[15];
        *reinterpret_cast<uint64_t*>(buffer) = g_driver->read<uint64_t>(addr);
        *reinterpret_cast<uint64_t*>(buffer + 7) = g_driver->read<uint64_t>(addr + 7);

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, buffer, sizeof(buffer), instruction, operands));
    }

    unsigned long long scan_backwards(unsigned long long start_addr, ZydisMnemonic target_mnemonic, int max_instructions) {
        unsigned long long scan_start = start_addr - 200;
        unsigned long long current_addr = scan_start;

        std::vector<unsigned long long> instruction_addrs;

        while (current_addr < start_addr) {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

            if (read_instruction(current_addr, &instruction, operands)) {
                instruction_addrs.push_back(current_addr);
                current_addr += instruction.length;
            }
            else {
                current_addr++;
            }
        }

        for (int i = instruction_addrs.size() - 1; i >= 0 && i >= (int)instruction_addrs.size() - max_instructions; i--) {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

            if (read_instruction(instruction_addrs[i], &instruction, operands)) {
                if (instruction.mnemonic == target_mnemonic) {
                    return instruction_addrs[i];
                }
            }
        }

        return 0;
    }

    unsigned long long scan_forwards(unsigned long long start_addr, ZydisMnemonic target_mnemonic, int max_instructions = 20) {
        unsigned long long current_addr = start_addr;

        for (int i = 0; i < max_instructions; i++) {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

            if (!read_instruction(current_addr, &instruction, operands)) {
                break;
            }

            if (instruction.mnemonic == target_mnemonic) {
                return current_addr;
            }

            current_addr += instruction.length;
        }
        return 0;
    }

    // Returns true if the mnemonic is any kind of jump (conditional or unconditional).
    // Uses only mnemonics that are valid in all common Zydis versions.
    static bool is_jump_mnemonic(ZydisMnemonic m) {
        return (m == ZYDIS_MNEMONIC_JMP ||
            m == ZYDIS_MNEMONIC_JZ ||   // JE  alias
            m == ZYDIS_MNEMONIC_JNZ ||   // JNE alias
            m == ZYDIS_MNEMONIC_JB ||   // JC / JNAE alias
            m == ZYDIS_MNEMONIC_JNB ||   // JAE / JNC alias
            m == ZYDIS_MNEMONIC_JNBE ||   // JA  alias
            m == ZYDIS_MNEMONIC_JBE ||   // JNA alias
            m == ZYDIS_MNEMONIC_JL ||   // JNGE alias
            m == ZYDIS_MNEMONIC_JNL ||   // JGE alias
            m == ZYDIS_MNEMONIC_JNLE ||   // JG  alias
            m == ZYDIS_MNEMONIC_JLE ||   // JNG alias
            m == ZYDIS_MNEMONIC_JS ||
            m == ZYDIS_MNEMONIC_JNS ||
            m == ZYDIS_MNEMONIC_JO ||
            m == ZYDIS_MNEMONIC_JNO ||
            m == ZYDIS_MNEMONIC_JP ||   // JPE alias
            m == ZYDIS_MNEMONIC_JNP ||   // JPO alias
            m == ZYDIS_MNEMONIC_JCXZ ||
            m == ZYDIS_MNEMONIC_JECXZ ||
            m == ZYDIS_MNEMONIC_JRCXZ);
    }

    // Scan forward looking for TEST, but stop and report any jump hit first.
    // Returns: address of TEST if found before any jump, 0 otherwise.
    // If a jump was hit first and it has a resolvable RIP-relative target,
    // *jump_target_out is set to that target; otherwise it stays 0.
    static unsigned long long scan_forwards_test_before_jump(
        unsigned long long start_addr,
        int max_instructions,
        unsigned long long* jump_target_out)
    {
        *jump_target_out = 0;
        unsigned long long current = start_addr;

        for (int i = 0; i < max_instructions; i++) {
            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

            if (!read_instruction(current, &instr, ops))
                break;

            if (instr.mnemonic == ZYDIS_MNEMONIC_TEST)
                return current;

            if (is_jump_mnemonic(instr.mnemonic)) {
                // Resolve the absolute jump target for RIP-relative / immediate jumps
                if (instr.operand_count >= 1 && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    ZyanU64 target = 0;
                    if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instr, &ops[0], current, &target))) {
                        *jump_target_out = (unsigned long long)target;
                    }
                }
                return 0; // TEST not found before this jump
            }

            current += instr.length;
        }
        return 0;
    }

    bool validate_actor_function(unsigned long long sig_addr) {
        unsigned long long movaps_addr = sig_addr;
        bool found_boundary = false;

        for (int offset = -5; offset <= 15; offset++) {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

            unsigned long long test_addr = sig_addr + offset;
            if (read_instruction(test_addr, &instruction, operands)) {
                if (instruction.mnemonic == ZYDIS_MNEMONIC_MOVAPS) {
                    movaps_addr = test_addr;
                    found_boundary = true;
                    break;
                }
            }
        }

        if (!found_boundary) {
            return false;
        }

        unsigned long long first_movaps = movaps_addr;
        unsigned long long current = movaps_addr;

        for (int i = 0; i < 10; i++) {
            unsigned long long prev = scan_backwards(current, ZYDIS_MNEMONIC_MOVAPS, 3);
            if (!prev) break;

            ZydisDecodedInstruction instr;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (read_instruction(prev, &instr, ops)) {
                if (instr.operand_count >= 2 &&
                    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    ops[0].mem.base == ZYDIS_REGISTER_RCX) {
                    first_movaps = prev;
                    current = prev;
                }
                else {
                    break;
                }
            }
            else {
                break;
            }
        }

        sig_addr = first_movaps;

        unsigned long long mov_addr = scan_backwards(sig_addr, ZYDIS_MNEMONIC_MOV, 10);
        if (!mov_addr) {
            return false;
        }

        unsigned long long sub_addr = scan_backwards(mov_addr, ZYDIS_MNEMONIC_SUB, 10);
        if (!sub_addr) {
            return false;
        }

        unsigned long long xchg_addr = scan_forwards(sig_addr, ZYDIS_MNEMONIC_XCHG, 15);
        if (!xchg_addr) {
            return false;
        }

        unsigned long long movzx_addr = scan_forwards(xchg_addr, ZYDIS_MNEMONIC_MOVZX, 5);
        if (!movzx_addr) {
            return false;
        }

        return true;
    }

    bool find_actor_patch(unsigned long long sig_addr,
        unsigned long long* patch_addr,
        unsigned char mov_instruction[3])
    {
        if (!validate_actor_function(sig_addr)) {
            return false;
        }

        unsigned long long mov_addr = scan_backwards(sig_addr, ZYDIS_MNEMONIC_MOV, 30);
        if (!mov_addr) {
            return false;
        }

        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        if (!read_instruction(mov_addr, &instruction, operands)) {
            return false;
        }

        if (instruction.operand_count < 1 || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return false;
        }

        ZydisRegister reg = operands[0].reg.value;
        ZydisRegister largest = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);

        if (largest < ZYDIS_REGISTER_RAX || largest > ZYDIS_REGISTER_R15) {
            return false;
        }

        int reg_index = largest - ZYDIS_REGISTER_RAX;
        unsigned char low3 = reg_index & 7;
        unsigned char rex = (reg_index >= 8) ? 0x4C : 0x48;
        unsigned char modrm = (unsigned char)((low3 << 3) | 0x05);

        mov_instruction[0] = rex;
        mov_instruction[1] = 0x89;
        mov_instruction[2] = modrm;

        unsigned long long xchg_addr = scan_forwards(sig_addr, ZYDIS_MNEMONIC_XCHG, 15);
        if (!xchg_addr) {
            return false;
        }

        unsigned long long movzx_addr = scan_forwards(xchg_addr, ZYDIS_MNEMONIC_MOVZX, 5);
        if (!movzx_addr) {
            return false;
        }

        // --- Smart TEST finder ---
        // Look for TEST forward from movzx, but stop at any jump encountered first.
        unsigned long long jump_target = 0;
        unsigned long long test_addr = scan_forwards_test_before_jump(movzx_addr, 30, &jump_target);

        if (!test_addr && jump_target != 0) {
            // A jump was hit before TEST. Follow it one level deep.
            printf("[SCANNER] Jump before TEST detected, following to 0x%llx\n", jump_target);

            unsigned long long inner_jump = 0;
            test_addr = scan_forwards_test_before_jump(jump_target, 30, &inner_jump);

            if (test_addr) {
                printf("[SCANNER] Using TEST inside jump target at 0x%llx\n", test_addr);
            }
            else {
                // Nothing inside the jump either — fall back to the first TEST anywhere after movzx
                printf("[SCANNER] No TEST inside jump target, falling back to outer TEST scan\n");
                test_addr = scan_forwards(movzx_addr, ZYDIS_MNEMONIC_TEST, 60);
            }
        }

        if (!test_addr) {
            return false;
        }

        *patch_addr = test_addr;
        return true;
    }

    bool validate_camera_function(unsigned long long sig_addr) {
        unsigned long long movzx_addr = scan_forwards(sig_addr, ZYDIS_MNEMONIC_MOVZX, 20);
        if (!movzx_addr) {
            return false;
        }

        unsigned long long xor_addr = scan_forwards(movzx_addr, ZYDIS_MNEMONIC_XOR, 15);
        if (!xor_addr) {
            return false;
        }

        unsigned long long mov_addr = scan_backwards(xor_addr, ZYDIS_MNEMONIC_MOV, 5);
        if (!mov_addr) {
            return false;
        }

        unsigned long long call_addr = scan_forwards(xor_addr, ZYDIS_MNEMONIC_CALL, 3);
        if (!call_addr) {
            return false;
        }

        return true;
    }

    bool find_camera_patch(unsigned long long sig_addr, unsigned long long* patch_addr) {
        if (!validate_camera_function(sig_addr)) {
            return false;
        }

        unsigned long long movzx_addr = scan_forwards(sig_addr, ZYDIS_MNEMONIC_MOVZX, 20);
        unsigned long long xor_addr = scan_forwards(movzx_addr, ZYDIS_MNEMONIC_XOR, 15);

        if (!xor_addr) {
            return false;
        }

        *patch_addr = xor_addr;
        return true;
    }

    bool find_code_caves(unsigned long long* cave_one, unsigned long long* cave_two,
        unsigned long long min_size = 0x1000,
        unsigned long long separation = 0x2000,
        unsigned long long safety_offset = 0x50)
    {
        auto data_section = pe_parser::get_data_section(g_driver->m_base_address);
        if (data_section.size == 0) {
            return false;
        }

        std::vector<unsigned long long> caves;
        unsigned long long current_cave_start = 0;
        unsigned long long current_cave_size = 0;

        const unsigned long long chunk_size = 0x10000;

        for (unsigned long long offset = 0; offset < data_section.size; offset += chunk_size) {
            unsigned long long current_chunk_size = (chunk_size < (data_section.size - offset)) ? chunk_size : (data_section.size - offset);
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
                        unsigned long long safe_address = current_cave_start + safety_offset;
                        caves.push_back(safe_address);
                    }
                    current_cave_size = 0;
                }
            }
        }

        if (current_cave_size >= (min_size + safety_offset)) {
            unsigned long long safe_address = current_cave_start + safety_offset;
            caves.push_back(safe_address);
        }

        if (caves.size() < 2) {
            return false;
        }

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

    bool auto_find_offsets(unsigned long long* actor_patch, unsigned long long* camera_patch,
        unsigned long long* cave_one, unsigned long long* cave_two,
        unsigned char actor_mov[3]) {

        printf("[SCANNER] Starting automatic offset finder...\n");

        auto text_section = pe_parser::get_text_section(g_driver->m_base_address);
        if (text_section.size == 0) {
            printf("[SCANNER] Failed: Could not find .text section\n");
            return false;
        }

        std::vector<std::string> actor_sigs = {
            "20 0F ? ? 30 0F ? ? 50 0F ? ? 40 0F ? ? 30 0F ? ? 20",
            "0F 28 ? 0F 28 ? ? 0F 28 ? ? 0F 28 ? ? 0F 29 ? ? 0F 29 ? ? 0F 29 ? ? 0F 29"
        };

        std::vector<std::string> camera_sigs = {
            "C7 44 24 28 00 08 00 00 4C 89",
            "C7 44 24 28 00 08 00 00"
        };

        // Scan for actor
        printf("[SCANNER] Searching for actor function...\n");
        bool actor_found = false;
        int total_actor_matches = 0;

        for (const auto& sig_str : actor_sigs) {
            Pattern pattern = parse_pattern(sig_str);
            auto results = scan_pattern(pattern, text_section.start, text_section.size);
            total_actor_matches += results.size();

            for (size_t idx = 0; idx < results.size(); idx++) {
                if (find_actor_patch(results[idx], actor_patch, actor_mov)) {
                    actor_found = true;
                    break;
                }
            }

            if (actor_found) break;
        }

        if (actor_found) {
            printf("[SCANNER] Actor function found (%d potential matches scanned)\n", total_actor_matches);
        }
        else {
            printf("[SCANNER] Failed: Actor function not found (%d matches scanned)\n", total_actor_matches);
            return false;
        }

        // Scan for camera
        printf("[SCANNER] Searching for camera function...\n");
        bool camera_found = false;
        int total_camera_matches = 0;

        for (const auto& sig_str : camera_sigs) {
            Pattern pattern = parse_pattern(sig_str);
            auto results = scan_pattern(pattern, text_section.start, text_section.size);
            total_camera_matches += results.size();

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

        // Find code caves
        printf("[SCANNER] Searching for code caves...\n");
        if (!find_code_caves(cave_one, cave_two)) {
            printf("[SCANNER] Failed: Could not find suitable code caves\n");
            return false;
        }
        printf("[SCANNER] Code caves found\n");

        printf("[SCANNER] Success! All offsets located\n");
        printf("[SCANNER]   Actor patch:  0x%llx\n", *actor_patch);
        printf("[SCANNER]   Camera patch: 0x%llx\n", *camera_patch);
        printf("[SCANNER]   Code cave 1:  0x%llx\n", *cave_one);
        printf("[SCANNER]   Code cave 2:  0x%llx\n", *cave_two);

        return true;
    }
}
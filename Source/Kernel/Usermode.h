
#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <vector>
#include <string>
#include <iostream>
#pragma comment(lib, "Psapi.lib")

class c_usermode_driver
{
public:
    unsigned long process_id = 0;
    unsigned long long process_base = 0;
    unsigned long long process_cr3 = 0;
    HANDLE process_handle = nullptr;

    void* get_handle()
    {
        return process_handle;
    }

    void get_process(const char* process_name)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        PROCESSENTRY32W entry = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                char narrow_name[MAX_PATH];
                WideCharToMultiByte(CP_ACP, 0, entry.szExeFile, -1, narrow_name, MAX_PATH, NULL, NULL);
                if (_stricmp(narrow_name, process_name) == 0)
                {
                    process_id = entry.th32ProcessID;
                    process_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, process_id);
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    void get_base_address()
    {
        if (!process_handle)
            return;
        HMODULE modules[1024];
        DWORD needed;
        if (EnumProcessModules(process_handle, modules, sizeof(modules), &needed))
        {
            process_base = reinterpret_cast<unsigned long long>(modules[0]);
        }
    }

    void get_cr3()
    {
        // CR3 not needed for ReadProcessMemory/WriteProcessMemory
    }

    void read_physical(void* address, void* buffer, unsigned long size)
    {
        if (!process_handle)
            return;
        SIZE_T bytes_read;
        ReadProcessMemory(process_handle, address, buffer, size, &bytes_read);
    }

    bool write_physical(void* address, void* buffer, unsigned long size)
    {
        if (!process_handle)
            return false;

        DWORD old_protect = 0;
        BOOL protected_mem = VirtualProtectEx(process_handle, address, size, PAGE_EXECUTE_READWRITE, &old_protect);

        SIZE_T bytes_written = 0;
        BOOL result = WriteProcessMemory(process_handle, address, buffer, size, &bytes_written);

        if (protected_mem) {
            VirtualProtectEx(process_handle, address, size, old_protect, &old_protect);
        }

        return result && bytes_written == size;
    }

    bool get_valid_address(const unsigned long long address)
    {
        if (!process_handle || address == 0)
            return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQueryEx(process_handle, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
            return false;
        return (mbi.State == MEM_COMMIT) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
    }

    template <typename T>
    T read(unsigned long long address)
    {
        T buffer{};
        read_physical(reinterpret_cast<void*>(address), &buffer, sizeof(T));
        return buffer;
    }

    template <typename T>
    void write(unsigned long long address, T buffer)
    {
        write_physical(reinterpret_cast<void*>(address), &buffer, sizeof(T));
    }

    // Get .text section info
    bool get_text_section(unsigned long long& text_start, size_t& text_size)
    {
        if (!process_handle || !process_base)
            return false;

        // Read DOS header
        IMAGE_DOS_HEADER dos_header;
        SIZE_T bytes_read;
        if (!ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(process_base), &dos_header, sizeof(IMAGE_DOS_HEADER), &bytes_read))
            return false;

        if (dos_header.e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        // Read NT headers
        IMAGE_NT_HEADERS64 nt_headers;
        if (!ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(process_base + dos_header.e_lfanew), &nt_headers, sizeof(IMAGE_NT_HEADERS64), &bytes_read))
            return false;

        if (nt_headers.Signature != IMAGE_NT_SIGNATURE)
            return false;

        // Find .text section
        WORD num_sections = nt_headers.FileHeader.NumberOfSections;
        unsigned long long section_header_addr = process_base + dos_header.e_lfanew + sizeof(IMAGE_NT_HEADERS64);

        for (WORD i = 0; i < num_sections; i++)
        {
            IMAGE_SECTION_HEADER section_header;
            if (!ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(section_header_addr + (i * sizeof(IMAGE_SECTION_HEADER))), &section_header, sizeof(IMAGE_SECTION_HEADER), &bytes_read))
                continue;

            char section_name[9] = { 0 };
            memcpy(section_name, section_header.Name, 8);

            if (strcmp(section_name, ".text") == 0)
            {
                text_start = process_base + section_header.VirtualAddress;
                text_size = section_header.Misc.VirtualSize;

                std::cout << "[+] Found .text section at: 0x" << std::hex << text_start << std::dec << "\n";
                std::cout << "[+] .text section size: 0x" << std::hex << text_size << std::dec << " bytes\n";

                return true;
            }
        }

        return false;
    }

    // Pattern scanning implementation (scans .text section only)
    unsigned long long pattern_scan(const char* pattern)
    {
        if (!process_handle || !process_base)
            return 0;

        // Parse pattern string
        std::vector<int> pattern_bytes;
        std::vector<bool> mask;
        parse_pattern(pattern, pattern_bytes, mask);

        if (pattern_bytes.empty())
            return 0;

        // Get .text section
        unsigned long long text_start = 0;
        size_t text_size = 0;

        if (!get_text_section(text_start, text_size))
        {
            std::cout << "[!] Failed to find .text section!\n";
            return 0;
        }

        // Read .text section into memory
        std::vector<BYTE> buffer(text_size);
        SIZE_T bytes_read;
        if (!ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(text_start), buffer.data(), text_size, &bytes_read))
        {
            std::cout << "[!] Failed to read .text section!\n";
            return 0;
        }

        std::cout << "[*] Scanning " << (bytes_read / 1024 / 1024) << " MB of .text section...\n";

        // Search for pattern
        for (size_t i = 0; i < buffer.size() - pattern_bytes.size(); i++)
        {
            bool found = true;
            for (size_t j = 0; j < pattern_bytes.size(); j++)
            {
                if (mask[j] && buffer[i + j] != pattern_bytes[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
            {
                unsigned long long result = text_start + i;
                std::cout << "[+] Pattern found at offset: +0x" << std::hex << (result - process_base) << std::dec << "\n";
                return result;
            }
        }

        return 0;
    }

    // Pattern scan with offset and extra
    unsigned long long pattern_scan(const char* pattern, int offset, int extra = 0)
    {
        unsigned long long address = pattern_scan(pattern);
        if (address == 0)
            return 0;

        address += offset;

        if (extra != 0)
        {
            int rel = read<int>(address);
            address = address + extra + rel;
        }

        return address;
    }

    ~c_usermode_driver()
    {
        if (process_handle)
            CloseHandle(process_handle);
    }

private:
    void parse_pattern(const char* pattern, std::vector<int>& bytes, std::vector<bool>& mask)
    {
        const char* p = pattern;
        while (*p)
        {
            // Skip spaces
            while (*p == ' ')
                p++;

            if (*p == '\0')
                break;

            // Check for wildcard
            if (*p == '?')
            {
                bytes.push_back(0);
                mask.push_back(false);
                p++;
            }
            else
            {
                // Parse hex byte
                char hex[3] = { 0 };
                hex[0] = *p++;
                if (*p && *p != ' ')
                    hex[1] = *p++;

                int byte_val = static_cast<int>(strtol(hex, nullptr, 16));
                bytes.push_back(byte_val);
                mask.push_back(true);
            }
        }
    }
};

inline c_usermode_driver usermode_driver;
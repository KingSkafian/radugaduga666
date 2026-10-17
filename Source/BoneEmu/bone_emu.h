#pragma once
// BoneEmu — x86-64 instruction emulator that solves bone-world positions
// by replaying the game's bone-transform function inside this process with
// Zydis. Ported from the seller's internal-cheat version: every direct
// memcpy((void*)addr, ...) of game memory has been replaced with a
// g_backend->read_memory call, routed through a small page cache to keep
// the per-solve driver round-trip count manageable.
#include <Windows.h>
#include <Psapi.h>
#include "../../Dependencies/Zydis/Zydis.h"
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <immintrin.h>
#include <vector>
#include <unordered_map>

// g_backend (core::c_backend*) is declared in Bootstrap.h; this header
// must be included after backend.hxx.

#define XMMF(idx) cpu.xmm[idx]

union M128 {
    __m128  v;
    __m128d vd;
    __m128i vi;
    float   f32[4];
    double  f64[2];
    int32_t i32[4];
    int64_t i64[2];
    uint32_t u32[4];
    uint64_t u64[2];
    uint8_t  u8[16];
};

// Direct-mapped 16-entry 4 KB page cache. The bone-transform function we
// emulate re-reads the same instruction bytes thousands of times across a
// solve (hot loops, sibling-call decoding); without caching that would be
// thousands of driver round-trips per pawn. Per-bone data is colder but
// usually clusters within a handful of pages too.
namespace BoneEmuCache {
    struct Page { uint64_t base = 0; uint8_t data[0x1000]; bool valid = false; };
    inline Page g_pages[16];
    inline uint32_t Slot(uint64_t pageBase) { return (uint32_t)((pageBase >> 12) & 15); }
    inline void InvalidateAll() { for (auto& p : g_pages) p.valid = false; }
}

inline bool BoneEmu_SafeRead(uint64_t addr, void* buf, size_t size) {
    if (!addr || !buf || !size) return false;
    uint8_t* out = (uint8_t*)buf;
    uint64_t cur = addr;
    size_t   rem = size;
    while (rem) {
        uint64_t pageBase = cur & ~uint64_t(0xFFF);
        size_t   pageOff  = (size_t)(cur - pageBase);
        size_t   chunk    = 0x1000 - pageOff;
        if (chunk > rem) chunk = rem;
        auto& p = BoneEmuCache::g_pages[BoneEmuCache::Slot(pageBase)];
        if (!p.valid || p.base != pageBase) {
            if (!g_backend->read_memory(pageBase, p.data, 0x1000)) {
                p.valid = false;
                // Page-spanning read fell off the end of a mapped region;
                // try the exact range as a fallback (smaller surface).
                return g_backend->read_memory(addr, buf, size);
            }
            p.base  = pageBase;
            p.valid = true;
        }
        memcpy(out, p.data + pageOff, chunk);
        out += chunk;
        cur += chunk;
        rem -= chunk;
    }
    return true;
}

inline bool BoneEmu_SafeWrite(uint64_t /*addr*/, const void* /*buf*/, size_t /*size*/) {
    // External: writes to game memory are dropped on purpose. The emulator's
    // own stack writes go through the IsStackAddr fast path inside ReadMem/
    // WriteMem before reaching this helper, so what arrives here is always
    // the function-under-emulation's spurious writes into game scratch/
    // globals. Returning true keeps the emulation continuing.
    return true;
}
 
// ============================================================================
// Snapshot: captured at bone-index IMUL/SHL during probe.
// Contains full CPU state so we can replay just the transform part.
// ============================================================================
struct BoneSnapshot {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
    alignas(16) M128 xmm[16];
 
    // 8KB of stack around RSP (transform uses ~2-3KB max)
    alignas(16) uint8_t stackData[0x2000];
    uint64_t stackSaveBase;
 
    int callDepth;
    uint64_t outputAddr;
    uint64_t mainReturnAddr;
 
    bool valid = false;
};
 
struct PawnProbe {
    uint64_t pawnAddr = 0;
    BoneSnapshot snapshot;
    bool probed = false;
    int failCount = 0;      // consecutive fast-path failures -> re-probe
    DWORD probeTime = 0;
};
 
class BoneEmulatorV3 {
private:
    struct CPUState {
        uint64_t rax, rbx, rcx, rdx;
        uint64_t rsi, rdi, rbp, rsp;
        uint64_t r8, r9, r10, r11;
        uint64_t r12, r13, r14, r15;
        uint64_t rip;
        uint64_t rflags;
 
        alignas(16) M128 xmm[16];
        alignas(16) uint8_t stack[0x20000];
 
        void Clear() {
            rax = rbx = rcx = rdx = 0;
            rsi = rdi = rbp = 0;
            r8 = r9 = r10 = r11 = 0;
            r12 = r13 = r14 = r15 = 0;
            rip = 0;
            rflags = 0x202;
            rsp = (uint64_t)&stack[0x10000];
            rbp = rsp;
            memset(stack + 0x10000 - 0x4000, 0, 0x4000);
            for (int i = 0; i < 16; i++)
                xmm[i].v = _mm_setzero_ps();
        }
 
        uint64_t* GetGPR(ZydisRegister reg) {
            switch (reg) {
            case ZYDIS_REGISTER_RAX: case ZYDIS_REGISTER_EAX: case ZYDIS_REGISTER_AX:
            case ZYDIS_REGISTER_AL: case ZYDIS_REGISTER_AH: return &rax;
            case ZYDIS_REGISTER_RBX: case ZYDIS_REGISTER_EBX: case ZYDIS_REGISTER_BX:
            case ZYDIS_REGISTER_BL: case ZYDIS_REGISTER_BH: return &rbx;
            case ZYDIS_REGISTER_RCX: case ZYDIS_REGISTER_ECX: case ZYDIS_REGISTER_CX:
            case ZYDIS_REGISTER_CL: case ZYDIS_REGISTER_CH: return &rcx;
            case ZYDIS_REGISTER_RDX: case ZYDIS_REGISTER_EDX: case ZYDIS_REGISTER_DX:
            case ZYDIS_REGISTER_DL: case ZYDIS_REGISTER_DH: return &rdx;
            case ZYDIS_REGISTER_RSI: case ZYDIS_REGISTER_ESI: case ZYDIS_REGISTER_SI:
            case ZYDIS_REGISTER_SIL: return &rsi;
            case ZYDIS_REGISTER_RDI: case ZYDIS_REGISTER_EDI: case ZYDIS_REGISTER_DI:
            case ZYDIS_REGISTER_DIL: return &rdi;
            case ZYDIS_REGISTER_RBP: case ZYDIS_REGISTER_EBP: case ZYDIS_REGISTER_BP:
            case ZYDIS_REGISTER_BPL: return &rbp;
            case ZYDIS_REGISTER_RSP: case ZYDIS_REGISTER_ESP: case ZYDIS_REGISTER_SP:
            case ZYDIS_REGISTER_SPL: return &rsp;
            case ZYDIS_REGISTER_R8: case ZYDIS_REGISTER_R8D: case ZYDIS_REGISTER_R8W:
            case ZYDIS_REGISTER_R8B: return &r8;
            case ZYDIS_REGISTER_R9: case ZYDIS_REGISTER_R9D: case ZYDIS_REGISTER_R9W:
            case ZYDIS_REGISTER_R9B: return &r9;
            case ZYDIS_REGISTER_R10: case ZYDIS_REGISTER_R10D: case ZYDIS_REGISTER_R10W:
            case ZYDIS_REGISTER_R10B: return &r10;
            case ZYDIS_REGISTER_R11: case ZYDIS_REGISTER_R11D: case ZYDIS_REGISTER_R11W:
            case ZYDIS_REGISTER_R11B: return &r11;
            case ZYDIS_REGISTER_R12: case ZYDIS_REGISTER_R12D: case ZYDIS_REGISTER_R12W:
            case ZYDIS_REGISTER_R12B: return &r12;
            case ZYDIS_REGISTER_R13: case ZYDIS_REGISTER_R13D: case ZYDIS_REGISTER_R13W:
            case ZYDIS_REGISTER_R13B: return &r13;
            case ZYDIS_REGISTER_R14: case ZYDIS_REGISTER_R14D: case ZYDIS_REGISTER_R14W:
            case ZYDIS_REGISTER_R14B: return &r14;
            case ZYDIS_REGISTER_R15: case ZYDIS_REGISTER_R15D: case ZYDIS_REGISTER_R15W:
            case ZYDIS_REGISTER_R15B: return &r15;
            default: return nullptr;
            }
        }
 
        M128* GetXMM(ZydisRegister reg) {
            if (reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM15)
                return &xmm[reg - ZYDIS_REGISTER_XMM0];
            return nullptr;
        }
 
        bool IsStackAddr(uint64_t addr) {
            uint64_t start = (uint64_t)stack;
            return addr >= start && addr < start + sizeof(stack);
        }
 
        bool IsHighReg(ZydisRegister reg) {
            return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH ||
                reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH;
        }
    };
 
    // Decode cache
    struct DecodedInstr {
        uint64_t rip;
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    };
    struct DecodedSet { DecodedInstr way[2]; };
    static constexpr uint32_t DCACHE_BITS = 14;
    static constexpr uint32_t DCACHE_SIZE = 1u << DCACHE_BITS;
    DecodedSet* m_dcache = nullptr;
 
    inline uint32_t dcacheSlot(uint64_t rip) const {
        rip ^= rip >> 33; rip *= 0xff51afd7ed558ccdULL;
        rip ^= rip >> 33; rip *= 0xc4ceb9fe1a85ec53ULL;
        rip ^= rip >> 33;
        return (uint32_t)rip & (DCACHE_SIZE - 1);
    }
 
    CPUState cpu;
    ZydisDecoder decoder;
 
    uint64_t moduleBase = 0, moduleEnd = 0, targetFunc = 0;
    int instrCount = 0, maxInstr = 5000;
    int callDepth = 0, maxCallDepth = 25;
    bool verbose = false;
 
    alignas(16) M128 outputBone;
    uint64_t outputAddr = 0;
    uint64_t initialRCX = 0;
 
    bool foundValidPosition = false;
    M128 lastValidPosition;
 
    uint16_t desiredBoneIndex = 0;
    uint16_t detectedBoneIndex = 0xFFFF;
    bool boneIndexOverridden = false;
    bool detectedA3 = false;
    uint64_t mainReturnAddr = 0;
 
    M128 bestCandidate;
    float bestScore = 0.0f;
    int candidateHits = 0;
 
    // ========================================================================
    // PROBE SYSTEM
    // ========================================================================
    static constexpr int MAX_PROBES = 12;
    PawnProbe probes[MAX_PROBES];
 
    // Probe capture state (active during full emulation probe)
    bool isProbing = false;
    BoneSnapshot* probeTarget = nullptr;
 
    // Stats
    int statFullEmulations = 0;
    int statFastEmulations = 0;
    int statProbes = 0;

    // Diagnostics — populated by RunEmulation and Execute so callers can
    // see why a solve produced bad output. Reset at the top of each solve.
    int           dbgLastInstrCount     = 0;
    uint64_t      dbgLastExitRip        = 0;
    const char*   dbgLastExitReason     = "";
    bool          dbgLastRunOk          = false;
    ZydisMnemonic dbgFirstUnhandled     = ZYDIS_MNEMONIC_INVALID;
    uint64_t      dbgFirstUnhandledRip  = 0;
    int           dbgUnhandledCount     = 0;
    int           dbgCallExternalCount  = 0;
    int           dbgIsBoneIndexMulHits = 0;
    int           dbgImulCount          = 0;
    int           dbgShlCount           = 0;
    // First 8 unique IMUL multipliers seen with both operands non-zero,
    // plus the index value paired with them. If the bone-stride lives
    // here we'll see it.
    static constexpr int kImulSamples   = 8;
    uint64_t      dbgImulMul[kImulSamples] = {};
    uint64_t      dbgImulIdx[kImulSamples] = {};
    int           dbgImulN              = 0;

public:
    int           GetLastInstrCount()     const { return dbgLastInstrCount; }
    uint64_t      GetLastExitRip()        const { return dbgLastExitRip; }
    const char*   GetLastExitReason()     const { return dbgLastExitReason; }
    bool          GetLastRunOk()          const { return dbgLastRunOk; }
    ZydisMnemonic GetFirstUnhandled()     const { return dbgFirstUnhandled; }
    uint64_t      GetFirstUnhandledRip()  const { return dbgFirstUnhandledRip; }
    int           GetUnhandledCount()     const { return dbgUnhandledCount; }
    int           GetCallExternalCount()  const { return dbgCallExternalCount; }
    int           GetIsBoneIndexMulHits() const { return dbgIsBoneIndexMulHits; }
    int           GetImulCount()          const { return dbgImulCount; }
    int           GetShlCount()           const { return dbgShlCount; }
    int           GetImulSampleCount()    const { return dbgImulN; }
    uint64_t      GetImulSampleMul(int i) const { return (i >= 0 && i < dbgImulN) ? dbgImulMul[i] : 0; }
    uint64_t      GetImulSampleIdx(int i) const { return (i >= 0 && i < dbgImulN) ? dbgImulIdx[i] : 0; }
    bool          GetFoundValidPosition() const { return foundValidPosition; }
    uint64_t      GetTargetFunc()         const { return targetFunc; }
    uint64_t      GetModuleBase()         const { return moduleBase; }
    uint64_t      GetModuleEnd()          const { return moduleEnd; }

    BoneEmulatorV3() {
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        outputBone.v = _mm_setzero_ps();
        lastValidPosition.v = _mm_setzero_ps();
        memset(probes, 0, sizeof(probes));
    }
 
    ~BoneEmulatorV3() {
        if (m_dcache) { delete[] m_dcache; m_dcache = nullptr; }
    }
 
    void Init(uint64_t base, uint64_t size) {
        moduleBase = base;
        moduleEnd = base + size;
        if (!m_dcache) m_dcache = new DecodedSet[DCACHE_SIZE]();
    }

    // External-friendly Init: only needs the module base; the PE
    // OptionalHeader.SizeOfImage is read through the driver.
    bool InitFromBase(uint64_t base) {
        IMAGE_DOS_HEADER dos = {};
        if (!BoneEmu_SafeRead(base, &dos, sizeof(dos))) return false;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS nt = {};
        if (!BoneEmu_SafeRead(base + dos.e_lfanew, &nt, sizeof(nt))) return false;
        if (nt.Signature != IMAGE_NT_SIGNATURE) return false;
        Init(base, nt.OptionalHeader.SizeOfImage);
        return true;
    }
 
    void InvalidateCache() {
        if (m_dcache) memset(m_dcache, 0, DCACHE_SIZE * sizeof(DecodedSet));
        targetFunc = 0;
    }
 
    void InvalidateAllProbes() {
        for (int i = 0; i < MAX_PROBES; i++) {
            probes[i].probed = false;
            probes[i].snapshot.valid = false;
            probes[i].pawnAddr = 0;
        }
    }
 
    void InvalidatePawnProbe(uint64_t pawn) {
        for (int i = 0; i < MAX_PROBES; i++)
            if (probes[i].pawnAddr == pawn) {
                probes[i].probed = false;
                probes[i].snapshot.valid = false;
            }
    }
 
    void SetVerbose(bool v) { verbose = v; }
    void SetMaxInstructions(int max) { maxInstr = max; }
    int GetInstructionCount() const { return instrCount; }
    uint16_t GetDetectedBoneIndex() const { return detectedBoneIndex; }
    bool GetBoneIndexOverridden() const { return boneIndexOverridden; }
    bool GetDetectedA3() const { return detectedA3; }
 
    void PrintStats() {
        printf("[BoneEmu] probes=%d fast=%d full=%d\n",
            statProbes, statFastEmulations, statFullEmulations);
        statProbes = statFastEmulations = statFullEmulations = 0;
    }
 
    bool IsGameAddr(uint64_t addr) { return addr >= moduleBase && addr < moduleEnd; }
 
    bool IsValidWorldPosition(const M128& val) {
        float x = val.f32[0], y = val.f32[1], z = val.f32[2];
        if (isnan(x) || isnan(y) || isnan(z)) return false;
        if (isinf(x) || isinf(y) || isinf(z)) return false;
        if (x == 0.0f && y == 0.0f && z == 0.0f) return false;
        if (fabsf(x) > 50000.0f || fabsf(y) > 50000.0f) return false;
        if (z < -1000.0f || z > 5000.0f) return false;
        return true;
    }
 
    static std::vector<uint64_t> FindPattern(uint64_t start, size_t size,
        const uint8_t* pattern, const char* mask) {
        std::vector<uint64_t> results;
        size_t len = strlen(mask);
        for (size_t i = 0; i < size - len; i++) {
            bool found = true;
            for (size_t j = 0; j < len; j++) {
                if (mask[j] != '?' && pattern[j] != *(uint8_t*)(start + i + j)) {
                    found = false; break;
                }
            }
            if (found) results.push_back(start + i);
        }
        return results;
    }
 
    bool FindTargetFunction() {
        if (targetFunc != 0) return true;
        if (moduleBase == 0) return false;

        // External port: every dereference of the target process's address
        // space goes through the driver. PE walk + section table read first,
        // then a per-instruction Zydis decode through the page cache (so
        // sequential RIPs are essentially memcpys out of the cache).
        IMAGE_DOS_HEADER dos = {};
        if (!BoneEmu_SafeRead(moduleBase, &dos, sizeof(dos))) return false;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS nt = {};
        if (!BoneEmu_SafeRead(moduleBase + dos.e_lfanew, &nt, sizeof(nt))) return false;
        if (nt.Signature != IMAGE_NT_SIGNATURE) return false;

        uint64_t secAddr = moduleBase + dos.e_lfanew + sizeof(IMAGE_NT_HEADERS);
        uint64_t textBase = 0; uint64_t textSize = 0;
        for (int i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sh = {};
            if (!BoneEmu_SafeRead(secAddr + i * sizeof(IMAGE_SECTION_HEADER), &sh, sizeof(sh))) continue;
            if (memcmp(sh.Name, ".text", 5) == 0) {
                textBase = moduleBase + sh.VirtualAddress;
                textSize = sh.Misc.VirtualSize; break;
            }
        }
        if (!textBase) return false;

        ZydisDecoder dec;
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        auto DecodeOne = [&](uint64_t rip, ZydisDecodedInstruction& ins,
                             ZydisDecodedOperand* ops) -> bool {
            uint8_t buf[16];
            if (!BoneEmu_SafeRead(rip, buf, sizeof(buf))) return false;
            return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, buf, sizeof(buf), &ins, ops));
        };

        for (uint64_t off = 0; off + 15 < textSize; ) {
            uint64_t rip0 = textBase + off;
            ZydisDecodedInstruction ins;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!DecodeOne(rip0, ins, ops)) { off++; continue; }

            // Step 1: MOV r32, [reg + 0xA4]
            if (ins.mnemonic != ZYDIS_MNEMONIC_MOV ||
                ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                ops[0].size != 32 ||
                ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
                !ops[1].mem.disp.has_displacement ||
                ops[1].mem.disp.value != 0xA4)
            {
                off += ins.length; continue;
            }

            // Step 2: within 15 insns find CMP r32, 2 then JNZ
            uint64_t p = rip0 + ins.length;
            bool foundCmp = false;
            for (int s = 0; s < 15 && p + 15 < textBase + textSize; s++) {
                ZydisDecodedInstruction i2;
                ZydisDecodedOperand o2[ZYDIS_MAX_OPERAND_COUNT];
                if (!DecodeOne(p, i2, o2)) break;

                if (i2.mnemonic == ZYDIS_MNEMONIC_CMP &&
                    o2[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    o2[0].size == 32 &&
                    o2[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                    o2[1].imm.value.u == 2)
                {
                    foundCmp = true;
                    p += i2.length;
                    break;
                }
                if (i2.mnemonic == ZYDIS_MNEMONIC_RET) break;
                p += i2.length;
            }
            if (!foundCmp) { off += ins.length; continue; }

            // Step 3: next insn should be JNZ
            {
                ZydisDecodedInstruction i3;
                ZydisDecodedOperand o3[ZYDIS_MAX_OPERAND_COUNT];
                if (!DecodeOne(p, i3, o3) || i3.mnemonic != ZYDIS_MNEMONIC_JNZ) {
                    off += ins.length; continue;
                }
                p += i3.length;
            }

            // Step 4: within 10 insns find MOV r8b/r8d, 1
            bool foundMov = false;
            for (int s = 0; s < 10 && p + 15 < textBase + textSize; s++) {
                ZydisDecodedInstruction i4;
                ZydisDecodedOperand o4[ZYDIS_MAX_OPERAND_COUNT];
                if (!DecodeOne(p, i4, o4)) break;

                if (i4.mnemonic == ZYDIS_MNEMONIC_MOV &&
                    o4[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    (o4[0].reg.value == ZYDIS_REGISTER_R8B || o4[0].reg.value == ZYDIS_REGISTER_R8D) &&
                    o4[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                    o4[1].imm.value.u == 1)
                {
                    foundMov = true;
                    p += i4.length;
                    break;
                }
                if (i4.mnemonic == ZYDIS_MNEMONIC_RET) break;
                p += i4.length;
            }
            if (!foundMov) { off += ins.length; continue; }

            // Step 5: next CALL within 3 insns = target
            for (int s = 0; s < 3 && p + 15 < textBase + textSize; s++) {
                ZydisDecodedInstruction i5;
                ZydisDecodedOperand o5[ZYDIS_MAX_OPERAND_COUNT];
                if (!DecodeOne(p, i5, o5)) break;

                if (i5.mnemonic == ZYDIS_MNEMONIC_CALL &&
                    o5[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                {
                    uint64_t next = p + i5.length;
                    targetFunc = next + (int64_t)o5[0].imm.value.s;
                    printf("[FindTarget] found @ RVA 0x%llX (call site RVA 0x%llX)\n",
                        targetFunc - moduleBase, p - moduleBase);
                    return true;
                }
                p += i5.length;
            }

            off += ins.length;
        }

        printf("[FindTarget] FAILED\n");
        return false;
    }
 
    void SetTargetFunction(uint64_t addr) { targetFunc = addr; }
 
    // ========================================================================
    // SNAPSHOT SAVE/RESTORE
    // ========================================================================
private:
    void TakeSnapshot(BoneSnapshot& snap) {
        snap.rax = cpu.rax; snap.rbx = cpu.rbx; snap.rcx = cpu.rcx; snap.rdx = cpu.rdx;
        snap.rsi = cpu.rsi; snap.rdi = cpu.rdi; snap.rbp = cpu.rbp; snap.rsp = cpu.rsp;
        snap.r8 = cpu.r8;   snap.r9 = cpu.r9;   snap.r10 = cpu.r10; snap.r11 = cpu.r11;
        snap.r12 = cpu.r12; snap.r13 = cpu.r13; snap.r14 = cpu.r14; snap.r15 = cpu.r15;
        snap.rip = cpu.rip; snap.rflags = cpu.rflags;
        memcpy(snap.xmm, cpu.xmm, sizeof(cpu.xmm));
 
        // Save 8KB of stack centered on RSP
        uint64_t stackStart = (uint64_t)cpu.stack;
        uint64_t stackEnd = stackStart + sizeof(cpu.stack);
        uint64_t saveBase = cpu.rsp - 0x800; // 2KB below RSP
        if (saveBase < stackStart) saveBase = stackStart;
        if (saveBase + 0x2000 > stackEnd) saveBase = stackEnd - 0x2000;
        snap.stackSaveBase = saveBase;
        memcpy(snap.stackData, (void*)saveBase, 0x2000);
 
        snap.callDepth = callDepth;
        snap.outputAddr = outputAddr;
        snap.mainReturnAddr = mainReturnAddr;
        snap.valid = true;
    }
 
    void LoadSnapshot(const BoneSnapshot& snap) {
        cpu.rax = snap.rax; cpu.rbx = snap.rbx; cpu.rcx = snap.rcx; cpu.rdx = snap.rdx;
        cpu.rsi = snap.rsi; cpu.rdi = snap.rdi; cpu.rbp = snap.rbp; cpu.rsp = snap.rsp;
        cpu.r8 = snap.r8;   cpu.r9 = snap.r9;   cpu.r10 = snap.r10; cpu.r11 = snap.r11;
        cpu.r12 = snap.r12; cpu.r13 = snap.r13; cpu.r14 = snap.r14; cpu.r15 = snap.r15;
        cpu.rip = snap.rip; cpu.rflags = snap.rflags;
        memcpy(cpu.xmm, snap.xmm, sizeof(cpu.xmm));
 
        // Restore stack region
        memcpy((void*)snap.stackSaveBase, snap.stackData, 0x2000);
 
        callDepth = snap.callDepth;
        outputAddr = snap.outputAddr;
        mainReturnAddr = snap.mainReturnAddr;
    }
 
    // ========================================================================
    // PROBE HELPERS
    // ========================================================================
    PawnProbe* FindProbe(uint64_t pawn) {
        for (int i = 0; i < MAX_PROBES; i++)
            if (probes[i].pawnAddr == pawn && probes[i].probed)
                return &probes[i];
        return nullptr;
    }
 
    PawnProbe* AllocProbe(uint64_t pawn) {
        // Reuse existing slot for this pawn
        for (int i = 0; i < MAX_PROBES; i++)
            if (probes[i].pawnAddr == pawn)
                return &probes[i];
        // Find free or oldest
        int oldest = 0; DWORD oldestTime = UINT_MAX;
        for (int i = 0; i < MAX_PROBES; i++) {
            if (!probes[i].probed) { oldest = i; break; }
            if (probes[i].probeTime < oldestTime) {
                oldestTime = probes[i].probeTime; oldest = i;
            }
        }
        probes[oldest].probed = false;
        probes[oldest].snapshot.valid = false;
        probes[oldest].pawnAddr = pawn;
        probes[oldest].failCount = 0;
        return &probes[oldest];
    }
 
    // ========================================================================
    // MEMORY
    // ========================================================================

    // Per-solve shadow memory. The emulated function writes intermediate
    // scratch values to game-memory scratch (RCX-passed buffers, sometimes
    // local globals via RIP-relative MOVs). In internal mode those writes
    // hit the real game memory and the same function reads them back a few
    // instructions later. In external mode we can't (and shouldn't) touch
    // the target process; instead writes land in this shadow, and reads
    // overlay shadow bytes on top of the driver-mediated game read so the
    // function observes a consistent view of what it just wrote.
    //
    // Cleared at the start of every GetBonePosition call. Page-keyed with
    // a per-byte bitmap so we know which bytes are actually shadowed.
private:
    struct ShadowPage {
        uint8_t data[0x1000];
        uint8_t mask[0x1000 / 8]; // bit i set => byte i is shadowed
    };
    std::unordered_map<uint64_t, ShadowPage> m_shadow;

    void ShadowClear() { m_shadow.clear(); }

    bool ShadowRead(uint64_t addr, void* buf, size_t size) {
        // Pull from game first (or zero on driver failure), then overlay
        // any shadowed bytes that intersect the request.
        if (!BoneEmu_SafeRead(addr, buf, size)) memset(buf, 0, size);
        uint8_t* out = (uint8_t*)buf;
        uint64_t cur = addr;
        size_t   rem = size;
        size_t   ofs = 0;
        while (rem) {
            uint64_t pageBase = cur & ~uint64_t(0xFFF);
            size_t   pageOff  = (size_t)(cur - pageBase);
            size_t   chunk    = 0x1000 - pageOff;
            if (chunk > rem) chunk = rem;
            auto it = m_shadow.find(pageBase);
            if (it != m_shadow.end()) {
                const ShadowPage& p = it->second;
                for (size_t i = 0; i < chunk; i++) {
                    size_t b = pageOff + i;
                    if (p.mask[b >> 3] & (1u << (b & 7))) out[ofs + i] = p.data[b];
                }
            }
            ofs += chunk; cur += chunk; rem -= chunk;
        }
        return true;
    }

    bool ShadowWrite(uint64_t addr, const void* buf, size_t size) {
        const uint8_t* src = (const uint8_t*)buf;
        uint64_t cur = addr;
        size_t   rem = size;
        size_t   ofs = 0;
        while (rem) {
            uint64_t pageBase = cur & ~uint64_t(0xFFF);
            size_t   pageOff  = (size_t)(cur - pageBase);
            size_t   chunk    = 0x1000 - pageOff;
            if (chunk > rem) chunk = rem;
            ShadowPage& p = m_shadow[pageBase]; // value-initialises on insert
            for (size_t i = 0; i < chunk; i++) {
                size_t b = pageOff + i;
                p.data[b] = src[ofs + i];
                p.mask[b >> 3] |= (uint8_t)(1u << (b & 7));
            }
            ofs += chunk; cur += chunk; rem -= chunk;
        }
        return true;
    }

public:
    bool ReadMem(uint64_t addr, void* buf, size_t size) {
        if (cpu.IsStackAddr(addr)) {
            memcpy(buf, (void*)addr, size);
            return true;
        }
        if (outputAddr && addr >= outputAddr && addr < outputAddr + 16) {
            memcpy(buf, outputBone.u8 + (addr - outputAddr), size);
            return true;
        }
        return ShadowRead(addr, buf, size);
    }
 
    // FIX: track output writes INSIDE stack handler (outputAddr is on the stack!)
    bool WriteMem(uint64_t addr, const void* buf, size_t size) {
        if (cpu.IsStackAddr(addr)) {
            memcpy((void*)addr, buf, size);
 
            if (outputAddr && addr == outputAddr && size >= 12) {
                memcpy(outputBone.u8, buf, size > 16 ? 16 : size);
 
                float x = outputBone.f32[0];
                float y = outputBone.f32[1];
                float z = outputBone.f32[2];
 
                // 🔥 LOOSE but SAFE validation
                if (!isfinite(x) || !isfinite(y) || !isfinite(z))
                    return true;
 
                if (fabsf(x) > 50000.f || fabsf(y) > 50000.f)
                    return true;
 
                if (z < -2000.f || z > 8000.f)
                    return true;
 
                float score = fabsf(x) + fabsf(y) + fabsf(z);
                if (score < 5.f)   // 🔥 WAR 30 → viel zu hart
                    return true;
 
                // ✅ valid world-space candidate
                lastValidPosition = outputBone;
                foundValidPosition = true;
            }
            return true;
        }

        return ShadowWrite(addr, buf, size);
    }
 
 
    // ========================================================================
    // HELPERS (unchanged from your original)
    // ========================================================================
    uint64_t CalcEffAddr(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand& op, uint64_t rip) {
        uint64_t addr = 0;
        if (op.mem.base == ZYDIS_REGISTER_RIP) addr = rip + ins.length;
        else if (op.mem.base != ZYDIS_REGISTER_NONE) { uint64_t* reg = cpu.GetGPR(op.mem.base); if (reg) addr = *reg; }
        if (op.mem.index != ZYDIS_REGISTER_NONE) { uint64_t* reg = cpu.GetGPR(op.mem.index); if (reg) addr += (*reg) * op.mem.scale; }
        if (op.mem.disp.has_displacement) addr += op.mem.disp.value;
        return addr;
    }
 
    uint64_t GetOpVal(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand& op, uint64_t rip) {
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint64_t* reg = cpu.GetGPR(op.reg.value);
            if (!reg) return 0;
            uint64_t val = *reg;
            if (cpu.IsHighReg(op.reg.value)) return (val >> 8) & 0xFF;
            if (op.size == 32) return val & 0xFFFFFFFF;
            if (op.size == 16) return val & 0xFFFF;
            if (op.size == 8) return val & 0xFF;
            return val;
        }
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) return op.imm.value.u;
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint64_t addr = CalcEffAddr(ins, op, rip);
            uint64_t val = 0;
            if (op.size == 64) ReadMem(addr, &val, 8);
            else if (op.size == 32) { uint32_t v; ReadMem(addr, &v, 4); val = v; }
            else if (op.size == 16) { uint16_t v; ReadMem(addr, &v, 2); val = v; }
            else if (op.size == 8) { uint8_t v; ReadMem(addr, &v, 1); val = v; }
            return val;
        }
        return 0;
    }
 
    void SetOpVal(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand& op, uint64_t val, uint64_t rip) {
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint64_t* reg = cpu.GetGPR(op.reg.value);
            if (!reg) return;
            if (cpu.IsHighReg(op.reg.value)) *reg = (*reg & 0xFFFFFFFFFFFF00FF) | ((val & 0xFF) << 8);
            else if (op.size == 32) *reg = val & 0xFFFFFFFF;
            else if (op.size == 16) *reg = (*reg & 0xFFFFFFFFFFFF0000) | (val & 0xFFFF);
            else if (op.size == 8) *reg = (*reg & 0xFFFFFFFFFFFFFF00) | (val & 0xFF);
            else *reg = val;
        }
        else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint64_t addr = CalcEffAddr(ins, op, rip);
            if (op.size == 64) WriteMem(addr, &val, 8);
            else if (op.size == 32) { uint32_t v = (uint32_t)val; WriteMem(addr, &v, 4); }
            else if (op.size == 16) { uint16_t v = (uint16_t)val; WriteMem(addr, &v, 2); }
            else if (op.size == 8) { uint8_t v = (uint8_t)val; WriteMem(addr, &v, 1); }
        }
    }
 
    M128 GetXmmOp(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand& op, uint64_t rip) {
        M128 result = {};
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            M128* x = cpu.GetXMM(op.reg.value);
            if (x) return *x;
        }
        else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            ReadMem(CalcEffAddr(ins, op, rip), &result, 16);
        }
        return result;
    }
 
    void SetXmmOp(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand& op, M128 val, uint64_t rip) {
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            M128* x = cpu.GetXMM(op.reg.value);
            if (x) {
                *x = val;
                if (IsValidWorldPosition(val)) { foundValidPosition = true; lastValidPosition = val; }
            }
        }
        else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint64_t addr = CalcEffAddr(ins, op, rip);
            WriteMem(addr, &val, 16);
            if (IsValidWorldPosition(val)) { foundValidPosition = true; lastValidPosition = val; }
        }
    }
 
    bool GetZF() { return (cpu.rflags & 0x40) != 0; }
    bool GetCF() { return (cpu.rflags & 0x01) != 0; }
    bool GetSF() { return (cpu.rflags & 0x80) != 0; }
    bool GetOF() { return (cpu.rflags & 0x800) != 0; }
    bool GetPF() { return (cpu.rflags & 0x04) != 0; }
 
    void SetZF(bool v) { if (v) cpu.rflags |= 0x40; else cpu.rflags &= ~0x40; }
    void SetCF(bool v) { if (v) cpu.rflags |= 0x01; else cpu.rflags &= ~0x01; }
    void SetSF(bool v) { if (v) cpu.rflags |= 0x80; else cpu.rflags &= ~0x80; }
    void SetOF(bool v) { if (v) cpu.rflags |= 0x800; else cpu.rflags &= ~0x800; }
 
    enum ExecResult { CONTINUE, STOP, JUMP_TAKEN };
 
    // Widened from the seller's `multiplier == 0x20` original to also catch
    // 0x30 / 0x40 / 0x60 / 0x80 bone-struct sizes. R6's pose array may use
    // any of those depending on the operator/skeleton; the seller's number
    // was specific to one game build.
    bool IsBoneIndexMul(uint64_t indexVal, uint64_t multiplier) {
        const bool sizeOk = (multiplier == 0x20 || multiplier == 0x30
                          || multiplier == 0x40 || multiplier == 0x60
                          || multiplier == 0x80);
        return sizeOk && (indexVal & 0xFFFF) < 0x1000 && indexVal != 0;
    }
 
#define XMM(idx) cpu.xmm[idx].v
    inline int XmmIndex(ZydisRegister r) { return (int)r - ZYDIS_REGISTER_XMM0; }
#define XMM_REG_REG (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
 
    // ========================================================================
    // EXECUTE - with snapshot capture at bone index detection
    // ========================================================================
    ExecResult Execute(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops, uint64_t rip) {
        uint64_t a, b, res;
        M128 xa, xb, xr;
 
        switch (ins.mnemonic) {
        case ZYDIS_MNEMONIC_MOV: SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_MOVZX: SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD:
            a = GetOpVal(ins, ops[1], rip);
            if (ops[1].size == 8) a = (int64_t)(int8_t)a;
            else if (ops[1].size == 16) a = (int64_t)(int16_t)a;
            else if (ops[1].size == 32) a = (int64_t)(int32_t)a;
            SetOpVal(ins, ops[0], a, rip); break;
 
        case ZYDIS_MNEMONIC_LEA: SetOpVal(ins, ops[0], CalcEffAddr(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_XCHG:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            SetOpVal(ins, ops[0], b, rip); SetOpVal(ins, ops[1], a, rip); break;
        case ZYDIS_MNEMONIC_PUSH:
            a = GetOpVal(ins, ops[0], rip); cpu.rsp -= 8; WriteMem(cpu.rsp, &a, 8); break;
        case ZYDIS_MNEMONIC_POP:
            ReadMem(cpu.rsp, &a, 8); cpu.rsp += 8; SetOpVal(ins, ops[0], a, rip); break;
 
        case ZYDIS_MNEMONIC_ADD:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip); res = a + b;
            SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(res < a);
            SetSF((res >> (ops[0].size - 1)) & 1); break;
        case ZYDIS_MNEMONIC_SUB:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip); res = a - b;
            SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(b > a);
            SetSF((res >> (ops[0].size - 1)) & 1); break;
        case ZYDIS_MNEMONIC_INC:
            a = GetOpVal(ins, ops[0], rip); SetOpVal(ins, ops[0], a + 1, rip); SetZF(a + 1 == 0); break;
        case ZYDIS_MNEMONIC_DEC:
            a = GetOpVal(ins, ops[0], rip); SetOpVal(ins, ops[0], a - 1, rip); SetZF(a - 1 == 0); break;
        case ZYDIS_MNEMONIC_NEG:
            a = GetOpVal(ins, ops[0], rip); SetOpVal(ins, ops[0], (~a) + 1, rip);
            SetCF(a != 0); SetZF(a == 0); break;
 
        case ZYDIS_MNEMONIC_MUL:
            a = cpu.rax; b = GetOpVal(ins, ops[0], rip);
            if (ops[0].size == 64) { cpu.rax = _umul128(a, b, &cpu.rdx); }
            else if (ops[0].size == 32) { uint64_t r = (a & 0xFFFFFFFF) * (b & 0xFFFFFFFF); cpu.rax = r & 0xFFFFFFFF; cpu.rdx = (r >> 32) & 0xFFFFFFFF; }
            break;
 
        case ZYDIS_MNEMONIC_IMUL:
            if (ins.operand_count == 1) {
                int64_t hi;
                cpu.rax = _mul128((int64_t)cpu.rax, (int64_t)GetOpVal(ins, ops[0], rip), &hi);
                cpu.rdx = (uint64_t)hi;
            }
            else if (ins.operand_count == 2) {
                a = (int64_t)GetOpVal(ins, ops[0], rip);
                b = (int64_t)GetOpVal(ins, ops[1], rip);
 
                if (IsBoneIndexMul(a, (uint64_t)b)) {
                    detectedBoneIndex = (uint16_t)(a & 0xFFFF);
                    a = (int64_t)desiredBoneIndex;
                    boneIndexOverridden = true; dbgIsBoneIndexMulHits++;
                    // PROBE: capture snapshot at this point
                    if (isProbing && probeTarget && !probeTarget->valid)
                        TakeSnapshot(*probeTarget);
                }
                else if (IsBoneIndexMul(b, (uint64_t)a)) {
                    detectedBoneIndex = (uint16_t)(b & 0xFFFF);
                    b = (int64_t)desiredBoneIndex;
                    boneIndexOverridden = true; dbgIsBoneIndexMulHits++;
                    if (isProbing && probeTarget && !probeTarget->valid)
                        TakeSnapshot(*probeTarget);
                }
 
                SetOpVal(ins, ops[0], a * b, rip);
            }
            else {
                a = (int64_t)GetOpVal(ins, ops[1], rip);
                b = (int64_t)GetOpVal(ins, ops[2], rip);
 
                if (IsBoneIndexMul(a, (uint64_t)b)) {
                    detectedBoneIndex = (uint16_t)(a & 0xFFFF);
                    a = (int64_t)desiredBoneIndex;
                    boneIndexOverridden = true; dbgIsBoneIndexMulHits++;
                    if (isProbing && probeTarget && !probeTarget->valid)
                        TakeSnapshot(*probeTarget);
                }
                else if (IsBoneIndexMul(b, (uint64_t)a)) {
                    detectedBoneIndex = (uint16_t)(b & 0xFFFF);
                    b = (int64_t)desiredBoneIndex;
                    boneIndexOverridden = true; dbgIsBoneIndexMulHits++;
                    if (isProbing && probeTarget && !probeTarget->valid)
                        TakeSnapshot(*probeTarget);
                }
 
                SetOpVal(ins, ops[0], a * b, rip);
            }
            break;
 
            // FIX: save dividend BEFORE modifying rax
        case ZYDIS_MNEMONIC_DIV:
            b = GetOpVal(ins, ops[0], rip);
            if (b == 0) return STOP;
            if (ops[0].size == 64) {
                uint64_t dividend = cpu.rax;
                cpu.rax = dividend / b;
                cpu.rdx = dividend % b;
            }
            else if (ops[0].size == 32) {
                uint64_t dividend = ((cpu.rdx & 0xFFFFFFFF) << 32) | (cpu.rax & 0xFFFFFFFF);
                uint32_t divisor = (uint32_t)(b & 0xFFFFFFFF);
                cpu.rax = (uint32_t)(dividend / divisor);
                cpu.rdx = (uint32_t)(dividend % divisor);
            }
            break;
 
        case ZYDIS_MNEMONIC_IDIV:
            b = (int64_t)GetOpVal(ins, ops[0], rip);
            if (b == 0) return STOP;
            if (ops[0].size == 64) {
                int64_t dividend = (int64_t)cpu.rax;
                int64_t divisor = (int64_t)b;
                cpu.rax = (uint64_t)(dividend / divisor);
                cpu.rdx = (uint64_t)(dividend % divisor);
            }
            else if (ops[0].size == 32) {
                int64_t dividend = (int64_t)(((uint64_t)(cpu.rdx & 0xFFFFFFFF) << 32) | (cpu.rax & 0xFFFFFFFF));
                int32_t divisor = (int32_t)(b & 0xFFFFFFFF);
                cpu.rax = (uint32_t)((int32_t)(dividend / divisor));
                cpu.rdx = (uint32_t)((int32_t)(dividend % divisor));
            }
            break;
 
        case ZYDIS_MNEMONIC_ADC:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            res = a + b + (GetCF() ? 1 : 0);
            SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(res < a); break;
        case ZYDIS_MNEMONIC_SBB:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            {
                uint64_t cf = GetCF() ? 1 : 0; res = a - b - cf;
                SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(b + cf > a);
            }
            break;
 
        case ZYDIS_MNEMONIC_AND:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip); res = a & b;
            SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(false); SetOF(false); break;
        case ZYDIS_MNEMONIC_OR:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip); res = a | b;
            SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(false); SetOF(false); break;
        case ZYDIS_MNEMONIC_XOR:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip); res = a ^ b;
            SetOpVal(ins, ops[0], res, rip); SetZF(res == 0); SetCF(false); SetOF(false); break;
        case ZYDIS_MNEMONIC_NOT:
            SetOpVal(ins, ops[0], ~GetOpVal(ins, ops[0], rip), rip); break;
 
        case ZYDIS_MNEMONIC_SHL:
            a = GetOpVal(ins, ops[0], rip);
            b = GetOpVal(ins, ops[1], rip) & 0x3F;
            if (b == 5 && a != 0 && (a & 0xFFFF) < 0x1000) {
                detectedBoneIndex = (uint16_t)(a & 0xFFFF);
                a = (uint64_t)desiredBoneIndex;
                boneIndexOverridden = true;
                // PROBE: capture snapshot at SHL bone index too
                if (isProbing && probeTarget && !probeTarget->valid)
                    TakeSnapshot(*probeTarget);
            }
            SetOpVal(ins, ops[0], a << b, rip); break;
        case ZYDIS_MNEMONIC_SHR:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip) & 0x3F;
            SetOpVal(ins, ops[0], a >> b, rip); break;
        case ZYDIS_MNEMONIC_SAR:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip) & 0x3F;
            SetOpVal(ins, ops[0], (int64_t)a >> b, rip); break;
 
        case ZYDIS_MNEMONIC_ROL: {
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            int bits = ops[0].size; b &= (bits - 1);
            if (bits == 64) res = (a << b) | (a >> (64 - b));
            else if (bits == 32) res = ((a & 0xFFFFFFFF) << b) | ((a & 0xFFFFFFFF) >> (32 - b));
            else if (bits == 16) res = ((a & 0xFFFF) << b) | ((a & 0xFFFF) >> (16 - b));
            else res = ((a & 0xFF) << b) | ((a & 0xFF) >> (8 - b));
            SetOpVal(ins, ops[0], res, rip); break;
        }
 
        case ZYDIS_MNEMONIC_ROR: {
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            int bits = ops[0].size; b &= (bits - 1);
            if (bits == 64) res = (a >> b) | (a << (64 - b));
            else if (bits == 32) res = ((a & 0xFFFFFFFF) >> b) | ((a & 0xFFFFFFFF) << (32 - b));
            else if (bits == 16) res = ((a & 0xFFFF) >> b) | ((a & 0xFFFF) << (16 - b));
            else res = ((a & 0xFF) >> b) | ((a & 0xFF) << (8 - b));
            SetOpVal(ins, ops[0], res, rip); break;
        }
 
        case ZYDIS_MNEMONIC_SHLD: {
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            uint64_t cnt = GetOpVal(ins, ops[2], rip) & 0x3F;
            if (ops[0].size == 64) res = (a << cnt) | (b >> (64 - cnt));
            else res = ((a << cnt) | (b >> (32 - cnt))) & 0xFFFFFFFF;
            SetOpVal(ins, ops[0], res, rip); break;
        }
        case ZYDIS_MNEMONIC_SHRD: {
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            uint64_t cnt = GetOpVal(ins, ops[2], rip) & 0x3F;
            if (ops[0].size == 64) res = (a >> cnt) | (b << (64 - cnt));
            else res = ((a >> cnt) | (b << (32 - cnt))) & 0xFFFFFFFF;
            SetOpVal(ins, ops[0], res, rip); break;
        }
 
        case ZYDIS_MNEMONIC_BT:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip) & 0x3F; SetCF((a >> b) & 1); break;
        case ZYDIS_MNEMONIC_BTS:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip) & 0x3F;
            SetCF((a >> b) & 1); SetOpVal(ins, ops[0], a | (1ULL << b), rip); break;
        case ZYDIS_MNEMONIC_BTR:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip) & 0x3F;
            SetCF((a >> b) & 1); SetOpVal(ins, ops[0], a & ~(1ULL << b), rip); break;
        case ZYDIS_MNEMONIC_BSF:
            a = GetOpVal(ins, ops[1], rip);
            if (a == 0) SetZF(true);
            else { unsigned long idx; _BitScanForward64(&idx, a); SetOpVal(ins, ops[0], idx, rip); SetZF(false); }
            break;
        case ZYDIS_MNEMONIC_BSR:
            a = GetOpVal(ins, ops[1], rip);
            if (a == 0) SetZF(true);
            else { unsigned long idx; _BitScanReverse64(&idx, a); SetOpVal(ins, ops[0], idx, rip); SetZF(false); }
            break;
        case ZYDIS_MNEMONIC_LZCNT: SetOpVal(ins, ops[0], __lzcnt64(GetOpVal(ins, ops[1], rip)), rip); break;
        case ZYDIS_MNEMONIC_TZCNT: SetOpVal(ins, ops[0], _tzcnt_u64(GetOpVal(ins, ops[1], rip)), rip); break;
        case ZYDIS_MNEMONIC_POPCNT: SetOpVal(ins, ops[0], __popcnt64(GetOpVal(ins, ops[1], rip)), rip); break;
        case ZYDIS_MNEMONIC_BSWAP:
            a = GetOpVal(ins, ops[0], rip);
            if (ops[0].size == 64) a = _byteswap_uint64(a); else a = _byteswap_ulong((uint32_t)a);
            SetOpVal(ins, ops[0], a, rip); break;
 
        case ZYDIS_MNEMONIC_CMP:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip); res = a - b;
            SetZF(res == 0); SetCF(b > a); SetSF((res >> (ops[0].size - 1)) & 1);
            SetOF(((a ^ b) & (a ^ res)) >> (ops[0].size - 1)); break;
        case ZYDIS_MNEMONIC_TEST:
            a = GetOpVal(ins, ops[0], rip); b = GetOpVal(ins, ops[1], rip);
            SetZF((a & b) == 0); SetCF(false); SetOF(false);
            SetSF(((a & b) >> (ops[0].size - 1)) & 1); break;
 
        case ZYDIS_MNEMONIC_CMOVZ:  if (GetZF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVNZ: if (!GetZF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVB:  if (GetCF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVNB: if (!GetCF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVBE: if (GetCF() || GetZF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVNBE: if (!GetCF() && !GetZF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVS:  if (GetSF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVNS: if (!GetSF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVL:  if (GetSF() != GetOF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVNL: if (GetSF() == GetOF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVLE: if (GetZF() || GetSF() != GetOF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
        case ZYDIS_MNEMONIC_CMOVNLE: if (!GetZF() && GetSF() == GetOF()) SetOpVal(ins, ops[0], GetOpVal(ins, ops[1], rip), rip); break;
 
        case ZYDIS_MNEMONIC_SETZ:   SetOpVal(ins, ops[0], GetZF() ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETNZ:  SetOpVal(ins, ops[0], !GetZF() ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETB:   SetOpVal(ins, ops[0], GetCF() ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETNB:  SetOpVal(ins, ops[0], !GetCF() ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETBE:  SetOpVal(ins, ops[0], (GetCF() || GetZF()) ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETNBE: SetOpVal(ins, ops[0], (!GetCF() && !GetZF()) ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETS:   SetOpVal(ins, ops[0], GetSF() ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETNS:  SetOpVal(ins, ops[0], !GetSF() ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETL:   SetOpVal(ins, ops[0], (GetSF() != GetOF()) ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETNL:  SetOpVal(ins, ops[0], (GetSF() == GetOF()) ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETLE:  SetOpVal(ins, ops[0], (GetZF() || GetSF() != GetOF()) ? 1 : 0, rip); break;
        case ZYDIS_MNEMONIC_SETNLE: SetOpVal(ins, ops[0], (!GetZF() && GetSF() == GetOF()) ? 1 : 0, rip); break;
 
        case ZYDIS_MNEMONIC_CDQ:  cpu.rdx = ((int32_t)cpu.rax < 0) ? 0xFFFFFFFF : 0; break;
        case ZYDIS_MNEMONIC_CQO:  cpu.rdx = ((int64_t)cpu.rax < 0) ? 0xFFFFFFFFFFFFFFFF : 0; break;
        case ZYDIS_MNEMONIC_CDQE: cpu.rax = (int64_t)(int32_t)cpu.rax; break;
        case ZYDIS_MNEMONIC_CBW:  cpu.rax = (cpu.rax & 0xFFFFFFFFFFFF0000) | (uint16_t)(int16_t)(int8_t)cpu.rax; break;
        case ZYDIS_MNEMONIC_CWDE: cpu.rax = (uint32_t)(int32_t)(int16_t)cpu.rax; break;
        case ZYDIS_MNEMONIC_CLC: SetCF(false); break;
        case ZYDIS_MNEMONIC_STC: SetCF(true); break;
        case ZYDIS_MNEMONIC_CMC: SetCF(!GetCF()); break;
        case ZYDIS_MNEMONIC_LEAVE:
            cpu.rsp = cpu.rbp;
            { uint64_t tmp; ReadMem(cpu.rsp, &tmp, 8); cpu.rbp = tmp; }
            cpu.rsp += 8; break;
 
            // ==== SSE (unchanged) ====
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS:
        case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU:
            if (XMM_REG_REG) XMM(XmmIndex(ops[0].reg.value)) = XMM(XmmIndex(ops[1].reg.value));
            else SetXmmOp(ins, ops[0], GetXmmOp(ins, ops[1], rip), rip); break;
 
        case ZYDIS_MNEMONIC_MOVSS:
            if (XMM_REG_REG) XMM(XmmIndex(ops[0].reg.value)).m128_f32[0] = XMM(XmmIndex(ops[1].reg.value)).m128_f32[0];
            else SetXmmOp(ins, ops[0], GetXmmOp(ins, ops[1], rip), rip); break;
 
        case ZYDIS_MNEMONIC_MOVSD:
            if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                M128* dst = cpu.GetXMM(ops[0].reg.value); M128* src = cpu.GetXMM(ops[1].reg.value);
                if (dst && src) dst->u64[0] = src->u64[0];
            }
            else if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                double val; ReadMem(CalcEffAddr(ins, ops[1], rip), &val, 8);
                M128* dst = cpu.GetXMM(ops[0].reg.value);
                if (dst) { dst->v = _mm_setzero_ps(); dst->f64[0] = val; }
            }
            else {
                M128* src = cpu.GetXMM(ops[1].reg.value);
                if (src) WriteMem(CalcEffAddr(ins, ops[0], rip), &src->u64[0], 8);
            } break;
 
        case ZYDIS_MNEMONIC_MOVD: case ZYDIS_MNEMONIC_MOVQ:
            if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && cpu.GetXMM(ops[0].reg.value)) {
                M128* dst = cpu.GetXMM(ops[0].reg.value);
                uint64_t val = GetOpVal(ins, ops[1], rip);
                dst->v = _mm_setzero_ps();
                if (ins.mnemonic == ZYDIS_MNEMONIC_MOVD) dst->u32[0] = (uint32_t)val; else dst->u64[0] = val;
            }
            else if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER && cpu.GetXMM(ops[1].reg.value)) {
                M128* src = cpu.GetXMM(ops[1].reg.value);
                if (ins.mnemonic == ZYDIS_MNEMONIC_MOVD) SetOpVal(ins, ops[0], src->u32[0], rip);
                else SetOpVal(ins, ops[0], src->u64[0], rip);
            } break;
 
        case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVHPS: {
            bool low = ins.mnemonic == ZYDIS_MNEMONIC_MOVLPS;
            if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                uint64_t val; ReadMem(CalcEffAddr(ins, ops[1], rip), &val, 8);
                M128* dst = cpu.GetXMM(ops[0].reg.value);
                if (dst) { if (low) dst->u64[0] = val; else dst->u64[1] = val; }
            }
            else {
                M128* src = cpu.GetXMM(ops[1].reg.value);
                if (src) { uint64_t val = low ? src->u64[0] : src->u64[1]; WriteMem(CalcEffAddr(ins, ops[0], rip), &val, 8); }
            } break;
        }
 
        case ZYDIS_MNEMONIC_ADDPS:
            if (XMM_REG_REG) XMM(XmmIndex(ops[0].reg.value)) = _mm_add_ps(XMM(XmmIndex(ops[0].reg.value)), XMM(XmmIndex(ops[1].reg.value)));
            else { xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_add_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); } break;
        case ZYDIS_MNEMONIC_ADDSS:
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            xr.v = _mm_add_ss(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_SUBPS:
            if (XMM_REG_REG) XMM(XmmIndex(ops[0].reg.value)) = _mm_sub_ps(XMM(XmmIndex(ops[0].reg.value)), XMM(XmmIndex(ops[1].reg.value)));
            else { xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_sub_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); } break;
        case ZYDIS_MNEMONIC_SUBSS:
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            xr.v = _mm_sub_ss(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_MULPS:
            if (XMM_REG_REG) XMM(XmmIndex(ops[0].reg.value)) = _mm_mul_ps(XMM(XmmIndex(ops[0].reg.value)), XMM(XmmIndex(ops[1].reg.value)));
            else { xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_mul_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); } break;
        case ZYDIS_MNEMONIC_MULSS:
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            xr.v = _mm_mul_ss(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_DIVPS:
            if (XMM_REG_REG) XMM(XmmIndex(ops[0].reg.value)) = _mm_div_ps(XMM(XmmIndex(ops[0].reg.value)), XMM(XmmIndex(ops[1].reg.value)));
            else { xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_div_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); } break;
        case ZYDIS_MNEMONIC_DIVSS:
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            xr.v = _mm_div_ss(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_SQRTPS: xa = GetXmmOp(ins, ops[1], rip); xr.v = _mm_sqrt_ps(xa.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_SQRTSS: xa = GetXmmOp(ins, ops[1], rip); xr.v = _mm_sqrt_ss(xa.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_RSQRTPS: xa = GetXmmOp(ins, ops[1], rip); xr.v = _mm_rsqrt_ps(xa.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_RSQRTSS: xa = GetXmmOp(ins, ops[1], rip); xr.v = _mm_rsqrt_ss(xa.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_RCPPS: xa = GetXmmOp(ins, ops[1], rip); xr.v = _mm_rcp_ps(xa.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_MINPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_min_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_MAXPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_max_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_ANDPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_and_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_ANDNPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_andnot_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_ORPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_or_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_XORPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_xor_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
 
        case ZYDIS_MNEMONIC_SHUFPS: {
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            uint8_t imm = (uint8_t)ops[2].imm.value.u;
            xr.f32[0] = xa.f32[(imm >> 0) & 3]; xr.f32[1] = xa.f32[(imm >> 2) & 3];
            xr.f32[2] = xb.f32[(imm >> 4) & 3]; xr.f32[3] = xb.f32[(imm >> 6) & 3];
            SetXmmOp(ins, ops[0], xr, rip); break;
        }
 
        case ZYDIS_MNEMONIC_UNPCKLPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_unpacklo_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_UNPCKHPS: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.v = _mm_unpackhi_ps(xa.v, xb.v); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_UNPCKLPD: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.vd = _mm_unpacklo_pd(xa.vd, xb.vd); SetXmmOp(ins, ops[0], xr, rip); break;
        case ZYDIS_MNEMONIC_UNPCKHPD: xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip); xr.vd = _mm_unpackhi_pd(xa.vd, xb.vd); SetXmmOp(ins, ops[0], xr, rip); break;
 
        case ZYDIS_MNEMONIC_CMPPS: {
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            uint8_t imm = (uint8_t)ops[2].imm.value.u;
            switch (imm & 7) {
            case 0: xr.v = _mm_cmpeq_ps(xa.v, xb.v); break; case 1: xr.v = _mm_cmplt_ps(xa.v, xb.v); break;
            case 2: xr.v = _mm_cmple_ps(xa.v, xb.v); break; case 3: xr.v = _mm_cmpunord_ps(xa.v, xb.v); break;
            case 4: xr.v = _mm_cmpneq_ps(xa.v, xb.v); break; case 5: xr.v = _mm_cmpnlt_ps(xa.v, xb.v); break;
            case 6: xr.v = _mm_cmpnle_ps(xa.v, xb.v); break; case 7: xr.v = _mm_cmpord_ps(xa.v, xb.v); break;
            } SetXmmOp(ins, ops[0], xr, rip); break;
        }
 
        case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_COMISS: {
            xa = GetXmmOp(ins, ops[0], rip); xb = GetXmmOp(ins, ops[1], rip);
            float fa = xa.f32[0], fb = xb.f32[0]; cpu.rflags &= ~0x8C5;
            if (isnan(fa) || isnan(fb)) cpu.rflags |= 0x45;
            else if (fa == fb) cpu.rflags |= 0x40;
            else if (fa < fb) cpu.rflags |= 0x01; break;
        }
 
        case ZYDIS_MNEMONIC_CVTSI2SS: { M128* dst = cpu.GetXMM(ops[0].reg.value); if (dst) dst->f32[0] = (float)(int64_t)GetOpVal(ins, ops[1], rip); break; }
        case ZYDIS_MNEMONIC_CVTTSS2SI: xa = GetXmmOp(ins, ops[1], rip); SetOpVal(ins, ops[0], (int64_t)xa.f32[0], rip); break;
        case ZYDIS_MNEMONIC_CVTPS2PD:
            if (XMM_REG_REG) cpu.xmm[XmmIndex(ops[0].reg.value)].vd = _mm_cvtps_pd(cpu.xmm[XmmIndex(ops[1].reg.value)].v);
            else { xa = GetXmmOp(ins, ops[1], rip); xr.vd = _mm_cvtps_pd(xa.v); SetXmmOp(ins, ops[0], xr, rip); } break;
        case ZYDIS_MNEMONIC_CVTPD2PS:
            if (XMM_REG_REG) cpu.xmm[XmmIndex(ops[0].reg.value)].v = _mm_cvtpd_ps(cpu.xmm[XmmIndex(ops[1].reg.value)].vd);
            else { xa = GetXmmOp(ins, ops[1], rip); xr.v = _mm_cvtpd_ps(xa.vd); SetXmmOp(ins, ops[0], xr, rip); } break;
 
            // ==== CONTROL FLOW ====
        case ZYDIS_MNEMONIC_JMP:
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) cpu.rip = rip + ins.length + ops[0].imm.value.s;
            else cpu.rip = GetOpVal(ins, ops[0], rip);
            return JUMP_TAKEN;
 
        case ZYDIS_MNEMONIC_JZ:   if (GetZF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNZ:  if (!GetZF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JB:   if (GetCF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNB:  if (!GetCF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JBE:  if (GetCF() || GetZF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNBE: if (!GetCF() && !GetZF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JS:   if (GetSF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNS:  if (!GetSF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JL:   if (GetSF() != GetOF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNL:  if (GetSF() == GetOF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JLE:  if (GetZF() || GetSF() != GetOF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNLE: if (!GetZF() && GetSF() == GetOF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JP:   if (GetPF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
        case ZYDIS_MNEMONIC_JNP:  if (!GetPF()) { cpu.rip = rip + ins.length + ops[0].imm.value.s; return JUMP_TAKEN; } break;
 
        case ZYDIS_MNEMONIC_CALL: {
            uint64_t target;
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) target = rip + ins.length + ops[0].imm.value.s;
            else target = GetOpVal(ins, ops[0], rip);
            uint64_t retAddr = rip + ins.length;
            if (IsGameAddr(target) && callDepth < maxCallDepth) {
                cpu.rsp -= 8; WriteMem(cpu.rsp, &retAddr, 8);
                cpu.rip = target; callDepth++; return JUMP_TAKEN;
            }
            else { cpu.rax = 0; dbgCallExternalCount++; }
            break;
        }
 
        case ZYDIS_MNEMONIC_RET: {
            uint64_t retAddr;
            ReadMem(cpu.rsp, &retAddr, 8); cpu.rsp += 8;
            if (callDepth == 0 || retAddr == mainReturnAddr) return STOP;
            callDepth--; cpu.rip = retAddr; return JUMP_TAKEN;
        }
 
        case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_ENDBR64: case ZYDIS_MNEMONIC_PAUSE: break;
        case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_UD2: return STOP;
        default:
            // Unhandled mnemonic. RIP advances but no semantic effect was
            // applied — register/XMM state diverges from real execution.
            // We record the first one so the caller can see which AVX/
            // SSE4/BMI variant the seller's switch is missing.
            if (dbgFirstUnhandled == ZYDIS_MNEMONIC_INVALID) {
                dbgFirstUnhandled    = ins.mnemonic;
                dbgFirstUnhandledRip = rip;
            }
            dbgUnhandledCount++;
            break;
        }
 
        cpu.rip = rip + ins.length;
        return CONTINUE;
    }
 
    // ========================================================================
    // RunEmulation (unchanged)
    // ========================================================================
    bool RunEmulation() {
        while (instrCount < maxInstr) {
            uint64_t rip = cpu.rip;
            if (rip < moduleBase || rip >= moduleEnd) {
                dbgLastInstrCount = instrCount;
                dbgLastExitRip    = rip;
                dbgLastExitReason = "RIP_OOB";
                dbgLastRunOk      = false;
                return false;
            }

            uint32_t slot = dcacheSlot(rip);
            DecodedSet& set = m_dcache[slot];
            DecodedInstr* hit = nullptr;

            if (set.way[0].rip == rip) hit = &set.way[0];
            else if (set.way[1].rip == rip) hit = &set.way[1];

            if (!hit) {
                DecodedInstr& victim = set.way[0];
                uint8_t insBuf[16];
                if (!BoneEmu_SafeRead(rip, insBuf, sizeof(insBuf))) {
                    dbgLastInstrCount = instrCount;
                    dbgLastExitRip    = rip;
                    dbgLastExitReason = "READ_FAIL";
                    dbgLastRunOk      = false;
                    return false;
                }
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, insBuf, sizeof(insBuf), &victim.ins, victim.ops))) {
                    dbgLastInstrCount = instrCount;
                    dbgLastExitRip    = rip;
                    dbgLastExitReason = "DECODE_FAIL";
                    dbgLastRunOk      = false;
                    return false;
                }
                victim.rip = rip;
                hit = &victim;
            }

            ExecResult result = Execute(hit->ins, hit->ops, rip);
            instrCount++;
            if (result == STOP) {
                dbgLastInstrCount = instrCount;
                dbgLastExitRip    = rip;
                dbgLastExitReason = "STOP";
                dbgLastRunOk      = true;
                return true;
            }
        }
        dbgLastInstrCount = instrCount;
        dbgLastExitRip    = cpu.rip;
        dbgLastExitReason = "MAX_INSTR";
        dbgLastRunOk      = false;
        return false;
    }
 
    // ========================================================================
    // RESULT EXTRACTION (shared by full & fast path)
    // ========================================================================
private:
    Vector3 ExtractResult() {
        M128 stackResult;
        memcpy(&stackResult, &cpu.stack[0x200], 16);
 
        Vector3 out{};
 
        if (IsValidWorldPosition(stackResult)) {
            out.x = stackResult.f32[0];
            out.y = stackResult.f32[1];
            out.z = stackResult.f32[2];
            return out;
        }
 
        if (foundValidPosition && IsValidWorldPosition(lastValidPosition)) {
            out.x = lastValidPosition.f32[0];
            out.y = lastValidPosition.f32[1];
            out.z = lastValidPosition.f32[2];
            return out;
        }
 
        // ❌ NO XMM FALLBACK (causes foot/head chaos)
        return Vector3{};
    }
 
 
    // ========================================================================
    // FULL EMULATION (used for probe + fallback)
    // ========================================================================
    Vector3 DoFullEmulation(uint64_t pawn, uint16_t boneIndex) {
        cpu.Clear();
        instrCount = 0; callDepth = 0;
        foundValidPosition = false;
        lastValidPosition.v = _mm_setzero_ps();
        detectedBoneIndex = 0xFFFF;
        boneIndexOverridden = false;
        detectedA3 = false;
        bestScore = 0.0f; candidateHits = 0;
        bestCandidate.v = _mm_setzero_ps();
        desiredBoneIndex = boneIndex;
 
        uint64_t resultAddr = (uint64_t) & cpu.stack[0x200];
        memset(&cpu.stack[0x200], 0, 0x100);
        outputAddr = resultAddr;
        outputBone.v = _mm_setzero_ps();
        initialRCX = resultAddr;
 
        cpu.rcx = resultAddr;
        cpu.rdx = pawn;
        cpu.r8 = 0; cpu.r9 = 0;
 
        cpu.rsp -= 0x28;
        mainReturnAddr = moduleBase;
        cpu.rsp -= 8;
        WriteMem(cpu.rsp, &mainReturnAddr, 8);
        cpu.rip = targetFunc;
 
        RunEmulation();
        return ExtractResult();
    }
 
    // ========================================================================
    // FAST EMULATION (from snapshot - transform only, ~500 instructions)
    // ========================================================================
    Vector3 DoFastEmulation(const BoneSnapshot& snap, uint16_t boneIndex) {
        // Restore CPU state to the point right before bone index IMUL
        LoadSnapshot(snap);
 
        // Reset output tracking (output buffer is on the stack, re-zero it)
        memset(&cpu.stack[0x200], 0, 0x100);
        outputBone.v = _mm_setzero_ps();
        foundValidPosition = false;
        lastValidPosition.v = _mm_setzero_ps();
 
        // Set the bone index we actually want
        desiredBoneIndex = boneIndex;
        boneIndexOverridden = false;
        detectedBoneIndex = 0xFFFF;
 
        instrCount = 0;
 
        // Run from snapshot point - IMUL re-executes with new bone index,
        // then transform reads fresh bone data from game memory
        RunEmulation();
        return ExtractResult();
    }
 
public:
    // ========================================================================
    // GetBonePosition - main entry point
    //
    // Flow:
    //   1. If we have a valid probe for this pawn -> FAST PATH (~500 instr)
    //   2. If no probe or probe failed -> FULL EMULATION + capture snapshot
    //   3. Snapshot is reused for ALL subsequent bones of this pawn
    //
    // No TTL delay. Every bone, every frame. Fresh game memory reads.
    // ========================================================================
    Vector3 GetBonePosition(uint64_t pawn, uint16_t boneIndex) {
        if (targetFunc == 0 || !m_dcache)
            return Vector3();

        // Wipe the per-solve shadow so writes from a previous pawn don't
        // leak into this one. The game-memory page cache (BoneEmuCache)
        // stays warm across calls; .text bytes don't change.
        ShadowClear();

        // Reset diagnostics so smoke-test printf reads this solve only.
        dbgLastInstrCount     = 0;
        dbgLastExitRip        = 0;
        dbgLastExitReason     = "";
        dbgLastRunOk          = false;
        dbgFirstUnhandled     = ZYDIS_MNEMONIC_INVALID;
        dbgFirstUnhandledRip  = 0;
        dbgUnhandledCount     = 0;
        dbgCallExternalCount  = 0;
        dbgIsBoneIndexMulHits = 0;
        dbgImulCount          = 0;
        dbgShlCount           = 0;
        dbgImulN              = 0;
        for (int i = 0; i < kImulSamples; i++) { dbgImulMul[i] = 0; dbgImulIdx[i] = 0; }

        PawnProbe* probe = FindProbe(pawn);
        if (probe && probe->probed && probe->snapshot.valid) {
            statFastEmulations++;
 
            Vector3 result = DoFastEmulation(probe->snapshot, boneIndex);
 
            if (isfinite(result.x) && isfinite(result.y) && isfinite(result.z) &&
                (fabsf(result.x) > 0.01f || fabsf(result.y) > 0.01f || fabsf(result.z) > 0.01f))
            {
                probe->failCount = 0;
                return result;
            }
 
            probe->failCount++;
            if (probe->failCount < 3)
                goto do_full;
 
            probe->probed = false;
            probe->snapshot.valid = false;
        }
 
 
    do_full:
        // === FULL EMULATION (probe or fallback) ===
        if (!probe || !probe->probed) {
            // Need to probe: capture snapshot during this emulation
            probe = AllocProbe(pawn);
            probe->snapshot.valid = false;
            probe->failCount = 0;
 
            isProbing = true;
            probeTarget = &probe->snapshot;
            statProbes++;
        }
        else {
            // Probe exists but fast path gave 0 this once -> just full emulate
            isProbing = false;
            probeTarget = nullptr;
        }
 
        statFullEmulations++;
        Vector3 result = DoFullEmulation(pawn, boneIndex);
 
        if (isProbing) {
            isProbing = false;
            probeTarget = nullptr;
            probe->probed = true;
            probe->probeTime = GetTickCount();
 
            if (!probe->snapshot.valid) {
                // IsBoneIndexMul never fired -> can't snapshot this pawn
                // Fast path won't work, but full emulation still does
            }
        }
 
        return result;
    }
};
 
inline BoneEmulatorV3 g_BoneEmulatorV3;
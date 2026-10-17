#pragma once
#include <cstdint>


namespace offsets
{
    // ActorPatch: RainbowSix.exe+E89D3E  -> 4C 89 1D 1FD2EE10  (mov [rip+...],r11)
    inline uint64_t CameraPatch          = 0x0;
    inline uint64_t ActorPatch           = 0x0;
    inline uint8_t  ActorMov[3]          = { 0x4C, 0x89, 0x1D };
    // Code Caves (found dynamically by scanner in .data section)
    inline uint64_t CodeCaveOne          = 0x0;
    inline uint64_t CodeCaveTwo          = 0x0;
    // Trampoline cave (found dynamically in .text section as CC padding)
    inline uint64_t ActorTrampolineEmpty = 0x0;
    inline int      ActorTrampolineSize  = 0x0;
}

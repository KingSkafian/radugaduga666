// Mouse.h
#pragma once
#include <Windows.h>
inline void MoveMouse(int x, int y)
{
    INPUT input[1] = {};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dx = static_cast<LONG>(x);
    input[0].mi.dy = static_cast<LONG>(y);
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    input[0].mi.time = 0;
    input[0].mi.dwExtraInfo = GetMessageExtraInfo();
    SendInput(1, input, sizeof(INPUT));
}
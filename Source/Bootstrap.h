#pragma once
#include <Windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <d3d11.h>
#include <tchar.h>
#include <dinput.h>
#include <psapi.h>
#include <sstream>
#include <vector>
#include <TlHelp32.h>
#include <iomanip>
#include <string>
#include <mutex>
#include <map>
#include <algorithm>
#include <fstream>
#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "ntdll.lib")
#define target_process L"RainbowSix.exe" //winver.exe

using namespace std;

HWND Hwnd;
HWND Overlay;
MSG Msg = { nullptr };

// In Bootstrap.h (add this line)
ID3D11Device* Device{ nullptr };
IDXGISwapChain* SwapChain{ nullptr };
ID3D11DeviceContext* DeviceContext{ nullptr };
ID3D11RenderTargetView* TargetView{ nullptr };
//ImGui Includes
#include "hde64/hde64.h"
//ImGui Includes
#include "Gui/Imgui/imgui.h"
#include "Gui/Imgui/imgui_impl_dx11.h"
#include "Gui/Imgui/imgui_impl_win32.h"
#include "Gui/interfont.h"
#include "Gui/font_awesome.h"
ImFont* DefaultFont{ nullptr };
ImFont* IconFont{ nullptr };

namespace Menu {
    inline float AccentColor[4] = { 0.0f, 0.957f, 1.0f, 1.0f }; // #00F4FF cyan
}

//Kernel Includes
#include <winternl.h>
#include "../Dependencies/skcrypt/skcrypter.h"
#include "../Runtime/Logging/logging.hxx"
#include "ia32/ia32.h"
#include "wdk/wdk.h"
#include "../Runtime/Backend/backend.hxx"
auto g_backend = new core::c_backend;


//Logger Includes
#include "Logger/Logger.h"

//Kernel Includes
#include "Kernel/Kernel.h"

//Usermode Includes
#include "Kernel/Usermode.h" //testing without kernel

//Offsets Includes
#include "../Core/SDK/Offsets/Offsets.h"

//Zydis Includes
#include "Zydis/Zydis.h"

//Vector Includes
#include "../Core/Math/Vector.h"

//BoneEmu Includes (external port of seller's bone-position emulator)
#include "BoneEmu/bone_emu.h"

//Structs Includes
#include "../Core/Structs/Structs.h"
//Custom UI Includes
#include "Gui/customimguishit.h"
//SDK Includes
#include "../Core/SDK/SDK.h"

//PatchEngine Includes
#include "../Core/State/Patches/Patches.h"



//EntityState Includes
#include "../Core/State/Entities/EntityCache.h"


//DrawUtils Includes
#include "DrawUtils/DrawUtils.h"

#include "../Core/Game/Gun/MoveMouse.h"  // add this
#include "../Core/Game/Gun/Recoil.h"
#include "../Core/Game/Players/Aimbot.h"
#include "../Core/Game/Players/Players.h"


//Render Includes
#include "../Core/Render/Render.h"


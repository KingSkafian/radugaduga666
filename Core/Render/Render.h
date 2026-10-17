#pragma once
#include "Overlay/Overlay.h"
#include "Menu/Menu.h"
#include "../../Source/Gui/MonoLisa-normal.h"
namespace Render
{
	auto CreateDevice() -> bool
	{
		// refresh rate
		DXGI_RATIONAL refresh_rate{};
		ZeroMemory(&refresh_rate, sizeof(DXGI_RATIONAL));
		refresh_rate.Numerator = 0;
		refresh_rate.Denominator = 1;

		// buffer
		DXGI_MODE_DESC buffer_desc{};
		ZeroMemory(&buffer_desc, sizeof(DXGI_MODE_DESC));
		buffer_desc.Width = Monitor.Width;
		buffer_desc.Height = Monitor.Height;
		buffer_desc.RefreshRate = refresh_rate;
		buffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		buffer_desc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		buffer_desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

		// sample
		DXGI_SAMPLE_DESC sample_desc{};
		ZeroMemory(&sample_desc, sizeof(DXGI_SAMPLE_DESC));
		sample_desc.Count = 1;
		sample_desc.Quality = 0;

		// Swapchain
		DXGI_SWAP_CHAIN_DESC swapchain_desc{};
		ZeroMemory(&swapchain_desc, sizeof(DXGI_SWAP_CHAIN_DESC));
		swapchain_desc.BufferDesc = buffer_desc;
		swapchain_desc.SampleDesc = sample_desc;
		swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapchain_desc.BufferCount = 2;
		swapchain_desc.OutputWindow = Overlay;
		swapchain_desc.Windowed = TRUE;
		swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

		auto ret = D3D11CreateDeviceAndSwapChain(
			NULL,
			D3D_DRIVER_TYPE_HARDWARE,
			NULL,
			NULL,
			0,
			0,
			D3D11_SDK_VERSION,
			&swapchain_desc,
			&SwapChain,
			&Device,
			0,
			&DeviceContext);

		return true;
	}
	auto CreateTarget() -> bool
	{
		ID3D11Texture2D* render_buffer{ nullptr };
		auto result = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&render_buffer));
		if (FAILED(result)) {
			return false;
		}
		result = Device->CreateRenderTargetView(render_buffer, nullptr, &TargetView);
		if (FAILED(result)) {
			return false;
		}
		render_buffer->Release();
		return true;

	}
	auto ReleaseObjects() -> void
	{
		if (TargetView) {

			TargetView->Release();
			TargetView = nullptr;
		}

		if (DeviceContext) {

			DeviceContext->Release();
			DeviceContext = nullptr;
		}

		if (Device) {

			Device->Release();
			Device = nullptr;
		}

		if (SwapChain) {

			SwapChain->Release();
			SwapChain = nullptr;
		}
	}
	auto CreateImgui() -> bool
	{
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		ImFontConfig fontCfg = ImFontConfig();
		io.DeltaTime = 1.0f / 60.0f;
		io.IniFilename = NULL;


		// ── Load Inter font (14px) ───────────────────────────────────
		ImFontConfig interCfg;
		interCfg.FontDataOwnedByAtlas = false;
		DefaultFont = io.Fonts->AddFontFromMemoryTTF(
			(void*)InterRegularTTF, sizeof(InterRegularTTF), 14.0f, &interCfg, io.Fonts->GetGlyphRangesDefault());

		// ── Merge Font Awesome Solid icons ───────────────────────────
		ImFontConfig faCfg;
		faCfg.MergeMode        = true;
		faCfg.PixelSnapH       = true;
		faCfg.GlyphMinAdvanceX = 12.0f;
		faCfg.FontDataOwnedByAtlas = false;
		static const ImWchar fa_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
		io.Fonts->AddFontFromMemoryCompressedTTF(
			fa6_solid_compressed_data, fa6_solid_compressed_size, 14.0f, &faCfg, fa_ranges);
		// NOTE: Do NOT call Build() here — backends handle this automatically

		auto imgui_win32 = ImGui_ImplWin32_Init(Overlay);
		if (!imgui_win32) {
			std::printf(("failed to load imgui Win32.\n"));
			return false;
		}
		auto imgui_dx11 = ImGui_ImplDX11_Init(Device, DeviceContext);
		if (!imgui_dx11) {
			std::printf(("failed to load imgui.\n"));
			return false;
		}
		Device->Release();
		DWORD assid = 0;
		(GetWindowThreadProcessId)(Overlay, &assid);
		return true;

	}
	void RenderThread()
	{
		while (Msg.message != WM_QUIT) {
			if (PeekMessage(&Msg, Overlay, 0, 0, PM_REMOVE)) {
				TranslateMessage(&Msg);
				DispatchMessage(&Msg);
			}
			if (GetAsyncKeyState(Menus.MenuKey) & 1) {
				Menus.ShowMenu = !Menus.ShowMenu;
			}
			if (GetAsyncKeyState(Menus.PanicKey) & 1) {
				ExitProcess(0);
			}


			ImGui_ImplDX11_NewFrame();
			POINT p;
			GetCursorPos(&p);
			ImGuiIO& io = ImGui::GetIO();
			io.MousePos = ImVec2(p.x, p.y);
			io.MouseDown[0] = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
			io.MouseDown[1] = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			if (Menus.ShowMenu)
			{
				Menu::Draw();
			}
			Players::PlayersRender();
			const float color[]{ 0, 0, 0, 0 };

			ImGui::Render();

			DeviceContext->OMSetRenderTargets(1, &TargetView, nullptr);
			DeviceContext->ClearRenderTargetView(TargetView, color);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

			SwapChain->Present(Monitor.Vsync ? 1 : 0, 0);
		}
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		ReleaseObjects();
		if (Overlay != nullptr) {
			::DestroyWindow(Overlay);
			Overlay = nullptr;
		}
	}
	bool StartImgui()
	{
		auto CreateSwapChain = Render::CreateDevice();
		if (!CreateSwapChain) {
			MessageBoxA(NULL, ("Failed to init."), ("Code : 0x01"), NULL);
			return false;
		}

		auto CreateRenderView = Render::CreateTarget();
		if (!CreateRenderView) {
			MessageBoxA(NULL, ("Failed to init."), ("Code : 0x02"), NULL);
			return false;
		}

		auto ImguiStartup = Render::CreateImgui();
		if (!ImguiStartup) {
			MessageBoxA(NULL, ("Failed to init."), ("Code : 0x03"), NULL);
			return false;
		}
		return true;
	}
}

namespace StartRender
{
	auto StartRendering() -> bool
	{
		Render::RenderThread();
		return true;
	}
}
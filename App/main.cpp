#include "../Source/Bootstrap.h"
int main()
{
	{

		if (!g_backend->setup())
			return std::getchar();

		if (!g_backend->attach(target_process)) {
			//logging::print(encrypt("Failed to attach process.\n"));
			g_backend->unload();
			return std::getchar();
		}
		Logger->print_hex("Base Address: ", g_backend->m_base_address);

		Overlays::HijackOverlay();
		Render::StartImgui();
		std::thread roundStateThread([]() {
			while (true) {
				EntityState::GetGameState();
				Sleep(10);
			}
			});

		roundStateThread.detach();
		std::thread([]() { EntityState::CachePlayers(); }).detach();
		// Hotkey thread. Originally this had no while(true) wrapper so the
		// F5/F6/F7 checks fired exactly once at startup and the thread
		// died. Now it loops and uses an edge-triggered F5 to pulse-patch
		// the trampolines at the start of each round.
		//
		// F5 (pulse patch — primary):
		//   1. Clear Cached.Actors / ActorEntries (force a fresh scan).
		//   2. Set should_patch.
		//   3. Install actor + camera trampolines.
		//   4. Sleep 500 ms — long enough for the game to run through the
		//      patched instructions several times and write fresh pointers
		//      into CodeCaveOne / CodeCaveTwo.
		//   5. Restore the original .text bytes immediately. The cave
		//      values persist (the game already wrote them), so the cheat
		//      keeps reading CameraPointer / EntityListPtr from the caves
		//      without the patches sitting on .text where ShieldGuard's
		//      integrity scan can see them.
		//
		// F6 — manual restore (keeps patches off for the rest of the round).
		// F7 — clear the code caves (zero CodeCaveOne / CodeCaveTwo).
		std::thread([]() {
			bool f5_pressed = false;
			bool should_patch = false;
			while (true) {
				if (GetAsyncKeyState(VK_F5) & 0x8000) {
					if (!f5_pressed) {
						{
							std::lock_guard<std::mutex> lk(EntityState::g_cache_mtx);
							Cached.Actors.clear();
							Cached.ActorEntries.clear();
						}
						should_patch = true;
						f5_pressed = true;
					}
				} else {
					f5_pressed = false;
				}

				if (should_patch) {
					PatchEngine::SetupActor();
					PatchEngine::SetupCamera();
					cout << "[+] Pulse patch installed (F5)" << endl;

					std::this_thread::sleep_for(std::chrono::milliseconds(500));

					PatchEngine::RestoreActor();
					PatchEngine::RestoreCamera();
					cout << "[+] Pulse patch reverted — pointers captured" << endl;

					should_patch = false;
				}

				if (GetAsyncKeyState(VK_F6) & 0x8000) {
					PatchEngine::RestoreActor();
					PatchEngine::RestoreCamera();
					cout << "[+] Restore Patches (F6)" << endl;
					Sleep(200);
				}
				if (GetAsyncKeyState(VK_F7) & 0x8000) {
					PatchEngine::ClearCodeCaves();
					cout << "[+] Clear Codecaves (F7)" << endl;
					Sleep(200);
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			}).detach();

		StartRender::StartRendering();
		return getchar();
	}
}

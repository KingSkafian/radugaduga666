#pragma once
#include <vector>

struct ActorEntry {
	uint64_t ptr;
	uint64_t fb;
	Vector3  pos;
	Vector3  pos50;
	Vector3  pos60;
	float    direction;
};

class structs
{
public:
	uint64_t
		CameraPointer,
		ActorPointer;
	int GameState = -1;
	std::vector<uint64_t>  Actors;
	std::vector<ActorEntry> ActorEntries;
}; structs Cached;

// Monitor struct
struct {
	bool Vsync = 1;
	int Width;
	int Height;
	int WidthCenter;
	int HeightCenter;
} inline Monitor;

// Menu struct
struct {
	bool ShowMenu = true;
	int  MenuKey  = VK_INSERT;   // Key to toggle the menu
	int  PanicKey = VK_END;      // Key to immediately exit the cheat
} inline Menus;

//Aimbot struct
struct {
	bool Enabled = false;
	bool FovEnable = false;
	bool UnlockYAxis = false;
	bool line = false;
	int fov = 150;
	int smooth = 5;
	int AimKey = VK_XBUTTON2;
} inline Aimbot;

// Visuals struct
struct {
	bool Enabled = true;
	bool Distances = true;
	bool Box = true;
	bool Skeletons = false;
	// HeadCircle and the Aimbot tab are force-disabled until the seller ships
	// proper head/stance tracking — the current head Z is a fixed 1.9 m above
	// the live position, which doesn't track crouch/prone, so the aimbot
	// would aim above the head. Keep these false; the menu controls are
	// also grayed out (see Core/Render/Menu/Menu.h).
	bool HeadCircle = false;
	int BoxType = 1; // 0 = 2D, 1 = Cornered 2D, 2 = 3D
	ImVec4 BoxColor = ImVec4(0.0f, 0.957f, 1.0f, 1.0f); // #00F4FF cyan
	ImVec4 ArrowColor = ImVec4(1.0f, 1.0f, 1.0f, 0.5f);     
	ImVec4 SkeletonColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  
	ImVec4 HeadColor = ImVec4(1.0f, 1.0f, 1.0f, 0.5f);
	bool RiceHat = false;
	bool Skeleton = false;
	bool Arrow = false;
	bool Snapline = false;
	bool DeadCheck = true;
	bool TeamCheck = true;
	float MaxDistance = 100.0f;
	bool DebugFilterByte = false;
} inline Visuals;

// Gadget struct
struct {
	bool ESP = false;
	bool Dot = false;
	bool Box3D = false;
	bool Outline3D = false;
	bool Names = false;
	ImVec4 GadgetColor = ImVec4(0.0f, 1.0f, 0.0f, 0.5f);
} inline Gadget;
//Gun struct
struct {
	bool Enabled = false;
	float SensX = 200.0f;
	float SensY = 500.0f;
} inline Gun;
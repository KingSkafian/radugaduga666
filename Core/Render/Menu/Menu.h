#pragma once
#include "../../../Source/Bootstrap.h"

namespace Menu {

    inline bool waiting_for_key       = false;
    inline bool waiting_for_menu_key  = false;
    inline bool waiting_for_panic_key = false;

    inline const char* GetKeyName(int vk) {
        switch (vk) {
        case VK_INSERT:  return "Insert"; case VK_DELETE: return "Delete";
        case VK_HOME:    return "Home";   case VK_END:    return "End";
        case VK_PRIOR:   return "PgUp";   case VK_NEXT:   return "PgDn";
        case VK_XBUTTON2:return "M5";     case VK_XBUTTON1:return "M4";
        case VK_LBUTTON: return "LMB";    case VK_RBUTTON: return "RMB";
        case VK_MBUTTON: return "MMB";    case VK_SHIFT:   return "Shift";
        case VK_CONTROL: return "Ctrl";   case VK_MENU:    return "Alt";
        case VK_CAPITAL: return "Caps";   case VK_TAB:     return "Tab";
        case VK_F1: return "F1";  case VK_F2: return "F2";  case VK_F3: return "F3";
        case VK_F4: return "F4";  case VK_F5: return "F5";  case VK_F6: return "F6";
        case VK_F7: return "F7";  case VK_F8: return "F8";  case VK_F9: return "F9";
        case VK_F10:return "F10"; case VK_F11:return "F11"; case VK_F12:return "F12";
        default:    return "?";
        }
    }

    inline bool CaptureKey(int* vk_out, bool& waiting) {
        const int m[] = { VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2 };
        for (int v : m) if (GetAsyncKeyState(v) & 0x8000) { *vk_out=v; waiting=false; return true; }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { waiting=false; return false; }
        for (int v=VK_SHIFT; v<=VK_F12; v++) if (GetAsyncKeyState(v) & 0x8000) { *vk_out=v; waiting=false; return true; }
        const int ex[]={VK_INSERT,VK_DELETE,VK_HOME,VK_END,VK_PRIOR,VK_NEXT};
        for (int v : ex) if (GetAsyncKeyState(v) & 0x8000) { *vk_out=v; waiting=false; return true; }
        return false;
    }

    // ── Helper: checkbox + optional color picker on the right ─────────────
    inline void CheckRow(const char* label, bool* v, ImVec4* col = nullptr) {
        float avail = ImGui::GetContentRegionAvail().x;
        float color_w = col ? 24.0f : 0.0f;
        float spacing  = col ? 8.0f  : 0.0f;

        ImGui::SetNextItemWidth(avail - color_w - spacing);
        AnimatedCheckbox(label, v);

        if (col) {
            ImGui::SameLine(avail - color_w);
            ImGui::SetNextItemWidth(color_w);
            char id[64]; snprintf(id, sizeof(id), "##col_%s", label);
            ImGui::ColorEdit4(id, (float*)col,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                ImGuiColorEditFlags_AlphaBar);
        }
    }

    // ── Apply dark/clean style ────────────────────────────────────────────
    inline void ApplyStyle() {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding    = 8.0f;  s.ChildRounding    = 6.0f;
        s.FrameRounding     = 5.0f;  s.ScrollbarRounding= 4.0f;
        s.GrabRounding      = 4.0f;  s.TabRounding      = 5.0f;
        s.PopupRounding     = 5.0f;
        s.WindowPadding     = ImVec2(12, 12);
        s.FramePadding      = ImVec2(7, 4);
        s.ItemSpacing       = ImVec2(8, 6);
        s.ItemInnerSpacing  = ImVec2(5, 5);
        s.ScrollbarSize     = 7.0f;
        s.GrabMinSize       = 8.0f;
        s.WindowBorderSize  = 0.0f;
        s.FrameBorderSize   = 0.0f;
        s.TabBorderSize     = 0.0f;
        s.SeparatorTextPadding = ImVec2(0, 5);

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]          = ImVec4(0.052f,0.052f,0.058f,1.0f);
        c[ImGuiCol_ChildBg]           = ImVec4(0.068f,0.068f,0.074f,1.0f);
        c[ImGuiCol_PopupBg]           = ImVec4(0.068f,0.068f,0.076f,1.0f);
        c[ImGuiCol_Border]            = ImVec4(0.11f, 0.11f, 0.12f, 1.0f);
        c[ImGuiCol_BorderShadow]      = ImVec4(0,0,0,0);
        c[ImGuiCol_FrameBg]           = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
        c[ImGuiCol_FrameBgHovered]    = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
        c[ImGuiCol_FrameBgActive]     = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
        c[ImGuiCol_TitleBg]           = ImVec4(0.038f,0.038f,0.043f,1.0f);
        c[ImGuiCol_TitleBgActive]     = ImVec4(0.038f,0.038f,0.043f,1.0f);
        c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.038f,0.038f,0.043f,1.0f);
        c[ImGuiCol_ScrollbarBg]       = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
        c[ImGuiCol_ScrollbarGrab]     = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.5f);
        c[ImGuiCol_ScrollbarGrabHovered]=ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.7f);
        c[ImGuiCol_ScrollbarGrabActive] =ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],1.0f);
        c[ImGuiCol_CheckMark]         = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],1.0f);
        c[ImGuiCol_SliderGrab]        = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.85f);
        c[ImGuiCol_SliderGrabActive]  = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],1.0f);
        c[ImGuiCol_Button]            = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
        c[ImGuiCol_ButtonHovered]     = ImVec4(AccentColor[0]*0.25f,AccentColor[1]*0.25f,AccentColor[2]*0.25f,0.55f);
        c[ImGuiCol_ButtonActive]      = ImVec4(AccentColor[0]*0.4f, AccentColor[1]*0.4f, AccentColor[2]*0.4f, 0.80f);
        c[ImGuiCol_Header]            = ImVec4(AccentColor[0]*0.18f,AccentColor[1]*0.18f,AccentColor[2]*0.18f,0.60f);
        c[ImGuiCol_HeaderHovered]     = ImVec4(AccentColor[0]*0.22f,AccentColor[1]*0.22f,AccentColor[2]*0.22f,0.80f);
        c[ImGuiCol_HeaderActive]      = ImVec4(AccentColor[0]*0.28f,AccentColor[1]*0.28f,AccentColor[2]*0.28f,1.0f);
        c[ImGuiCol_Separator]         = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
        c[ImGuiCol_SeparatorHovered]  = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.4f);
        c[ImGuiCol_SeparatorActive]   = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.8f);
        c[ImGuiCol_Text]              = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
        c[ImGuiCol_TextDisabled]      = ImVec4(0.38f, 0.38f, 0.38f, 1.0f);
        c[ImGuiCol_Tab]               = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
        c[ImGuiCol_TabHovered]        = ImVec4(AccentColor[0]*0.14f,AccentColor[1]*0.14f,AccentColor[2]*0.14f,1.0f);
        c[ImGuiCol_TabActive]         = ImVec4(AccentColor[0]*0.18f,AccentColor[1]*0.18f,AccentColor[2]*0.18f,1.0f);
        c[ImGuiCol_TabUnfocused]      = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
        c[ImGuiCol_TabUnfocusedActive]= ImVec4(0.11f, 0.11f, 0.13f, 1.0f);
        c[ImGuiCol_ResizeGrip]        = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.18f);
        c[ImGuiCol_ResizeGripHovered] = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.45f);
        c[ImGuiCol_ResizeGripActive]  = ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],0.75f);
    }

    inline void Draw() {
        if (!DefaultFont) return;
        ApplyStyle();
        ImGui::PushFont(DefaultFont);
        ImGui::SetNextWindowSize(ImVec2(500, 460), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Dominate", &Menus.ShowMenu, ImGuiWindowFlags_NoCollapse)) {

            // Thin accent top-bar
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(wp.x + 8, wp.y),
                ImVec2(wp.x + ws.x - 8, wp.y + 2),
                ImGui::GetColorU32(ImVec4(AccentColor[0], AccentColor[1], AccentColor[2], 1.0f)), 2.0f);

            if (ImGui::BeginTabBar("MainTabBar")) {

                // ── VISUALS ──────────────────────────────────────────────
                if (ImGui::BeginTabItem(ICON_FA_EYE "  Visuals")) {
                    ImGui::Spacing();

                    // Box section
                    ImGui::SeparatorText("Box");
                    CheckRow("Box ESP", &Visuals.Box, &Visuals.BoxColor);
                    ImGui::Spacing();
                    const char* box_types[] = { "2D", "Cornered 2D", "3D" };
                    CustomCombo("Box Type", &Visuals.BoxType, box_types, IM_ARRAYSIZE(box_types));
                    ImGui::Spacing();

                    // Player section
                    ImGui::SeparatorText("Player");
                    // Head Circle is grayed out — current head Z is a fixed offset
                    // above the live position and doesn't track stance. Re-enable
                    // once the seller ships head/view-height tracking.
                    Visuals.HeadCircle = false;
                    ImGui::BeginDisabled();
                    CheckRow("Head Circle", &Visuals.HeadCircle, &Visuals.HeadColor);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("testing");
                    CheckRow("Distance ESP", &Visuals.Distances);
                    CheckRow("Snapline",     &Visuals.Snapline);
                    ImGui::Spacing();

                    // Arrows section
                    ImGui::SeparatorText("Arrows");
                    CheckRow("Directional Arrows", &Visuals.Arrow, &Visuals.ArrowColor);
                    ImGui::Spacing();

                    // Filters section
                    ImGui::SeparatorText("Filters");
                    AnimatedCheckbox("Dead Check",        &Visuals.DeadCheck);
                    AnimatedCheckbox("Team Check",        &Visuals.TeamCheck);
                    AnimatedCheckbox("Debug Filter Byte", &Visuals.DebugFilterByte);
                    ImGui::Spacing();

                    // Range
                    ImGui::SeparatorText("Range");
                    CustomSliderFloat("Max Distance", &Visuals.MaxDistance, 10.0f, 500.0f, "%.0fm");
                    ImGui::Spacing();

                    ImGui::EndTabItem();
                }

                // ── AIMBOT ───────────────────────────────────────────────
                // Aimbot is force-disabled — head Z is a fixed offset above the
                // live position, so the aim point doesn't match the player's
                // actual head when they crouch/prone. Re-enable once the seller
                // ships head/view-height tracking.
                if (ImGui::BeginTabItem(ICON_FA_CROSSHAIRS "  Aimbot")) {
                    Aimbot.Enabled = false;
                    ImGui::Spacing();
                    ImGui::BeginDisabled();
                    ImGui::SeparatorText("General");
                    AnimatedCheckbox("Enable Aimbot",  &Aimbot.Enabled);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("testing");
                    ImGui::BeginDisabled();
                    AnimatedCheckbox("Show FOV Circle", &Aimbot.FovEnable);
                    AnimatedCheckbox("Aim Line",        &Aimbot.line);
                    AnimatedCheckbox("Unlock Y Axis",   &Aimbot.UnlockYAxis);
                    ImGui::Spacing();
                    ImGui::SeparatorText("Values");
                    CustomSliderInt("FOV",    &Aimbot.fov,    10,  500);
                    ImGui::Spacing();
                    CustomSliderInt("Smooth", &Aimbot.smooth, 1,   100);
                    ImGui::Spacing();
                    ImGui::SeparatorText("Keybind");
                    ImGui::Text("Aim Key:");
                    ImGui::SameLine();
                    char kb[64];
                    if (waiting_for_key) snprintf(kb, sizeof(kb), "[ Press... ]");
                    else                 snprintf(kb, sizeof(kb), "[ %s ]", GetKeyName(Aimbot.AimKey));
                    if (ImGui::Button(kb, ImVec2(140, 22))) waiting_for_key = true;
                    if (waiting_for_key) {
                        CaptureKey(&Aimbot.AimKey, waiting_for_key);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],1.0f), "...");
                    }
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.4f,0.4f,0.4f,1.0f), "Hold [ %s ] to aim", GetKeyName(Aimbot.AimKey));
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }

                // ── GADGET ───────────────────────────────────────────────
                if (ImGui::BeginTabItem(ICON_FA_BOMB "  Gadget")) {
                    ImGui::Spacing();
                    ImGui::SeparatorText("Gadget ESP");
                    CheckRow("Gadget ESP", &Gadget.ESP, &Gadget.GadgetColor);
                    AnimatedCheckbox("Dot",    &Gadget.Dot);
                    AnimatedCheckbox("3D Box", &Gadget.Box3D);
                    ImGui::Spacing();
                    ImGui::EndTabItem();
                }

                // ── SETTINGS ─────────────────────────────────────────────
                if (ImGui::BeginTabItem(ICON_FA_GEAR "  Settings")) {
                    ImGui::Spacing();
                    ImGui::SeparatorText("Keybinds");
                    ImGui::Spacing();

                    // Menu Key
                    ImGui::Text("Menu Key:"); ImGui::SameLine();
                    char mk[64];
                    if (waiting_for_menu_key) snprintf(mk, sizeof(mk), "[ Press... ]");
                    else                       snprintf(mk, sizeof(mk), "[ %s ]", GetKeyName(Menus.MenuKey));
                    if (ImGui::Button(mk, ImVec2(140, 22))) waiting_for_menu_key = true;
                    if (waiting_for_menu_key) {
                        CaptureKey(&Menus.MenuKey, waiting_for_menu_key);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],1.0f), "...");
                    }
                    ImGui::TextColored(ImVec4(0.38f,0.38f,0.38f,1.0f), "Toggle: [ %s ]", GetKeyName(Menus.MenuKey));

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    // Panic Key
                    ImGui::Text("Panic Key:"); ImGui::SameLine();
                    char pk[64];
                    if (waiting_for_panic_key) snprintf(pk, sizeof(pk), "[ Press... ]");
                    else                        snprintf(pk, sizeof(pk), "[ %s ]", GetKeyName(Menus.PanicKey));
                    if (ImGui::Button(pk, ImVec2(140, 22))) waiting_for_panic_key = true;
                    if (waiting_for_panic_key) {
                        CaptureKey(&Menus.PanicKey, waiting_for_panic_key);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(AccentColor[0],AccentColor[1],AccentColor[2],1.0f), "...");
                    }
                    ImGui::TextColored(ImVec4(1.0f,0.28f,0.28f,1.0f), "Panic: [ %s ]", GetKeyName(Menus.PanicKey));

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::End();
        }
        ImGui::PopFont();
    }
}
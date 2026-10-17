#pragma once
#include "Imgui/imgui.h"
#include "Imgui/imgui_internal.h"
#include <map>
#include <string>

struct check_state {
    ImVec2 background_offset = ImVec2(2, 2);
    ImVec4 background = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    ImVec4 mark = ImVec4(0, 0, 0, 0);
    float mark_animation = 18.0f;
    ImVec4 text = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
};

struct slider_state {
    float slow_anim = 0.0f;
    ImVec4 text_number = ImVec4(1, 1, 1, 1);
    ImVec4 text = ImVec4(1, 1, 1, 1);
};

namespace CustomMath {
    inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline ImVec2 Vec2Lerp(ImVec2 a, ImVec2 b, float t) {
        return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }
    inline ImVec4 Vec4Lerp(ImVec4 a, ImVec4 b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }
}


void SubTab(const char* label, int index, int* selected, const ImVec4& accent) {
    bool is_selected = (*selected == index);
    if (is_selected) ImGui::PushStyleColor(ImGuiCol_Text, accent);
    else ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));

    if (ImGui::Button(label, ImVec2(90, 35))) *selected = index;

    if (is_selected) {
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        // Fix: Manuelle ImVec2 Konstruktion ohne Operatoren
        ImVec2 bar_min(min.x + 20, max.y - 2);
        ImVec2 bar_max(max.x - 20, max.y);
        ImGui::GetWindowDrawList()->AddRectFilled(bar_min, bar_max, ImGui::GetColorU32(accent), 10.0f);
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
}

bool CustomColorPicker(const char* label, float col[4]) {
    ImGui::PushID(label);
    bool value_changed = ImGui::ColorEdit4("##edit", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    ImGui::SameLine();
    ImGui::Text(label);
    ImGui::PopID();
    return value_changed;
}

bool AnimatedCheckbox(const char* label, bool* v) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

    const float square_sz = 18.0f;
    const ImVec2 pos = window->DC.CursorPos;

    ImVec2 total_bb_max(pos.x + square_sz + style.ItemInnerSpacing.x + label_size.x,
        pos.y + (square_sz > label_size.y ? square_sz : label_size.y));
    const ImRect total_bb(pos, total_bb_max);

    ImGui::ItemSize(total_bb, 0.0f);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);

    if (pressed) {
        *v = !(*v);
        ImGui::MarkItemEdited(id);
    }
    float anim_state = window->StateStorage.GetFloat(id, *v ? 1.0f : 0.0f);

    float t = g.IO.DeltaTime * 12.0f;

    if (*v) anim_state = CustomMath::Lerp(anim_state, 1.0f, t);
    else    anim_state = CustomMath::Lerp(anim_state, 0.0f, t);
    window->StateStorage.SetFloat(id, anim_state);
    ImVec4 accent(Menu::AccentColor[0], Menu::AccentColor[1], Menu::AccentColor[2], 1.0f);
    ImVec4 inactive(0.15f, 0.15f, 0.15f, 1.0f);

    ImVec4 current_bg = CustomMath::Vec4Lerp(inactive, accent, anim_state);
    float current_offset = CustomMath::Lerp(2.0f, 0.0f, anim_state);
    float mark_anim = CustomMath::Lerp(square_sz, 0.0f, anim_state);
    ImVec4 text_col = CustomMath::Vec4Lerp(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ImVec4(1, 1, 1, 1), anim_state);

    const ImRect check_bb(pos, ImVec2(pos.x + square_sz, pos.y + square_sz));

    window->DrawList->AddRect(check_bb.Min, check_bb.Max, ImGui::GetColorU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f)), style.FrameRounding);

    ImVec2 bg_min(check_bb.Min.x + current_offset, check_bb.Min.y + current_offset);
    ImVec2 bg_max(check_bb.Max.x - current_offset, check_bb.Max.y - current_offset);
    window->DrawList->AddRectFilled(bg_min, bg_max, ImGui::GetColorU32(current_bg), style.FrameRounding);

    if (anim_state > 0.01f) {
        ImVec2 clip_max(check_bb.Max.x - mark_anim, check_bb.Max.y);
        window->DrawList->PushClipRect(check_bb.Min, clip_max, true);

        float thickness = 2.0f;
        float size = square_sz - 10.0f;
        ImVec2 center(check_bb.Min.x + square_sz * 0.5f, check_bb.Min.y + square_sz * 0.5f);

        window->DrawList->PathLineTo(ImVec2(center.x - size * 0.5f, center.y));
        window->DrawList->PathLineTo(ImVec2(center.x - size * 0.1f, center.y + size * 0.4f));
        window->DrawList->PathLineTo(ImVec2(center.x + size * 0.5f, center.y - size * 0.4f));

        window->DrawList->PathStroke(ImGui::GetColorU32(ImVec4(0, 0, 0, anim_state)), false, thickness);

        window->DrawList->PopClipRect();
    }

    ImVec2 text_pos(check_bb.Max.x + style.ItemInnerSpacing.x, check_bb.Min.y + (square_sz - label_size.y) * 0.5f);
    window->DrawList->AddText(text_pos, ImGui::GetColorU32(text_col), label);

    return pressed;
}



bool CustomSliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.2f") {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    ImVec2 label_size = ImGui::CalcTextSize(label, label_end, true);

    float width = ImGui::CalcItemWidth();
    float slider_height = 10.0f;
    float vertical_spacing = 8.0f;
    float total_height = label_size.y + vertical_spacing + slider_height;

    ImVec2 pos = window->DC.CursorPos;
    ImRect total_bb(pos, ImVec2(pos.x + width, pos.y + total_height));

    ImGui::ItemSize(total_bb, 0.0f);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    // Interaktion
    ImRect frame_bb(ImVec2(pos.x, pos.y + label_size.y + vertical_spacing),
        ImVec2(pos.x + width, pos.y + label_size.y + vertical_spacing + slider_height));

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(frame_bb, id, &hovered, &held);

    if (held) {
        float t = ImSaturate((g.IO.MousePos.x - frame_bb.Min.x) / (frame_bb.Max.x - frame_bb.Min.x));
        *v = v_min + t * (v_max - v_min);
        ImGui::MarkItemEdited(id);
    }

    // --- FIX: Keine std::map verwenden! ---
    // Wir holen den letzten Animationswert aus dem StateStorage (Standardwert ist der aktuelle Wert von *v)
    float anim_val = window->StateStorage.GetFloat(id, *v);

    // Animation berechnen
    anim_val = ImLerp(anim_val, *v, g.IO.DeltaTime * 15.0f);

    // Wert f�r den n�chsten Frame speichern
    window->StateStorage.SetFloat(id, anim_val);
    // ---------------------------------------

    ImDrawList* draw = window->DrawList;
    ImU32 col_bg = ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
    ImU32 col_accent = ImGui::GetColorU32(ImVec4(Menu::AccentColor[0], Menu::AccentColor[1], Menu::AccentColor[2], 1.0f));

    // Label zeichnen
    draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), label, label_end);

    // Wert-Text rechts oben zeichnen
    char value_buf[64];
    ImGui::DataTypeFormatString(value_buf, IM_ARRAYSIZE(value_buf), ImGuiDataType_Float, v, format);
    ImVec2 val_size = ImGui::CalcTextSize(value_buf);
    draw->AddText(ImVec2(pos.x + width - val_size.x, pos.y), ImGui::GetColorU32(ImGuiCol_Text), value_buf);

    // Hintergrund des Sliders
    draw->AddRectFilled(frame_bb.Min, frame_bb.Max, col_bg, 4.0f);

    // Aktiver Teil des Sliders (basierend auf anim_val f�r den Smooth-Effekt)
    float visual_t = ImSaturate((anim_val - v_min) / (v_max - v_min));
    if (visual_t > 0.0f) {
        float active_x = frame_bb.Min.x + (frame_bb.Max.x - frame_bb.Min.x) * visual_t;
        // Sicherstellen, dass die Rundung bei kleinen Werten nicht glitcht
        float display_x = ImMax(active_x, frame_bb.Min.x + 4.0f);
        draw->AddRectFilled(frame_bb.Min, ImVec2(display_x, frame_bb.Max.y), col_accent, 4.0f);
    }

    return held; // Gibt true zur�ck, solange der User schiebt
}

inline bool CustomSliderInt(const char* label, int* v, int v_min, int v_max) {
    float v_f = (float)*v;
    bool res = CustomSliderFloat(label, &v_f, (float)v_min, (float)v_max, "%1.0f");
    *v = (int)v_f;
    return res;
}


bool CustomCombo(const char* label, int* current_item, const char* const items[], int items_count) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    ImVec2 label_size = ImGui::CalcTextSize(label, label_end, true);

    float width = ImGui::CalcItemWidth();
    float combo_height = 22.0f;
    float vertical_spacing = 6.0f;
    float total_height = label_size.y + vertical_spacing + combo_height;

    ImVec2 pos = window->DC.CursorPos;
    ImRect total_bb(pos, ImVec2(pos.x + width, pos.y + total_height));

    ImGui::ItemSize(total_bb, 0.0f);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    ImRect frame_bb(ImVec2(pos.x, pos.y + label_size.y + vertical_spacing),
        ImVec2(pos.x + width, pos.y + label_size.y + vertical_spacing + combo_height));

    bool hovered = ImGui::IsMouseHoveringRect(frame_bb.Min, frame_bb.Max);

    ImGuiID opened_id = id + 999;
    bool popup_open = window->StateStorage.GetBool(opened_id, false);

    if (hovered && ImGui::IsMouseClicked(0)) {
        window->StateStorage.SetBool(opened_id, !popup_open);
        popup_open = !popup_open;
    }

    float hover_anim = window->StateStorage.GetFloat(id, 0.0f);
    hover_anim = CustomMath::Lerp(hover_anim, (hovered || popup_open) ? 1.0f : 0.0f, g.IO.DeltaTime * 10.0f);
    window->StateStorage.SetFloat(id, hover_anim);

    float open_anim = window->StateStorage.GetFloat(id + 1, 0.0f);
    open_anim = CustomMath::Lerp(open_anim, popup_open ? 1.0f : 0.0f, g.IO.DeltaTime * 12.0f);
    window->StateStorage.SetFloat(id + 1, open_anim);

    ImDrawList* draw = window->DrawList;
    ImU32 col_bg = ImGui::GetColorU32(ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    ImVec4 accent_v4 = ImVec4(Menu::AccentColor[0], Menu::AccentColor[1], Menu::AccentColor[2], 1.0f);

    draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), label, label_end);
    draw->AddRectFilled(frame_bb.Min, frame_bb.Max, col_bg, 3.0f);

    if (hover_anim > 0.01f) {
        ImVec4 border_col = accent_v4;
        border_col.w = hover_anim * 0.4f;
        draw->AddRect(frame_bb.Min, frame_bb.Max, ImGui::GetColorU32(border_col), 3.0f, 0, 1.0f);
    }

    const char* current_text = items[*current_item];
    ImVec2 text_size = ImGui::CalcTextSize(current_text);
    ImVec2 text_pos(frame_bb.Min.x + 10.0f, frame_bb.Min.y + (combo_height - text_size.y) * 0.5f);
    draw->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), current_text);

    bool value_changed = false;

    if (open_anim > 0.01f) {
        ImDrawList* fg_draw = ImGui::GetForegroundDrawList();

        float item_height = 20.0f;
        float list_height = item_height * items_count;

        ImVec2 dropdown_min = ImVec2(frame_bb.Min.x, frame_bb.Max.y + 2);
        ImVec2 dropdown_max = ImVec2(dropdown_min.x + frame_bb.GetWidth(), dropdown_min.y + (list_height * open_anim));

        fg_draw->AddRectFilled(dropdown_min, dropdown_max, col_bg, 3.0f);
        fg_draw->AddRect(dropdown_min, dropdown_max, ImGui::GetColorU32(ImVec4(0.15f, 0.15f, 0.15f, 1.0f)), 3.0f, 0, 1.0f);

        fg_draw->PushClipRect(dropdown_min, dropdown_max, true);

        for (int i = 0; i < items_count; i++) {
            bool is_selected = (*current_item == i);

            float item_y = dropdown_min.y + (i * item_height * open_anim);
            ImVec2 item_min(dropdown_min.x, item_y);
            ImVec2 item_max(dropdown_max.x, item_y + (item_height * open_anim));

            bool item_hovered = ImGui::IsMouseHoveringRect(item_min, item_max) && popup_open;

            ImGuiID item_id = id + 100 + i;
            float item_hover_anim = window->StateStorage.GetFloat(item_id, 0.0f);
            item_hover_anim = CustomMath::Lerp(item_hover_anim, item_hovered ? 1.0f : 0.0f, g.IO.DeltaTime * 12.0f);
            window->StateStorage.SetFloat(item_id, item_hover_anim);

            if (item_hover_anim > 0.01f) {
                ImU32 hover_bg = ImGui::GetColorU32(ImVec4(0.15f, 0.15f, 0.15f, item_hover_anim));
                fg_draw->AddRectFilled(item_min, item_max, hover_bg);
            }

            ImVec4 text_color;
            if (is_selected) {
                text_color = accent_v4;
            }
            else {
                text_color = CustomMath::Vec4Lerp(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ImVec4(0.9f, 0.9f, 0.9f, 1.0f), item_hover_anim);
            }

            float text_x = item_min.x + 10.0f + (5.0f * item_hover_anim);
            float text_y = item_min.y + ((item_height * open_anim) - ImGui::CalcTextSize(items[i]).y) * 0.5f;

            fg_draw->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(text_color), items[i]);

            if (item_hovered && ImGui::IsMouseClicked(0)) {
                *current_item = i;
                value_changed = true;
                window->StateStorage.SetBool(opened_id, false);
            }
        }

        fg_draw->PopClipRect();

        if (popup_open) {
            ImVec2 dropdown_full_max = ImVec2(dropdown_min.x + frame_bb.GetWidth(), dropdown_min.y + list_height);
            bool list_hovered = ImGui::IsMouseHoveringRect(dropdown_min, dropdown_full_max);
            if (!list_hovered && !hovered && ImGui::IsMouseClicked(0)) {
                window->StateStorage.SetBool(opened_id, false);
            }
        }
    }

    return value_changed;
}

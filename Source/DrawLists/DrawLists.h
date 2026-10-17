#pragma once

void Text(ImDrawList* drawlist, float x, float y, ImU32 Color, const char* text, bool outlined)
{
	if (outlined)
	{
		ImVec2 offsets[] = { {-1, 0}, {1, 0}, {0, -1}, {0, 1} };

		for (const auto& offset : offsets)
		{
			drawlist->AddText(ImVec2(x + offset.x, y + offset.y), (ImColor(0, 0, 0, 255)), text);
		}
	}

	drawlist->AddText(ImVec2(x, y), Color, text);
}
void DrawCorneredBox(int x, int y, int w, int h, const ImColor color, int thickness)
{
    auto draw = ImGui::GetForegroundDrawList();

    int outline = thickness + 2;
    ImColor black = ImColor(0, 0, 0, 255);

    draw->AddLine(ImVec2(x, y), ImVec2(x, y + (h / 3)), black, outline);
    draw->AddLine(ImVec2(x, y), ImVec2(x + (w / 3), y), black, outline);
    draw->AddLine(ImVec2(x + w - (w / 3), y), ImVec2(x + w, y), black, outline);
    draw->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + (h / 3)), black, outline);
    draw->AddLine(ImVec2(x, y + h - (h / 3)), ImVec2(x, y + h), black, outline);
    draw->AddLine(ImVec2(x, y + h), ImVec2(x + (w / 3), y + h), black, outline);
    draw->AddLine(ImVec2(x + w - (w / 3), y + h), ImVec2(x + w, y + h), black, outline);
    draw->AddLine(ImVec2(x + w, y + h - (h / 3)), ImVec2(x + w, y + h), black, outline);

    draw->AddLine(ImVec2(x, y), ImVec2(x, y + (h / 3)), color, thickness);
    draw->AddLine(ImVec2(x, y), ImVec2(x + (w / 3), y), color, thickness);
    draw->AddLine(ImVec2(x + w - (w / 3), y), ImVec2(x + w, y), color, thickness);
    draw->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + (h / 3)), color, thickness);
    draw->AddLine(ImVec2(x, y + h - (h / 3)), ImVec2(x, y + h), color, thickness);
    draw->AddLine(ImVec2(x, y + h), ImVec2(x + (w / 3), y + h), color, thickness);
    draw->AddLine(ImVec2(x + w - (w / 3), y + h), ImVec2(x + w, y + h), color, thickness);
    draw->AddLine(ImVec2(x + w, y + h - (h / 3)), ImVec2(x + w, y + h), color, thickness);
}
void DrawLine(const Vector2& start, const Vector2& end, ImU32 color = IM_COL32(255, 0, 0, 255), float thickness = 1.0f)
{

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    draw_list->AddLine(
        ImVec2(start.x, start.y),
        ImVec2(end.x, end.y),
        color,
        thickness
    );
}
void DrawLine(int x1, int y1, int x2, int y2, ImColor Color, int thickness)
{
    ImGui::GetForegroundDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), Color, thickness);
}
void DrawArrow(float x, float y, float angle, float size, ImU32 color) {
    float arrow_half_size = size / 2.0f;

    ImVec2 points[3];
    points[0] = ImVec2(x + cosf(angle) * size, y + sinf(angle) * size);
    points[1] = ImVec2(x + cosf(angle + 1.5f) * arrow_half_size, y + sinf(angle + 1.5f) * arrow_half_size);
    points[2] = ImVec2(x + cosf(angle - 1.5f) * arrow_half_size, y + sinf(angle - 1.5f) * arrow_half_size);

    ImGui::GetBackgroundDrawList()->AddTriangleFilled(points[0], points[1], points[2], color);
    ImGui::GetForegroundDrawList()->AddLine(points[0], points[1], color, 1.0f);
    ImGui::GetForegroundDrawList()->AddLine(points[1], points[2], color, 1.0f);
    ImGui::GetForegroundDrawList()->AddLine(points[2], points[0], color, 1.0f);
}

//void DrawPlayerArrow(const fvector& vOrigin, const ImColor& viscolor, int fov) {
//    frotator vAngle = CameraThread.Rotation;
//    float fYaw = vAngle.yaw * M_PI / 180.0f;
//    float dx = vOrigin.x - CameraThread.Location.x;
//    float dy = vOrigin.y - CameraThread.Location.y;
//
//    float fsin_yaw = sinf(fYaw);
//    float fminus_cos_yaw = -cosf(fYaw);
//
//    float x = dy * fminus_cos_yaw + dx * fsin_yaw;
//    x = -x;
//    float y = dx * fminus_cos_yaw - dy * fsin_yaw;
//
//    float length = sqrtf(x * x + y * y);
//    x /= length;
//    y /= length;
//
//    float angle = atan2f(y, x);
//    float fov_radius = fov + 8.0f;
//
//
//    float screen_center_x = Monitor.Width / 2.0f;
//    float screen_center_y = Monitor.Height / 2.0f;
//
//    fvector2d arrow_pos = { screen_center_x + x * fov_radius, screen_center_y + y * fov_radius };
//
//
//    DrawArrow(arrow_pos.x, arrow_pos.y, angle, 15.0f, viscolor);
//}

void FovCutsom(float r, bool filled, bool rainbow, bool toMouse, float rainbowSpeed)
{
    auto& io = ImGui::GetIO();

    ImVec2 center = toMouse ? ImVec2(io.MousePos.x, io.MousePos.y) : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    auto drawList = ImGui::GetBackgroundDrawList();
    int sides = 100;
    for (int i = 0; i < sides; ++i)
    {
        auto pos = center;
        float angle = (i / static_cast<float>(sides)) * 2 * M_PI;
        auto lastPos = ImVec2(pos.x + cos(angle) * r, pos.y + sin(angle) * r);
        auto nextPos = ImVec2(pos.x + cos(angle + 2 * M_PI / sides) * r, pos.y + sin(angle + 2 * M_PI / sides) * r);


        ImU32 currentColor = rainbow ? ImGui::ColorConvertFloat4ToU32(ImColor::HSV((fmod(ImGui::GetTime() * rainbowSpeed, 5.0f) / 5.0f - i / static_cast<float>(sides)) + 1.0f, 0.5f, 1.0f)) : IM_COL32(255, 255, 255, 255);

        ImU32 fillCol = filled ? ImGui::ColorConvertFloat4ToU32({ ImGui::ColorConvertU32ToFloat4(currentColor).x, ImGui::ColorConvertU32ToFloat4(currentColor).y, ImGui::ColorConvertU32ToFloat4(currentColor).z, 0.2f }) : 0; // 0.2f = fill opacity


        if (false)
        {
            drawList->AddLine(lastPos, nextPos, IM_COL32(0, 0, 0, 255), 2.5f); // outline 
        }

        drawList->AddLine(lastPos, nextPos, currentColor, 1.5f); // main 
    }
}

//void RiceHat(const fvector& head, float radius, float height, int segments, float tick) {
//    fvector tip = { head.x, head.y, head.z + height };
//
//    std::vector<ImVec2> basePoints2D;
//
//    for (int i = 0; i < segments; ++i) {
//        float angle = (2 * M_PI / segments) * i;
//        float x = cosf(angle) * radius;
//        float y = sinf(angle) * radius;
//
//        fvector basePoint3D = { head.x + x, head.y + y, head.z + 5 };
//        fvector2d screenBase = SDK::WorldToScreen(basePoint3D);
//        basePoints2D.push_back(ImVec2(screenBase.x, screenBase.y));
//    }
//
//    fvector2d screenTip = SDK::WorldToScreen(tip);
//    ImVec2 tip2D = ImVec2(screenTip.x, screenTip.y);
//
//    auto draw_list = ImGui::GetForegroundDrawList();
//
//    ImU32 cyan = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
//    ImU32 black = ImGui::GetColorU32(ImVec4(0, 0, 0, 1.0f));
//    float outlineThickness = tick + 2.0f;
//    for (int i = 0; i < segments; ++i) {
//        draw_list->AddLine(tip2D, basePoints2D[i], black, outlineThickness);
//        draw_list->AddLine(basePoints2D[i], basePoints2D[(i + 1) % segments], black, outlineThickness);
//    }
//    for (int i = 0; i < segments; ++i) {
//        draw_list->AddLine(tip2D, basePoints2D[i], cyan, tick);
//        draw_list->AddLine(basePoints2D[i], basePoints2D[(i + 1) % segments], cyan, tick);
//    }
//}
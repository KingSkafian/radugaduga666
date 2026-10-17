#pragma once
#include "../../../Source/Bootstrap.h"
namespace Overlays {

	bool HijackOverlay()
	{
		Monitor.Width = (GetSystemMetrics)(SM_CXSCREEN);
		Monitor.Height = (GetSystemMetrics)(SM_CYSCREEN);

		Monitor.WidthCenter = Monitor.Width / 2;
		Monitor.HeightCenter = Monitor.Height / 2;

		Overlay = FindWindowA((("Chrome_WidgetWin_1")), (("Discord Overlay")));
		if (!Overlay)
		{
			MessageBoxA(NULL, ("Please make sure the Discord overlay is enabled and R6S is in borderless mode."), ("Code : 0x04"), NULL);
			return false;
		}
		UpdateWindow(Overlay);
		ShowWindow(Overlay, (SW_SHOW));
		return true;
	}

}
/*#pragma once



void perform(Vector2 Head2D)
{
	float x = Head2D.x; float y = Head2D.y;
	float AimSpeed = Aimbot.smooth;

	Vector2 Target;

	if (x != 0.f)
	{
		Target.x = (x > Monitor.Width / 2) ? -(Monitor.Width / 2 - x) : x - Monitor.Width / 2;
		Target.x /= AimSpeed;
		Target.x = (x > Monitor.Width / 2 && Target.x + Monitor.Width / 2 > Monitor.Width / 2 * 2) ? 0 : Target.x;
		Target.x = (x < Monitor.Width / 2 && Target.x + Monitor.Width / 2 < 0) ? 0 : Target.x;
	}

	if (y != 0.f)
	{
		Target.y = (y > Monitor.Height / 2) ? -(Monitor.Height / 2 - y) : y - Monitor.Height / 2;
		Target.y /= AimSpeed;
		Target.y = (y > Monitor.Height / 2 && Target.y + Monitor.Height / 2 > Monitor.Height / 2 * 2) ? 0 : Target.y;
		Target.y = (y < Monitor.Height / 2 && Target.y + Monitor.Height / 2 < 0) ? 0 : Target.y;
	}

	INPUT input[1] = {};
	input[0].type = INPUT_MOUSE;
	input[0].mi.dx = static_cast<LONG>(Target.x);
	input[0].mi.dy = static_cast<LONG>(Target.y);
	input[0].mi.dwFlags = MOUSEEVENTF_MOVE;
	input[0].mi.time = 0;
	input[0].mi.dwExtraInfo = GetMessageExtraInfo();
	SendInput(1, input, sizeof(INPUT));

}
*/
#pragma once



void perform(Vector2 Head2D)
{
	float centerX = Monitor.Width / 2.0f;
	float centerY = Monitor.Height / 2.0f;

	float deltaX = Head2D.x - centerX;
	float deltaY = Head2D.y - centerY;

	// Dead zone — avoid micro-jitter when already on target
	if (fabsf(deltaX) < 3.0f && fabsf(deltaY) < 3.0f) return;

	float smoothFactor = fmaxf(1.0f, (float)Aimbot.smooth);

	float moveX = deltaX / smoothFactor;
	float moveY = deltaY / smoothFactor;

	if (Aimbot.UnlockYAxis)
		moveY = 0.0f;

	// Guarantee at least 1px of movement so it always creeps toward center
	int mx = (moveX != 0.f) ? (int)moveX + (moveX > 0 ? 1 : -1) : 0;
	int my = (moveY != 0.f) ? (int)moveY + (moveY > 0 ? 1 : -1) : 0;

	Recoil::MoveMouseTracked(mx, my);
}
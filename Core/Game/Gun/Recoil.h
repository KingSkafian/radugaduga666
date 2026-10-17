#pragma once
#include <cmath>

namespace Recoil
{
    Vector3 lastForward = { 0.f, 0.f, 0.f };
    int pendingX = 0;
    int pendingY = 0;
    bool initialized = false;
    float accumX = 0.f;
    float accumY = 0.f;

    float velX = 0.f;   // NEW: filtered recoil velocity
    float velY = 0.f;   // NEW: filtered recoil velocity

    inline bool MoveMouseTracked(int x, int y)
    {
        pendingX += x;
        pendingY += y;
        MoveMouse(x, y);
        return true;
    }

    inline void ForwardToAngles(const Vector3& fwd, float& yaw, float& pitch)
    {
        yaw = atan2f(fwd.y, fwd.x);
        pitch = atan2f(fwd.z, sqrtf(fwd.x * fwd.x + fwd.y * fwd.y));
    }

    inline float AngleDelta(float a, float b)
    {
        float d = a - b;
        while (d > 180.f) d -= 360.f;
        while (d < -180.f) d += 360.f;
        return d;
    }

    inline void Compensate(const Vector3& currentForward)
    {
        if (!Gun.Enabled || (Aimbot.Enabled && GetAsyncKeyState(VK_RBUTTON)))
        {
            lastForward = currentForward;
            accumX = accumY = 0.f;
            velX = velY = 0.f;
            initialized = false;
            return;
        }

        if (!initialized)
        {
            lastForward = currentForward;
            initialized = true;
            return;
        }

        float lastYaw, lastPitch, curYaw, curPitch;
        ForwardToAngles(lastForward, lastYaw, lastPitch);
        ForwardToAngles(currentForward, curYaw, curPitch);

        // unwrap deltas (prevents 180° spikes)
        float dYaw = AngleDelta(curYaw, lastYaw);
        float dPitch = AngleDelta(curPitch, lastPitch);

        // recoil in mouse space (apply sens once)
        float recoilX = dYaw * Gun.SensX;
        float recoilY = dPitch * Gun.SensY;

        // low-pass filter (removes noise)
        const float smooth = 0.25f;
        velX += (recoilX - velX) * smooth;
        velY += (recoilY - velY) * smooth;

        // accumulate fractional motion
        accumX += velX;
        accumY += velY;

        int outX = (int)std::round(accumX);
        int outY = (int)std::round(accumY);

        if (outX || outY)
        {
            MoveMouse(outX, -outY);
            accumX -= outX;
            accumY -= outY;
        }

        lastForward = currentForward;
    }
}
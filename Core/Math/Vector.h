#pragma once
#include <cmath>
#include <algorithm>
#include <numbers>
#include <string>

#define M_PI       3.14159265358979323846   // pi
#define M_RADPI 57.295779513082f

#define M_PI_F ((float)(M_PI))
#define RAD2DEG(x) ((float)(x) * (float)(180.f / M_PI_F))
#define DEG2RAD(x) ((float)(x) * (float)(M_PI_F / 180.f))
inline double GetCrossDistance(double x1, double y1, double x2, double y2) {
    return sqrtf(powf((x2 - x1), 2) + powf((y2 - y1), 2));
}
struct Vector2
{
    float x, y;

    Vector2(float x_val = 0.0f, float y_val = 0.0f) : x(x_val), y(y_val) {}

    Vector2 operator+(const Vector2& v) const { return { x + v.x, y + v.y }; }
    Vector2 operator-(const Vector2& v) const { return { x - v.x, y - v.y }; }
    Vector2 operator*(float scalar) const { return { x * scalar, y * scalar }; }
    Vector2 operator/(float scalar) const { return { x / scalar, y / scalar }; }

    Vector2& operator+=(const Vector2& v) { x += v.x; y += v.y; return *this; }
    Vector2& operator-=(const Vector2& v) { x -= v.x; y -= v.y; return *this; }

    bool operator==(const Vector2& v) const { return x == v.x && y == v.y; }
    bool operator!=(const Vector2& v) const { return !(*this == v); }

    float dot(const Vector2& v) const { return x * v.x + y * v.y; }
    bool empty() const { return x == 0.0f && y == 0.0f; }
    float magnitude() const { return std::sqrt(x * x + y * y); }
    Vector2 normalize() const {
        float mag = magnitude();
        return (mag == 0) ? Vector2{ 0, 0 } : Vector2{ x / mag, y / mag };
    }
    float distance(const Vector2& v) const { return (*this - v).magnitude(); }
};

struct Vector3
{
    float x, y, z;

    Vector3(float x_val = 0.0f, float y_val = 0.0f, float z_val = 0.0f) : x(x_val), y(y_val), z(z_val) {}

    float& operator[](int i) { return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }

    Vector3 operator+(const Vector3& v) const { return { x + v.x, y + v.y, z + v.z }; }
    Vector3 operator-(const Vector3& v) const { return { x - v.x, y - v.y, z - v.z }; }
    Vector3 operator*(float scalar) const { return { x * scalar, y * scalar, z * scalar }; }
    Vector3 operator/(float scalar) const { return { x / scalar, y / scalar, z / scalar }; }

    Vector3& operator+=(const Vector3& v) { x += v.x; y += v.y; z += v.z; return *this; }
    Vector3& operator-=(const Vector3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    Vector3& operator/=(float scalar) { x /= scalar; y /= scalar; z /= scalar; return *this; }

    Vector3 operator-() const { return { -x, -y, -z }; }

    bool operator==(const Vector3& v) const { return x == v.x && y == v.y && z == v.z; }
    bool operator!=(const Vector3& v) const { return !(*this == v); }

    float dot(const Vector3& v) const { return x * v.x + y * v.y + z * v.z; }
    float magnitude() const { return std::sqrt(x * x + y * y + z * z); }
    float distance(const Vector3& v) const { return (*this - v).magnitude(); }
    Vector3 cross(const Vector3& v) const {
        return {
            y * v.z - z * v.y,
            z * v.x - x * v.z,
            x * v.y - y * v.x
        };
    }
    Vector3 normalize() const {
        float len = magnitude();
        return (len == 0.0f) ? Vector3{ 0.0f, 0.0f, 0.0f } : (*this / len);
    }

    bool empty() const { return x == 0.0f && y == 0.0f && z == 0.0f; }
    std::string to_string() const {
        return "(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")";
    }
};
struct Vector4
{
    float x, y, z, w;
    Vector4(float x_val = 0.0f, float y_val = 0.0f, float z_val = 0.0f, float w_val = 0.0f) : x(x_val), y(y_val), z(z_val), w(w_val) {}
    float& operator[](int i) { return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }
    Vector4 operator+(const Vector4& v) const { return { x + v.x, y + v.y, z + v.z, w + v.w }; }
    Vector4 operator-(const Vector4& v) const { return { x - v.x, y - v.y, z - v.z, w - v.w }; }
    Vector4 operator*(float scalar) const { return { x * scalar, y * scalar, z * scalar, w * scalar }; }
    Vector4 operator/(float scalar) const { return { x / scalar, y / scalar, z / scalar, w / scalar }; }
    Vector4& operator+=(const Vector4& v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
    Vector4& operator-=(const Vector4& v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
    Vector4& operator*=(float scalar) { x *= scalar; y *= scalar; z *= scalar; w *= scalar; return *this; }
    Vector4& operator/=(float scalar) { x /= scalar; y /= scalar; z /= scalar; w /= scalar; return *this; }
    Vector4 operator-() const { return { -x, -y, -z, -w }; }
    bool operator==(const Vector4& v) const { return x == v.x && y == v.y && z == v.z && w == v.w; }
    bool operator!=(const Vector4& v) const { return !(*this == v); }
    float dot(const Vector4& v) const { return x * v.x + y * v.y + z * v.z + w * v.w; }
    float magnitude() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    float distance(const Vector4& v) const { return (*this - v).magnitude(); }
    Vector4 normalize() const {
        float len = magnitude();
        return (len == 0.0f) ? Vector4{ 0.0f, 0.0f, 0.0f, 0.0f } : (*this / len);
    }
    bool empty() const { return x == 0.0f && y == 0.0f && z == 0.0f && w == 0.0f; }
    Vector3 xyz() const { return { x, y, z }; }
    std::string to_string() const {
        return "(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ", " + std::to_string(w) + ")";
    }
};
struct Matrix4 {
    float m[16];
};
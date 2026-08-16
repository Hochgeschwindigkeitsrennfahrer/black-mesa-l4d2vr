#pragma once

class Vector;
class QAngle;
class Color;
class OverlayText_t;

class IVDebugOverlay
{
public:
    virtual void AddEntityTextOverlay(int ent_index, int line_offset, float duration, int r, int g, int b, int a, const char* format, ...) = 0;
    virtual void AddBoxOverlay(const Vector& origin, const Vector& mins, const Vector& max, const QAngle& orientation, int r, int g, int b, int a, float duration) = 0;
    virtual void AddTriangleOverlay(const Vector& p1, const Vector& p2, const Vector& p3, int r, int g, int b, int a, bool noDepthTest, float duration) = 0;
    virtual void AddLineOverlay(const Vector& origin, const Vector& dest, int r, int g, int b, bool noDepthTest, float duration) = 0;
    virtual void AddTextOverlay(const Vector& origin, float duration, const char* format, ...) = 0;
    virtual void AddTextOverlay(const Vector& origin, int line_offset, float duration, const char* format, ...) = 0;
    virtual void AddScreenTextOverlay(float flXPos, float flYPos, float flDuration, int r, int g, int b, int a, const char* text) = 0;
    virtual void AddSweptBoxOverlay(const Vector& start, const Vector& end, const Vector& mins, const Vector& max, const QAngle& angles, int r, int g, int b, int a, float flDuration) = 0;
    virtual void AddGridOverlay(const Vector& origin) = 0;
    virtual int ScreenPosition(const Vector& point, Vector& screen) = 0;
    virtual int ScreenPosition(float flXPos, float flYPos, Vector& screen) = 0;
    virtual OverlayText_t* GetFirst(void) = 0;
    virtual OverlayText_t* GetNext(OverlayText_t* current) = 0;
    virtual void ClearDeadOverlays(void) = 0;
    virtual void ClearAllOverlays() = 0;
    virtual void AddTextOverlayRGB(const Vector& origin, int line_offset, float duration, float r, float g, float b, float alpha, const char* format, ...) = 0;
    virtual void AddTextOverlayRGB(const Vector& origin, int line_offset, float duration, int r, int g, int b, int a, const char* format, ...) = 0;
    virtual void AddLineOverlayAlpha(const Vector& origin, const Vector& dest, int r, int g, int b, int a, bool noDepthTest, float duration) = 0;
};

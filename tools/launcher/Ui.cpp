#include "Ui.hpp"

#include <algorithm>
#include <objidl.h>

using std::max;
using std::min;
#include <gdiplus.h>

namespace ui {

namespace {

int g_dpi = 96;
ULONG_PTR g_gdiplusToken = 0;
HFONT g_fonts[kFontCount] = {};

Gdiplus::Color ToColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

Gdiplus::RectF ToRectF(const RECT& rect) {
    return Gdiplus::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
                          static_cast<float>(rect.bottom - rect.top));
}

void RoundedPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    const float diameter = std::min({radius * 2.0f, rect.Width, rect.Height});
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

HFONT MakeFont(int pixels, int weight, const wchar_t* face) {
    return CreateFontW(-S(pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

}  // namespace

void Init() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetAware = BOOL(WINAPI*)();
        if (auto setAware = reinterpret_cast<SetAware>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDPIAware")))) setAware();
    }
    if (HDC screen = GetDC(nullptr)) {
        g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    g_fonts[kFontTitle] = MakeFont(30, FW_SEMIBOLD, L"Segoe UI");
    g_fonts[kFontSubtitle] = MakeFont(13, FW_NORMAL, L"Segoe UI");
    g_fonts[kFontBody] = MakeFont(14, FW_NORMAL, L"Segoe UI");
    g_fonts[kFontSmall] = MakeFont(13, FW_NORMAL, L"Segoe UI");
    g_fonts[kFontCardTitle] = MakeFont(12, FW_BOLD, L"Segoe UI");
    g_fonts[kFontButton] = MakeFont(14, FW_SEMIBOLD, L"Segoe UI");
    g_fonts[kFontPlay] = MakeFont(24, FW_BOLD, L"Segoe UI");
    g_fonts[kFontMono] = MakeFont(13, FW_NORMAL, L"Consolas");
}

void Shutdown() {
    for (HFONT& font : g_fonts) {
        if (font) DeleteObject(font);
        font = nullptr;
    }
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

int S(int value) {
    return MulDiv(value, g_dpi, 96);
}

RECT Rect(int x, int y, int width, int height) {
    return RECT{S(x), S(y), S(x + width), S(y + height)};
}

HFONT FontHandle(Font font) {
    return g_fonts[font];
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, bool withBorder, BYTE fillAlpha) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF area = ToRectF(rect);
    area.X += 0.5f;
    area.Y += 0.5f;
    area.Width -= 1.0f;
    area.Height -= 1.0f;
    Gdiplus::GraphicsPath path;
    RoundedPath(path, area, static_cast<float>(radius));
    Gdiplus::SolidBrush brush(ToColor(fill, fillAlpha));
    graphics.FillPath(&brush, &path);
    if (withBorder) {
        Gdiplus::Pen pen(ToColor(border), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void FillRoundGradient(HDC dc, const RECT& rect, int radius, COLORREF left, COLORREF right) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF area = ToRectF(rect);
    area.X += 0.5f;
    area.Y += 0.5f;
    area.Width -= 1.0f;
    area.Height -= 1.0f;
    Gdiplus::GraphicsPath path;
    RoundedPath(path, area, static_cast<float>(radius));
    Gdiplus::LinearGradientBrush brush(area, ToColor(left), ToColor(right), Gdiplus::LinearGradientModeHorizontal);
    graphics.FillPath(&brush, &path);
}

void HeaderBackdrop(HDC dc, const RECT& destination, int fullWidth, int fullHeight, int originX, int originY) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetClip(Gdiplus::Rect(destination.left, destination.top, destination.right - destination.left, destination.bottom - destination.top));
    const Gdiplus::RectF full(static_cast<float>(-originX), static_cast<float>(-originY), static_cast<float>(fullWidth), static_cast<float>(fullHeight));
    Gdiplus::LinearGradientBrush body(full, ToColor(kHeaderLeft), ToColor(kHeaderRight), Gdiplus::LinearGradientModeHorizontal);
    graphics.FillRectangle(&body, full);
    const float line = static_cast<float>(S(3));
    const Gdiplus::RectF accent(full.X, full.Y + full.Height - line, full.Width, line);
    Gdiplus::LinearGradientBrush stripe(accent, ToColor(kViolet), ToColor(kBlue), Gdiplus::LinearGradientModeHorizontal);
    graphics.FillRectangle(&stripe, accent);
}

void FillEllipse(HDC dc, const RECT& box, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(ToColor(color));
    graphics.FillEllipse(&brush, ToRectF(box));
}

void FillTriangle(HDC dc, const RECT& box, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::PointF points[3] = {{static_cast<float>(box.left), static_cast<float>(box.top)},
                                       {static_cast<float>(box.right), (box.top + box.bottom) / 2.0f},
                                       {static_cast<float>(box.left), static_cast<float>(box.bottom)}};
    Gdiplus::SolidBrush brush(ToColor(color));
    graphics.FillPolygon(&brush, points, 3);
}

void DrawCheck(HDC dc, const RECT& box, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float w = static_cast<float>(box.right - box.left);
    const float h = static_cast<float>(box.bottom - box.top);
    Gdiplus::Pen pen(ToColor(color), std::max(2.0f, w / 9.0f));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const Gdiplus::PointF points[3] = {{box.left + w * 0.22f, box.top + h * 0.54f}, {box.left + w * 0.43f, box.top + h * 0.74f}, {box.left + w * 0.78f, box.top + h * 0.28f}};
    graphics.DrawLines(&pen, points, 3);
}

void DrawChevron(HDC dc, const RECT& box, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float w = static_cast<float>(box.right - box.left);
    const float h = static_cast<float>(box.bottom - box.top);
    Gdiplus::Pen pen(ToColor(color), std::max(1.6f, w / 7.0f));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const Gdiplus::PointF points[3] = {{box.left + w * 0.1f, box.top + h * 0.3f}, {box.left + w * 0.5f, box.top + h * 0.72f}, {box.left + w * 0.9f, box.top + h * 0.3f}};
    graphics.DrawLines(&pen, points, 3);
}

void DrawTrack(HDC dc, const RECT& track, int filledRight, COLORREF filled, COLORREF empty) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float radius = (track.bottom - track.top) / 2.0f;
    Gdiplus::GraphicsPath full;
    RoundedPath(full, ToRectF(track), radius);
    Gdiplus::SolidBrush emptyBrush(ToColor(empty));
    graphics.FillPath(&emptyBrush, &full);
    if (filledRight > track.left) {
        RECT part = track;
        part.right = std::min<LONG>(filledRight, track.right);
        Gdiplus::GraphicsPath partPath;
        RoundedPath(partPath, ToRectF(part), radius);
        Gdiplus::LinearGradientBrush brush(ToRectF(track), ToColor(kViolet), ToColor(kBlue), Gdiplus::LinearGradientModeHorizontal);
        graphics.FillPath(&brush, &partPath);
    }
    (void)filled;
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, Font font, COLORREF color, UINT flags) {
    const int previousMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColor = SetTextColor(dc, color);
    HGDIOBJ previousFont = SelectObject(dc, g_fonts[font]);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, previousFont);
    SetTextColor(dc, previousColor);
    SetBkMode(dc, previousMode);
}

void StyleWindow(HWND window) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (setAttribute) {
        const BOOL on = TRUE;
        setAttribute(window, 20, &on, sizeof on);
        const COLORREF caption = kBackground;
        setAttribute(window, 35, &caption, sizeof caption);
        const COLORREF text = kText;
        setAttribute(window, 36, &text, sizeof text);
    }
    FreeLibrary(dwm);
}

void StyleScrollBars(HWND control) {
    HMODULE theme = LoadLibraryW(L"uxtheme.dll");
    if (!theme) return;
    using SetTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    if (auto setTheme = reinterpret_cast<SetTheme>(reinterpret_cast<void*>(GetProcAddress(theme, "SetWindowTheme")))) setTheme(control, L"DarkMode_Explorer", nullptr);
    FreeLibrary(theme);
}

}  // namespace ui

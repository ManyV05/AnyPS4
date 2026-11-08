#pragma once

#include <windows.h>

#include <string>

// Drawing helpers for the launcher's dark theme: anti-aliased rounded shapes (GDI+), text (GDI), DPI scaling and fonts.
namespace ui {

constexpr COLORREF kBackground = RGB(15, 15, 22);
constexpr COLORREF kCard = RGB(24, 24, 36);
constexpr COLORREF kCardBorder = RGB(44, 44, 66);
constexpr COLORREF kField = RGB(17, 17, 27);
constexpr COLORREF kFieldBorder = RGB(52, 52, 78);
constexpr COLORREF kButton = RGB(38, 38, 58);
constexpr COLORREF kButtonHover = RGB(52, 52, 80);
constexpr COLORREF kText = RGB(234, 234, 246);
constexpr COLORREF kMuted = RGB(144, 146, 174);
constexpr COLORREF kDisabled = RGB(86, 88, 112);
constexpr COLORREF kViolet = RGB(139, 92, 246);
constexpr COLORREF kVioletLight = RGB(176, 148, 255);
constexpr COLORREF kBlue = RGB(56, 130, 255);
constexpr COLORREF kGreen = RGB(52, 211, 120);
constexpr COLORREF kAmber = RGB(250, 190, 40);
constexpr COLORREF kHeaderLeft = RGB(58, 28, 120);
constexpr COLORREF kHeaderRight = RGB(24, 52, 138);

enum Font { kFontTitle, kFontSubtitle, kFontBody, kFontSmall, kFontCardTitle, kFontButton, kFontPlay, kFontMono, kFontCount };

void Init();
void Shutdown();

int S(int value);  // scales a 96-dpi pixel count to the screen's dpi
RECT Rect(int x, int y, int width, int height);  // scaled
HFONT FontHandle(Font font);

void FillSolid(HDC dc, const RECT& rect, COLORREF color);
void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, bool withBorder = true, BYTE fillAlpha = 255);
void FillRoundGradient(HDC dc, const RECT& rect, int radius, COLORREF left, COLORREF right);
// The header gradient (and its accent line) as it would look in a window of the given size, clipped to destination.
void HeaderBackdrop(HDC dc, const RECT& destination, int fullWidth, int fullHeight, int originX, int originY);
void FillEllipse(HDC dc, const RECT& box, COLORREF color);
void FillTriangle(HDC dc, const RECT& box, COLORREF color);  // pointing right
void DrawCheck(HDC dc, const RECT& box, COLORREF color);
void DrawTrack(HDC dc, const RECT& track, int filledRight, COLORREF filled, COLORREF empty);
void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, Font font, COLORREF color, UINT flags);

void StyleWindow(HWND window);       // dark title bar where the system supports it
void StyleScrollBars(HWND control);  // dark scroll bars where the system supports it

}  // namespace ui

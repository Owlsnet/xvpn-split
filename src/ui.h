// The window's contents: theme, widgets and the four views.
#pragma once
#include "common.h"

// Unscaled sizes that the window procedure needs for hit testing.
constexpr float kHeaderHeight       = 44.0f;   // the header strip is the drag area
constexpr float kWindowButtonsWidth = 124.0f;  // pin / minimise / close at its right end

enum UiTab { TAB_DASHBOARD = 0, TAB_APPS, TAB_EXCEPTIONS, TAB_PROXY };

void  UiInit(HWND hwnd, float dpi, int startTab);   // after ImGui::CreateContext
void  UiSetDpi(float dpi);                           // takes effect at the next UiPrepare
float UiDpi();
void  UiPrepare();                                   // before ImGui::NewFrame
void  UiFrame();                                     // between NewFrame and Render

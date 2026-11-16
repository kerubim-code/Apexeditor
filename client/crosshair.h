#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace apex::crosshair {

constexpr int kGridSize = 32;

bool Create();
void Destroy();
void Tick();
HWND Window();

int  GridSize();
bool GridPixel(int x, int y);
void SetGridPixel(int x, int y, bool on);
void ClearGrid();
void SaveGrid();
void ReloadGrid();

void  Toggle();
void  SetEnabled(bool on);
bool  Enabled();
int   Shape();
void  SetShape(int shape);
int   Size();
void  SetSize(int size);
int   Thickness();
void  SetThickness(int thickness);
float* Rgb();

}

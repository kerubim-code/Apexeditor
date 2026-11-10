#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace apex::crosshair {

bool Create();
void Destroy();
void Tick();
HWND Window();

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

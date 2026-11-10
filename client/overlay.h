#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace apex::overlay {

bool Create();
void Destroy();
void Tick();
HWND Window();

void Toggle();
void Show(bool on);
bool Visible();

int         Count();
const char* Name(int index);
int         Type(int index);
int         Selected();
void        Select(int index);
void        Deselect();
void        RemoveSelected();
float       Opacity();
void        SetOpacity(float opacity);

bool AddFromDialog(int type);

bool  ClickThrough();
void  SetClickThrough(bool on);
bool  Locked();
void  SetLocked(bool on);

int  ResolutionIndex();
void SetResolutionIndex(int index);
void Resolution(int* w, int* h);
void Place(int mode);

bool        SaveDialog(int qtKeybind);
bool        LoadDialog(int* qtKeybind);
const char* LastPath();

int QtKeyToVk(int qtKey);
int VkToQtKey(int vk);

}

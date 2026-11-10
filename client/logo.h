#pragma once

#include <d3d11.h>

#include "imgui.h"

namespace apex::logo {

bool Load(ID3D11Device* device, const char* path);

bool LoadFromMemory(ID3D11Device* device, const void* data, size_t size);

void Release();

ImTextureID Texture();

bool Ready();

bool FromDisk();

}

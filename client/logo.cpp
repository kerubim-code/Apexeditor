#include "logo.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace apex::logo {

namespace {

ID3D11ShaderResourceView* g_srv = nullptr;
bool                        g_fromDisk = false;

std::wstring Widen(const char* text) {
    if (text == nullptr || *text == '\0')
        return std::wstring();
    const int needed =
        MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (needed <= 1)
        return std::wstring();
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), needed);
    return out;
}

bool Upload(ID3D11Device* device, IWICImagingFactory* factory,
            IWICBitmapDecoder* decoder) {
    if (device == nullptr || factory == nullptr || decoder == nullptr)
        return false;

    IWICBitmapFrameDecode*  frame     = nullptr;
    IWICFormatConverter*    converter = nullptr;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);

    UINT width = 0, height = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);

    std::vector<unsigned char> pixels;
    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        const size_t bytes = static_cast<size_t>(width) * height * 4u;
        pixels.resize(bytes);
        hr = converter->CopyPixels(nullptr, width * 4u,
                                   static_cast<UINT>(bytes), pixels.data());
    }

    if (converter != nullptr) converter->Release();
    if (frame != nullptr)     frame->Release();

    if (FAILED(hr) || pixels.empty())
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem     = pixels.data();
    initial.SysMemPitch = width * 4u;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || texture == nullptr)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format                    = desc.Format;
    view.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels       = 1;

    const HRESULT created = device->CreateShaderResourceView(texture, &view, &g_srv);
    texture->Release();

    if (FAILED(created)) {
        g_srv = nullptr;
        return false;
    }
    return true;
}

IWICImagingFactory* MakeFactory() {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        factory == nullptr)
        return nullptr;
    return factory;
}

}

bool Load(ID3D11Device* device, const char* path) {
    Release();

    if (device == nullptr || path == nullptr || *path == '\0')
        return false;

    IWICImagingFactory* factory = MakeFactory();
    if (factory == nullptr)
        return false;

    IWICBitmapDecoder* decoder = nullptr;
    const HRESULT hr = factory->CreateDecoderFromFilename(
        Widen(path).c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    const bool ok = SUCCEEDED(hr) && Upload(device, factory, decoder);

    if (decoder != nullptr) decoder->Release();
    factory->Release();
    g_fromDisk = ok;
    return ok;
}

bool LoadFromMemory(ID3D11Device* device, const void* data, size_t size) {
    Release();
    g_fromDisk = false;

    if (device == nullptr || data == nullptr || size == 0 ||
        size > static_cast<size_t>(0xFFFFFFFFu))
        return false;

    IWICImagingFactory* factory = MakeFactory();
    if (factory == nullptr)
        return false;

    bool ok = false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (mem != nullptr) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, data, size);
            GlobalUnlock(mem);

            IStream* stream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(mem, TRUE, &stream)) &&
                stream != nullptr) {
                IWICBitmapDecoder* decoder = nullptr;
                const HRESULT hr = factory->CreateDecoderFromStream(
                    stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
                ok = SUCCEEDED(hr) && Upload(device, factory, decoder);
                if (decoder != nullptr) decoder->Release();
                stream->Release();
            } else {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }

    factory->Release();
    return ok;
}

void Release() {
    if (g_srv != nullptr) {
        g_srv->Release();
        g_srv = nullptr;
    }
}

ImTextureID Texture() {
    return g_srv != nullptr ? reinterpret_cast<ImTextureID>(g_srv) : ImTextureID_Invalid;
}

bool Ready() {
    return g_srv != nullptr;
}

bool FromDisk() {
    return g_fromDisk && g_srv != nullptr;
}

}

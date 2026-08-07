// WIC (Windows Imaging Component)-based implementation of
// load_texture_image() from TextureLoader.hpp. Decodes the source image
// and converts it to 32bpp RGBA before copying pixels out, so callers
// always get a consistent 4-channel byte buffer regardless of the source
// format.

#include "Hollow/TextureLoader.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <stdexcept>
#include <string>

namespace Hollow {
namespace {

class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(IUnknown* ptr) : ptr_(ptr) {}
    ~ComPtr() {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    template <typename T>
    T** put() {
        return reinterpret_cast<T**>(&ptr_);
    }

    template <typename T>
    T* get() const {
        return reinterpret_cast<T*>(ptr_);
    }

private:
    IUnknown* ptr_ = nullptr;
};

void check_hresult(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

} // namespace

TextureImage load_texture_image(const std::filesystem::path& path) {
    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coinit) && coinit != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("COM initialization failed for texture loading");
    }

    ComPtr factory;
    check_hresult(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put<IWICImagingFactory>())),
        "could not create WIC imaging factory");

    ComPtr decoder;
    check_hresult(
        factory.get<IWICImagingFactory>()->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put<IWICBitmapDecoder>()),
        "could not open texture file");

    ComPtr frame;
    check_hresult(decoder.get<IWICBitmapDecoder>()->GetFrame(0, frame.put<IWICBitmapFrameDecode>()), "could not read texture frame");

    ComPtr converter;
    check_hresult(factory.get<IWICImagingFactory>()->CreateFormatConverter(converter.put<IWICFormatConverter>()), "could not create texture converter");
    check_hresult(
        converter.get<IWICFormatConverter>()->Initialize(
            frame.get<IWICBitmapFrameDecode>(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        "could not convert texture to RGBA");

    UINT width = 0;
    UINT height = 0;
    check_hresult(converter.get<IWICFormatConverter>()->GetSize(&width, &height), "could not get texture size");
    if (width == 0 || height == 0) {
        throw std::runtime_error("texture has no pixels");
    }

    TextureImage image;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    check_hresult(
        converter.get<IWICFormatConverter>()->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()),
        "could not copy texture pixels");
    return image;
}

} // namespace Hollow

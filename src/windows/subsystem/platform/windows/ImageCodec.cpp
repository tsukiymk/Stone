#include <jni.h>
#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>

namespace {
using Microsoft::WRL::ComPtr;

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ComApartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

jintArray JNICALL DecodeWebp(JNIEnv* env, jclass, jbyteArray data)
{
    if (!data)
        return nullptr;
    const jsize length = env->GetArrayLength(data);
    if (length < 12)
        return nullptr;
    jbyte header[12];
    env->GetByteArrayRegion(data, 0, sizeof(header), header);
    if (env->ExceptionCheck() || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WEBP", 4) != 0)
        return nullptr;

    const ComApartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return nullptr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return nullptr;

    jbyte* encoded = env->GetByteArrayElements(data, nullptr);
    if (!encoded)
        return nullptr;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(encoded), static_cast<UINT>(length)));
    env->ReleaseByteArrayElements(data, encoded, JNI_ABORT);
    if (!stream)
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;

    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height)
        return nullptr;
    const auto count = static_cast<unsigned long long>(width) * height;
    if (count > std::numeric_limits<UINT>::max() / sizeof(jint) || count > std::numeric_limits<jsize>::max() - 2)
        return nullptr;

    jintArray output = env->NewIntArray(static_cast<jsize>(count + 2));
    if (!output)
        return nullptr;
    jint* pixels = env->GetIntArrayElements(output, nullptr);
    if (!pixels)
        return nullptr;
    // Straight BGRA bytes are 0xAARRGGBB integers on little-endian Windows.
    pixels[0] = static_cast<jint>(width);
    pixels[1] = static_cast<jint>(height);
    const HRESULT status =
        converter->CopyPixels(nullptr, static_cast<UINT>(width * sizeof(jint)), static_cast<UINT>(count * sizeof(jint)),
                              reinterpret_cast<BYTE*>(pixels + 2));
    env->ReleaseIntArrayElements(output, pixels, SUCCEEDED(status) ? 0 : JNI_ABORT);
    return SUCCEEDED(status) ? output : nullptr;
}
} // namespace

bool RegisterWindowsImageCodec(JNIEnv* env)
{
    const jclass codec = env->FindClass("com/trebledot/stone/loader/NativeImageCodec");
    if (!codec)
        return false;
    const JNINativeMethod methods[]{
        {const_cast<char*>("decode"), const_cast<char*>("([B)[I"), reinterpret_cast<void*>(DecodeWebp)}};
    const bool registered = env->RegisterNatives(codec, methods, 1) == JNI_OK;
    env->DeleteLocalRef(codec);
    return registered;
}

#include "stub-libandroid-fonts.h"
#include "stub-libc.h"

#include <windows.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <xmllite.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace stone::windows {
namespace {
std::filesystem::path systemFonts;
bool imageFonts = false;

struct Font {
    std::string path;
    std::string locale;
    uint16_t weight = 400;
    bool italic = false;
    size_t index = 0;
    std::vector<std::pair<uint32_t, float>> axes;
    bool operator==(const Font&) const = default;
};

struct FontIterator {
    std::vector<Font> fonts;
    size_t next = 0;
};

std::string Utf8(std::wstring_view text)
{
    auto result = std::filesystem::path(text).u8string();
    return {result.begin(), result.end()};
}

std::wstring Attribute(IXmlReader& reader, const wchar_t* name)
{
    if (reader.MoveToAttributeByName(name, nullptr) != S_OK)
        return {};
    const wchar_t* value = nullptr;
    UINT length = 0;
    std::wstring result;
    if (reader.GetValue(&value, &length) == S_OK)
        result.assign(value, length);
    reader.MoveToElement();
    return result;
}

template <typename T> T Number(std::wstring_view text, T fallback)
{
    auto bytes = Utf8(text);
    T value{};
    const auto end = bytes.data() + bytes.size();
    const auto result = std::from_chars(bytes.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end ? value : fallback;
}

void ReadAxis(IXmlReader& reader, Font& font)
{
    auto tag = Attribute(reader, L"tag");
    if (tag.size() != 4)
        return;
    uint32_t packedTag = 0;
    for (wchar_t character : tag) {
        if (character < 0x20 || character > 0x7e)
            return;
        packedTag = (packedTag << 8) | character;
    }
    const float value = Number(Attribute(reader, L"stylevalue"), std::numeric_limits<float>::quiet_NaN());
    if (std::isfinite(value))
        font.axes.emplace_back(packedTag, value);
}

std::vector<Font> ReadFonts(std::string_view config, std::string_view prefix, std::wstring_view rootName)
{
    using Microsoft::WRL::ComPtr;
    const auto configPath = AndroidSystemFontPath(config);
    if (configPath.empty())
        return {};
    ComPtr<IStream> input;
    if (FAILED(SHCreateStreamOnFileEx(configPath.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                                      FALSE, nullptr, input.GetAddressOf())))
        return {};
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(IID_PPV_ARGS(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(input.Get())))
        return {};

    constexpr UINT Outside = std::numeric_limits<UINT>::max();
    UINT family = Outside, face = Outside;
    std::vector<Font> fonts;
    std::string locale;
    std::wstring filename;
    Font font;
    XmlNodeType type{};
    HRESULT status;
    while ((status = reader->Read(&type)) == S_OK) {
        UINT depth = 0;
        if (FAILED(reader->GetDepth(&depth)))
            return {};
        if (type == XmlNodeType_Element || type == XmlNodeType_EndElement) {
            const wchar_t* name = nullptr;
            UINT length = 0;
            if (FAILED(reader->GetLocalName(&name, &length)))
                return {};
            const std::wstring_view element(name, length);
            if (type == XmlNodeType_Element) {
                if (depth == 0 && element != rootName)
                    return {};
                if (element == L"family" && !reader->IsEmptyElement()) {
                    family = depth;
                    locale = Utf8(Attribute(*reader.Get(), L"lang"));
                }
                else if (family != Outside && depth == family + 1 && element == L"font" && !reader->IsEmptyElement()) {
                    face = depth;
                    filename.clear();
                    font = Font{};
                    font.locale = locale;
                    font.weight = Number<uint16_t>(Attribute(*reader.Get(), L"weight"), 400);
                    font.index = Number<size_t>(Attribute(*reader.Get(), L"index"), 0);
                    font.italic = Attribute(*reader.Get(), L"style") == L"italic";
                }
                else if (face != Outside && depth == face + 1 && element == L"axis") {
                    ReadAxis(*reader.Get(), font);
                }
            }
            else if (element == L"font" && face != Outside) {
                face = Outside;
                const auto first = filename.find_first_not_of(L" \t\r\n");
                if (first != filename.npos) {
                    const auto last = filename.find_last_not_of(L" \t\r\n");
                    font.path = std::string(prefix) + Utf8(std::wstring_view(filename).substr(first, last - first + 1));
                    std::error_code error;
                    const auto file = AndroidSystemFontPath(font.path);
                    if (!file.empty() && std::filesystem::is_regular_file(file, error) &&
                        std::ranges::find(fonts, font) == fonts.end()) {
                        fonts.push_back(std::move(font));
                    }
                }
            }
            else if (element == L"family") {
                family = Outside;
            }
        }
        else if (face != Outside && depth == face + 1 && (type == XmlNodeType_Text || type == XmlNodeType_CDATA)) {
            const wchar_t* text = nullptr;
            UINT length = 0;
            if (FAILED(reader->GetValue(&text, &length)))
                return {};
            filename.append(text, length);
        }
    }
    if (status != S_FALSE)
        return {};
    return fonts;
}

FontIterator* OpenFontIterator() noexcept
{
    try {
        auto fonts = ReadFonts("/system/etc/font_fallback.xml", "/system/fonts/", L"familyset");
        if (fonts.empty())
            fonts = ReadFonts("/system/etc/fonts.xml", "/system/fonts/", L"familyset");
        auto customization =
            ReadFonts("/product/etc/fonts_customization.xml", "/product/fonts/", L"fonts-modification");
        for (auto& font : customization)
            fonts.push_back(std::move(font));
        return new FontIterator{std::move(fonts)};
    }
    catch (const std::bad_alloc&) {
        return nullptr;
    }
    catch (const std::filesystem::filesystem_error&) {
        return nullptr;
    }
}

Font* NextFont(FontIterator* iterator) noexcept
{
    if (!iterator || iterator->next == iterator->fonts.size())
        return nullptr;
    return new (std::nothrow) Font(std::move(iterator->fonts[iterator->next++]));
}
}

void SetAndroidSystemFonts(std::wstring path)
{
    systemFonts = std::filesystem::path(std::move(path)).lexically_normal().make_preferred();
    std::error_code error;
    imageFonts = std::filesystem::is_regular_file(systemFonts / L".ready", error);
}

std::filesystem::path AndroidSystemFontPath(std::string_view path)
{
    if (systemFonts.empty())
        return {};
    if (path == "/system/etc/fonts.xml") {
        return imageFonts ? systemFonts / L"etc" / L"fonts.xml" : systemFonts / L"fonts.xml";
    }
    if (imageFonts && path == "/system/etc/font_fallback.xml")
        return systemFonts / L"etc" / L"font_fallback.xml";
    if (imageFonts && path == "/product/etc/fonts_customization.xml") {
        return systemFonts / L"product" / L"etc" / L"fonts_customization.xml";
    }

    constexpr std::string_view systemPrefix = "/system/fonts";
    constexpr std::string_view productPrefix = "/product/fonts";
    const auto mapFont = [&](std::string_view prefix, const std::filesystem::path& directory) -> std::filesystem::path {
        auto suffix = path.substr(prefix.size());
        if (suffix.empty() || suffix == "/")
            return directory;
        if (suffix.front() != '/')
            return {};
        suffix.remove_prefix(1);
        if (suffix == "." || suffix == ".." || suffix.back() == '.' || suffix.back() == ' ' ||
            suffix.find_first_of("/\\:") != suffix.npos)
            return {};
        return directory / std::filesystem::path(
                               std::u8string_view(reinterpret_cast<const char8_t*>(suffix.data()), suffix.size()));
    };
    if (path.starts_with(systemPrefix))
        return mapFont(systemPrefix, imageFonts ? systemFonts / L"fonts" : systemFonts);
    if (imageFonts && path.starts_with(productPrefix)) {
        return mapFont(productPrefix, systemFonts / L"product" / L"fonts");
    }
    return {};
}

bool IsAndroidImageFontPath(const std::filesystem::path& path)
{
    if (!imageFonts || !path.is_absolute())
        return false;
    // Directory descriptors use Win32's extended path format.
    auto name = path.lexically_normal().make_preferred().wstring();
    if (name.starts_with(L"\\\\?\\UNC\\"))
        name.replace(0, 8, L"\\\\");
    else if (name.starts_with(L"\\\\?\\"))
        name.erase(0, 4);
    const auto& root = systemFonts.native();
    return name.size() >= root.size() && (name.size() == root.size() || name[root.size()] == L'\\') &&
           CompareStringOrdinal(name.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()),
                                TRUE) == CSTR_EQUAL;
}

uintptr_t AndroidFontImport(std::string_view symbol)
{
#define BIND(name, ...)                                                                                                \
    if (symbol == #name)                                                                                               \
    return BridgeAndroidImport(symbol, reinterpret_cast<uintptr_t>(__VA_ARGS__))
    BIND(ASystemFontIterator_open, &OpenFontIterator);
    BIND(ASystemFontIterator_next, &NextFont);
    BIND(
        ASystemFontIterator_close, +[](FontIterator* iterator) {
            delete iterator;
        });
    BIND(
        AFont_close, +[](Font* font) {
            delete font;
        });
    BIND(
        AFont_getFontFilePath, +[](const Font* font) {
            return font->path.c_str();
        });
    BIND(
        AFont_getLocale, +[](const Font* font) {
            return font->locale.empty() ? nullptr : font->locale.c_str();
        });
    BIND(
        AFont_getWeight, +[](const Font* font) -> uint16_t {
            return font->weight;
        });
    BIND(
        AFont_isItalic, +[](const Font* font) {
            return font->italic;
        });
    BIND(
        AFont_getCollectionIndex, +[](const Font* font) {
            return font->index;
        });
    BIND(
        AFont_getAxisCount, +[](const Font* font) {
            return font->axes.size();
        });
    BIND(
        AFont_getAxisTag, +[](const Font* font, uint32_t index) {
            return font->axes[index].first;
        });
    BIND(
        AFont_getAxisValue, +[](const Font* font, uint32_t index) {
            return font->axes[index].second;
        });
#undef BIND
    return 0;
}
}

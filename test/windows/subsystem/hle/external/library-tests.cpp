#include "../../support/test-suite.h"

#include "loader/ElfImage.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"
#include "cpu/arm64/Arm64ToX64.h"
#include "cpu/arm64/Arm64BlockJit.h"
#include "abi/TypedAndroidImport.h"
#include "abi/Arm64HostJni.h"
#include "runtime/art/ArtJni.h"
#include "runtime/art/ArtGs.h"
#include "signals/AndroidSignalContext.h"
#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libc.h"
#include "stub-liblog.h"
#include "hle/system/stub-libc-properties.h"
#include "hle/system/stub-libc-users.h"
#include "memory/GuestMappings.h"
#include "hle/system/stub-libc-sockets.h"
#include "hle/external/LibraryJniStubs.h"
#include "hle/external/stub-libclang_rt.ubsan_standalone.h"
#include "hle/system/stub-libnativeloader.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <io.h>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <optional>
#include <xmmintrin.h>

#include "subsystem/support/native-checks.h"

namespace stone::windows::test::checks {
struct FixtureString : _jstring {
    explicit FixtureString(const char* value) : value(value)
    {
    }

    const char* value;
};

struct ClassName : _jclass {
    explicit ClassName(const char* value) : value(value)
    {
    }

    FixtureString value;
};

void CheckChromiumJni()
{
    JNINativeInterface_ functions{};
    _jclass javaClass;
    functions.GetObjectClass = +[](JNIEnv*, jobject object) {
        return static_cast<jclass>(object);
    };
    functions.GetMethodID = +[](JNIEnv*, jclass type, const char* name, const char* signature) -> jmethodID {
        if (std::string_view(name) != "getName" || std::string_view(signature) != "()Ljava/lang/String;")
            return nullptr;
        return reinterpret_cast<jmethodID>(type);
    };
    functions.CallObjectMethodV = +[](JNIEnv*, jobject object, jmethodID, va_list) -> jobject {
        return &static_cast<ClassName*>(object)->value;
    };
    functions.GetStringUTFChars = +[](JNIEnv*, jstring string, jboolean*) {
        return static_cast<FixtureString*>(string)->value;
    };
    functions.ReleaseStringUTFChars = +[](JNIEnv*, jstring, const char*) {
    };
    functions.DeleteLocalRef = +[](JNIEnv*, jobject) {
    };
    functions.ExceptionCheck = +[](JNIEnv*) -> jboolean {
        return JNI_FALSE;
    };
    JNIEnv env{&functions};
    ClassName paths("org.chromium.base.PathUtils"), draw("com.android.webview.chromium.DrawFunctor");
    STONE_CHECK(IsWebViewDirectoryMethod(&env, &paths, "getDataDirectory", "()Ljava/lang/String;"));
    STONE_CHECK(!IsWebViewDirectoryMethod(&env, &javaClass, "getDataDirectory", "()I"));
    STONE_CHECK(!IsWebViewDirectoryMethod(&env, &draw, "getDataDirectory", "()Ljava/lang/String;"));
    STONE_CHECK(IsWebViewDrawFunctionTable(&env, &draw, "nativeGetFunctionTable", "()J"));
    STONE_CHECK(!IsWebViewDrawFunctionTable(&env, &paths, "nativeGetFunctionTable", "()J"));
}

void CheckDrawTable()
{
    struct DrawTable {
        int version;
        uintptr_t queryRenderMode, create, release, createWithVersion;
    };

    const auto table = reinterpret_cast<const DrawTable*>(WebViewDrawFunctionTable(nullptr, nullptr));
    STONE_CHECK(table && table->version == 3);
    STONE_CHECK(table->create && table->release && table->createWithVersion);
    const auto mode = Guest<int (*)()>(table->queryRenderMode, "()I");
    STONE_CHECK(mode() == 0); // OpenGL ES.
}

void CheckUnknownUbsan()
{
    const auto bytes = MakeElf(false);
    ElfImage image(bytes);
    STONE_CHECK(!PrepareUbsanRuntime("libfixture.so", bytes, image));
    try {
        PrepareUbsanRuntime(ubsanSoname, bytes, image);
    }
    catch (const std::runtime_error& error) {
        STONE_CHECK(std::string_view(error.what()) == "unknown Android UBSan runtime syscall layout");
        return;
    }
    throw std::runtime_error("unrecognized UBSan build accepted");
}
}

namespace stone::windows::test {
using namespace checks;

void TestUbsanImage(Suite& suite, const std::filesystem::path& elf)
{
    suite.Run("libclang-rt-ubsan-pinned-build", [&] {
        std::ifstream file(elf, std::ios::binary);
        STONE_CHECK(file);
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
        ElfImage image(bytes, ElfImage::Machine::X64, true);
        STONE_CHECK(PrepareUbsanRuntime(ubsanSoname, bytes, image));
    });
}
}

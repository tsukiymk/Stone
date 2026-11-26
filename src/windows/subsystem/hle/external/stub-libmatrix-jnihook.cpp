#include "LibraryJniStubs.h"

#include <jni.h>

namespace stone::windows {

void* MatrixJniHookFallback(std::string_view name, std::string_view signature)
{
    if (name != "initJniHookByPlaceHolderMethod" || signature != "(Ljava/lang/reflect/Method;)I")
        return nullptr;
    // Matrix probes ART's private method layout and can fall back when the runtime isn't ART.
    const auto unavailable = +[](JNIEnv*, jclass, jobject) -> jint {
        return JNI_ERR;
    };
    return reinterpret_cast<void*>(unavailable);
}

}

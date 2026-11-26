#include <stddef.h>
#include <android/hardware_buffer.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int dependency_add(int value);
__SIZE_TYPE__ strlen(const char* value);
int strcmp(const char* left, const char* right);
long syscall(long number, ...);
int* __errno(void);
void* dlopen(const char* name, int flags);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);
double fma(double a, double b, double c);
int ilogbf(float value);
float modff(float value, float* integral);
float scalbnf(float value, int exponent);
void sincosf(float value, float* sine, float* cosine);

struct DlInfo {
    const char* file;
    void* base;
    const char* symbol;
    void* address;
};

int dladdr(const void* address, struct DlInfo* info);

void* AAssetManager_fromJava(JNIEnv* env, jobject manager);
void* AAssetManager_open(void* manager, const char* name, int mode);
const void* AAsset_getBuffer(const void* asset);
long AAsset_getLength64(const void* asset);
int AAsset_isAllocated(const void* asset);
void AAsset_close(void* asset);
long AAsset_seek64(void* asset, long offset, int whence);
int AAsset_read(void* asset, void* buffer, __SIZE_TYPE__ size);
void* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);
void ANativeWindow_release(void* window);
int ANativeWindow_getWidth(void* window);
int ANativeWindow_getHeight(void* window);
int ANativeWindow_getFormat(void* window);

struct WindowBuffer {
    int width, height, stride, format;
    unsigned* bits;
    unsigned reserved[6];
};

int ANativeWindow_lock(void* window, struct WindowBuffer* buffer, void* dirty);
int ANativeWindow_unlockAndPost(void* window);

static int verifyPacketSockets(void* libc)
{
#define RESOLVE(name)                                                                                                  \
    __typeof__(name)* guest_##name = dlsym(libc, #name);                                                               \
    if (!guest_##name)                                                                                                 \
    return 0
    RESOLVE(socketpair);
    RESOLVE(setsockopt);
    RESOLVE(getsockopt);
    RESOLVE(sendmsg);
    RESOLVE(recvmsg);
    RESOLVE(poll);
    RESOLVE(fcntl);
    RESOLVE(getpid);
    RESOLVE(getuid);
    RESOLVE(getgid);
    RESOLVE(shutdown);
    RESOLVE(close);
#undef RESOLVE
    int fds[2];
    if (guest_socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds))
        return 0;
    int valid = 0;
    int type = 0, enabled = 1;
    socklen_t length = sizeof(type);
    if (guest_getsockopt(fds[0], SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET ||
        guest_fcntl(fds[0], F_GETFD) != FD_CLOEXEC || !(guest_fcntl(fds[0], F_GETFL) & O_NONBLOCK) ||
        guest_setsockopt(fds[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)))
        goto done;

    char output[8] = {0};
    struct iovec input[] = {{(void*)"ab", 2}, {(void*)"cde", 3}};
    struct iovec buffer = {output, 2};
    struct msghdr outgoing = {.msg_iov = input, .msg_iovlen = 2};
    struct msghdr incoming = {.msg_iov = &buffer, .msg_iovlen = 1};
    struct pollfd pollfd = {fds[1], POLLIN, 0};
    if (guest_poll(&pollfd, 1, 0) || guest_recvmsg(fds[1], &incoming, 0) != -1 || *__errno() != 11)
        goto done;
    if (guest_sendmsg(fds[0], &outgoing, 0) != 5)
        goto done;
    outgoing.msg_iov = &input[1];
    outgoing.msg_iovlen = 1;
    if (guest_sendmsg(fds[0], &outgoing, 0) != 3 || guest_poll(&pollfd, 1, 0) != 1 || !(pollfd.revents & POLLIN))
        goto done;
    if (guest_recvmsg(fds[1], &incoming, MSG_PEEK | MSG_TRUNC) != 5 ||
        (incoming.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != (MSG_TRUNC | MSG_CTRUNC) || strcmp(output, "ab"))
        goto done;

    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(struct ucred))];
    } control;

    incoming.msg_control = control.bytes;
    incoming.msg_controllen = sizeof(control.bytes);
    if (guest_recvmsg(fds[1], &incoming, 0) != 2 || incoming.msg_flags != MSG_TRUNC)
        goto done;
    const struct cmsghdr* header = CMSG_FIRSTHDR(&incoming);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_CREDENTIALS ||
        header->cmsg_len != CMSG_LEN(sizeof(struct ucred)))
        goto done;
    const struct ucred* cred = (const struct ucred*)CMSG_DATA(header);
    if (cred->pid != guest_getpid() || cred->uid != guest_getuid() || cred->gid != guest_getgid())
        goto done;
    if (guest_shutdown(fds[0], SHUT_WR))
        goto done;
    incoming.msg_control = 0;
    incoming.msg_controllen = 0;
    buffer.iov_len = sizeof(output);
    if (guest_recvmsg(fds[1], &incoming, 0) != 3 || strcmp(output, "cde"))
        goto done;
    valid = guest_recvmsg(fds[1], &incoming, 0) == 0 && guest_sendmsg(fds[0], &outgoing, MSG_NOSIGNAL) == -1 &&
            *__errno() == 32;
done:
    if (guest_close(fds[0]))
        valid = 0;
    if (guest_close(fds[1]))
        valid = 0;
    return valid;
}

static int verifyStreamSockets(JNIEnv* env, void* libc)
{
#define RESOLVE(name)                                                                                                  \
    __typeof__(name)* guest_##name = dlsym(libc, #name);                                                               \
    if (!guest_##name)                                                                                                 \
    return 0
    RESOLVE(socketpair);
    RESOLVE(fcntl);
    RESOLVE(send);
    RESOLVE(recvmsg);
    RESOLVE(close);
#undef RESOLVE
    void* android = dlopen("libandroid.so", 2);
    if (!android)
        return 0;
    int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**) = dlsym(android, "AHardwareBuffer_allocate");
    void (*release)(AHardwareBuffer*) = dlsym(android, "AHardwareBuffer_release");
    void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*) = dlsym(android, "AHardwareBuffer_describe");
    int (*sendBuffer)(const AHardwareBuffer*, int) = dlsym(android, "AHardwareBuffer_sendHandleToUnixSocket");
    int (*receiveBuffer)(int, AHardwareBuffer**) = dlsym(android, "AHardwareBuffer_recvHandleFromUnixSocket");
    AHardwareBuffer* buffer = 0;
    AHardwareBuffer* received = 0;
    jobject parcel = 0;
    jmethodID closeParcel = 0;
    AHardwareBuffer_Desc description = {.width = 8,
                                        .height = 6,
                                        .layers = 1,
                                        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN};
    int valid = 0;
    int fds[2] = {-1, -1};
    if (!allocate || !release || !describe || !sendBuffer || !receiveBuffer)
        goto done;
    if (guest_socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
        goto done;
    char first[3] = {0}, second[4] = {0};
    struct iovec buffers[] = {{first, 2}, {second, 3}};
    struct msghdr message = {.msg_iov = buffers, .msg_iovlen = 2};
    if (guest_fcntl(fds[0], F_GETFD) != 0 || guest_fcntl(fds[0], F_SETFD, FD_CLOEXEC) ||
        guest_fcntl(fds[0], F_GETFD) != FD_CLOEXEC || guest_fcntl(fds[0], F_SETFL, O_NONBLOCK) ||
        guest_fcntl(fds[1], F_SETFL, O_NONBLOCK) || guest_recvmsg(fds[1], &message, 0) != -1 || *__errno() != 11)
        goto done;
    if (guest_send(fds[0], "a", 1, 0) != 1 || guest_send(fds[0], "bcdef", 5, 0) != 5 ||
        guest_recvmsg(fds[1], &message, MSG_PEEK) != 5 || message.msg_flags != 0 || strcmp(first, "ab") ||
        strcmp(second, "cde") || guest_recvmsg(fds[1], &message, 0) != 5 || strcmp(first, "ab") ||
        strcmp(second, "cde"))
        goto done;
    if (allocate(&description, &buffer) || guest_send(fds[1], "x", 1, 0) != 1 || sendBuffer(buffer, fds[1]))
        goto done;
    release(buffer);
    buffer = 0;
    buffers[0].iov_len = 1;
    message.msg_iovlen = 1;
    if (guest_recvmsg(fds[0], &message, 0) != 1 || first[0] != 'x' || receiveBuffer(fds[0], &received) || !received)
        goto done;
    AHardwareBuffer_Desc transferred = {0};
    describe(received, &transferred);
    if (transferred.width != description.width || transferred.height != description.height ||
        transferred.format != description.format)
        goto done;
    jclass parcelType = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
    if (!parcelType)
        goto done;
    jmethodID fromFd = (*env)->GetStaticMethodID(env, parcelType, "fromFd", "(I)Landroid/os/ParcelFileDescriptor;");
    jmethodID getFd = (*env)->GetMethodID(env, parcelType, "getFd", "()I");
    closeParcel = (*env)->GetMethodID(env, parcelType, "close", "()V");
    if (!fromFd || !getFd || !closeParcel)
        goto done;
    jvalue args[] = {{.i = fds[0]}};
    parcel = (*env)->CallStaticObjectMethodA(env, parcelType, fromFd, args);
    if (!parcel)
        goto done;
    int copy = (*env)->CallIntMethodA(env, parcel, getFd, 0);
    if (copy < 0 || copy == fds[0] || guest_fcntl(copy, F_GETFD) != FD_CLOEXEC)
        goto done;
    if (guest_close(fds[0]))
        goto done;
    fds[0] = -1;
    if (guest_send(copy, "g", 1, 0) != 1)
        goto done;
    (*env)->CallVoidMethodA(env, parcel, closeParcel, 0);
    parcel = 0;
    valid = guest_recvmsg(fds[1], &message, 0) == 1 && first[0] == 'f' && guest_recvmsg(fds[1], &message, 0) == 1 &&
            first[0] == 'g' && guest_recvmsg(fds[1], &message, 0) == 0;
done:
    if (parcel && !(*env)->ExceptionCheck(env))
        (*env)->CallVoidMethodA(env, parcel, closeParcel, 0);
    if (received)
        release(received);
    if (buffer)
        release(buffer);
    if (fds[0] >= 0 && guest_close(fds[0]))
        valid = 0;
    if (fds[1] >= 0 && guest_close(fds[1]))
        valid = 0;
    return dlclose(android) == 0 && valid;
}

static int verifySharedMemory(void* libc)
{
    void* android = dlopen("libandroid.so", 2);
    if (!android)
        return 0;
    int (*create)(const char*, size_t) = dlsym(android, "ASharedMemory_create");
    size_t (*getSize)(int) = dlsym(android, "ASharedMemory_getSize");
    int (*setProtection)(int, int) = dlsym(android, "ASharedMemory_setProt");
    __typeof__(mmap)* map = dlsym(libc, "mmap");
    __typeof__(munmap)* unmap = dlsym(libc, "munmap");
    __typeof__(mprotect)* protect = dlsym(libc, "mprotect");
    __typeof__(fcntl)* descriptor = dlsym(libc, "fcntl");
    __typeof__(close)* closeFile = dlsym(libc, "close");
    const size_t size = 128 * 1024;
    int valid = 0, fd = -1, duplicate = -1;
    volatile unsigned char* writer = MAP_FAILED;
    volatile unsigned char* reader = MAP_FAILED;
    if (!create || !getSize || !setProtection || !map || !unmap || !protect || !descriptor || !closeFile)
        goto done;
    fd = create(0, size);
    if (fd < 0 || getSize(fd) != size || descriptor(fd, F_GETFD) != FD_CLOEXEC)
        goto done;
    if (setProtection(fd, PROT_READ | PROT_WRITE))
        goto done;
    writer = map(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (writer == MAP_FAILED)
        goto done;
    duplicate = descriptor(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0 || setProtection(duplicate, PROT_READ))
        goto done;
    reader = map(0, size, PROT_READ, MAP_SHARED, duplicate, 0);
    if (reader == MAP_FAILED || setProtection(fd, PROT_READ | PROT_WRITE) != -1 ||
        protect((void*)reader, size, PROT_READ | PROT_WRITE) != -1)
        goto done;
    void* denied = map(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (denied != MAP_FAILED) {
        unmap(denied, size);
        goto done;
    }
    // The writer was mapped before the permission reduction.
    writer[size - 1] = 42;
    valid = reader[size - 1] == 42;
done:
    if (reader != MAP_FAILED && unmap((void*)reader, size))
        valid = 0;
    if (writer != MAP_FAILED && unmap((void*)writer, size))
        valid = 0;
    if (duplicate >= 0 && closeFile(duplicate))
        valid = 0;
    if (fd >= 0 && closeFile(fd))
        valid = 0;
    return dlclose(android) == 0 && valid;
}

static int verifyCpuClocks(void* libc)
{
    __typeof__(clock_gettime)* getTime = dlsym(libc, "clock_gettime");
    __typeof__(clock_getres)* getResolution = dlsym(libc, "clock_getres");
    if (!getTime || !getResolution)
        return 0;
    struct timespec process, thread, later, resolution;
    if (getTime(CLOCK_THREAD_CPUTIME_ID, &thread) || getTime(CLOCK_PROCESS_CPUTIME_ID, &process) ||
        getResolution(CLOCK_THREAD_CPUTIME_ID, &resolution) || resolution.tv_sec != 0 || resolution.tv_nsec <= 0 ||
        resolution.tv_nsec >= 1000000000)
        return 0;
    if (process.tv_sec < thread.tv_sec || (process.tv_sec == thread.tv_sec && process.tv_nsec < thread.tv_nsec))
        return 0;
    return getTime(CLOCK_THREAD_CPUTIME_ID, &later) == 0 &&
           (later.tv_sec > thread.tv_sec || (later.tv_sec == thread.tv_sec && later.tv_nsec >= thread.tv_nsec));
}

static int verifyExecutableLink(void* libc)
{
    __typeof__(readlink)* getLink = dlsym(libc, "readlink");
    if (!getLink)
        return 0;
    char prefix[2] = {0, 'x'};
    char path[4096];
    const char filename[] = "/StoneClient.exe";
    const size_t suffix = sizeof(filename) - 1;
    if (getLink("/proc/self/exe", prefix, 1) != 1 || prefix[0] != '/' || prefix[1] != 'x')
        return 0;
    const ssize_t length = getLink("/proc/self/exe", path, sizeof(path) - 1);
    if (length < (ssize_t)suffix)
        return 0;
    path[length] = '\0';
    return strcmp(path + length - suffix, filename) == 0;
}

JNIEXPORT jint JNICALL Java_com_trebledot_stone_cit_nativeapp_NativeActivity_nativeAndroid(JNIEnv* env, jclass type,
                                                                                           jobject manager,
                                                                                           jobject surface)
{
    (void)type;
    void* assets = AAssetManager_fromJava(env, manager);
    if (!assets || assets != AAssetManager_fromJava(env, manager) || AAssetManager_open(assets, "missing.txt", 0))
        return -1;
    void* asset = AAssetManager_open(assets, "native-check.txt", 3);
    if (!asset)
        return -2;
    const char* contents = AAsset_getBuffer(asset);
    char last = 0;
    int readable = AAsset_getLength64(asset) >= 12 && AAsset_isAllocated(asset) && contents && contents[0] == 'N' &&
                   AAsset_seek64(asset, 11, 0) == 11 && AAsset_read(asset, &last, 1) == 1 && last == 't' &&
                   AAsset_seek64(asset, -1, 0) == -1;
    AAsset_close(asset);
    if (!readable)
        return -3;
    void* android = dlopen("libandroid.so", 2);
    if (!android || !dlsym(android, "AAssetManager_open") || dlclose(android))
        return -6;

    void* window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return -4;
    struct WindowBuffer buffer;
    int locked = ANativeWindow_lock(window, &buffer, 0) == 0;
    int valid = locked && buffer.bits && buffer.width > 0 && buffer.height > 0 &&
                buffer.width == ANativeWindow_getWidth(window) && buffer.height == ANativeWindow_getHeight(window) &&
                buffer.stride >= buffer.width && buffer.format == 1 && ANativeWindow_getFormat(window) == 1 &&
                ANativeWindow_lock(window, &buffer, 0) == -16;
    if (valid) {
        for (int i = 0; i < buffer.height * buffer.stride; ++i)
            buffer.bits[i] = 0xff0080ffu;
    }
    int posted = locked && ANativeWindow_unlockAndPost(window) == 0;
    ANativeWindow_release(window);
    return valid && posted ? 42 : -5;
}

JNIEXPORT jint JNICALL Java_com_trebledot_stone_cit_nativeapp_NativeActivity_dynamic(JNIEnv* env, jclass type)
{
    (void)env;
    (void)type;
    const int now = 2, no_load = 4, global = 0x100;
    if (dlopen("libdynamic.so", now | no_load) != 0 || !dlerror() || dlerror())
        return -1;
    if (dlopen("C:/libdynamic.so", now) != 0 || !dlerror() || dlerror())
        return -2;
    void* handle = dlopen("libdynamic.so", now);
    if (!handle || dlerror())
        return -3;
    void* address = dlsym(handle, "dynamic_add");
    if (!address || !dlsym(handle, "dependency_add"))
        return -4;
    if (dlsym(0, "dynamic_add") || !dlerror() || dlerror())
        return -5;
    struct DlInfo info;
    if (!dladdr(address, &info) || !info.base || info.address != address || !info.file ||
        strcmp(info.file, "libdynamic.so") || !info.symbol || strcmp(info.symbol, "dynamic_add"))
        return -6;
    if (dlopen("libdynamic.so", now | no_load | global) != handle || dlsym(0, "dynamic_add") != address)
        return -7;
    if (dlclose(handle) || dlclose(handle) || dlclose(handle) != -1 || !dlerror() || dlerror())
        return -8;
    void* math = dlopen("libm.so", now);
    double (*multiply_add)(double, double, double) = math ? dlsym(math, "fma") : 0;
    if (!multiply_add || multiply_add(2.0, 3.0, 0.5) != 6.5 || dlclose(math))
        return -9;
    float integral = 0, sine = -1, cosine = -1;
    sincosf(0.0f, &sine, &cosine);
    if (sine != 0.0f || cosine != 1.0f || scalbnf(1.5f, 3) != 12.0f || modff(-2.25f, &integral) != -0.25f ||
        integral != -2.0f || fma(2.0, 3.0, 0.5) != 6.5 || ilogbf(0.0f) != -__INT_MAX__ || ilogbf(8.0f) != 3)
        return -10;
    const long sys_futex = 202;
    const int futex_wait_private = 128, futex_wake_bitset_private = 138;
    unsigned futex = 0;
    if (syscall(sys_futex, &futex, futex_wait_private, 1, (void*)0, (void*)0, 0) != -1 || *__errno() != 11)
        return -11; // EAGAIN
    if (syscall(sys_futex, &futex, futex_wake_bitset_private, 1, (void*)1, (void*)0, 0) != -1 || *__errno() != 22)
        return -12; // EINVAL: empty bitset
    if (syscall(sys_futex, &futex, futex_wake_bitset_private, 1, (void*)1, (void*)0, 1) != 0)
        return -13;
    void* libc = dlopen("libc.so", now);
    if (!libc)
        return -14;
    const char* (*error_text)(int) = dlsym(libc, "strerror");
    if (!error_text || strcmp(error_text(38), "Function not implemented") ||
        strcmp(error_text(36), "File name too long"))
        return -18;
    void* (*map)(void*, __SIZE_TYPE__, int, int, int, long) = dlsym(libc, "mmap");
    int (*advise)(void*, __SIZE_TYPE__, int) = dlsym(libc, "madvise");
    int (*unmap)(void*, __SIZE_TYPE__) = dlsym(libc, "munmap");
    if (!map || !advise || !unmap)
        return -15;
    const int read_write = 3, private_anonymous = 0x22, discard = 4;
    volatile unsigned char* page = map(0, 4096, read_write, private_anonymous, -1, 0);
    if (page == (void*)-1)
        return -16;
    page[0] = 42;
    int discarded = advise((void*)page, 4096, discard) == 0 && page[0] == 0;
    if (unmap((void*)page, 4096) || !discarded)
        return -17;
    if (!verifyPacketSockets(libc))
        return -19;
    if (!verifyStreamSockets(env, libc))
        return -23;
    if (!verifySharedMemory(libc))
        return -20;
    if (!verifyCpuClocks(libc))
        return -21;
    if (!verifyExecutableLink(libc) || dlclose(libc))
        return -22;
    return ((int (*)(int))address)(11);
}

static jmethodID callback;

static jint registered(JNIEnv* env, jclass type, jint value)
{
    jvalue args[1];
    args[0].i = dependency_add(value + (jint)strlen("abc") - 3);
    return (*env)->CallStaticIntMethodA(env, type, callback, args);
}

JNIEXPORT jdouble JNICALL Java_com_trebledot_stone_cit_nativeapp_NativeActivity_mixed(JNIEnv* env, jclass type,
                                                                                      jfloat a, jint b, jdouble c,
                                                                                      jint d, jint e)
{
    (void)env;
    (void)type;
    return a + b + c + d + e;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    (void)reserved;
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    jclass type = (*env)->FindClass(env, "com/trebledot/stone/cit/nativeapp/NativeActivity");
    if (!type)
        return JNI_ERR;
    callback = (*env)->GetStaticMethodID(env, type, "javaCallback", "(I)I");
    if (!callback)
        return JNI_ERR;
    const JNINativeMethod methods[] = {{"registered", "(I)I", (void*)registered}};
    if ((*env)->RegisterNatives(env, type, methods, 1) != JNI_OK)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}

void* AAssetManager_fromJava(void* env, void* manager)
{
    return 0;
}

void* AAssetManager_open(void* manager, const char* name, int mode)
{
    return 0;
}

const void* AAsset_getBuffer(const void* asset)
{
    return 0;
}

long AAsset_getLength64(const void* asset)
{
    return 0;
}

int AAsset_isAllocated(const void* asset)
{
    return 0;
}

void AAsset_close(void* asset)
{
}

long AAsset_seek64(void* asset, long offset, int whence)
{
    return -1;
}

int AAsset_read(void* asset, void* buffer, __SIZE_TYPE__ size)
{
    return -1;
}

void* ANativeWindow_fromSurface(void* env, void* surface)
{
    return 0;
}

void ANativeWindow_release(void* window)
{
}

int ANativeWindow_getWidth(void* window)
{
    return 0;
}

int ANativeWindow_getHeight(void* window)
{
    return 0;
}

int ANativeWindow_getFormat(void* window)
{
    return 0;
}

int ANativeWindow_lock(void* window, void* buffer, void* dirty)
{
    return -1;
}

int ANativeWindow_unlockAndPost(void* window)
{
    return -1;
}

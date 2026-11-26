// Link-time symbols only. The Windows host supplies the implementations.
void* dlopen(const char* name, int flags)
{
    (void)name;
    (void)flags;
    return 0;
}

void* dlsym(void* handle, const char* name)
{
    (void)handle;
    (void)name;
    return 0;
}

int dlclose(void* handle)
{
    (void)handle;
    return -1;
}

char* dlerror(void)
{
    return 0;
}

int dladdr(const void* address, void* info)
{
    (void)address;
    (void)info;
    return 0;
}

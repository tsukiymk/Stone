__SIZE_TYPE__ strlen(const char* value)
{
    (void)value;
    return 0;
}

int strcmp(const char* left, const char* right)
{
    (void)left;
    (void)right;
    return 0;
}

long syscall(long number, ...)
{
    (void)number;
    return -1;
}

int* __errno(void)
{
    return 0;
}

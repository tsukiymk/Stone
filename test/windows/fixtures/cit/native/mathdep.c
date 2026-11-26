static volatile int base;

__attribute__((constructor)) static void initialize(void)
{
    base = 30;
}

int dependency_add(int value)
{
    return base + value;
}

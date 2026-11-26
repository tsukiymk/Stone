extern int dependency_add(int value);

static int initialized;

__attribute__((constructor)) static void initialize(void)
{
    initialized++;
}

int dynamic_add(int value)
{
    return dependency_add(value) + initialized;
}

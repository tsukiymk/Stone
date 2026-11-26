#include "abi/AndroidLibraryImports.h"

#include <icu.h>

namespace stone::windows {
uintptr_t Libandroidicu(std::string_view name)
{
#define STONE_ICU(function)                                                                                            \
    if (name == #function "_android")                                                                                  \
    return Import(name, &function)
    STONE_ICU(u_foldCase);
    STONE_ICU(ubrk_open);
    STONE_ICU(ubrk_first);
    STONE_ICU(ubrk_close);
    STONE_ICU(ubrk_current);
    STONE_ICU(ubrk_next);
    STONE_ICU(u_isspace);
    STONE_ICU(u_strToUTF8);
    STONE_ICU(ucol_open);
    STONE_ICU(ucol_setStrength);
    STONE_ICU(ucol_close);
    STONE_ICU(uregex_setText);
    STONE_ICU(uregex_matches);
    STONE_ICU(uregex_open);
    STONE_ICU(uregex_close);
    STONE_ICU(u_strToUpper);
    STONE_ICU(u_strToLower);
    STONE_ICU(u_errorName);
    STONE_ICU(ucol_strcoll);
    STONE_ICU(ucol_setAttribute);
    STONE_ICU(uiter_setUTF8);
    STONE_ICU(ucol_strcollIter);
#undef STONE_ICU
    return 0;
}
}

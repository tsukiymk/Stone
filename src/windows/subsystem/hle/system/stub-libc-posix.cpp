#include "stub-libc.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace stone::windows {
namespace {
constexpr int NoEscape = 1, Pathname = 2, Period = 4, LeadingDir = 8, CaseFold = 16;
enum class PatternKind {
    Literal,
    Any,
    Star,
    Bracket
};

struct PatternToken {
    PatternKind kind = PatternKind::Literal;
    unsigned char literal = 0;
    std::bitset<256> bracket;
};

unsigned char FoldByte(unsigned char byte, int flags)
{
    return flags & CaseFold ? static_cast<unsigned char>(std::tolower(byte)) : byte;
}

unsigned char PatternByte(std::string_view pattern, size_t& offset, int flags)
{
    if (pattern[offset] == '\\' && !(flags & NoEscape) && offset + 1 < pattern.size())
        ++offset;
    return static_cast<unsigned char>(pattern[offset++]);
}

using CharacterClass = int (*)(int);

CharacterClass ClassByName(std::string_view name)
{
    if (name == "alnum")
        return &std::isalnum;
    if (name == "alpha")
        return &std::isalpha;
    if (name == "blank")
        return &std::isblank;
    if (name == "cntrl")
        return &std::iscntrl;
    if (name == "digit")
        return &std::isdigit;
    if (name == "graph")
        return &std::isgraph;
    if (name == "lower")
        return &std::islower;
    if (name == "print")
        return &std::isprint;
    if (name == "punct")
        return &std::ispunct;
    if (name == "space")
        return &std::isspace;
    if (name == "upper")
        return &std::isupper;
    if (name == "xdigit")
        return &std::isxdigit;
    return nullptr;
}

size_t ReadBracket(std::string_view pattern, size_t start, int flags, std::bitset<256>& result)
{
    size_t i = start;
    bool negate = false;
    if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
        negate = true;
        ++i;
    }
    const size_t first = i;
    while (i < pattern.size()) {
        if (i != first && pattern[i] == ']') {
            if (negate)
                result.flip();
            return i + 1;
        }
        if (pattern.substr(i).starts_with("[:")) {
            const auto end = pattern.find(":]", i + 2);
            if (end == pattern.npos)
                break;
            const auto test = ClassByName(pattern.substr(i + 2, end - i - 2));
            if (!test)
                break;
            for (int byte = 0; byte < 256; ++byte) {
                if (test(byte))
                    result.set(FoldByte(static_cast<unsigned char>(byte), flags));
            }
            i = end + 2;
            continue;
        }
        const auto low = FoldByte(PatternByte(pattern, i, flags), flags);
        if (i + 1 < pattern.size() && pattern[i] == '-' && pattern[i + 1] != ']') {
            ++i;
            const auto high = FoldByte(PatternByte(pattern, i, flags), flags);
            for (unsigned byte = low; byte <= high; ++byte)
                result.set(byte);
        }
        else {
            result.set(low);
        }
    }
    return start; // An unclosed bracket is a literal '['.
}

int MatchFilename(const char* patternText, const char* inputText, int flags)
{
    try {
        const std::string_view pattern(patternText), input(inputText);
        std::vector<PatternToken> tokens;
        for (size_t i = 0; i < pattern.size();) {
            PatternToken token;
            const auto c = pattern[i];
            if (c == '?' || c == '*') {
                token.kind = c == '*' ? PatternKind::Star : PatternKind::Any;
                ++i;
                if (c == '*' && !tokens.empty() && tokens.back().kind == PatternKind::Star)
                    continue;
            }
            else if (c == '[') {
                const auto end = ReadBracket(pattern, i + 1, flags, token.bracket);
                if (end != i + 1)
                    token.kind = PatternKind::Bracket;
                else
                    token.literal = '[';
                i = end;
            }
            else {
                token.literal = FoldByte(PatternByte(pattern, i, flags), flags);
            }
            tokens.push_back(token);
        }

        std::vector<unsigned char> active(tokens.size() + 1), next(active.size());
        active[0] = 1;
        for (size_t i = 0; i <= input.size(); ++i) {
            const bool end = i == input.size();
            const auto c = end ? 0 : static_cast<unsigned char>(input[i]);
            const bool slash = (flags & Pathname) && c == '/';
            const bool leadingDot =
                (flags & Period) && c == '.' && (i == 0 || ((flags & Pathname) && input[i - 1] == '/'));
            const auto folded = FoldByte(static_cast<unsigned char>(c), flags);
            std::fill(next.begin(), next.end(), static_cast<unsigned char>(0));
            for (size_t j = 0; j < tokens.size(); ++j) {
                if (!active[j])
                    continue;
                const auto& token = tokens[j];
                switch (token.kind) {
                case PatternKind::Star:
                    if (!leadingDot) {
                        active[j + 1] = 1;
                        if (!end && !slash)
                            next[j] = 1;
                    }
                    break;
                case PatternKind::Literal:
                    if (!end && folded == token.literal)
                        next[j + 1] = 1;
                    break;
                case PatternKind::Any:
                case PatternKind::Bracket:
                    if (!end && !leadingDot && !slash && (token.kind == PatternKind::Any || token.bracket.test(folded)))
                        next[j + 1] = 1;
                    break;
                }
            }
            if (active.back() && (end || ((flags & LeadingDir) && c == '/')))
                return 0;
            active.swap(next);
        }
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
    }
    return 1; // FNM_NOMATCH
}

struct LongOption {
    const char* name;
    int hasArgument;
    int* flag;
    int value;
};

static_assert(sizeof(LongOption) == 32);

char* optionArgument = nullptr;
int optionIndex = 1, optionErrors = 1, optionCharacter = 0, optionReset = 0;

struct OptionState {
    int firstOperand = 1, lastOperand = 1, expectedIndex = 1;
    const char* shortOptions = nullptr;
    bool finished = false;
};

OptionState options;

void ExchangeArguments(char** argv)
{
    std::rotate(argv + options.firstOperand, argv + options.lastOperand, argv + optionIndex);
    options.firstOperand += optionIndex - options.lastOperand;
    options.lastOperand = optionIndex;
}

int LongOptionResult(char* const* argv, int argc, char* word, std::string_view specification,
                     const LongOption* longOptions, int* index)
{
    const std::string_view input(word);
    const auto separator = input.find('=');
    const auto name = input.substr(0, separator);
    int match = -1;
    bool ambiguous = false;
    for (int i = 0; longOptions[i].name; ++i) {
        const std::string_view candidate(longOptions[i].name);
        if (candidate == name) {
            match = i;
            ambiguous = false;
            break;
        }
        if (name.empty() || !candidate.starts_with(name))
            continue;
        if (match < 0)
            match = i;
        else {
            const auto& previous = longOptions[match];
            const auto& current = longOptions[i];
            ambiguous |= current.hasArgument != previous.hasArgument || current.flag != previous.flag ||
                         current.value != previous.value;
        }
    }
    if (match < 0 || ambiguous) {
        if (optionErrors && !specification.starts_with(':'))
            std::fprintf(stderr, "%s: %s option '--%s'\n", argv[0], ambiguous ? "ambiguous" : "unrecognized", word);
        optionCharacter = 0;
        return '?';
    }

    const auto& option = longOptions[match];
    if (separator != input.npos && option.hasArgument)
        optionArgument = word + separator + 1;
    else if (option.hasArgument == 1 && optionIndex < argc)
        optionArgument = argv[optionIndex++];
    else if (separator != input.npos || option.hasArgument == 1) {
        if (optionErrors && !specification.starts_with(':')) {
            std::fprintf(stderr, "%s: option '--%s' %s an argument\n", argv[0], option.name,
                         option.hasArgument == 1 ? "requires" : "does not allow");
        }
        optionCharacter = option.flag ? 0 : option.value;
        return option.hasArgument == 1 && specification.starts_with(':') ? ':' : '?';
    }
    if (index)
        *index = match;
    if (!option.flag)
        return option.value;
    *option.flag = option.value;
    return 0;
}

int ParseOption(int argc, char* const* argv, const char* spec, const LongOption* longOptions, int* index)
{
    optionArgument = nullptr;
    if (optionReset || optionIndex <= 0 || optionIndex != options.expectedIndex) {
        optionIndex = std::max(optionIndex, 1);
        options = {optionIndex, optionIndex, optionIndex};
        optionReset = 0;
    }
    // An application may restart parsing by setting optind to 0 or optreset to 1.
    if (options.finished)
        return -1;

    std::string_view specification(spec);
    const bool returnOperands = specification.starts_with('-');
    size_t posixModeSize = 0;
    getenv_s(&posixModeSize, nullptr, 0, "POSIXLY_CORRECT");
    const bool stopAtOperand = specification.starts_with('+') || !longOptions || posixModeSize != 0;
    if (specification.starts_with('-') || specification.starts_with('+'))
        specification.remove_prefix(1);
    const bool permute = !returnOperands && !stopAtOperand;
    const auto operand = [](const char* word) {
        return word[0] != '-' || word[1] == '\0';
    };
    if (!options.shortOptions || !*options.shortOptions) {
        if (permute) {
            if (options.firstOperand != options.lastOperand && options.lastOperand != optionIndex)
                ExchangeArguments(const_cast<char**>(argv));
            else if (options.lastOperand != optionIndex)
                options.firstOperand = optionIndex;
            while (optionIndex < argc && operand(argv[optionIndex]))
                ++optionIndex;
            options.lastOperand = optionIndex;
        }
        if (optionIndex < argc && std::string_view(argv[optionIndex]) == "--") {
            ++optionIndex;
            if (permute && options.firstOperand != options.lastOperand) {
                ExchangeArguments(const_cast<char**>(argv));
                optionIndex = options.firstOperand;
            }
            options.finished = true;
        }
        else if (optionIndex >= argc) {
            if (permute && options.firstOperand != options.lastOperand)
                optionIndex = options.firstOperand;
            options.finished = true;
        }
        else if (operand(argv[optionIndex])) {
            if (!returnOperands) {
                options.finished = true;
                return -1;
            }
            optionArgument = argv[optionIndex++];
            return 1;
        }
        else if (longOptions && argv[optionIndex][1] == '-') {
            return LongOptionResult(argv, argc, argv[optionIndex++] + 2, specification, longOptions, index);
        }
        else {
            options.shortOptions = argv[optionIndex] + 1;
        }
        if (options.finished)
            return -1;
    }

    const auto character = static_cast<unsigned char>(*options.shortOptions++);
    if (!*options.shortOptions)
        ++optionIndex;
    const auto position = specification.find(static_cast<char>(character));
    if (position == specification.npos || character == ':') {
        optionCharacter = character;
        if (optionErrors && !specification.starts_with(':'))
            std::fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], character);
        return '?';
    }
    if (position + 1 == specification.size() || specification[position + 1] != ':')
        return character;
    const bool optional = position + 2 < specification.size() && specification[position + 2] == ':';
    if (*options.shortOptions) {
        optionArgument = const_cast<char*>(options.shortOptions);
        options.shortOptions = nullptr;
        ++optionIndex;
    }
    else if (!optional) {
        if (optionIndex < argc)
            optionArgument = argv[optionIndex++];
        else {
            optionCharacter = character;
            if (optionErrors && !specification.starts_with(':'))
                std::fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], character);
            return specification.starts_with(':') ? ':' : '?';
        }
    }
    return character;
}

int GetoptLong(int argc, char* const* argv, const char* spec, const LongOption* longOptions, int* index)
{
    const int result = ParseOption(argc, argv, spec, longOptions, index);
    options.expectedIndex = optionIndex;
    return result;
}
}

uintptr_t AndroidPosixImport(std::string_view name, bool hostAbi)
{
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(name, target);                                                   \
    }
    BIND(fnmatch, &MatchFilename);
    BIND(getopt_long, &GetoptLong);
    BIND(
        getopt, +[](int argc, char* const* argv, const char* spec) {
            return GetoptLong(argc, argv, spec, nullptr, nullptr);
        });
#undef BIND
    if (hostAbi)
        return 0;
    if (name == "optarg")
        return reinterpret_cast<uintptr_t>(&optionArgument);
    if (name == "optind")
        return reinterpret_cast<uintptr_t>(&optionIndex);
    if (name == "opterr")
        return reinterpret_cast<uintptr_t>(&optionErrors);
    if (name == "optopt")
        return reinterpret_cast<uintptr_t>(&optionCharacter);
    if (name == "optreset")
        return reinterpret_cast<uintptr_t>(&optionReset);
    return 0;
}
}

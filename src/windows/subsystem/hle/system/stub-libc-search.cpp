#include "abi/AndroidLibraryImports.h"
#include "loader/AndroidSystemElf.h"
#include <cerrno>
#include <cstdlib>

namespace stone::windows {
namespace {
struct SearchNode {
    const void* key;
    SearchNode* left;
    SearchNode* right;
};

using Compare = int (*)(const void*, const void*);

Compare Comparator(uintptr_t compare)
{
    return reinterpret_cast<Compare>(MakeNativeAndroidThunk(compare, "(LL)I"));
}

void* FindNode(const void* key, SearchNode* const* rootPointer, uintptr_t compare)
{
    auto* root = rootPointer ? *rootPointer : nullptr;
    const auto order = Comparator(compare);
    while (root) {
        const int value = order(key, root->key);
        if (!value)
            return root;
        root = value < 0 ? root->left : root->right;
    }
    return nullptr;
}

void* InsertNode(const void* key, SearchNode** root, uintptr_t compare)
{
    if (!root)
        return nullptr;
    const auto order = Comparator(compare);
    while (*root) {
        const int value = order(key, (*root)->key);
        if (!value)
            return *root;
        root = value < 0 ? &(*root)->left : &(*root)->right;
    }
    auto* node = static_cast<SearchNode*>(std::malloc(sizeof(SearchNode)));
    if (!node) {
        errno = ENOMEM;
        return nullptr;
    }
    *node = {key, nullptr, nullptr};
    *root = node;
    return node;
}

void* DeleteNode(const void* key, SearchNode** root, uintptr_t compare)
{
    if (!root)
        return nullptr;
    const auto order = Comparator(compare);
    SearchNode* parent = nullptr;
    while (*root) {
        const int value = order(key, (*root)->key);
        if (!value)
            break;
        parent = *root;
        root = value < 0 ? &parent->left : &parent->right;
    }
    if (!*root)
        return nullptr;
    auto* removed = *root;
    if (removed->left && removed->right) {
        auto** successor = &removed->right;
        while ((*successor)->left)
            successor = &(*successor)->left;
        removed->key = (*successor)->key;
        auto* redundant = *successor;
        *successor = redundant->right;
        std::free(redundant);
    }
    else {
        *root = removed->left ? removed->left : removed->right;
        std::free(removed);
    }
    // POSIX leaves the root-deletion return value unspecified.
    return parent ? static_cast<void*>(parent) : static_cast<void*>(root);
}

void DestroyNodes(SearchNode* root, uintptr_t callback)
{
    const auto release = reinterpret_cast<void (*)(const void*)>(MakeNativeAndroidThunk(callback, "(L)V"));
    // Rotate to a right-only chain so sorted input cannot overflow the host stack.
    while (root) {
        if (root->left) {
            auto* left = root->left;
            root->left = left->right;
            left->right = root;
            root = left;
        }
        else {
            auto* next = root->right;
            release(root->key);
            std::free(root);
            root = next;
        }
    }
}
}

uintptr_t AndroidSearchImport(std::string_view name)
{
    if (name == "tsearch")
        return Import(name, &InsertNode);
    if (name == "tfind")
        return Import(name, &FindNode);
    if (name == "tdelete")
        return Import(name, &DeleteNode);
    if (name == "tdestroy")
        return Import(name, &DestroyNodes);
    return 0;
}
}

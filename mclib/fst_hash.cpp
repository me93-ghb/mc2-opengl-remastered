//===========================================================================//
// mclib/fst_hash.cpp
//
// Leaf-TU implementation of elfHash() and the FST key normalizer. See
// mclib/fst_hash.h for rationale.
//
// elfHash() body is byte-for-byte the historical mclib/fastfile.cpp:120
// definition; relocated so the tests/unit target can link it without
// dragging in heap.h / ffile.h / the broader engine includes. The decl
// in mclib/fastfile.h still resolves here at link time.
//===========================================================================//

#include "fst_hash.h"

extern "C" unsigned long elfHash(const char* name)
{
    // macos-port: the accumulator MUST be exactly 32-bit. The historical hash ran
    // on 32-bit `unsigned long` (Win32), where `h << 4` wraps at 32 bits. On LP64
    // (macOS/Linux) `unsigned long` is 64-bit, so `h` would grow past 32 bits and
    // produce different values -> FST asset keys (baked with the 32-bit hash) would
    // never match. `unsigned int` is 32-bit on every target we build, so this
    // reproduces the original values exactly.
    unsigned int h = 0, g;
    while (*name)
    {
        h = (h << 4) + (unsigned char)(*name++);
        if ((g = h & 0xF0000000U))
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

extern "C" void fst_normalize_key(char* dst, const char* src)
{
    // memory/fst_forward_slash_invariant.md: backslash inputs must collapse
    // to forward slash before hashing. file.cpp does this in-place around
    // the elfHash() call; this leaf version writes to a separate buffer.
    while (*src)
    {
        char c = *src++;
        *dst++ = (c == '\\') ? '/' : c;
    }
    *dst = '\0';
}

#include <cctype>

// Byte-for-byte the loop previously inlined in mclib/file.cpp::NormalizeKey:
// backslash -> '/', every other byte -> tolower. Folds case AND slashes so a
// mod-overlay path resolves to one canonical index key regardless of how the
// caller spelled it.
std::string fst_normalize_loose_key(const char* src)
{
    std::string s = src ? src : "";
    for (char& c : s)
        c = (c == '\\') ? '/' : (char)std::tolower((unsigned char)c);
    return s;
}

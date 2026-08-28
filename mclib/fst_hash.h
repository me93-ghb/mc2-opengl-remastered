//===========================================================================//
// mclib/fst_hash.h
//
// Leaf-TU declarations for the FST hash + key-normalization primitives.
// No transitive engine dependencies; safe to include from a doctest-driven
// unit test target.
//
// Background:
//   - elfHash() is the standard ELF dictionary hash used by FastFileFind
//     (see mclib/fastfile.cpp historical home; definition relocated here
//     so the tests/unit target can link it without pulling heap.h / ffile.h
//     / the full mclib graphics stack).
//   - fst_normalize_key() collapses backslashes to forward slashes. This
//     mirrors the inline loop in mclib/file.cpp around the elfHash() call
//     (memory/fst_forward_slash_invariant.md). Several callers normalize
//     in place; this leaf version writes to a separate dst.
//
// Engine code keeps using elfHash() via the existing fastfile.h decl;
// fastfile.h is left untouched.
//===========================================================================//

#ifndef MCLIB_FST_HASH_H
#define MCLIB_FST_HASH_H

#ifdef __cplusplus
extern "C" {
#endif

// ELF dictionary hash. Byte-stream-deterministic; case-sensitive.
// Matches the historical definition in mclib/fastfile.cpp 1:1.
extern "C" unsigned long elfHash(const char* name); // macos-port: linkage matches fst_hash.cpp

// Write a slash-normalized copy of src into dst (null-terminated).
// Every '\\' byte becomes '/'; all other bytes pass through.
// dst must have room for strlen(src)+1 bytes.
void fst_normalize_key(char* dst, const char* src);

#ifdef __cplusplus
}

#include <string>

// Loose-file / mod-overlay index key normalizer: lowercase + forward-slash in
// one pass (the canonical key used by mclib/file.cpp's loose-file index, which
// now delegates here). Distinct from fst_normalize_key(): that one is the FST
// elfHash path and is slash-only (the engine lowercases separately via
// S_strlwr); this one folds case AND slashes together so a mod path and its
// case/slash variants resolve to ONE index key. tolower follows the existing
// engine behavior (C locale; ASCII asset paths fold deterministically).
std::string fst_normalize_loose_key(const char* src);
#endif

#endif // MCLIB_FST_HASH_H

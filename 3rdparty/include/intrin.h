#ifdef _WIN32
/* Windows: use the real MSVC intrinsics header (this shim sits on the -I path). */
#include_next <intrin.h>
#else
/* macos-port / non-Windows: clang's own <intrin.h> is Windows-target only and
 * fails here. The engine only uses __rdtsc and __popcnt from it, so provide
 * portable equivalents. */
#ifndef MC2_COMPAT_INTRIN_H
#define MC2_COMPAT_INTRIN_H

#include <cstdint>

#if defined(__aarch64__) || defined(__arm64__)
static inline uint64_t __rdtsc(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); /* ARM64 virtual counter */
    return v;
}
#elif defined(__x86_64__) || defined(__i386__)
static inline uint64_t __rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile(".byte 0x0f,0x31" : "=a"(lo), "=d"(hi)); /* RDTSC */
    return ((uint64_t)hi << 32) | lo;
}
#endif

static inline unsigned int __popcnt(unsigned int x)
{
    return (unsigned int)__builtin_popcount(x);
}

#endif /* MC2_COMPAT_INTRIN_H */
#endif /* _WIN32 */

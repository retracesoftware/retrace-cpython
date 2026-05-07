#ifndef Py_INTERNAL_RETRACE_MIXERS_H
#define Py_INTERNAL_RETRACE_MIXERS_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdint.h>

#define PY_RETRACE_MIXER_WY64 1

#ifndef PY_RETRACE_COORDINATE_MIXER
#  define PY_RETRACE_COORDINATE_MIXER PY_RETRACE_MIXER_WY64
#endif

#if PY_RETRACE_COORDINATE_MIXER == PY_RETRACE_MIXER_WY64
static inline uint64_t
_PyRetrace_Mix64(uint64_t state, uint64_t value)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t product =
        (__uint128_t)(state ^ UINT64_C(0xa0761d6478bd642f)) *
        (value ^ UINT64_C(0xe7037ed1a0b428db));
    return (uint64_t)product ^ (uint64_t)(product >> 64);
#else
    state ^= UINT64_C(0xa0761d6478bd642f);
    value ^= UINT64_C(0xe7037ed1a0b428db);
    state ^= state >> 32;
    value ^= value >> 32;
    state *= UINT64_C(0xe7037ed1a0b428db);
    value *= UINT64_C(0xa0761d6478bd642f);
    return state ^ value ^ (state >> 32) ^ (value >> 32);
#endif
}
#else
#  error "unknown Retrace coordinate mixer"
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_RETRACE_MIXERS_H */

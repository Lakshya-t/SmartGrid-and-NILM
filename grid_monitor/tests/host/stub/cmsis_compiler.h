/* Minimal stand-in for CMSIS core so CMSIS-DSP's plain-C code builds on a PC. */
#ifndef STUB_CMSIS_COMPILER_H
#define STUB_CMSIS_COMPILER_H
#include <stdint.h>
#include <string.h>
#define __STATIC_INLINE        static inline
#define __STATIC_FORCEINLINE   static inline
#define __ALIGNED(x)           __attribute__((aligned(x)))
#define __WEAK                 __attribute__((weak))
#define __PACKED               __attribute__((packed))
#define __NO_RETURN            __attribute__((noreturn))
#define __RESTRICT             __restrict
#define __USED                 __attribute__((used))
#define __COMPILER_BARRIER()   __asm volatile("" ::: "memory")
#endif

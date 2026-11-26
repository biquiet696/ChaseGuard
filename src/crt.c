/*
 * ChaseGuard - the few C runtime routines the compiler may emit calls to.
 * The plugin links no C runtime at all. License: GPL-3.0-or-later (see LICENSE)
 */
typedef unsigned int size_t;

void *memset(void *d, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    volatile unsigned char *dp = (volatile unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    volatile unsigned char *dp = (volatile unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

int _fltused = 0;

/* 64-bit unsigned division helper of the MSVC x86 ABI (operands on the stack, callee pops 16
 * bytes, quotient in EDX:EAX). Only referenced by Zydis' formatter, which the plugin does not use,
 * but the linker needs the symbol. */
__attribute__((used)) unsigned long long cg_udiv64(unsigned long long n, unsigned long long d)
{
    unsigned long long q = 0, r = 0;
    int i;
    if (d == 0) return 0;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    return q;
}

__declspec(naked) void _aulldiv(void)
{
    __asm__ volatile (
        "pushl 16(%esp)\n\t"   /* divisor high  */
        "pushl 16(%esp)\n\t"   /* divisor low   */
        "pushl 16(%esp)\n\t"   /* dividend high */
        "pushl 16(%esp)\n\t"   /* dividend low  */
        "call  _cg_udiv64\n\t"
        "addl  $16, %esp\n\t"
        "ret   $16\n\t");
}

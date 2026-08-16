/* Single-threaded 64-bit atomic shims for the PSP qualification fixture.

   MIPS32 has no native 8-byte atomics, and the SDK's libatomic drags in
   a pthread-backed gthread layer.  The engine runs single-threaded on
   the PSP main thread (the budget concurrent pool's atomic_flag is the
   only intentional atomic and is 32-bit), so plain load/store semantics
   are sufficient here.  Revisit if a second engine thread ever exists. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

uint64_t __atomic_load_8(const volatile void *pointer, int memorder)
{
    (void) memorder;
    return *(const volatile uint64_t *) pointer;
}

void __atomic_store_8(volatile void *pointer, uint64_t value, int memorder)
{
    (void) memorder;
    *(volatile uint64_t *) pointer = value;
}

uint64_t __atomic_exchange_8(volatile void *pointer, uint64_t value,
                             int memorder)
{
    (void) memorder;
    uint64_t previous = *(volatile uint64_t *) pointer;
    *(volatile uint64_t *) pointer = value;
    return previous;
}

bool __atomic_compare_exchange_8(volatile void *pointer, void *expected,
                                 uint64_t desired, bool weak,
                                 int success_memorder, int failure_memorder)
{
    (void) weak; (void) success_memorder; (void) failure_memorder;
    uint64_t current = *(volatile uint64_t *) pointer;
    if (current == *(uint64_t *) expected) {
        *(volatile uint64_t *) pointer = desired;
        return true;
    }
    memcpy(expected, &current, sizeof(current));
    return false;
}

#define PSP_ATOMIC_FETCH_OP(name, expression)                               \
    uint64_t __atomic_fetch_##name##_8(volatile void *pointer,              \
                                       uint64_t value, int memorder)        \
    {                                                                       \
        (void) memorder;                                                    \
        uint64_t previous = *(volatile uint64_t *) pointer;                 \
        *(volatile uint64_t *) pointer = (expression);                      \
        return previous;                                                    \
    }

PSP_ATOMIC_FETCH_OP(add, previous + value)
PSP_ATOMIC_FETCH_OP(sub, previous - value)
PSP_ATOMIC_FETCH_OP(and, previous & value)
PSP_ATOMIC_FETCH_OP(or, previous | value)
PSP_ATOMIC_FETCH_OP(xor, previous ^ value)

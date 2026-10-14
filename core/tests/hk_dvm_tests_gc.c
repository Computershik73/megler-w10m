/*
 * The collector (docs/JAVA.md, "The collector"), from the heap up: what an address inside an object is found to be.
 */
#include "../src/dvm/hk_dvm_internal.h"
#include "hk_dvm_tests.h"

void test_gc(void)
{
    hk_jobj *small = hk_dvm_new_array("[B", 10), *mid = hk_dvm_new_array("[I", 300), *big = hk_dvm_new_array("[J", 2000);
    int local = 0;
    say("the heap: an address inside an object is that object's");
    CHECK(small && mid && big);
    if (!small || !mid || !big) return;
    hk_dvm_heap_lock();
    CHECK(hk_dvm_heap_find((hk_addr)small) == (void *)small);
    /* Inside the elements: the array they belong to, which is what keeps them when a C local holds only them. */
    CHECK(hk_dvm_heap_find((hk_addr)small->u.arr.data + 9) == (void *)small);
    CHECK(hk_dvm_heap_find((hk_addr)mid->u.arr.data + 1199) == (void *)mid);
    /* A large one (over 2 KB), from its own allocation. */
    CHECK(hk_dvm_heap_find((hk_addr)big) == (void *)big);
    CHECK(hk_dvm_heap_find((hk_addr)big->u.arr.data + 15999) == (void *)big);
    /* Not the heap's: an address on the stack, and none. */
    CHECK(hk_dvm_heap_find((hk_addr)&local) == NULL);
    CHECK(hk_dvm_heap_find(0) == NULL);
    hk_dvm_heap_unlock();
}

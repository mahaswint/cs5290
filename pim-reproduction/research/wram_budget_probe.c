/* Compile-only layout check, not an implemented allocator optimization.
 * Bitmap first ensures 8-byte alignment for UPMEM MRAM DMA.
 */
#include <defs.h>
#include <stdint.h>
#include <stddef.h>

struct __attribute__((aligned(8))) HotPageDescriptor {
    uint32_t bitmap[4];
    __mram_ptr void *page;
    uint32_t class_tag;
    uint32_t valid;
    uint32_t dirty;
};

_Static_assert(sizeof(__mram_ptr void *) == 4, "Unexpected MRAM pointer width");
_Static_assert(sizeof(struct HotPageDescriptor) == 32, "Unexpected descriptor size");
_Static_assert(__alignof__(struct HotPageDescriptor) >= 8, "DMA alignment");
_Static_assert(__builtin_offsetof(struct HotPageDescriptor, bitmap) == 0, "DMA alignment");

unsigned descriptor_bytes(void) { return sizeof(struct HotPageDescriptor); }
unsigned one_slot_16_tasklets_bytes(void) {
    return sizeof(struct HotPageDescriptor) * 16;
}
unsigned one_slot_24_tasklets_bytes(void) {
    return sizeof(struct HotPageDescriptor) * 24;
}
unsigned eight_slots_16_tasklets_bytes(void) {
    return sizeof(struct HotPageDescriptor) * 8 * 16;
}

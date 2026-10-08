#ifndef CODBOZ_SWITCH_CODE_MEMORY_H
#define CODBOZ_SWITCH_CODE_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_PAGE_SIZE 0x1000

/*
 * Horizon has no mmap and never maps a page writable and executable. Code is
 * loaded by filling ordinary heap pages and then remapping them as process
 * code memory at another address, executable up to a split point and writable
 * after it. The address is reserved up front so the image can be relocated
 * for it before the remap.
 */
struct switch_code_memory {
    uint8_t *backing; /* heap pages the image is assembled in; unusable once committed */
    uint8_t *address; /* where those pages run once committed */
    size_t size;
    void *reservation;
    bool committed;
};

bool switch_code_memory_reserve(struct switch_code_memory *memory, size_t size);
bool switch_code_memory_commit(struct switch_code_memory *memory, size_t executable_size);
void switch_code_memory_release(struct switch_code_memory *memory);

#endif

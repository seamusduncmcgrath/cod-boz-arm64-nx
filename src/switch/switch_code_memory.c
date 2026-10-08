#include "switch_code_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

/* hbloader hands homebrew a real handle to its own process; direct NSO/NSP launches do not. */
static Handle own_process(void) {
    Handle handle = envGetOwnProcessHandle();
    return handle != INVALID_HANDLE ? handle : CUR_PROCESS_HANDLE;
}

bool switch_code_memory_reserve(struct switch_code_memory *memory, size_t size) {
    memset(memory, 0, sizeof(*memory));
    if (!size || (size & (SWITCH_PAGE_SIZE - 1))) {
        return false;
    }

    uint8_t *backing = aligned_alloc(SWITCH_PAGE_SIZE, size);
    if (!backing) {
        return false;
    }
    memset(backing, 0, size);

    virtmemLock();
    void *address = virtmemFindCodeMemory(size, SWITCH_PAGE_SIZE);
    VirtmemReservation *reservation = address ? virtmemAddReservation(address, size) : NULL;
    virtmemUnlock();
    if (!reservation) {
        free(backing);
        return false;
    }

    memory->backing = backing;
    memory->address = address;
    memory->size = size;
    memory->reservation = reservation;
    return true;
}

bool switch_code_memory_commit(struct switch_code_memory *memory, size_t executable_size) {
    if (!memory->backing || memory->committed || !executable_size ||
        executable_size >= memory->size || (executable_size & (SWITCH_PAGE_SIZE - 1))) {
        return false;
    }

    /* The backing pages become inaccessible once remapped, so write them back first. */
    armDCacheFlush(memory->backing, memory->size);

    Handle process = own_process();
    u64 address = (u64)(uintptr_t)memory->address;
    Result rc = svcMapProcessCodeMemory(process, address, (u64)(uintptr_t)memory->backing,
                                        memory->size);
    if (R_FAILED(rc)) {
        fprintf(stderr, "[loader] svcMapProcessCodeMemory failed: 0x%x\n", rc);
        return false;
    }
    memory->committed = true;

    rc = svcSetProcessMemoryPermission(process, address, executable_size, Perm_Rx);
    if (R_SUCCEEDED(rc)) {
        rc = svcSetProcessMemoryPermission(process, address + executable_size,
                                           memory->size - executable_size, Perm_Rw);
    }
    if (R_FAILED(rc)) {
        fprintf(stderr, "[loader] svcSetProcessMemoryPermission failed: 0x%x\n", rc);
        return false;
    }

    armICacheInvalidate(memory->address, executable_size);
    return true;
}

void switch_code_memory_release(struct switch_code_memory *memory) {
    if (!memory->backing) {
        return;
    }
    if (memory->committed) {
        Result rc = svcUnmapProcessCodeMemory(own_process(), (u64)(uintptr_t)memory->address,
                                              (u64)(uintptr_t)memory->backing, memory->size);
        if (R_FAILED(rc)) {
            /* The backing pages are still inaccessible, so they cannot go back to the heap. */
            fprintf(stderr, "[loader] svcUnmapProcessCodeMemory failed: 0x%x\n", rc);
            memset(memory, 0, sizeof(*memory));
            return;
        }
    }
    virtmemLock();
    virtmemRemoveReservation(memory->reservation);
    virtmemUnlock();
    free(memory->backing);
    memset(memory, 0, sizeof(*memory));
}

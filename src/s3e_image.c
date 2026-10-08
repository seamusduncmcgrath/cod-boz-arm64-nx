#include "s3e_image.h"

#include "LzmaDec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "switch_code_memory.h"

#define S3E_MAGIC 0x55334558u

/*
 * The game's package holds the image as an LZMA "alone" file: the decoder's properties, the size
 * it unpacks to as 64 bits, then the stream.
 */
enum {
    PACKED_SIZE_OFFSET = LZMA_PROPS_SIZE,
    PACKED_STREAM_OFFSET = LZMA_PROPS_SIZE + 8,
    /* Several times what any build of the game unpacks to: a size beyond it is not one. */
    MAX_UNPACKED_SIZE = 64 * 1024 * 1024,
};

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool range_ok(size_t size, uint32_t offset, uint32_t length) {
    return offset <= size && length <= size - offset;
}

static size_t page_round(size_t size) {
    size_t mask = (size_t)SWITCH_PAGE_SIZE - 1;
    return (size + mask) & ~mask;
}

static void *unpack_alloc(ISzAllocPtr allocator, size_t size) {
    (void)allocator;
    return malloc(size);
}

static void unpack_free(ISzAllocPtr allocator, void *address) {
    (void)allocator;
    free(address);
}

/*
 * Replaces a packed image with what it unpacks to. Anything that does not unpack to exactly the
 * size it declares is left as it was read.
 */
static void unpack_image(struct s3e_image *image) {
    const uint8_t *packed = image->file_data;
    if (image->file_size <= PACKED_STREAM_OFFSET) {
        return;
    }
    uint64_t size = rd32(packed + PACKED_SIZE_OFFSET) |
                    ((uint64_t)rd32(packed + PACKED_SIZE_OFFSET + 4) << 32);
    if (size > MAX_UNPACKED_SIZE) {
        return;
    }

    uint8_t *unpacked = malloc((size_t)size);
    if (!unpacked) {
        return;
    }
    SizeT unpacked_size = (SizeT)size;
    SizeT packed_size = image->file_size - PACKED_STREAM_OFFSET;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    ISzAlloc allocator = {unpack_alloc, unpack_free};
    SRes result = LzmaDecode(unpacked, &unpacked_size, packed + PACKED_STREAM_OFFSET, &packed_size,
                             packed, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &allocator);
    if (result != SZ_OK || unpacked_size != size) {
        free(unpacked);
        return;
    }

    free(image->file_data);
    image->file_data = unpacked;
    image->file_size = unpacked_size;
}

bool s3e_image_load(const char *path, struct s3e_image *image) {
    memset(image, 0, sizeof(*image));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[loader] open %s: %s\n", path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr, "[loader] stat %s failed\n", path);
        close(fd);
        return false;
    }

    image->file_size = (size_t)st.st_size;
    image->file_data = malloc(image->file_size);
    if (!image->file_data) {
        close(fd);
        return false;
    }

    size_t done = 0;
    while (done < image->file_size) {
        ssize_t got = read(fd, image->file_data + done, image->file_size - done);
        if (got <= 0) {
            fprintf(stderr, "[loader] read %s failed\n", path);
            close(fd);
            s3e_image_free(image);
            return false;
        }
        done += (size_t)got;
    }
    close(fd);

    /* An image is taken as the game's package has it or already unpacked. */
    if (image->file_size >= 4 && rd32(image->file_data) != S3E_MAGIC) {
        unpack_image(image);
    }

    if (image->file_size < 68) {
        fprintf(stderr, "[loader] %s is too small for an S3E header\n", path);
        s3e_image_free(image);
        return false;
    }

    const uint8_t *p = image->file_data;
    struct s3e_header *h = &image->header;

    h->ident = rd32(p + 0);
    h->version = rd32(p + 4);
    h->flags = rd16(p + 8);
    h->arch = rd16(p + 10);
    h->fixup_offset = rd32(p + 12);
    h->fixup_size = rd32(p + 16);
    h->code_offset = rd32(p + 20);
    h->code_file_size = rd32(p + 24);
    h->code_mem_size = rd32(p + 28);
    h->sig_offset = rd32(p + 32);
    h->sig_size = rd32(p + 36);
    h->entry_offset = rd32(p + 40);
    h->config_offset = rd32(p + 44);
    h->config_size = rd32(p + 48);
    h->base_addr_orig = rd32(p + 52);
    h->extra_offset = rd32(p + 56);
    h->extra_size = rd32(p + 60);
    h->ext_header_size = rd32(p + 64);

    if (h->ident != S3E_MAGIC) {
        fprintf(stderr, "[loader] %s is not an S3E image, packed or unpacked\n", path);
        s3e_image_free(image);
        return false;
    }

    if (h->ext_header_size == 0x0c) {
        if (image->file_size < 76) {
            s3e_image_free(image);
            return false;
        }
        h->data_offset = rd32(p + 68);
        h->is_juice = rd32(p + 72);
    }

    if (!range_ok(image->file_size, h->fixup_offset, h->fixup_size) ||
        !range_ok(image->file_size, h->code_offset, h->code_file_size) ||
        h->code_file_size > h->code_mem_size || h->entry_offset >= h->code_mem_size) {
        fprintf(stderr, "[loader] S3E header ranges are invalid\n");
        s3e_image_free(image);
        return false;
    }

    return true;
}

void s3e_image_free(struct s3e_image *image) {
    if (!image) {
        return;
    }
    for (size_t i = 0; i < image->symbols.count; ++i) {
        free(image->symbols.items[i]);
    }
    free(image->symbols.items);
    free(image->file_data);
    memset(image, 0, sizeof(*image));
}

bool s3e_image_parse_symbols(struct s3e_image *image) {
    const struct s3e_header *h = &image->header;
    const uint8_t *data = image->file_data;
    uint32_t pos = h->fixup_offset;
    uint32_t end = h->fixup_offset + h->fixup_size;

    while (pos < end) {
        if (end - pos < 8) {
            return false;
        }

        uint32_t type = rd32(data + pos);
        uint32_t size = rd32(data + pos + 4);
        uint32_t body = pos + 8;
        if (size < 8 || size > end - pos) {
            return false;
        }

        if (type == 0) {
            if (size < 10) {
                return false;
            }
            uint16_t count = rd16(data + body);
            char **items = calloc(count, sizeof(*items));
            if (!items) {
                return false;
            }

            uint32_t cursor = body + 2;
            for (uint16_t i = 0; i < count; ++i) {
                uint32_t start = cursor;
                while (cursor < pos + size && data[cursor] != 0) {
                    cursor++;
                }
                if (cursor >= pos + size) {
                    free(items);
                    return false;
                }

                size_t len = cursor - start;
                items[i] = malloc(len + 1);
                if (!items[i]) {
                    for (uint16_t j = 0; j < i; ++j) {
                        free(items[j]);
                    }
                    free(items);
                    return false;
                }
                memcpy(items[i], data + start, len);
                items[i][len] = 0;
                cursor++;
            }

            image->symbols.items = items;
            image->symbols.count = count;
        }

        pos += size;
    }

    return image->symbols.count > 0;
}

/* Relocation slots hold the image's pointers, which are 64 bits wide. */
static uintptr_t read_slot(const uint8_t *slot) {
    uintptr_t value;
    memcpy(&value, slot, sizeof(value));
    return value;
}

static void write_slot(uint8_t *slot, uintptr_t value) {
    memcpy(slot, &value, sizeof(value));
}

static bool slot_in_image(const struct s3e_image *image, uint32_t offset) {
    return image->header.code_mem_size >= sizeof(uintptr_t) &&
           offset <= image->header.code_mem_size - sizeof(uintptr_t);
}

static bool apply_internal_relocs(const struct s3e_image *image, uint8_t *base, uint32_t pos,
                                  uint32_t size, uintptr_t load_delta) {
    const uint8_t *data = image->file_data;
    uint32_t body = pos + 8;
    if (size < 12) {
        return false;
    }

    uint32_t count = rd32(data + body);
    uint32_t cursor = body + 4;
    if (count > (size - 12) / 4) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t offset = rd32(data + cursor);
        cursor += 4;
        if (!slot_in_image(image, offset)) {
            return false;
        }
        uint8_t *slot = base + offset;
        uintptr_t value = read_slot(slot);
        /* The 32-bit build of the game, read through these slots, points outside itself. */
        if (value - image->header.base_addr_orig > image->header.code_mem_size) {
            fprintf(stderr, "[loader] relocation 0x%x does not point into the image: this is "
                            "not the 64-bit (AArch64) build of the game\n",
                    offset);
            return false;
        }
        write_slot(slot, value + load_delta);
    }

    return true;
}

static bool apply_external_relocs(const struct s3e_image *image, uint8_t *base, uint32_t pos,
                                  uint32_t size, void *(*resolve)(const char *symbol)) {
    const uint8_t *data = image->file_data;
    uint32_t body = pos + 8;
    if (size < 12) {
        return false;
    }

    uint32_t count = rd32(data + body);
    uint32_t cursor = body + 4;
    if (count > (size - 12) / 6) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t hi = rd16(data + cursor);
        uint32_t lo = rd16(data + cursor + 2);
        uint16_t symbol_index = rd16(data + cursor + 4);
        cursor += 6;

        uint32_t offset = (hi << 16) | lo;
        if (!slot_in_image(image, offset) || symbol_index >= image->symbols.count) {
            return false;
        }

        const char *symbol = image->symbols.items[symbol_index];
        void *addr = resolve(symbol);
        if (!addr) {
            fprintf(stderr, "[loader] unresolved import: %s\n", symbol);
            return false;
        }

        write_slot(base + offset, (uintptr_t)addr);
    }

    return true;
}

/*
 * Where the image is built and where it runs. Horizon never maps a page writable and
 * executable, so the image is assembled in heap memory and only becomes runnable, at a different
 * address, once mapping_finish() remaps it.
 */
struct image_mapping {
    uint8_t *write_base;
    uint8_t *run_base;
    size_t size;
};

static struct switch_code_memory g_code_memory;

/* Everything below the image's data offset is code and constants; the rest must stay writable. */
static size_t executable_size(const struct s3e_header *h) {
    return h->data_offset & ~(size_t)(SWITCH_PAGE_SIZE - 1);
}

static bool mapping_create(const struct s3e_header *h, size_t size, struct image_mapping *mapping) {
    if (!executable_size(h) || executable_size(h) >= size) {
        fprintf(stderr, "[loader] S3E image does not declare where its writable data starts\n");
        return false;
    }
    if (!switch_code_memory_reserve(&g_code_memory, size)) {
        fprintf(stderr, "[loader] unable to reserve 0x%zx bytes of code memory\n", size);
        return false;
    }
    mapping->write_base = g_code_memory.backing;
    mapping->run_base = g_code_memory.address;
    mapping->size = size;
    return true;
}

static bool mapping_finish(const struct s3e_header *h, struct image_mapping *mapping) {
    (void)mapping;
    return switch_code_memory_commit(&g_code_memory, executable_size(h));
}

static void mapping_destroy(struct image_mapping *mapping) {
    (void)mapping;
    switch_code_memory_release(&g_code_memory);
}

bool s3e_image_map_and_relocate(const struct s3e_image *image, void *(*resolve)(const char *symbol),
                                struct s3e_loaded_image *loaded) {
    memset(loaded, 0, sizeof(*loaded));
    const struct s3e_header *h = &image->header;
    struct image_mapping mapping;
    if (!mapping_create(h, page_round(h->code_mem_size), &mapping)) {
        return false;
    }
    uint8_t *base = mapping.write_base;

    memcpy(base, image->file_data + h->code_offset, h->code_file_size);
    memset(base + h->code_file_size, 0, h->code_mem_size - h->code_file_size);

    uint32_t pos = h->fixup_offset;
    uint32_t end = h->fixup_offset + h->fixup_size;
    uintptr_t load_delta = (uintptr_t)mapping.run_base - h->base_addr_orig;
    while (pos < end) {
        uint32_t type = rd32(image->file_data + pos);
        uint32_t size = rd32(image->file_data + pos + 4);
        if (size < 8 || size > end - pos) {
            mapping_destroy(&mapping);
            return false;
        }

        bool ok = true;
        if (type == 1) {
            ok = apply_internal_relocs(image, base, pos, size, load_delta);
        } else if (type == 2 || type == 3 || type == 4) {
            ok = apply_external_relocs(image, base, pos, size, resolve);
        }

        if (!ok) {
            mapping_destroy(&mapping);
            return false;
        }

        pos += size;
    }

    if (!mapping_finish(h, &mapping)) {
        mapping_destroy(&mapping);
        return false;
    }

    loaded->base = mapping.run_base;
    loaded->map_size = mapping.size;
    loaded->entry_offset = h->entry_offset;
    return true;
}

void s3e_loaded_image_unmap(struct s3e_loaded_image *loaded) {
    if (loaded && loaded->base) {
        struct image_mapping mapping = {
            .write_base = loaded->base,
            .run_base = loaded->base,
            .size = loaded->map_size,
        };
        mapping_destroy(&mapping);
        memset(loaded, 0, sizeof(*loaded));
    }
}

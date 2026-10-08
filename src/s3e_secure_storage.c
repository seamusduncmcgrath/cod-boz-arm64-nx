#include "s3e_host_internal.h"

/* Marmalade's secure storage is one opaque blob per app; it is kept as a file in the port root. */
static void secure_storage_path(char *out, size_t out_size) {
    snprintf(out, out_size, "%s/secure-storage.bin", g_root);
}

int32_t s3eSecureStorageGet(void *data, uint16_t size) {
    if (!data || !size) {
        return 1;
    }
    char path[1200];
    secure_storage_path(path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 1;
    }
    size_t received = fread(data, 1, size, file);
    fclose(file);
    return received == size ? 0 : 1;
}

int32_t s3eSecureStoragePut(const void *data, uint16_t size) {
    if (!data || !size) {
        return 1;
    }
    char path[1200];
    secure_storage_path(path, sizeof(path));
    FILE *file = fopen(path, "wb");
    if (!file) {
        return 1;
    }
    size_t written = fwrite(data, 1, size, file);
    int close_result = fclose(file);
    return written == size && close_result == 0 ? 0 : 1;
}

#include "s3e_host_internal.h"

static void make_path(char *out, size_t out_size, const char *name) {
    if (name && name[0] == '/') {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "%s/%s", g_root, name ? name : "");
    }
}

static int path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static int use_existing_path(char *out, size_t out_size, const char *path) {
    if (!path_exists(path)) {
        return 0;
    }
    snprintf(out, out_size, "%s", path);
    return 1;
}

static int try_asset_path(char *out, size_t out_size, const char *name) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/assets/%s", g_root, name);
    return use_existing_path(out, out_size, path);
}

static const char *base_name(const char *name) {
    const char *slash = strrchr(name ? name : "", '/');
    return slash ? slash + 1 : (name ? name : "");
}

/*
 * Where a file the game reads from storage is: in the port's folder, or in the assets folder
 * under its own name or under the last part of it.
 */
static int resolve_read_path(const char *name, char *out, size_t out_size) {
    char path[1200];
    const char *safe_name = name ? name : "";
    make_path(path, sizeof(path), safe_name);
    if (use_existing_path(out, out_size, path)) {
        return 1;
    }
    if (safe_name[0] == '/') {
        return 0;
    }
    if (try_asset_path(out, out_size, safe_name)) {
        return 1;
    }
    const char *leaf = base_name(safe_name);
    return leaf != safe_name && try_asset_path(out, out_size, leaf);
}

static int is_read_mode(const char *mode) {
    return mode && mode[0] == 'r';
}

static int is_user_file_mode(const char *mode) {
    return mode && (strchr(mode, 'U') || strchr(mode, '+') || mode[0] == 'w' || mode[0] == 'a');
}

static int is_user_file_name(const char *name) {
    const char *dot = strrchr(name ? name : "", '.');
    return dot && strcasecmp(dot, ".i3d") == 0;
}

static void sanitize_file_mode(const char *mode, char *out, size_t out_size) {
    size_t n = 0;
    if (!out_size) {
        return;
    }
    for (const char *p = mode ? mode : "rb"; *p && n + 1 < out_size; ++p) {
        if (*p != 'U') {
            out[n++] = *p;
        }
    }
    out[n] = 0;
    if (!out[0]) {
        snprintf(out, out_size, "rb");
    }
}

static void make_user_path(char *out, size_t out_size, const char *name) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        home = g_root;
    }
    if (name && name[0] == '/') {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "%s/%s", home, name ? name : "");
    }
}

static void make_parent_dirs(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return 0;
    }
    make_parent_dirs(dst);
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    uint8_t buffer[8192];
    int ok = 1;
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), in);
        if (got > 0 && fwrite(buffer, 1, got, out) != got) {
            ok = 0;
            break;
        }
        if (got < sizeof(buffer)) {
            if (ferror(in)) {
                ok = 0;
            }
            break;
        }
    }
    fclose(out);
    fclose(in);
    return ok;
}

static long file_size_for_seek(FILE *file) {
    long here = ftell(file);
    if (here < 0 || fseek(file, 0, SEEK_END) != 0) {
        return -1;
    }
    long size = ftell(file);
    fseek(file, here, SEEK_SET);
    return size;
}

/*
 * User file systems. The game registers callback tables and expects every read-mode open to be
 * offered to them before real storage is tried. Its archive code relies on this to serve, and
 * decompress, the files inside blackops_gles1.dz. The table layout was read out of the game.
 */
struct user_file_system {
    void *(*open)(const char *name, const char *mode);
    uint32_t (*read)(void *buffer, uint32_t elem_size, uint32_t count, void *handle);
    uint8_t (*eof)(void *handle);
    int32_t (*seek)(void *handle, int32_t offset, int32_t origin);
    int32_t (*tell)(void *handle);
    int32_t (*close)(void *handle);
    void *(*list_directory)(const char *path);
    int32_t (*list_next)(void *list, char *name, int32_t length);
    int32_t (*list_close)(void *list);
};

/* What s3eFileOpen returns for a file that a user file system is serving. */
struct user_file {
    const struct user_file_system *file_system;
    void *handle;
    struct user_file *next;
};

enum { USER_FILE_SYSTEM_MAX = 8 };

static struct user_file_system g_user_file_systems[USER_FILE_SYSTEM_MAX];
static size_t g_user_file_system_count;
static struct user_file *g_user_files;

static struct user_file *find_user_file(void *file) {
    for (struct user_file *item = g_user_files; item; item = item->next) {
        if (item == file) {
            return item;
        }
    }
    return NULL;
}

/*
 * The game asks for names such as "bootstrap//iwui_style/style.group.bin", while its archive
 * code matches folder names exactly and only knows the single-separator form. So the name is
 * tidied before a file system sees it: one kind of separator, no repeats, no leading "./".
 */
static void clean_user_file_name(const char *name, char *out, size_t out_size) {
    size_t length = 0;
    if (name[0] == '.' && (name[1] == '/' || name[1] == '\\')) {
        name += 2;
    }
    for (; *name && length + 1 < out_size; ++name) {
        char c = *name == '\\' ? '/' : *name;
        /* Keep the "//" of a drive prefix such as "raw://". */
        int after_drive = length >= 2 && out[length - 2] == ':';
        if (c == '/' && length && out[length - 1] == '/' && !after_drive) {
            continue;
        }
        out[length++] = c;
    }
    out[length] = 0;
}

static void *open_user_file(const char *requested_name, const char *mode) {
    char name[1200];
    clean_user_file_name(requested_name, name, sizeof(name));
    for (size_t i = g_user_file_system_count; i-- > 0;) {
        const struct user_file_system *file_system = &g_user_file_systems[i];
        void *handle = file_system->open ? file_system->open(name, mode) : NULL;
        if (!handle) {
            continue;
        }
        struct user_file *file = malloc(sizeof(*file));
        if (!file) {
            if (file_system->close) {
                file_system->close(handle);
            }
            return NULL;
        }
        file->file_system = file_system;
        file->handle = handle;
        file->next = g_user_files;
        g_user_files = file;
        return file;
    }
    return NULL;
}

static int32_t close_user_file(struct user_file *file) {
    struct user_file **link = &g_user_files;
    while (*link && *link != file) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = file->next;
    }
    int32_t result = file->file_system->close ? file->file_system->close(file->handle) : 0;
    free(file);
    return result;
}

static uint32_t read_user_file(struct user_file *file, void *buffer, uint32_t elem_size,
                               uint32_t count) {
    return file->file_system->read ? file->file_system->read(buffer, elem_size, count, file->handle)
                                   : 0;
}

static int32_t seek_user_file(struct user_file *file, int32_t offset, int32_t origin) {
    return file->file_system->seek ? file->file_system->seek(file->handle, offset, origin) : -1;
}

static int32_t tell_user_file(struct user_file *file) {
    return file->file_system->tell ? file->file_system->tell(file->handle) : -1;
}

static int32_t user_file_size(struct user_file *file) {
    int32_t here = tell_user_file(file);
    if (here < 0 || seek_user_file(file, 0, SEEK_END) != 0) {
        return -1;
    }
    int32_t size = tell_user_file(file);
    seek_user_file(file, here, SEEK_SET);
    return size;
}

/* Names the game asked for and did not get; capped so a per-frame probe cannot flood the log. */
static void log_missing_file(const char *name) {
    static unsigned reported;
    if (reported < 200) {
        reported++;
        fprintf(stderr, "[file] not found: %s\n", name);
    }
}

/* Horizon has no /dev/null, so a zero-length file is kept in the port's folder instead. */
static FILE *open_empty_file(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/.empty", g_root);
    FILE *file = fopen(path, "wb");
    if (file) {
        fclose(file);
    }
    return fopen(path, "rb");
}

void *s3eFileOpen(const char *name, const char *mode) {
    char path[1200];
    const char *safe_name = name ? name : "";
    const char *safe_mode = mode ? mode : "rb";
    char fopen_mode[16];
    sanitize_file_mode(safe_mode, fopen_mode, sizeof(fopen_mode));
    FILE *file = NULL;

    if (is_user_file_mode(safe_mode) || is_user_file_name(safe_name)) {
        char seed_path[1200];
        make_user_path(path, sizeof(path), safe_name);
        make_parent_dirs(path);
        if (!is_user_file_name(safe_name) && safe_mode[0] == 'r' && !path_exists(path) &&
            resolve_read_path(safe_name, seed_path, sizeof(seed_path))) {
            copy_file(seed_path, path);
        }
        file = fopen(path, fopen_mode);
    } else if (is_read_mode(safe_mode) && (file = open_user_file(safe_name, safe_mode)) != NULL) {
        return file;
    } else if (is_read_mode(safe_mode) && resolve_read_path(safe_name, path, sizeof(path))) {
        file = fopen(path, fopen_mode);
    } else {
        make_path(path, sizeof(path), safe_name);
        if (!is_read_mode(safe_mode)) {
            make_parent_dirs(path);
        }
        file = fopen(path, fopen_mode);
    }
    if (!file && is_read_mode(safe_mode) && safe_name[0] != '/') {
        snprintf(path, sizeof(path), "%s/assets/%s", g_root, safe_name);
        file = fopen(path, fopen_mode);
    }
    if (!file && is_read_mode(safe_mode) && strcmp(base_name(safe_name), "console.bin") == 0) {
        file = open_empty_file();
    }
    if (!file && is_read_mode(safe_mode)) {
        log_missing_file(safe_name);
    }
    return file;
}

int32_t s3eFileClose(void *file) {
    if (!file) {
        return -1;
    }
    struct user_file *user_file = find_user_file(file);
    if (user_file) {
        return close_user_file(user_file);
    }
    return fclose((FILE *)file);
}

uint32_t s3eFileRead(void *buffer, uint32_t elem_size, uint32_t count, void *file) {
    struct user_file *user_file = find_user_file(file);
    if (user_file) {
        return read_user_file(user_file, buffer, elem_size, count);
    }
    return file ? (uint32_t)fread(buffer, elem_size, count, (FILE *)file) : 0;
}

uint32_t s3eFileWrite(const void *buffer, uint32_t elem_size, uint32_t count, void *file) {
    if (find_user_file(file)) {
        return 0;
    }
    return file ? (uint32_t)fwrite(buffer, elem_size, count, (FILE *)file) : 0;
}

int32_t s3eFileGetChar(void *file) {
    struct user_file *user_file = find_user_file(file);
    if (user_file) {
        uint8_t value;
        return read_user_file(user_file, &value, 1, 1) == 1 ? value : -1;
    }
    return file ? fgetc((FILE *)file) : -1;
}

int32_t s3eFilePutChar(int32_t c, void *file) {
    if (find_user_file(file)) {
        return -1;
    }
    return file ? fputc(c, (FILE *)file) : -1;
}

int32_t s3eFileFlush(void *file) {
    if (find_user_file(file)) {
        return 0;
    }
    return file ? fflush((FILE *)file) : -1;
}

int32_t s3eFileSeek(void *file, int32_t offset, int32_t origin) {
    struct user_file *user_file = find_user_file(file);
    if (user_file) {
        return seek_user_file(user_file, offset, origin);
    }
    return file ? fseek((FILE *)file, offset, origin) : -1;
}

int32_t s3eFileTell(void *file) {
    struct user_file *user_file = find_user_file(file);
    if (user_file) {
        return tell_user_file(user_file);
    }
    return file ? (int32_t)ftell((FILE *)file) : -1;
}

int32_t s3eFileGetSize(void *file) {
    struct user_file *user_file = find_user_file(file);
    if (user_file) {
        return user_file_size(user_file);
    }
    return file ? (int32_t)file_size_for_seek((FILE *)file) : -1;
}

int32_t s3eFileCheckExists(const char *name) {
    char path[1200];
    const char *safe_name = name ? name : "";
    if (is_user_file_name(safe_name)) {
        make_user_path(path, sizeof(path), safe_name);
        return access(path, F_OK) == 0 ? 1 : 0;
    }
    /* User file systems have no existence query, so a file exists if one of them can open it. */
    struct user_file *user_file = open_user_file(safe_name, "rb");
    if (user_file) {
        close_user_file(user_file);
        return 1;
    }
    if (resolve_read_path(safe_name, path, sizeof(path))) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/assets/%s", g_root, safe_name);
    return access(path, F_OK) == 0 ? 1 : 0;
}

int32_t s3eFileGetError(void) {
    return errno;
}

const char *s3eFileGetErrorString(void) {
    return strerror(errno);
}

void *s3eFileOpenFromMemory(void *buffer, uint32_t size) {
    return buffer && size ? fmemopen(buffer, size, "rb") : NULL;
}

int32_t s3eFileGetFileInt(void *file, uint32_t key) {
    (void)file;
    (void)key;
    return 0;
}

int32_t s3eFileMakeDirectory(const char *name) {
    char path[1200];
    make_path(path, sizeof(path), name);
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

int32_t s3eFileDelete(const char *name) {
    char path[1200];
    make_path(path, sizeof(path), name);
    return unlink(path);
}

int32_t s3eFileRename(const char *old_name, const char *new_name) {
    char old_path[1200];
    char new_path[1200];
    make_path(old_path, sizeof(old_path), old_name);
    make_path(new_path, sizeof(new_path), new_name);
    return rename(old_path, new_path);
}

int32_t s3eFileAddUserFileSys(const void *file_system) {
    if (!file_system || g_user_file_system_count >= USER_FILE_SYSTEM_MAX) {
        return 1;
    }
    /* The caller's table lives on its stack, and only these entries are always filled in. */
    memcpy(&g_user_file_systems[g_user_file_system_count++], file_system,
           sizeof(struct user_file_system));
    return 0;
}

void *s3eFileListDirectory(const char *path) {
    (void)path;
    return NULL;
}

int32_t s3eFileListClose(void *list) {
    (void)list;
    return 0;
}

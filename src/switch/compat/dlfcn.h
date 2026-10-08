#ifndef CODBOZ_SWITCH_DLFCN_H
#define CODBOZ_SWITCH_DLFCN_H

/*
 * Horizon has no dynamic linker for homebrew, so SDL2, SDL2_mixer, EGL and
 * OpenGL ES are linked statically. This shim keeps the loader's dlopen/dlsym
 * call sites working by resolving names from static tables (see switch_dl.c).
 */

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x0
#define RTLD_GLOBAL 0x100

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);
int dladdr(const void *address, Dl_info *info);

#endif

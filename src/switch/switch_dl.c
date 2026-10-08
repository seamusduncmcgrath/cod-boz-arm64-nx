#include <dlfcn.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <EGL/egl.h>
#include <SDL.h>
#include <SDL_mixer.h>

/*
 * dlopen/dlsym for a platform without a dynamic linker. Each "library" is a
 * token, and symbols come from tables of statically linked functions. The
 * tables list exactly what the loader looks up, so a new dlsym call elsewhere
 * needs a matching entry here.
 */

struct symbol {
    const char *name;
    void *address;
};

#define SYMBOL(name) {#name, (void *)(uintptr_t)&name}

static const struct symbol SDL2_SYMBOLS[] = {
    SYMBOL(SDL_CloseAudioDevice),
    SYMBOL(SDL_CreateWindow),
    SYMBOL(SDL_DestroyWindow),
    SYMBOL(SDL_FlushEvents),
    SYMBOL(SDL_GameControllerClose),
    SYMBOL(SDL_GameControllerGetAttached),
    SYMBOL(SDL_GameControllerGetAxis),
    SYMBOL(SDL_GameControllerGetButton),
    SYMBOL(SDL_GameControllerGetJoystick),
    SYMBOL(SDL_GameControllerNameForIndex),
    SYMBOL(SDL_GameControllerOpen),
    SYMBOL(SDL_GameControllerUpdate),
    SYMBOL(SDL_GetCurrentVideoDriver),
    SYMBOL(SDL_GetError),
    SYMBOL(SDL_GetNumTouchDevices),
    SYMBOL(SDL_GetNumTouchFingers),
    SYMBOL(SDL_GetTouchDevice),
    SYMBOL(SDL_GetTouchFinger),
    SYMBOL(SDL_GL_CreateContext),
    SYMBOL(SDL_GL_DeleteContext),
    SYMBOL(SDL_GL_GetProcAddress),
    SYMBOL(SDL_GL_MakeCurrent),
    SYMBOL(SDL_GL_SetAttribute),
    SYMBOL(SDL_GL_SwapWindow),
    SYMBOL(SDL_HasEvent),
    SYMBOL(SDL_InitSubSystem),
    SYMBOL(SDL_IsGameController),
    SYMBOL(SDL_JoystickGetHat),
    SYMBOL(SDL_JoystickNameForIndex),
    SYMBOL(SDL_JoystickNumHats),
    SYMBOL(SDL_NumJoysticks),
    SYMBOL(SDL_OpenAudioDevice),
    SYMBOL(SDL_PauseAudioDevice),
    SYMBOL(SDL_PumpEvents),
    SYMBOL(SDL_QuitSubSystem),
    SYMBOL(SDL_RWFromConstMem),
};

static const struct symbol SDL2_MIXER_SYMBOLS[] = {
    SYMBOL(Mix_AllocateChannels),
    SYMBOL(Mix_ChannelFinished),
    SYMBOL(Mix_CloseAudio),
    SYMBOL(Mix_FreeChunk),
    SYMBOL(Mix_HaltChannel),
    SYMBOL(Mix_Init),
    SYMBOL(Mix_LoadWAV_RW),
    SYMBOL(Mix_OpenAudio),
    SYMBOL(Mix_Pause),
    SYMBOL(Mix_Paused),
    SYMBOL(Mix_PlayChannelTimed),
    SYMBOL(Mix_Playing),
    SYMBOL(Mix_QuerySpec),
    SYMBOL(Mix_Quit),
    SYMBOL(Mix_Resume),
    SYMBOL(Mix_SetPostMix),
    SYMBOL(Mix_Volume),
    /* Mix_GetError is a macro for SDL_GetError. */
    {"Mix_GetError", (void *)(uintptr_t)&SDL_GetError},
};

static const struct symbol EGL_SYMBOLS[] = {
    SYMBOL(eglBindAPI),
    SYMBOL(eglBindTexImage),
    SYMBOL(eglCreateContext),
    SYMBOL(eglCreatePbufferSurface),
    SYMBOL(eglCreateWindowSurface),
    SYMBOL(eglDestroyContext),
    SYMBOL(eglDestroySurface),
    SYMBOL(eglGetConfigAttrib),
    SYMBOL(eglGetConfigs),
    SYMBOL(eglGetCurrentContext),
    SYMBOL(eglGetCurrentDisplay),
    SYMBOL(eglGetCurrentSurface),
    SYMBOL(eglGetDisplay),
    SYMBOL(eglGetError),
    SYMBOL(eglGetProcAddress),
    SYMBOL(eglInitialize),
    SYMBOL(eglMakeCurrent),
    SYMBOL(eglQueryContext),
    SYMBOL(eglQueryString),
    SYMBOL(eglQuerySurface),
    SYMBOL(eglReleaseTexImage),
    SYMBOL(eglSwapBuffers),
    SYMBOL(eglTerminate),
};

struct library {
    const char *name_fragment;
    const struct symbol *symbols;
    size_t symbol_count;
};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* Order matters: "SDL2_mixer" has to be tried before "SDL2". */
static const struct library LIBRARIES[] = {
    {"SDL2_mixer", SDL2_MIXER_SYMBOLS, COUNT(SDL2_MIXER_SYMBOLS)},
    {"SDL2", SDL2_SYMBOLS, COUNT(SDL2_SYMBOLS)},
    {"EGL", EGL_SYMBOLS, COUNT(EGL_SYMBOLS)},
    /* Mesa's one dispatch table serves OpenGL ES 1 and 2 names alike; see dlsym(). */
    {"GLES", NULL, 0},
};

static char g_error[160];
static int g_error_pending;

static void set_error(const char *what, const char *name) {
    snprintf(g_error, sizeof(g_error), "%s: %s", what, name ? name : "(null)");
    g_error_pending = 1;
}

void *dlopen(const char *path, int mode) {
    (void)mode;
    for (size_t i = 0; path && i < COUNT(LIBRARIES); ++i) {
        if (strstr(path, LIBRARIES[i].name_fragment)) {
            return (void *)(uintptr_t)&LIBRARIES[i];
        }
    }
    set_error("library is not linked into this build", path);
    return NULL;
}

void *dlsym(void *handle, const char *symbol) {
    const struct library *library = handle;
    if (!library || !symbol || library < LIBRARIES || library >= LIBRARIES + COUNT(LIBRARIES)) {
        set_error("invalid dlsym request", symbol);
        return NULL;
    }
    if (!library->symbols) {
        void *address = (void *)(uintptr_t)eglGetProcAddress(symbol);
        if (!address) {
            set_error("unknown OpenGL ES function", symbol);
        }
        return address;
    }
    for (size_t i = 0; i < library->symbol_count; ++i) {
        if (strcmp(library->symbols[i].name, symbol) == 0) {
            return library->symbols[i].address;
        }
    }
    set_error("symbol is not in the static table", symbol);
    return NULL;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

char *dlerror(void) {
    if (!g_error_pending) {
        return NULL;
    }
    g_error_pending = 0;
    return g_error;
}

int dladdr(const void *address, Dl_info *info) {
    (void)address;
    (void)info;
    return 0;
}

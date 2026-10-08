#include "codboz_hud_handlers.h"
#include "s3e_host.h"
#include "s3e_image.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

/* Everything the port reads or writes lives here: the game image, assets/, config and saves. */
#define PORT_ROOT "sdmc:/switch/boz"

enum {
    DISPLAY_WIDTH = 1280,
    DISPLAY_HEIGHT = 720,
    /* As much stack as a main thread has on Android, where this image otherwise runs. */
    GAME_STACK_SIZE = 8 * 1024 * 1024,
};

/*
 * The game image, first where it is once the assets folder of the game's package has been
 * copied over, then where one unpacked by hand used to go.
 */
static const char *const IMAGE_PATHS[] = {
    PORT_ROOT "/assets/boz_aarch64.s3e",
    PORT_ROOT "/boz.s3e",
    PORT_ROOT "/boz_aarch64",
};

static int g_log_fd = -1;
static uintptr_t g_image_base;
static size_t g_image_size;

/* Every write is flushed to the card so the log survives the game crashing or hanging. */
static ssize_t log_write(struct _reent *reent, void *fd, const char *data, size_t length) {
    (void)reent;
    (void)fd;
    if (g_log_fd >= 0 && write(g_log_fd, data, length) >= 0) {
        fsync(g_log_fd);
    }
    return (ssize_t)length;
}

static const devoptab_t g_log_device = {
    .name = "bozlog",
    .write_r = log_write,
};

/* stdout and stderr go to an nxlink host when there is one, otherwise to boz.log. */
static void log_open(void) {
    if (nxlinkStdio() >= 0) {
        return;
    }
    g_log_fd = open(PORT_ROOT "/boz.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_log_fd < 0) {
        return;
    }
    devoptab_list[STD_OUT] = &g_log_device;
    devoptab_list[STD_ERR] = &g_log_device;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

static void log_image_address(const char *name, u64 address) {
    if (g_image_base && address >= g_image_base && address - g_image_base < g_image_size) {
        fprintf(stderr, "[crash] %s is game image offset 0x%lx\n", name, address - g_image_base);
    }
}

/* libnx runs this on its own stack when a thread faults; the process is terminated afterwards. */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *context) {
    fprintf(stderr, "[crash] error=0x%x esr=0x%x far=0x%lx pc=0x%lx lr=0x%lx sp=0x%lx\n",
            context->error_desc, context->esr, context->far.x, context->pc.x, context->lr.x,
            context->sp.x);
    log_image_address("pc", context->pc.x);
    log_image_address("lr", context->lr.x);
    for (int i = 0; i < 8; ++i) {
        fprintf(stderr, "[crash] x%d=0x%lx\n", i, context->cpu_gprs[i].x);
    }
}

/* Shows a message on the text console until + is pressed. Only usable while SDL video is down. */
static void fatal_screen(const char *format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    fprintf(stderr, "[fatal] %s\n", message);

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("Call of Duty: Black Ops Zombies loader\n\n%s\n\nPress + to exit.\n", message);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

static const char *find_image(void) {
    for (size_t i = 0; i < sizeof(IMAGE_PATHS) / sizeof(IMAGE_PATHS[0]); ++i) {
        if (access(IMAGE_PATHS[i], F_OK) == 0) {
            return IMAGE_PATHS[i];
        }
    }
    return NULL;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    uint8_t *data = NULL;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
        fseek(file, 0, SEEK_SET) == 0) {
        data = malloc((size_t)length);
    }
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    if (data) {
        *size = (size_t)length;
    }
    return data;
}

/* Appends one ICF file to the buffer, newline-terminated. Returns false if it is not there. */
static bool append_config_file(const char *name, uint8_t **config, size_t *config_size) {
    static const char *const directories[] = {PORT_ROOT "/assets", PORT_ROOT};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", directories[i], name);
        size_t size = 0;
        uint8_t *data = read_file(path, &size);
        if (!data) {
            continue;
        }
        uint8_t *grown = realloc(*config, *config_size + size + 1);
        if (grown) {
            memcpy(grown + *config_size, data, size);
            grown[*config_size + size] = '\n';
            *config = grown;
            *config_size += size + 1;
            fprintf(stderr, "[loader] config: %s\n", path);
        }
        free(data);
        return grown != NULL;
    }
    return false;
}

/*
 * The game's settings come beside its image, as assets/s3e.icf and assets/app.icf; app.icf goes
 * last so its values win.
 */
static void load_config(void) {
    uint8_t *config = NULL;
    size_t config_size = 0;
    append_config_file("s3e.icf", &config, &config_size);
    if (!append_config_file("app.icf", &config, &config_size)) {
        fprintf(stderr, "[loader] no app.icf under " PORT_ROOT "/assets: the game will run on "
                        "the loader's defaults\n");
    }
    s3e_host_set_config(config, (uint32_t)config_size);
    free(config);
}

struct game_run {
    uintptr_t entry;
    int result;
    bool returned;
};

static void *game_thread(void *opaque) {
    struct game_run *run = opaque;
    int (*entry)(void) = (int (*)(void))run->entry;
    run->result = entry();
    run->returned = true;
    return NULL;
}

/* Returns false if the thread could not be started. */
static bool run_game(struct game_run *run) {
    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes) != 0) {
        return false;
    }
    bool started = pthread_attr_setstacksize(&attributes, GAME_STACK_SIZE) == 0 &&
                   pthread_create(&thread, &attributes, game_thread, run) == 0;
    pthread_attr_destroy(&attributes);
    if (started) {
        pthread_join(thread, NULL);
    }
    return started;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    socketInitializeDefault();
    mkdir(PORT_ROOT, 0777);
    log_open();
    fprintf(stderr, "[loader] Black Ops Zombies for Nintendo Switch, version " BOZ_VERSION "\n");

    AppletType applet_type = appletGetAppletType();
    if (applet_type != AppletType_Application && applet_type != AppletType_SystemApplication) {
        fprintf(stderr, "[loader] running as an applet: memory is limited. Launch hbmenu by "
                        "holding R on a game for full memory.\n");
    }

    const char *image_path = find_image();
    if (!image_path) {
        fatal_screen("Game files not found.\n\nCopy the assets folder from the game's APK to\n"
                     "  %s/assets\n\nThe game itself is the file\n  %s",
                     PORT_ROOT, IMAGE_PATHS[0]);
        socketExit();
        return 1;
    }
    fprintf(stderr, "[loader] image: %s\n", image_path);

    struct s3e_image image;
    if (!s3e_image_load(image_path, &image) || !s3e_image_parse_symbols(&image)) {
        fatal_screen("%s\nis not a usable game image. Try copying it again.\nSee %s/boz.log.",
                     image_path, PORT_ROOT);
        s3e_image_free(&image);
        socketExit();
        return 1;
    }
    fprintf(stderr, "[loader] S3E version 0x%x, architecture 0x%x, %zu imports, %zu bytes\n",
            image.header.version, image.header.arch, image.symbols.count, image.file_size);

    if (!s3e_host_set_display_size(DISPLAY_WIDTH, DISPLAY_HEIGHT) || !s3e_host_init(PORT_ROOT)) {
        fatal_screen("Host initialisation failed.\nSee %s/boz.log.", PORT_ROOT);
        s3e_image_free(&image);
        socketExit();
        return 1;
    }
    load_config();

    struct s3e_loaded_image loaded;
    if (!s3e_image_map_and_relocate(&image, s3e_host_resolve, &loaded)) {
        s3e_host_shutdown();
        fatal_screen("Could not map the game image as executable code.\nSee %s/boz.log.",
                     PORT_ROOT);
        s3e_image_free(&image);
        socketExit();
        return 1;
    }
    g_image_base = (uintptr_t)loaded.base;
    g_image_size = loaded.map_size;
    fprintf(stderr, "[loader] image mapped at %p\n", (void *)loaded.base);
    if (!codboz_install_hud_handlers(&loaded)) {
        fprintf(stderr, "[loader] not the build of the game this port knows: every action is "
                        "sent as a key, and FieldOfView, FrameRate and LookSensitivity do "
                        "nothing\n");
    }

    struct game_run run = {.entry = (uintptr_t)(loaded.base + loaded.entry_offset)};
    if (!run_game(&run)) {
        fprintf(stderr, "[loader] unable to start the game thread\n");
    } else if (run.returned) {
        fprintf(stderr, "[loader] the game returned %d\n", run.result);
    } else {
        fprintf(stderr, "[loader] the game exited\n");
    }

    g_image_base = 0;
    s3e_loaded_image_unmap(&loaded);
    s3e_host_shutdown();
    s3e_image_free(&image);
    socketExit();
    return 0;
}

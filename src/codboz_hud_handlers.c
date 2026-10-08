#include "codboz_hud_handlers.h"
#include "s3e_host_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The 1.3.1 AArch64 build cannot reload or throw a frag grenade from a key: its
 * reloadChangeWeapon command only arms a second press that changes weapon, and +throwGrenade
 * never raises the grenade. The handlers the HUD attaches to its on-screen buttons do both.
 * They take no arguments and look the player up themselves, so the controller calls them.
 *
 * Crouch and prone are different. The game's own routes to them, the crouchProne keys and the
 * HUD's prone button, post an event to the player's entity, and on this build neither moves
 * the player. The player controller's event handler is given the event directly instead.
 *
 * Fire is a flag the input manager drops every frame. The game's touch controls raise it again
 * each frame the button is held, but its +shoot key raises it once, so a held trigger fired for
 * a single frame: a shot from most guns and not enough to spin a minigun up. The setter the
 * touch controls use is called every frame instead.
 *
 * Using things and sprinting share a key, actionSprint, which is no way to have them on a
 * controller. Its command uses whatever the player is standing at and raises a flag in the
 * input manager; while the flag is up the move stick's events are posted as a sprint. So the
 * command is called for the one, with the flag put back afterwards, and the flag is written
 * for the other.
 *
 * The field of view is the game's own setting, widened. When a match starts the player's
 * weapons copy the level camera's value (50, about 71 degrees across a 16:9 screen) as the one
 * to ease back to after aiming. Writing another there has the game ease the camera to it.
 *
 * The frame rate is a matter of two clocks. The world clock ticks channels of systems at fixed
 * intervals, and the systems on its second channel take turns at two steps of 16 ms, so each is
 * updated every 32 ms: 31.25 times a second however often a frame is drawn. That interval is
 * shortened until they are updated once a frame, and each tick is told the shorter time. The
 * frame timer is set by the game to advance one step of 33 ms every frame whatever time has
 * passed, twice real time at 60 frames a second, so it is given the time that did pass.
 *
 * Most systems take the time a tick tells them, but the player's controller was written for an
 * update every 32 ms. It counts updates where it means time, and in each it moves and turns the
 * player a set amount if a stick has sent an event since the last. The sticks send one a frame,
 * so with an update for every frame each frame's event moves the player, twice as often. Those
 * counts and amounts are each scaled by a tunable the game looks up by name, and the tunables
 * are changed to suit, or the player alone runs fast.
 *
 * The tunable that scales turning is also how the look stick is made quicker. The game has a
 * sensitivity setting, but it reads as 1 for the kind of controls it uses with a controller.
 */
enum {
    HUD_HANDLERS_OFFSET = 0x0f1c0c,
    HUD_HANDLERS_SIZE = 0x190,
    HUD_RELOAD_OFFSET = 0x0f1c0c,
    HUD_SWAP_WEAPON_OFFSET = 0x0f1c40,
    HUD_HOLD_GRENADE_OFFSET = 0x0f1cf4,
    HUD_THROW_GRENADE_OFFSET = 0x0f1d48,
    /* Where a stick's event is posted, as a sprint while the input manager's flag is up. */
    INPUT_MANAGER_POST_STICK_OFFSET = 0x0fb214,
    INPUT_MANAGER_POST_STICK_SIZE = 0x60,
    INPUT_MANAGER_SET_FIRE_OFFSET = 0x0fb2c4,
    INPUT_MANAGER_SET_FIRE_SIZE = 0x50,
    /* The input manager's commands: how one is picked by its number, and the actionSprint pair. */
    INPUT_MANAGER_COMMAND_OFFSET = 0x0fb840,
    INPUT_MANAGER_COMMAND_SIZE = 0x34,
    INPUT_MANAGER_ACTION_SPRINT_OFFSET = 0x0fbaa0,
    INPUT_MANAGER_ACTION_SPRINT_SIZE = 0x48,
    CONTROLLER_LOOKUP_OFFSET = 0x13bfbc,
    CONTROLLER_LOOKUP_SIZE = 0x14,
    /* Reads the controller's weapons, as the offset below does. */
    CONTROLLER_WEAPONS_GETTER_OFFSET = 0x13d3b0,
    CONTROLLER_WEAPONS_GETTER_SIZE = 0x14,
    /* The player's movement step, as far as the time allowed in water as a count of updates. */
    CONTROLLER_MOVE_STEP_OFFSET = 0x142d28,
    CONTROLLER_MOVE_STEP_SIZE = 0xe8,
    /* Where it divides the distance for one update by a tunable. */
    CONTROLLER_MOVE_DAMPING_OFFSET = 0x1432fc,
    CONTROLLER_MOVE_DAMPING_SIZE = 0x6c,
    /* Where it turns the player and tilts the view by what the look stick's events left. */
    CONTROLLER_TURN_OFFSET = 0x143b74,
    CONTROLLER_TURN_SIZE = 0x2c,
    CONTROLLER_TILT_OFFSET = 0x143df4,
    CONTROLLER_TILT_SIZE = 0x24,
    /* Where it counts updates in water down to the player's complaint. */
    CONTROLLER_WATER_VOICE_OFFSET = 0x144040,
    CONTROLLER_WATER_VOICE_SIZE = 0x78,
    CONTROLLER_EVENT_HANDLER_OFFSET = 0x1442ac,
    CONTROLLER_EVENT_HANDLER_SIZE = 0x3c,
    /* The part of that handler that takes the look stick, scaled by a tunable. */
    CONTROLLER_LOOK_EVENT_OFFSET = 0x1447e0,
    CONTROLLER_LOOK_EVENT_SIZE = 0xac,
    /* The parts that act on the crouch and prone events. */
    CONTROLLER_STANCE_EVENTS_OFFSET = 0x14517c,
    CONTROLLER_STANCE_EVENTS_SIZE = 0x90,
    CONTROLLER_CROUCH_TAIL_OFFSET = 0x1457b4,
    CONTROLLER_CROUCH_TAIL_SIZE = 0x1c,
    /* Where the weapons ease the camera between the aiming and resting fields of view. */
    WEAPONS_VIEW_EASING_OFFSET = 0x1a5b7c,
    WEAPONS_VIEW_EASING_SIZE = 0x98,
    /* Where they take the resting one from the level's camera. */
    WEAPONS_VIEW_SETUP_OFFSET = 0x1a983c,
    WEAPONS_VIEW_SETUP_SIZE = 0x10,
    /* The world clock: seven channels at 60 a second, and what a tick tells the systems. */
    WORLD_CLOCK_CONSTRUCTOR_OFFSET = 0x2c0f28,
    WORLD_CLOCK_CONSTRUCTOR_SIZE = 0x50,
    WORLD_CLOCK_TICK_OFFSET = 0x2c1070,
    WORLD_CLOCK_TICK_SIZE = 0x74,
    /* Where the one world clock is built, which fixes its address. */
    WORLD_CLOCK_INSTANCE_OFFSET = 0x2c110c,
    WORLD_CLOCK_INSTANCE_SIZE = 0x34,
    /* How its channels are laid out, and how their intervals turn time into ticks. */
    WORLD_CLOCK_CHANNELS_OFFSET = 0x2c347c,
    WORLD_CLOCK_CHANNELS_SIZE = 0xa4,
    WORLD_CLOCK_UPDATE_OFFSET = 0x2c3548,
    WORLD_CLOCK_UPDATE_SIZE = 0x1bc,
    /* The frame timer: what the game reads as a frame's time, and how a step is scaled to it. */
    FRAME_TIMER_STEP_GETTER_OFFSET = 0x2d1adc,
    FRAME_TIMER_STEP_GETTER_SIZE = 0x8,
    FRAME_TIMER_SCALE_OFFSET = 0x2d1c20,
    FRAME_TIMER_SCALE_SIZE = 0x50,
    /* Its update, as far as the end of the branch that takes one step a frame. */
    FRAME_TIMER_UPDATE_OFFSET = 0x2d1d1c,
    FRAME_TIMER_UPDATE_SIZE = 0xc0,
    FRAME_TIMER_CONSTRUCTOR_OFFSET = 0x2d2030,
    FRAME_TIMER_CONSTRUCTOR_SIZE = 0x78,
    /* Not code: where the input manager's commands are, by number. */
    INPUT_MANAGER_COMMAND_TABLE_OFFSET = 0x46f90c,
    INPUT_MANAGER_COMMAND_TABLE_SIZE = 0x20,
    WORLD_CLOCK_TICK_SLOT_OFFSET = 0x4d4658,
    FRAME_TIMER_STEP_GETTER_SLOT_OFFSET = 0x4d4ea0,
    CONTROLLER_EVENT_HANDLER_SLOT_OFFSET = 0x4f6d20,
    PRONE_EVENT_SLOT_OFFSET = 0x529898,
    CROUCH_EVENT_SLOT_OFFSET = 0x529a38,
    GAME_POINTER_OFFSET = 0x543218,
    HUD_POINTER_OFFSET = 0x545ad8,
    INPUT_MANAGER_POINTER_OFFSET = 0x550bd0,
    CROUCH_EVENT_OFFSET = 0x551858,
    PRONE_EVENT_OFFSET = 0x55185c,
    /* Where the controller keeps each tunable once it has looked it up: a flag, then a pointer. */
    WATER_TIME_TUNABLE_OFFSET = 0x55a0f0,
    MOVE_DAMPING_TUNABLE_OFFSET = 0x55a100,
    WATER_VOICE_TUNABLE_OFFSET = 0x55a240,
    LOOK_SCALE_TUNABLE_OFFSET = 0x55a370,
    WORLD_CLOCK_OFFSET = 0x5b9fd0,
    FRAME_TIMER_POINTER_OFFSET = 0x5c32b8,
    /* Within the game object: its player, null outside a match. */
    GAME_PLAYER_OFFSET = 496,
    /* Within the input manager: up while the actionSprint key is down. */
    INPUT_MANAGER_SPRINT_OFFSET = 133,
    /* The number of the command for that key going down. */
    ACTION_SPRINT_COMMAND = 2,
    /* Within the player controller. */
    CONTROLLER_WEAPONS_OFFSET = 248,
    /* Within the player's weapons. */
    WEAPONS_CAMERA_OFFSET = 224,
    WEAPONS_RESTING_VIEW_OFFSET = 320,
    /* Within the place a tunable is kept, and within the tunable. */
    TUNABLE_POINTER_OFFSET = 8,
    TUNABLE_PLACE_SIZE = 16,
    TUNABLE_VALUE_OFFSET = 32,
    /* Within the world clock, and within each of its channels. */
    WORLD_CLOCK_CHANNEL_ARRAY_OFFSET = 24,
    WORLD_CLOCK_CHANNEL_COUNT_OFFSET = 32,
    WORLD_CLOCK_SIZE = 40,
    CHANNEL_SIZE = 40,
    CHANNEL_STEPS_OFFSET = 4,
    CHANNEL_INTERVAL_OFFSET = 16,
    /* Within the frame timer: its step in milliseconds, raw and scaled, and "one a frame". */
    FRAME_TIMER_STEP_OFFSET = 24,
    FRAME_TIMER_SCALED_STEP_OFFSET = 28,
    FRAME_TIMER_STEP_EACH_FRAME_OFFSET = 47,
};

/*
 * In the game's own unit, which is not degrees of what is seen: its 50 shows about 71. The game
 * only eases outwards to the resting value, so one below its own would not take hold.
 */
enum {
    DEFAULT_FIELD_OF_VIEW = 65,
    MIN_FIELD_OF_VIEW = 50,
    MAX_FIELD_OF_VIEW = 150,
};

/* A 60 Hz screen shows nothing above 60, and at 30 the world clock is the game's own again. */
enum {
    DEFAULT_FRAME_RATE = 60,
    MIN_FRAME_RATE = 30,
    MAX_FRAME_RATE = 60,
    /* The channel most of what moves is on, as the game sets it up. */
    WORLD_CHANNEL = 1,
    WORLD_CHANNEL_STEPS = 2,
    WORLD_CHANNEL_INTERVAL_MS = 16,
    /* The clock gives a channel at most this many ticks in a frame and drops the time left. */
    WORLD_CLOCK_TICKS_A_FRAME = 4,
    /* The longest frame the game's own timer would count, at its slowest setting. */
    MAX_FRAME_STEP_MS = 100,
};

/* A percentage of the speed the game turns the player at for the look stick. */
enum {
    DEFAULT_LOOK_SENSITIVITY = 200,
    MIN_LOOK_SENSITIVITY = 25,
    MAX_LOOK_SENSITIVITY = 1000,
    OWN_LOOK_SENSITIVITY = 100,
};

enum {
    WORLD_CLOCK_UNSEEN,
    WORLD_CLOCK_KNOWN,
    WORLD_CLOCK_UNKNOWN,
};

/* How a tunable bears on the player, which is what it has to be scaled by. */
enum {
    /* A time the controller turns into a count of its updates. */
    COUNTS_UPDATES,
    /* Divides, or multiplies, what a stick's event does in the update that takes it. */
    DIVIDES_AN_EVENT,
    MULTIPLIES_AN_EVENT,
};

enum {
    USES_INPUT_MANAGER = 1,
    USES_HUD = 2,
};

/*
 * FNV-1a of the code above and the one table. The instructions address the objects they use
 * relative to themselves, so a match fixes those offsets too.
 */
#define GAME_CODE_HASH UINT64_C(0xa1e487838d55e4b3)

typedef void (*hud_handler_fn)(void);
typedef void (*set_fire_fn)(uint8_t *input_manager, uint32_t firing, uint32_t held);
typedef void (*input_command_fn)(uint8_t *input_manager, uint32_t command);
typedef uint8_t *(*controller_lookup_fn)(const uint8_t *player);
typedef void (*controller_event_fn)(uint8_t *controller, void *subject, uint32_t event,
                                    void *argument1, void *argument2);
typedef int32_t (*frame_timer_scale_fn)(uint8_t *timer, int32_t step);

struct hud_handler {
    size_t offset;
    uint8_t uses;
};

struct update_tunable {
    size_t offset;
    uint8_t bearing;
    /* Held as an int32_t, not a float. */
    uint8_t whole;
    /*
     * The game's own value and the one last left there, as they are stored. Both start as
     * zero, which is right for a tunable first found holding zero.
     */
    uint32_t own;
    uint32_t left;
};

static uint8_t *g_image;
/*
 * The game's playerMoveSpeedDamp, TurnMult, MaxTimeInWater and PlayerVoxWaterDelay. The distance
 * the move stick's event moves the player is divided by the first, and the turn the look stick's
 * asks for is multiplied by the second. The others are times in milliseconds that the controller
 * turns into counts of updates at 30 a second.
 */
static struct update_tunable g_update_tunables[] = {
    {MOVE_DAMPING_TUNABLE_OFFSET, DIVIDES_AN_EVENT, 0, 0, 0},
    {LOOK_SCALE_TUNABLE_OFFSET, MULTIPLIES_AN_EVENT, 0, 0, 0},
    {WATER_TIME_TUNABLE_OFFSET, COUNTS_UPDATES, 1, 0, 0},
    {WATER_VOICE_TUNABLE_OFFSET, COUNTS_UPDATES, 1, 0, 0},
};
static const struct hud_handler g_reload = {HUD_RELOAD_OFFSET, USES_INPUT_MANAGER};
static const struct hud_handler g_swap_weapon = {HUD_SWAP_WEAPON_OFFSET, USES_INPUT_MANAGER};
/* The grenade handlers ask the HUD which grenade is selected. */
static const struct hud_handler g_hold_grenade = {HUD_HOLD_GRENADE_OFFSET,
                                                  USES_INPUT_MANAGER | USES_HUD};
static const struct hud_handler g_throw_grenade = {HUD_THROW_GRENADE_OFFSET,
                                                   USES_INPUT_MANAGER | USES_HUD};
static uint8_t g_sprinting;
static float g_field_of_view;
static uint32_t g_look_sensitivity;
static uint32_t g_frame_rate;
static uint64_t g_last_frame_us;
static uint8_t g_world_clock;

static bool image_range_valid(const struct s3e_loaded_image *loaded, size_t offset, size_t size) {
    return loaded && loaded->base && offset <= loaded->map_size &&
           size <= loaded->map_size - offset;
}

static uint64_t image_hash(uint64_t hash, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    }
    return hash;
}

static uint8_t *read_pointer(const uint8_t *address) {
    uint8_t *pointer;
    memcpy(&pointer, address, sizeof(pointer));
    return pointer;
}

static const uint8_t *current_player(void) {
    const uint8_t *game = read_pointer(g_image + GAME_POINTER_OFFSET);
    return game ? read_pointer(game + GAME_PLAYER_OFFSET) : NULL;
}

/*
 * The handlers use these objects unchecked, as the HUD is only on screen while they all exist:
 * with no player they read through a null pointer.
 */
static bool handler_usable(const struct hud_handler *handler) {
    if (!current_player()) {
        return false;
    }
    if ((handler->uses & USES_INPUT_MANAGER) &&
        !read_pointer(g_image + INPUT_MANAGER_POINTER_OFFSET)) {
        return false;
    }
    return !(handler->uses & USES_HUD) || read_pointer(g_image + HUD_POINTER_OFFSET);
}

static void run_handler(const struct hud_handler *handler) {
    if (handler_usable(handler)) {
        ((hud_handler_fn)(uintptr_t)(g_image + handler->offset))();
    }
}

static void hud_reload(void) {
    run_handler(&g_reload);
}

static void hud_swap_weapon(void) {
    run_handler(&g_swap_weapon);
}

static void hud_hold_grenade(void) {
    run_handler(&g_hold_grenade);
}

static void hud_throw_grenade(void) {
    run_handler(&g_throw_grenade);
}

/* Raises the fire flag for one frame, as the game's touch controls do while a finger is down. */
static void input_manager_fire(void) {
    uint8_t *input_manager = read_pointer(g_image + INPUT_MANAGER_POINTER_OFFSET);
    if (current_player() && input_manager) {
        ((set_fire_fn)(uintptr_t)(g_image + INPUT_MANAGER_SET_FIRE_OFFSET))(input_manager, 1, 0);
    }
}

/*
 * The command uses what the player is standing at, if the game is offering anything there. It
 * leaves the sprint flag up, as the key it is for would until let go.
 */
static void input_manager_action(void) {
    uint8_t *input_manager = read_pointer(g_image + INPUT_MANAGER_POINTER_OFFSET);
    if (current_player() && input_manager) {
        ((input_command_fn)(uintptr_t)(g_image + INPUT_MANAGER_COMMAND_OFFSET))(
            input_manager, ACTION_SPRINT_COMMAND);
        input_manager[INPUT_MANAGER_SPRINT_OFFSET] = g_sprinting;
    }
}

static void input_manager_sprint(uint8_t sprinting) {
    uint8_t *input_manager = read_pointer(g_image + INPUT_MANAGER_POINTER_OFFSET);
    g_sprinting = sprinting;
    if (input_manager) {
        input_manager[INPUT_MANAGER_SPRINT_OFFSET] = sprinting;
    }
}

static void input_manager_sprint_start(void) {
    input_manager_sprint(1);
}

static void input_manager_sprint_stop(void) {
    input_manager_sprint(0);
}

/* Gives the player controller a stance event as its input manager would. */
static void controller_stance_event(size_t event_offset) {
    controller_lookup_fn lookup =
        (controller_lookup_fn)(uintptr_t)(g_image + CONTROLLER_LOOKUP_OFFSET);
    controller_event_fn handle_event =
        (controller_event_fn)(uintptr_t)(g_image + CONTROLLER_EVENT_HANDLER_OFFSET);
    const uint8_t *player = current_player();
    uint8_t *controller = player ? lookup(player) : NULL;
    uint32_t event;
    memcpy(&event, g_image + event_offset, sizeof(event));
    if (controller && event) {
        handle_event(controller, NULL, event, NULL, NULL);
    }
}

/*
 * Runs every frame, as each match gives the player new weapons. A resting value of zero means
 * they have not found the level's camera, and the game would ease a camera it does not have.
 */
static void widen_field_of_view(void) {
    controller_lookup_fn lookup =
        (controller_lookup_fn)(uintptr_t)(g_image + CONTROLLER_LOOKUP_OFFSET);
    const uint8_t *player = current_player();
    const uint8_t *controller = player ? lookup(player) : NULL;
    uint8_t *weapons = controller ? read_pointer(controller + CONTROLLER_WEAPONS_OFFSET) : NULL;
    if (!weapons || !read_pointer(weapons + WEAPONS_CAMERA_OFFSET)) {
        return;
    }

    float resting;
    memcpy(&resting, weapons + WEAPONS_RESTING_VIEW_OFFSET, sizeof(resting));
    if (resting > 0.0f && resting != g_field_of_view) {
        memcpy(weapons + WEAPONS_RESTING_VIEW_OFFSET, &g_field_of_view, sizeof(g_field_of_view));
    }
}

/* [LOADER] FieldOfView in app.icf: 0 leaves the game's own, and without it there is a default. */
static void configure_field_of_view(void) {
    int32_t configured = DEFAULT_FIELD_OF_VIEW;
    if (s3eConfigGetInt("LOADER", "FieldOfView", &configured) == 0 && configured != 0 &&
        (configured < MIN_FIELD_OF_VIEW || configured > MAX_FIELD_OF_VIEW)) {
        fprintf(stderr, "[game] FieldOfView=%d is outside %d to %d: using %d\n", (int)configured,
                MIN_FIELD_OF_VIEW, MAX_FIELD_OF_VIEW, DEFAULT_FIELD_OF_VIEW);
        configured = DEFAULT_FIELD_OF_VIEW;
    }
    g_field_of_view = (float)configured;
}

/* [LOADER] LookSensitivity in app.icf, a percentage: 0 or 100 is the game's own turning. */
static void configure_look_sensitivity(void) {
    int32_t configured = DEFAULT_LOOK_SENSITIVITY;
    if (s3eConfigGetInt("LOADER", "LookSensitivity", &configured) == 0 && configured != 0 &&
        (configured < MIN_LOOK_SENSITIVITY || configured > MAX_LOOK_SENSITIVITY)) {
        fprintf(stderr, "[game] LookSensitivity=%d is outside %d to %d: using %d\n",
                (int)configured, MIN_LOOK_SENSITIVITY, MAX_LOOK_SENSITIVITY,
                DEFAULT_LOOK_SENSITIVITY);
        configured = DEFAULT_LOOK_SENSITIVITY;
    }
    g_look_sensitivity = configured ? (uint32_t)configured : OWN_LOOK_SENSITIVITY;
}

/*
 * The channel is looked at once, when the clock first has it, and is left alone unless it is as
 * the game is known to set it up. A frame too long for the shorter interval's ticks to cover is
 * given the game's own, so that the world is drawn less smoothly instead of running slow.
 * Returns the interval the channel is left with, or zero if it is not one this can set.
 */
static uint32_t quicken_world_clock(uint32_t frame_ms) {
    const uint8_t *clock = g_image + WORLD_CLOCK_OFFSET;
    uint8_t *channels = read_pointer(clock + WORLD_CLOCK_CHANNEL_ARRAY_OFFSET);
    uint32_t count;
    memcpy(&count, clock + WORLD_CLOCK_CHANNEL_COUNT_OFFSET, sizeof(count));
    if (g_world_clock == WORLD_CLOCK_UNKNOWN || !channels || count <= WORLD_CHANNEL) {
        return 0;
    }

    uint8_t *channel = channels + WORLD_CHANNEL * CHANNEL_SIZE;
    uint64_t interval;
    memcpy(&interval, channel + CHANNEL_INTERVAL_OFFSET, sizeof(interval));
    if (g_world_clock == WORLD_CLOCK_UNSEEN) {
        uint32_t steps;
        memcpy(&steps, channel + CHANNEL_STEPS_OFFSET, sizeof(steps));
        if (steps != WORLD_CHANNEL_STEPS || interval != WORLD_CHANNEL_INTERVAL_MS) {
            g_world_clock = WORLD_CLOCK_UNKNOWN;
            fprintf(stderr, "[game] world clock has %u steps of %u ms: FrameRate is not applied\n",
                    (unsigned)steps, (unsigned)interval);
            return 0;
        }
        g_world_clock = WORLD_CLOCK_KNOWN;
    }

    /* At the lowest rate allowed this is the game's own interval, and nothing is written. */
    uint64_t wanted = 1000 / (g_frame_rate * WORLD_CHANNEL_STEPS);
    if (2 * frame_ms > (2 * WORLD_CLOCK_TICKS_A_FRAME - 1) * wanted) {
        wanted = WORLD_CHANNEL_INTERVAL_MS;
    }
    if (wanted != interval) {
        memcpy(channel + CHANNEL_INTERVAL_OFFSET, &wanted, sizeof(wanted));
    }
    return (uint32_t)wanted;
}

/*
 * The game's own value, more over fewer times as large. One that is not positive, or would not
 * fit once scaled, is not a value this knows how to scale and is left as it is. So is any value
 * when the two are the same, which arithmetic on a float would not promise.
 */
static uint32_t scale_tunable(const struct update_tunable *tunable, uint32_t more,
                              uint32_t fewer) {
    uint32_t stored = tunable->own;
    if (more == fewer) {
        return stored;
    }
    if (tunable->whole) {
        int32_t own;
        memcpy(&own, &tunable->own, sizeof(own));
        int64_t scaled = (int64_t)own * more / fewer;
        if (own > 0 && scaled <= INT32_MAX) {
            own = (int32_t)scaled;
            memcpy(&stored, &own, sizeof(stored));
        }
    } else {
        float own;
        memcpy(&own, &tunable->own, sizeof(own));
        if (own > 0.0f) {
            own = own * (float)more / (float)fewer;
            memcpy(&stored, &own, sizeof(stored));
        }
    }
    return stored;
}

/*
 * The game looks each tunable up the first time the code that uses it runs, so they turn up one
 * by one. A value other than the one last left there is the game's own again, set since.
 *
 * At the game's own interval an update acts on the first stick event of the two frames or so
 * since the last, as it always has. At a shorter one it acts on every frame's, which is made to
 * stand for the time that frame took: at least an update's, as two in one update count once.
 * The one tunable that multiplies an event is the look stick's, which the sensitivity asked for
 * scales as well.
 */
static void pace_tunables(uint32_t interval, uint32_t frame_us) {
    uint32_t own_update_us = WORLD_CHANNEL_INTERVAL_MS * WORLD_CHANNEL_STEPS * 1000;
    uint32_t update_us = interval * WORLD_CHANNEL_STEPS * 1000;
    uint32_t event_us = own_update_us;
    if (interval != WORLD_CHANNEL_INTERVAL_MS) {
        event_us = frame_us > update_us ? frame_us : update_us;
    }

    for (size_t i = 0; i < sizeof(g_update_tunables) / sizeof(g_update_tunables[0]); ++i) {
        struct update_tunable *tunable = &g_update_tunables[i];
        const uint8_t *place = g_image + tunable->offset;
        uint8_t *object = (place[0] & 1) ? read_pointer(place + TUNABLE_POINTER_OFFSET) : NULL;
        if (!object) {
            continue;
        }

        uint32_t stored;
        memcpy(&stored, object + TUNABLE_VALUE_OFFSET, sizeof(stored));
        if (stored != tunable->left) {
            tunable->own = stored;
        }
        if (tunable->bearing == COUNTS_UPDATES) {
            tunable->left = scale_tunable(tunable, own_update_us, update_us);
        } else if (tunable->bearing == DIVIDES_AN_EVENT) {
            tunable->left = scale_tunable(tunable, own_update_us, event_us);
        } else {
            tunable->left = scale_tunable(tunable, event_us * g_look_sensitivity,
                                          own_update_us * OWN_LOOK_SENSITIVITY);
        }
        if (tunable->left != stored) {
            memcpy(object + TUNABLE_VALUE_OFFSET, &tunable->left, sizeof(tunable->left));
        }
    }
}

/*
 * The step is what the last frame took. The game's own scaling of it, which is nothing while
 * paused, is what the rest of the game reads as the time a frame covers.
 */
static void follow_frame_time(uint32_t frame_ms) {
    frame_timer_scale_fn scale =
        (frame_timer_scale_fn)(uintptr_t)(g_image + FRAME_TIMER_SCALE_OFFSET);
    uint8_t *timer = read_pointer(g_image + FRAME_TIMER_POINTER_OFFSET);
    if (!frame_ms || !timer || !timer[FRAME_TIMER_STEP_EACH_FRAME_OFFSET]) {
        return;
    }

    int32_t step = (int32_t)frame_ms;
    int32_t scaled = scale(timer, step);
    memcpy(timer + FRAME_TIMER_STEP_OFFSET, &step, sizeof(step));
    memcpy(timer + FRAME_TIMER_SCALED_STEP_OFFSET, &scaled, sizeof(scaled));
}

/* [LOADER] FrameRate in app.icf: 0 leaves the game's own timing; without it there is a default. */
static void configure_frame_rate(void) {
    int32_t configured = DEFAULT_FRAME_RATE;
    if (s3eConfigGetInt("LOADER", "FrameRate", &configured) == 0 && configured != 0 &&
        (configured < MIN_FRAME_RATE || configured > MAX_FRAME_RATE)) {
        fprintf(stderr, "[game] FrameRate=%d is outside %d to %d: using %d\n", (int)configured,
                MIN_FRAME_RATE, MAX_FRAME_RATE, DEFAULT_FRAME_RATE);
        configured = DEFAULT_FRAME_RATE;
    }
    g_frame_rate = (uint32_t)configured;
    g_last_frame_us = 0;
    g_world_clock = WORLD_CLOCK_UNSEEN;
    if (g_frame_rate) {
        video_set_frame_rate(g_frame_rate);
    }
}

/*
 * The time since the last frame is zero for the first, which has none to measure from. In
 * milliseconds it is the difference between two readings in milliseconds, so that what one
 * frame loses to rounding the next one counts.
 *
 * A world clock that is not being quickened is taken to keep the game's own interval, and the
 * tunables are then changed only by the look sensitivity.
 */
static void game_frame(void) {
    uint32_t interval = 0;
    uint32_t frame_us = 0;
    if (g_field_of_view > 0.0f) {
        widen_field_of_view();
    }
    if (g_frame_rate) {
        uint64_t now = monotonic_us();
        uint64_t elapsed_us = g_last_frame_us ? now - g_last_frame_us : 0;
        uint64_t elapsed = g_last_frame_us ? now / 1000 - g_last_frame_us / 1000 : 0;
        if (g_last_frame_us && !elapsed) {
            elapsed = 1;
        }
        g_last_frame_us = now;
        uint32_t frame_ms = elapsed > MAX_FRAME_STEP_MS ? MAX_FRAME_STEP_MS : (uint32_t)elapsed;
        frame_us = elapsed_us > MAX_FRAME_STEP_MS * 1000 ? MAX_FRAME_STEP_MS * 1000
                                                         : (uint32_t)elapsed_us;
        interval = quicken_world_clock(frame_ms);
        follow_frame_time(frame_ms);
    }
    pace_tunables(interval ? interval : WORLD_CHANNEL_INTERVAL_MS, frame_us);
}

static void controller_crouch(void) {
    controller_stance_event(CROUCH_EVENT_OFFSET);
}

static void controller_prone(void) {
    controller_stance_event(PRONE_EVENT_OFFSET);
}

bool codboz_install_hud_handlers(struct s3e_loaded_image *loaded) {
    static struct input_hud_handlers handlers = {
        .reload = hud_reload,
        .change_weapon = hud_swap_weapon,
        .grenade_hold = hud_hold_grenade,
        .grenade_throw = hud_throw_grenade,
        .fire = input_manager_fire,
        .crouch = controller_crouch,
        .prone = controller_prone,
        .action = input_manager_action,
        .sprint_start = input_manager_sprint_start,
        .sprint_stop = input_manager_sprint_stop,
    };
    static const size_t code[][2] = {
        {HUD_HANDLERS_OFFSET, HUD_HANDLERS_SIZE},
        {INPUT_MANAGER_POST_STICK_OFFSET, INPUT_MANAGER_POST_STICK_SIZE},
        {INPUT_MANAGER_SET_FIRE_OFFSET, INPUT_MANAGER_SET_FIRE_SIZE},
        {INPUT_MANAGER_COMMAND_OFFSET, INPUT_MANAGER_COMMAND_SIZE},
        {INPUT_MANAGER_ACTION_SPRINT_OFFSET, INPUT_MANAGER_ACTION_SPRINT_SIZE},
        {CONTROLLER_LOOKUP_OFFSET, CONTROLLER_LOOKUP_SIZE},
        {CONTROLLER_WEAPONS_GETTER_OFFSET, CONTROLLER_WEAPONS_GETTER_SIZE},
        {CONTROLLER_MOVE_STEP_OFFSET, CONTROLLER_MOVE_STEP_SIZE},
        {CONTROLLER_MOVE_DAMPING_OFFSET, CONTROLLER_MOVE_DAMPING_SIZE},
        {CONTROLLER_TURN_OFFSET, CONTROLLER_TURN_SIZE},
        {CONTROLLER_TILT_OFFSET, CONTROLLER_TILT_SIZE},
        {CONTROLLER_WATER_VOICE_OFFSET, CONTROLLER_WATER_VOICE_SIZE},
        {CONTROLLER_EVENT_HANDLER_OFFSET, CONTROLLER_EVENT_HANDLER_SIZE},
        {CONTROLLER_LOOK_EVENT_OFFSET, CONTROLLER_LOOK_EVENT_SIZE},
        {CONTROLLER_STANCE_EVENTS_OFFSET, CONTROLLER_STANCE_EVENTS_SIZE},
        {CONTROLLER_CROUCH_TAIL_OFFSET, CONTROLLER_CROUCH_TAIL_SIZE},
        {WEAPONS_VIEW_EASING_OFFSET, WEAPONS_VIEW_EASING_SIZE},
        {WEAPONS_VIEW_SETUP_OFFSET, WEAPONS_VIEW_SETUP_SIZE},
        {WORLD_CLOCK_CONSTRUCTOR_OFFSET, WORLD_CLOCK_CONSTRUCTOR_SIZE},
        {WORLD_CLOCK_TICK_OFFSET, WORLD_CLOCK_TICK_SIZE},
        {WORLD_CLOCK_INSTANCE_OFFSET, WORLD_CLOCK_INSTANCE_SIZE},
        {WORLD_CLOCK_CHANNELS_OFFSET, WORLD_CLOCK_CHANNELS_SIZE},
        {WORLD_CLOCK_UPDATE_OFFSET, WORLD_CLOCK_UPDATE_SIZE},
        {FRAME_TIMER_STEP_GETTER_OFFSET, FRAME_TIMER_STEP_GETTER_SIZE},
        {FRAME_TIMER_SCALE_OFFSET, FRAME_TIMER_SCALE_SIZE},
        {FRAME_TIMER_UPDATE_OFFSET, FRAME_TIMER_UPDATE_SIZE},
        {FRAME_TIMER_CONSTRUCTOR_OFFSET, FRAME_TIMER_CONSTRUCTOR_SIZE},
        {INPUT_MANAGER_COMMAND_TABLE_OFFSET, INPUT_MANAGER_COMMAND_TABLE_SIZE},
    };
    /*
     * Reached through pointers: the handler, tick and step getter by their objects' vtables,
     * the events by address.
     */
    static const size_t pointers[][2] = {
        {CONTROLLER_EVENT_HANDLER_SLOT_OFFSET, CONTROLLER_EVENT_HANDLER_OFFSET},
        {WORLD_CLOCK_TICK_SLOT_OFFSET, WORLD_CLOCK_TICK_OFFSET},
        {FRAME_TIMER_STEP_GETTER_SLOT_OFFSET, FRAME_TIMER_STEP_GETTER_OFFSET},
        {CROUCH_EVENT_SLOT_OFFSET, CROUCH_EVENT_OFFSET},
        {PRONE_EVENT_SLOT_OFFSET, PRONE_EVENT_OFFSET},
    };

    if (!image_range_valid(loaded, GAME_POINTER_OFFSET, sizeof(void *)) ||
        !image_range_valid(loaded, HUD_POINTER_OFFSET, sizeof(void *)) ||
        !image_range_valid(loaded, INPUT_MANAGER_POINTER_OFFSET, sizeof(void *)) ||
        !image_range_valid(loaded, CROUCH_EVENT_OFFSET, sizeof(uint32_t)) ||
        !image_range_valid(loaded, PRONE_EVENT_OFFSET, sizeof(uint32_t)) ||
        !image_range_valid(loaded, WORLD_CLOCK_OFFSET, WORLD_CLOCK_SIZE) ||
        !image_range_valid(loaded, FRAME_TIMER_POINTER_OFFSET, sizeof(void *))) {
        return false;
    }
    for (size_t i = 0; i < sizeof(g_update_tunables) / sizeof(g_update_tunables[0]); ++i) {
        if (!image_range_valid(loaded, g_update_tunables[i].offset, TUNABLE_PLACE_SIZE)) {
            return false;
        }
    }

    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); ++i) {
        if (!image_range_valid(loaded, code[i][0], code[i][1])) {
            return false;
        }
        hash = image_hash(hash, loaded->base + code[i][0], code[i][1]);
    }
    if (hash != GAME_CODE_HASH) {
        return false;
    }
    /*
     * A command's number is its place in the table, and what is there is how many instructions
     * its code starts after the end of the code that picks one.
     */
    int16_t command;
    memcpy(&command,
           loaded->base + INPUT_MANAGER_COMMAND_TABLE_OFFSET +
               (ACTION_SPRINT_COMMAND - 1) * sizeof(command),
           sizeof(command));
    if (INPUT_MANAGER_COMMAND_OFFSET + INPUT_MANAGER_COMMAND_SIZE + command * 4 !=
        INPUT_MANAGER_ACTION_SPRINT_OFFSET) {
        return false;
    }
    for (size_t i = 0; i < sizeof(pointers) / sizeof(pointers[0]); ++i) {
        if (!image_range_valid(loaded, pointers[i][0], sizeof(void *)) ||
            read_pointer(loaded->base + pointers[i][0]) != loaded->base + pointers[i][1]) {
            return false;
        }
    }

    g_image = loaded->base;
    configure_field_of_view();
    configure_look_sensitivity();
    configure_frame_rate();
    fprintf(stderr, "[game] FieldOfView=%d FrameRate=%u LookSensitivity=%u\n",
            (int)g_field_of_view, (unsigned)g_frame_rate, (unsigned)g_look_sensitivity);
    bool paced = g_frame_rate || g_look_sensitivity != OWN_LOOK_SENSITIVITY;
    handlers.frame = g_field_of_view > 0.0f || paced ? game_frame : NULL;
    input_set_hud_handlers(&handlers);
    return true;
}

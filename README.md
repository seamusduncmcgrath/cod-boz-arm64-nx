# Call of Duty: Black Ops Zombies for Nintendo Switch

An unofficial homebrew port of the mobile game *Call of Duty: Black Ops Zombies* to the Nintendo
Switch.

The port is a loader, not a rewrite. It takes the game's own 64-bit ARM code from the Android
release, runs it natively on the Switch, and supplies everything that code expects from the
phone it was written for: files, graphics, sound, input and timing. On top of that it gives the
game a console-style controller layout, a wider field of view and 60 frames per second.

## What you need

- A Switch that can run homebrew, with the Homebrew Menu started in title takeover (hold **R**
  while launching any game). Make sure not to start from the album homebrew menu or you'll crash atmosphere.
- [This specific APK](https://github.com/12brendon34/COD-BOZ-Partially-Decompiled/releases/tag/1.3.5), it's the ARM64 iPhone .s3e ported to android with converted textures, it's based off version 1.3.1. You can tell it is the right
  one by its `assets` folder, which contains `boz_aarch64.s3e`.
- About 220 MB free on the SD card.

## Installing

1. Open the APK with any archive tool (an APK is a zip file) and extract its `assets` folder.
2. Copy `boz.nro` and the whole `assets` folder into `/switch/boz/` on the SD card:

   ```text
   switch/
   └── boz/
       ├── boz.nro
       └── assets/
           ├── app.icf
           ├── s3e.icf
           ├── blackops_gles1.dz
           ├── boz_aarch64.s3e
           ├── blackops-music/
           └── deadops-music/
   ```

3. Start **Black Ops Zombies** from the Homebrew Menu.

Nothing in `assets` needs unpacking or renaming. Saves, settings and the log are written next to
it, in `/switch/boz/`.

## Controls

The layout follows the console versions of Black Ops, button for button by position.

| Switch             | Action                                         |
| ------------------ | ---------------------------------------------- |
| Left stick         | Move                                           |
| Left stick click   | Sprint                                         |
| Right stick        | Look                                           |
| Right stick click  | Melee                                          |
| ZL                 | Aim                                            |
| ZR                 | Fire                                           |
| L                  | Tactical grenade                               |
| R                  | Frag grenade: hold to cook, release to throw   |
| A                  | Crouch; hold for prone                         |
| B                  | Use whatever the on-screen prompt offers       |
| X                  | Switch weapon                                  |
| Y                  | Reload; hold to use                            |
| D-pad left         | Alternate fire                                 |
| Plus               | Pause                                          |
| Minus              | Switch between game controls and a menu cursor |

**Sprinting.** Click the left stick while pushing it. You keep sprinting without holding the
click until you let the stick go, aim or fire. When the game has tired you out, click again once
you have recovered.

**Menus.** The menus are the phone game's, made for a touchscreen, and the Switch's touchscreen
works on them at any time. When the screen is out of reach, as it is when docked, press **Minus**
for a cursor: the left stick or D-pad moves it and **B** presses. Press **Minus** again to go
back to the game controls.

## Settings

The loader reads three settings of its own from `assets/app.icf`. To change one, add a `[LOADER]`
section at the end of that file:

```ini
[LOADER]
FieldOfView=65
FrameRate=60
LookSensitivity=200
```

| Setting           | Default | Range      | Meaning                                                  |
| ----------------- | ------- | ---------- | -------------------------------------------------------- |
| `FieldOfView`     | 65      | 50 to 150  | How wide the view is, in the game's own unit             |
| `FrameRate`       | 60      | 30 to 60   | How often the game world is updated and drawn            |
| `LookSensitivity` | 200     | 25 to 1000 | Turning speed, as a percentage of the game's own         |

The field of view is not in degrees: 50 shows about 71° across the screen, 65 about 81° and 90
about 90°.

Setting any of the three to `0` leaves that part of the game as it shipped: a field of view of
50, a world updated about 31 times a second, and the original turning speed. A value outside its
range is replaced by the default, and the log says so.

## How it works

The game was built with the Marmalade SDK, which packages a game as an *S3E image*: a block of
machine code with no ties to any operating system. It does not call Android. Everything it wants
from the outside world it asks for through a fixed list of named functions, the S3E API, together
with OpenGL ES and EGL for graphics. This game's image names 395 of them. That makes the code
portable to anything that can run 64-bit ARM code and answer those calls, which a Switch can.

**Loading.** At launch the loader reads `assets/boz_aarch64.s3e`, which is stored
LZMA-compressed, and unpacks it in memory. It copies the code to where it will run, rewrites the
addresses inside it to match (the image carries a table of them), and points each imported
function at the loader's own implementation. The Switch never allows memory to be writable and
executable at once, so the image is assembled in ordinary memory and then remapped as code. The
game's entry point then runs on a thread of its own.

**Standing in for the phone.** The rest of the loader is the implementation of those imports on
top of libnx, SDL2, SDL2_mixer and Mesa:

- *Files.* The game's data lives in `blackops_gles1.dz`, an archive only the game's own code
  can read. The game registers that code with the loader as a file system, and the loader offers
  every file it opens to it first, falling back to the SD card.
- *Graphics.* The game's EGL and OpenGL ES calls go to Mesa through an SDL window.
- *Sound.* Effects and the MP3 music are played through SDL2_mixer.
- *Input.* The game is told it is running on a Sony Xperia Play, a phone with a built-in
  gamepad that it supported, which makes it use its gamepad controls. The sticks arrive as that
  phone's touch pads, the buttons as its keys, and the touchscreen as itself.
- *Settings.* `s3e.icf` and `app.icf` are read the way Marmalade would read them, with a few
  values overridden to switch off the store, the downloader and online accounts.

**Fitting the game to a controller.** A few things cannot be fixed from outside the game, and
for those the loader reaches into it. `src/codboz_hud_handlers.c` first checks a hash of the game
code it depends on, so it only ever acts on the exact build it was written against, and then:

- calls the handlers behind the game's on-screen buttons for reload, weapon switch and grenades,
  which its key bindings do not reach in this build, and sends crouch and prone straight to the
  player;
- raises the fire flag on every frame while the trigger is held, as the touch controls do, so
  automatic weapons fire continuously;
- separates sprinting from using things, which share one key in the game;
- widens the field of view by changing the value the game eases the camera back to;
- raises the frame rate by shortening the interval of the game's world clock and telling its
  frame timer how long each frame really took. The player's movement was written for a fixed
  update rate, so the game's own tunables for it are rescaled every frame to keep walking,
  turning and the time allowed in water as they were.

If the hash does not match, none of this is applied: the game still runs, with its buttons sent
as plain key presses and the three settings above doing nothing.

## Troubleshooting

Each run writes `/switch/boz/boz.log`. It is the first thing to look at, and to attach when
reporting a problem.

- **"Game files not found"** on screen: `assets/boz_aarch64.s3e` is not in `/switch/boz/`. Check
  that the `assets` folder itself was copied, not just its contents.
- **"is not a usable game image"**: the file did not copy completely. Copy it again.
- **A crash, or a failure to load a map**: check that the Homebrew Menu was not started from the
  Album. The log says `running as an applet` when it was.
- **`not the build of the game this port knows`** in the log: the APK is a different version, and
  the controller fixes and settings are off.
- **`[game] assert: error loading extension: s3eSocketOpt`** appears in every log and is
  harmless.

## Status

Playing solo has been tested on a Switch. Local Wi-Fi and online play have not: the networking
code of the loader this port is based on is included, but nothing is known about how it behaves
here.

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64, libnx and CMake,
and these packages:

```sh
pacman -S switch-sdl2 switch-sdl2_mixer switch-mesa switch-libdrm_nouveau
```

On Windows, run `build.bat release` (or `build.bat debug`). Elsewhere, from a shell with
`DEVKITPRO` set:

```sh
cmake --preset release
cmake --build --preset release
```

The result is `build/release/boz.nro`.

### Layout

- `src/s3e_*.c`: the S3E API, one file per area (files, graphics, sound, input, timers, network).
- `src/codboz_hud_handlers.c`: everything specific to this game.
- `src/switch/`: the entry point, executable memory, and a stand-in for `dlopen` over the
  libraries linked into the NRO.
- `include/`: headers.
- `tests/`: host-side unit tests.
- `third_party/lzma/`: the LZMA SDK's decoder, which is in the public domain.

## Credits and licence

This port is built on [cod-boz-port](https://github.com/Producdevity/cod-boz-port) by
Producdevity, a loader for the 32-bit build of the game on Linux handhelds, and is released under
the same MIT licence (see `LICENSE`).

*Call of Duty: Black Ops Zombies* was developed by Ideaworks Game Studio and published by
Activision. This project is not affiliated with or endorsed by either.

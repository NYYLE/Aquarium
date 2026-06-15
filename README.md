# Aquarium watchface

An animated aquarium for Pebble. Fish swim around a tank with sand and plants,
the time sits at the bottom, and you feed the fish by shaking the watch.

## Gameplay
- **Shake** to drop food pellets (rate-limited to once per `FEED_COOLDOWN_S`
  = 10s so you can't spam it). Each fish chases a **different** pellet rather
  than all swarming the same one.
- Food that isn't caught **settles on the sand** and lingers for a few seconds
  (still edible) before **dissolving**.
- Fish occasionally **hide behind the rock** for a while.
- Feed them **every day** and after `BREED_DAYS` (3) consecutive fed days a
  **baby fish** appears. Babies grow up over a few days.
- The tank holds **8 fish max**. When it's full and a baby is born, the
  **oldest fish dies off** to make room. Fish also die of old age.
- Go a **full day without feeding** and they **all die** -> *"Shake to add
  fish!"*. Shaking then adds **2 fish**. This is also the very first state.

## Look
- Frutiger-Aero water: a vertical aqua->deep-blue **gradient**, soft diagonal
  **light rays**, and **rising bubbles** (colour platforms).
- A **wavy sand bed**, plants, and a glossy **rock**.
- **Time on the left, date on the right**, centred in the sand bank.

## Easter egg
At **23:09** a 2-frame **octopus** drifts across the tank, tentacles wiggling.
(Times/frames are set by `OCTO_HOUR` / `OCTO_MIN` / `OCTO_FRAME_DIV`.)

## Supported devices (all 5 SDK platforms)
| Platform | Watch | Sprites |
|----------|-------|---------|
| `aplite`  | original Pebble / Steel | B&W (`~bw`) |
| `diorite` | **Pebble 2 Duo**        | B&W (`~bw`) |
| `basalt`  | Pebble Time / Steel     | colour |
| `chalk`   | Pebble Time Round       | colour |
| `emery`   | **Pebble Time 2**       | colour |

## Project layout
```
package.json            app manifest + resource list (the modern appinfo)
wscript                 build script (waf) - standard, don't edit
src/c/aquarium.c        the watchface
tools/gen_sprites.py    regenerates all sprites with Pillow
resources/images/*.png  fish (2 species x 3 sizes x 2 directions) + plants
```
Each sprite has a colour `name.png` plus a B&W `name~bw.png`; the SDK picks the
right one per platform automatically.

## Regenerate sprites
```
python tools/gen_sprites.py
```

## Build & install
You need the Pebble SDK (the `pebble` CLI is not yet installed on this machine).
The maintained, modern build is from the Pebble revival project:

1. Install the SDK by following https://help.rebble.io / the Pebble dev docs
   (the `pebble-tool` + arm toolchain, or the official Docker image).
2. From this folder:
   ```
   pebble build                 # builds all 5 platforms
   pebble install --emulator emery   # try Pebble Time 2 in the emulator
   pebble install --emulator diorite # try Pebble 2 Duo (B&W)
   pebble install --phone <ip>       # install on a real watch
   ```
The compiled `aquarium.pbw` (in `build/`) is the bundle you side-load.

## Tuning
Timing constants are at the top of `src/c/aquarium.c`:
`GROW_MED_S`, `GROW_BIG_S`, `LIFESPAN_S`, `STARVE_S`, `BREED_DAYS`. They use the
real clock, so the life cycle advances across days even while the watch sleeps.
For quick testing, temporarily shrink them (e.g. seconds/minutes instead of
days).

## Notes / next steps
- The animation runs on an 80 ms timer. Smooth, but constant animation uses
  battery; raising `ANIM_MS` trades smoothness for battery life.
- "Big" fish currently use a single wider sprite. If you want the
  two-tile-bodied big fish, `gen_sprites.py` is where to add a second tile.
- `chalk` is round (180x180); the layout fills it but you may want to inset the
  plants from the curved edges.

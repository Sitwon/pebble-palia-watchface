# pebble-palia-face

A Pebble watchface that shows the in-game time of day for the game
[Palia](https://palia.wiki.gg/wiki/Guide:Time_Passage_in_Palia) on a one-hand
24-hour analog dial.

> **Disclaimer:** This is an unofficial, fan-made watch face. It is not affiliated with, endorsed by, or sponsored by Tamalpa Studios or any of their partners. All game assets and trademarks belong to their respective owners.

## How it works

Palia runs at 24x real speed: one in-game day passes in one real hour, so one
in-game minute passes every 2.5 real seconds. Anchors (minutes past the real
hour): 12:00 AM = :00, 6:00 AM = :15, 12:00 PM = :30, 6:00 PM = :45. The face
computes Palia time entirely on-watch from the local clock — no phone or
internet needed.

The dial shows the Palia day/night cycle with colors matching the in-game
clock as full-screen angular sectors (noon at top, midnight at bottom): Dawn
3 AM–6 AM and Dusk 6 PM–9 PM as slices on the left/right edges, Day 6 AM–6 PM
filling the top half including the top corners, and Night 9 PM–3 AM filling
the bottom middle and bottom corners. Matching the in-game clock, a small
white sun sits at the noon position and a crescent moon with a few stars at
midnight. A single needle points at the current
in-game time, and the real-world time is shown in the center badge, following
the watch's 12/24-hour setting.

Updates use a self-rescheduling 2.5s `app_timer` (one wake per in-game
minute, redrawn only when something changed) instead of per-second ticks, to
keep battery drain low.

## Building & running

```sh
pebble build                          # build the PBW
pebble install --emulator basalt      # install on an emulator
pebble install --emulator chalk       # (also: emery, gabbro)
pebble install --phone <ip>           # install to a paired phone
pebble install --cloudpebble          # deploy via CloudPebble
```

## Target platforms

`targetPlatforms` in `package.json` covers every Pebble with a color screen:
`basalt` (Pebble Time), `chalk` (Pebble Time Round), `emery` (Pebble Time 2)
and `gabbro` (Pebble Round 2). Layout scales off the screen bounds, round
screens get circular dial ticks, and the black-and-white platforms (aplite,
diorite, flint) are not supported — the color-coded day/night sectors would
be unreadable on them.

## Project layout

```
src/c/           C source for the watchface (src/c/main.c)
resources/       Images, fonts, and other bundled resources (currently empty)
package.json     Project metadata (UUID, platforms, resources)
wscript          Build rules — usually no need to edit
```

## Documentation

Full SDK docs, tutorials, and API reference: <https://developer.repebble.com>

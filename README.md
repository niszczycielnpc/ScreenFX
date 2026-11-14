# ScreenFX

Screen effects for any PS5 game, as a plugin for [OnionHEN](https://github.com/aydencharles/onionHEN):
scanlines, vignette, colour tint, black bars and dimming, drawn over the game and
switched on and off with a button combo.

> **Status: 0.30, experimental.** Tested on a single console (PS5 firmware 9.60).
> It loads a small module into the system UI, so treat it as a beta and read
> [Safety](#safety) first.

## Features

- **Scanlines** with adjustable strength and line size
- **Vignette** (darkened corners)
- **Colour tint**: warm, night (blue light cut), green phosphor, amber
- **Black bars**: cinema 21:9 or retro 4:3
- **Dim** for late-night play
- **Presets**: CRT TV, Arcade, Night, Cinema 21:9, Green phosphor, or your own (Custom)
- **In-game menu** on a button combo: pick a preset or switch effects off without leaving the game
- Settings page inside the OnionHEN Toolbox, saved between sessions
- Works in every game: nothing is patched in the game itself

## Requirements

- A jailbroken PS5 running **OnionHEN 0.9.0 or newer** (the version with ELF plugins)
- An FTP client to copy one file to the console

ScreenFX is an OnionHEN plugin. It does **not** work with etaHEN: the two plugin
formats are not compatible.

## Install

1. Download `SCFX00001.elf` from the [Releases](../../releases) page.
2. Upload it over FTP to `/data/OnionHEN/plugins/SCFX00001.installing`
3. Rename it to `/data/OnionHEN/plugins/SCFX00001.elf`
4. OnionHEN picks it up and starts it. Open **Toolbox → Plugins → ScreenFX**.

To update, repeat steps 2 and 3 with the new file.

## Usage

1. Start a game.
2. Open **Toolbox → Plugins → ScreenFX** and switch on
   **Draw on screen (experimental)**. This is off by default.
3. Go back to the game. The effects appear within a few seconds.
4. Pick a preset, or change the individual effects. Changing any effect by hand
   switches the preset to *Custom*.

### In-game menu

Press the button combo in game to open the ScreenFX menu.

| Input | Result |
| --- | --- |
| Tap the combo | Open / close the menu |
| D-pad up / down | Move through *Off* and the presets; the look changes as you move |
| Circle or Cross | Close the menu |
| Hold the combo for one second | Effects on / off, without the menu |

The menu closes by itself after about ten seconds without input. *Custom* shows
up in the list once you have changed an effect by hand on the Toolbox page.

The combo is **L3 + R3** by default. You can change it to *L1 + R1 + Options* or
*L2 + R2 + Touchpad*, or turn it off, on the ScreenFX page.

The game still receives your button presses while the menu is open, so pause the
game first if that matters.

### Presets

| Preset | What it does |
| --- | --- |
| CRT TV | Classic scanlines and a soft vignette |
| Arcade | Stronger, tighter scanlines and a light vignette |
| Night | Blue light cut and a dimmed picture |
| Cinema 21:9 | Black bars top and bottom, vignette |
| Green phosphor | Light scanlines with a green tint |

## Good to know

- These are **overlay** effects: ScreenFX draws on top of the picture. It cannot
  read or change the game image, so there is no blur, sharpening, CRT curvature
  or real rescaling. Black bars cover part of the picture; they do not shrink it.
- Scanlines are drawn as many thin panels (up to 400). If the system UI feels
  slower, choose a larger *Scanline size*.
- Stopping the plugin from the Toolbox clears the screen.

## Safety

ScreenFX loads a drawing module into the PS5 system UI (SceShellUI), the same way
OnionHEN loads its own Toolbox. That is the experimental part.

- The module is only loaded after you switch on *Draw on screen*.
- It is loaded once the OnionHEN Toolbox is ready, and it hooks into OnionHEN's
  existing per-frame hook. If that hook is not found, it installs nothing.
- If the system UI restarts within 90 seconds of loading the module, ScreenFX
  switches *Draw on screen* off by itself and saves that.
- Nothing is written to the system. If something goes wrong, restart the console.
  To remove the plugin, delete `/data/OnionHEN/plugins/SCFX00001.elf`.

You use this at your own risk.

## Troubleshooting

| Problem | Try this |
| --- | --- |
| No ScreenFX page in the Toolbox | Check the file name is exactly `SCFX00001.elf` and that your OnionHEN version supports ELF plugins |
| Page is there, nothing on screen | Make sure *Draw on screen* and *Effects enabled* are both on, then wait a few seconds in game |
| Combo does nothing | Check *Button combo* is not set to *Off* and that the game is in the foreground |
| No menu after updating the plugin | Restart the console: the old drawing module stays loaded until then |
| *Draw on screen* switched itself off | The system UI restarted right after loading; see the logs below and open an issue |

Logs and files on the console:

| Path | What |
| --- | --- |
| `/data/OnionHEN/SCFX00001.log` | Plugin log |
| `/data/OnionHEN/screenfx/renderer.log` | Drawing module log |
| `/data/OnionHEN/plugins/SCFX00001.ini` | Saved settings |

When you report a problem, please attach both logs and say which firmware and
OnionHEN version you use.

## Roadmap

Planned for the next version:

- **Per-game settings**: remember a separate preset and effect setup for each game

## Building from source

The loader and the drawing module use OnionHEN internals, so the build runs
inside an OnionHEN checkout.

```sh
git clone https://github.com/aydencharles/onionHEN
git clone https://github.com/OnionBuddies/onionHEN-plugin-sdk
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
./build.sh onionHEN onionHEN-plugin-sdk     # -> build/bin/SCFX00001.elf
```

You need the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), CMake 3.20+,
Ninja and Python 3. `build.sh` adds one line to the OnionHEN `source/CMakeLists.txt`
and skips OnionHEN's own daemon targets.

### How it works

| Part | Runs in | Job |
| --- | --- | --- |
| `source/` | plugin process | Toolbox page, presets, settings; publishes the active look to `state.bin`; loads the drawing module |
| `renderer/` | SceShellUI | Draws PUI panels on the "Game" scene, reads the pad combo, sends `toggle` / `next_preset` back |

## Credits

- Holendersky - assisting Claude
- Claude (Anthropic) - AI developer
- [OnionHEN](https://github.com/aydencharles/onionHEN) and the
  [OnionHEN Plugin SDK](https://github.com/OnionBuddies/onionHEN-plugin-sdk):
  plugin system, remote ELF loader, and the monitor bar this drawing code follows
- [onionHEN-ntp-plugin](https://github.com/OnionBuddies/onionHEN-ntp-plugin): project layout
- John Törnblom and contributors: [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)
- The etaHEN team, whose work OnionHEN builds on

## License

[GPL-3.0](LICENSE). ScreenFX links OnionHEN libraries, which are GPL-3.0.

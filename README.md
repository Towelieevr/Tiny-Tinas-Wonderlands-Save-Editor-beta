# Tiny Tina's Wonderlands Save Editor (PS4 / GoldHEN) - beta

In-game save editor plugin for Tiny Tina's Wonderlands on jailbroken PS4s with GoldHEN, by Toweliee/CLAUDE AI.

> **Beta:** back up your saves before using it (Apollo Save Tool or the PS4's save backup). Please report problems on the [Issues](https://github.com/Towelieevr/Tiny-Tinas-Wonderlands-Save-Editor-beta/issues) page.

**Game:** Tiny Tina's Wonderlands, **CUSA23766 v1.10** only.

**Tabs:** Character (level, gold, moon orbs, skeleton keys), Item Codes, SDUs, Cosmetics, Style (menu colours, opacity, font and text size).

## Download

Grab the zip from [Releases](https://github.com/Towelieevr/Tiny-Tinas-Wonderlands-Save-Editor-beta/releases).

## Install

1. Copy the `GoldHEN` folder from the zip to `/data/GoldHEN/` on your PS4 over FTP (`ttw_editor.prx` goes in `plugins/`; the icon and `ttw_codes.txt` go in `/data/GoldHEN/`).
2. Add the editor to `/data/GoldHEN/plugins.ini` as shown in the zip's `plugins.ini.txt`. If `XeMo.prx` is listed under `[CUSA23766]`, put the editor line **after** it.
3. Start the game, load into gameplay, and hold **L3 + R3** to open the editor.

## Building

`source/` has the plugin code. `build.py plugin ttw_editor` expects a `tools/` folder next to it with the GoldHEN Plugins SDK and repository and a PS4 toolchain. None of those are included here.


PLEASE NOTE THIS IS STILL A WORK IN PROGRESS ADVICE IS ALWAYS ACCEPTED THANK YOU FOR YOUR PATIENCE.


## License

Free to use, to modify All i ask is you give some credit or dont up to you.

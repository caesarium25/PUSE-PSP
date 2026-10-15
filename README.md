# PUSE-PSP

PUSE-PSP is a native PlayStation Portable port of the save-editing engines from
[PUSE](https://github.com/Zannael/PUSE). It follows the same model as a Nintendo
DS homebrew save editor: launch the application from the PSP menu, select a save
on the Memory Stick, edit it, and write it back with an automatic backup.

## Current features

- Searches TempGBA and common PSP folders for 128 KiB `.sav` and `.srm` files.
- Validates Pokémon Unbound's save container before editing.
- Party editing: nickname, OT, species, level, item, happiness, nature, shiny,
  ability, IVs, EVs, and moves.
- PC boxes 1-24: browse, insert, release, and edit core Pokémon fields.
- Bag pocket detection and item/quantity editing.
- Money and Battle Point editing.
- Pokédex seen/caught flags.
- RTC Time Fixer reset and legacy Quick Fix candidates.
- Game-progress and save-health inspection.
- Automatic `.puse.bak` backup before every write.

## Install

Copy the complete `PUSE-PSP` folder to:

```text
ms0:/PSP/GAME/PUSE-PSP/
```

The `data` folder must remain beside `EBOOT.PBP`. Start **PUSE-PSP** from the PSP
Game menu. Use `X` to select and `O` to go back.

## Safety

Exit Pokémon Unbound and TempGBA before opening the editor. PUSE-PSP creates
`<save>.puse.bak` before overwriting a selected save. Keep an additional manual
backup while this port is experimental.

## Build

The included `build.ps1` expects a PSPDEV toolchain at `../../tools/pspdev`.

```powershell
.\build.ps1
```

## Credits and license

The save engines and catalog data are derived from PUSE by Zannael and its
contributors under the MIT License. See `LICENSE-PUSE`. PUSE-PSP's platform code
is also released under the MIT License.

Pokémon, Pokémon Unbound, and related assets belong to their respective owners.

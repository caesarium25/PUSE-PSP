# PUSE-PSP

PUSE-PSP is a native PlayStation Portable port of the save-editing engines from
[PUSE](https://github.com/Zannael/PUSE). It follows the same model as a Nintendo
DS homebrew save editor: launch the application from the PSP menu, select a save
on the Memory Stick, edit it, and write it back with an automatic backup.

## Current features

- Searches TempGBA and common PSP folders for 128 KiB `.sav` and `.srm` files.
- Validates Pokémon Unbound's save container before editing.
- Party and PC editing with named, sortable species, item, nature, ability, and
  move pickers. Regional and alternate forms show their form labels.
- Pokémon Unbound-specific abilities are limited to the selected species' first,
  second (when present), and hidden ability.
- Move selection supports **Compatible** and **All Moves** modes. Compatible
  moves and displayed type/category/power/accuracy/PP come from the
  [Pokémon Unbound Wiki](https://unboundwiki.com/), with Unbound 2.1.1.1's own
  ROM-derived tables used to verify IDs and fill forms the wiki does not split.
- Current PP, PP Ups, PP Max, move removal, and automatic move-slot compaction.
- Party and PC creation with Natural, Competitive, and Custom presets.
- Release from Party or PC, plus Party/PC deposit, withdrawal, and swapping.
- Species-change normalization and Legality Safeguards for EV totals, ability
  slots, and documented Unbound move compatibility. This is not an event or
  trade-legality certificate.
- Bag pocket detection, ID/alphabetical display sorting, item creation from the
  correct pocket catalog, quantity editing, and item removal.
- Money and Battle Point editing.
- Pokédex seen/caught flags.
- RTC Time Fixer reset and legacy Quick Fix candidates.
- Game-progress and save-health inspection.
- Validated temporary writes and an automatic `.puse.bak` backup before every
  original-save replacement.

## Install

Copy the complete `PUSE-PSP` folder to:

```text
ms0:/PSP/GAME/PUSE-PSP/
```

The `data` folder must remain beside `EBOOT.PBP`. Start **PUSE-PSP** from the PSP
Game menu. Use `O` to select/confirm and `X` to cancel/go back.

Catalog controls:

- `Triangle`: switch between numeric and alphabetical sorting.
- `Square`: switch between Compatible and All Moves where available.
- `L` / `R`: move by a page.
- `Select`: show full move details.

## Safety

Exit Pokémon Unbound and TempGBA before opening the editor. PUSE-PSP creates
`<save>.puse.bak` before overwriting a selected save. Keep an additional manual
backup while this port is experimental.

Created Pokémon use the current trainer's OT name and trainer ID, calculated
experience, valid species abilities, and selected Unbound-compatible moves.
Unbound's compact PC format does not preserve a conventional met date/location,
so PUSE-PSP does not fabricate one. Created Pokémon may therefore be structurally
valid for Unbound without passing external event or trade-legality checks.

## Build

The included `build.ps1` expects a PSPDEV toolchain at `../../tools/pspdev`.

```powershell
.\build.ps1
```

`tools/build_wiki_move_details.py` rebuilds the bundled offline move and
compatibility tables exclusively from `unboundwiki.com`. The PSP does not need
network access.

## Credits and license

The save engines and catalog data are derived from PUSE by Zannael and its
contributors under the MIT License. See `LICENSE-PUSE`. PUSE-PSP's platform code
is also released under the MIT License.

Move details and compatibility labels are compiled from the Pokémon Unbound Wiki;
no Wikipedia or general/mainline Pokémon wiki supplies move or ability data.

Pokémon, Pokémon Unbound, and related assets belong to their respective owners.

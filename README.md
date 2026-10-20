# PUSE-PSP

**PUSE-PSP** is an experimental, completely offline Pokemon Unbound save editor
that runs as native PlayStation Portable homebrew. It ports the save-editing
engines and data catalogs from [Zannael's PUSE](https://github.com/Zannael/PUSE)
to the PSP, making it possible to edit a save directly from the Memory Stick
without moving it to a computer or uploading it anywhere.

It follows the same idea as save editors packaged as Nintendo DS homebrew:
launch PUSE-PSP from the PSP Game menu, choose a `.sav` or `.srm` file, make the
edits, and write the save back. The editor validates the result and creates a
backup before replacing the original save.

> [!WARNING]
> This port is experimental. Keep a separate copy of your save. Exit Pokemon
> Unbound and your GBA emulator completely before opening PUSE-PSP.

## Download and install

**[Download PUSE-PSP v0.5.0](https://github.com/caesarium25/PUSE-PSP/releases/download/v0.5.0/PUSE-PSP-v0.5.0.zip)**

1. Download `PUSE-PSP-v0.5.0.zip` using the link above. Older and future builds
   are available on the repository's [Releases](../../releases) page.
2. Extract the complete `PUSE-PSP` folder to `ms0:/PSP/GAME/`.
3. Confirm that `EBOOT.PBP` and the `data` folder are both present inside
   `ms0:/PSP/GAME/PUSE-PSP/`.
4. Start **PUSE-PSP** from the PSP Game menu.
5. Select a supported Pokemon Unbound save from the file browser.

The application scans TempGBA and common PSP folders for 128 KiB `.sav` and
`.srm` files. It does not require Wi-Fi or an internet connection.

## Controls

| Button | Action |
| --- | --- |
| `D-pad` | Navigate and change values |
| `O` | Select or confirm |
| `X` | Cancel or go back |
| `Triangle` | Switch numeric/ID and alphabetical sorting in catalogs |
| `Square` | Switch Compatible and All Moves modes in the move catalog |
| `L` / `R` | Move one page in long catalogs |
| `Select` | Show full details for the highlighted move |

In the IV and EV editors, `Triangle` sets the selected value to its legal
maximum, `Square` clears it, and `Select` toggles changes from the original
calculated stats. `L` and `R` change the selected value by 10.

## Features

- Original `ICON0.PNG` and `PIC1.PNG` artwork for the PSP Game menu, built
  directly into the EBOOT. The background credits Zannael and caesarium25.
- A native SDL2 graphical renderer with Source Code Pro fonts, hardware-drawn
  surfaces, selection panels, controller badges, stat panels, and consistent
  status colors. The app no longer uses the PSP debug-text display backend.

### Party and PC

- Edit species, level, nature, ability, held item, moves, IVs, EVs, nickname,
  friendship, gender, shiny state, Pokerus, and related stored data.
- Browse named and sortable species, item, nature, ability, and move catalogs.
  Regional and alternate forms include their form labels.
- Show only the selected species' first ability, second ability when present,
  and hidden ability.
- Select moves from **Compatible** or **All Moves** mode and view their Unbound
  type, category, power, accuracy, and PP.
- See live calculated HP, Attack, Defense, Special Attack, Special Defense, and
  Speed while viewing a Pokemon or editing its species, level, nature, IVs, or
  EVs. The IV editor also shows Hidden Power type.
- See total EVs and remaining points out of 510. EV Max uses only the legal
  points remaining; existing over-limit saves may be reduced but not increased.
- Edit current PP and PP Ups, apply PP Max, remove moves, and compact empty move
  slots automatically. Choosing a move gives it PP Max by default and refills
  current PP for Party Pokemon.
- Create Pokemon in the Party or PC with Natural, Competitive, or Custom presets.
- Release Pokemon from either location. The editor protects the last remaining
  Party Pokemon.
- Deposit, withdraw, and swap Pokemon between the Party and PC.

Created Pokemon use the current trainer's OT name and trainer ID, calculated
experience, a valid species ability, and the selected Unbound-compatible moves.
Pokemon Unbound's compact PC record does not retain a conventional met date or
location, so PUSE-PSP does not invent those fields. A created Pokemon can be
structurally valid for Unbound without passing external event or trade-legality
checks.

### Other save data

- Create, edit, sort, and remove items in the correct Bag pocket.
- Edit money and Battle Points.
- Edit Pokedex Seen and Caught flags.
- Reset the one-time RTC Time Fixer NPC and inspect legacy Quick Fix candidates.
- Inspect game progress and save health.

### Editing safeguards

IV limits (0-31), per-stat EV limits (0-252), and the 510 total EV limit are
always enforced. There is no raw editing mode. Changes shown in the live stat
panel are previews until `O` is pressed; `X` discards the draft.

The separate Legality Review can inspect the total EV count, selected species'
ability slot, and move compatibility documented for Pokemon Unbound. It can
repair an invalid ability selection and remove incompatible moves.

These safeguards are structural guardrails, not a full legality checker. They do
not certify encounter location, event distribution, trade history, or
competitive legality.

Calculated Stats follow the Pokemon's saved species, level, nature, IVs, and
EVs. They do not include held-item, ability, weather, status, or battle-stage
modifiers.

## Save safety

Before replacing a save, PUSE-PSP:

1. Writes the edited data to a temporary file.
2. Reloads and validates that temporary file.
3. Creates `<save name>.puse.bak` beside the original save.
4. Replaces the original and verifies the bytes written.
5. Restores the backup if replacement or verification fails.

The backup is protection against a failed write, not a substitute for keeping a
separate manual backup before using an experimental editor.

## Compatibility and data sources

The current PSP interface supports **Pokemon Unbound 2.1.1.1** saves. PUSE's
vendored engine also contains Radical Red profile data, but that profile remains
disabled until it receives device-level round-trip testing on PSP.

Move details, compatibility lists, and ability references are compiled for
offline use from the [Pokemon Unbound Wiki](https://unboundwiki.com/). Exact
species/form slots and IDs are checked against Pokemon Unbound 2.1.1.1
ROM-derived tables. PUSE-PSP does not use Wikipedia or a general/mainline
Pokemon wiki for its move or ability behavior.

Forms that share a National Pokedex number receive the documented move union
when the Unbound Wiki does not publish separate learnsets for those forms.

## Build and test

The included `build.ps1` expects a PSPDEV toolchain at `../../tools/pspdev`.

```powershell
.\build.ps1 -Clean
```

To run the host-side save round-trip tests against a disposable Unbound save:

```powershell
.\test-core.ps1 -SavePath "C:\path\to\backup.sav"
```

To smoke-test the resulting EBOOT in PPSSPP:

```powershell
.\test-ppsspp.ps1
```

`tools/build_wiki_move_details.py` rebuilds the bundled offline move,
compatibility, and ability reference tables exclusively from `unboundwiki.com`.
The PSP itself never connects to the wiki.

## Credits

**PUSE-PSP exists because of [Zannael's PUSE](https://github.com/Zannael/PUSE).**
Zannael designed and implemented the upstream save editor, reverse-engineered
the supported save structures, and published the portable save engines and data
catalogs this PSP port builds upon. PUSE contributors are credited with the
upstream project.

The vendored PUSE code is based on revision
[`eab18a7`](https://github.com/Zannael/PUSE/commit/eab18a7baacec0db27ea19422d0e7e5d7ba44fab)
from October 15, 2026. Detailed provenance is recorded in [UPSTREAM.md](UPSTREAM.md),
and the upstream MIT license is included as [LICENSE-PUSE](LICENSE-PUSE).

Move details and compatibility labels are compiled from the
[Pokemon Unbound Wiki](https://unboundwiki.com/).

The in-app typeface is
[Source Code Pro](https://github.com/adobe-fonts/source-code-pro), distributed
under the SIL Open Font License included in `data/ui/FONT-LICENSE.md`.

Pokemon, Pokemon Unbound, and related names and assets belong to their respective
owners. PUSE-PSP is an unofficial community project and is not affiliated with
Nintendo, The Pokemon Company, Game Freak, or the Pokemon Unbound team.

## License

PUSE-PSP's PSP-specific platform code is released under the [MIT License](LICENSE).
Vendored PUSE components remain covered by the included upstream
[MIT License](LICENSE-PUSE).

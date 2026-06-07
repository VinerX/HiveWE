# HiveWE — VinerX Edition

> **A Warcraft III World Editor fork designed for agent-driven development.**
> Built on [stijnherfst/HiveWE](https://github.com/stijnherfst/HiveWE) —
> all credit for the base editor goes to **eejin (stijnherfst)**.

---

## The Agent-Driven Triad

This fork is built around one idea: **an AI agent should see everything — both the map's static
data AND what actually happens at runtime.** Closing that loop turns hours of guesswork into
seconds of automated iteration.

The system is a three-part loop:

```
┌─────────────┐          ┌──────────────────┐          ┌─────────────────┐
│   HiveWE    │  read/write │    AI Agent       │  observe/control │   23-Race-Legion  │
│  (Editor)   │◄──────────►│  (Python + CLI)   │◄───────────────►│   (the Map)       │
│             │  map data   │                  │  probe/eval/hotpatch │                 │
└─────────────┘          └──────────────────┘          └─────────────────┘
     static                     the brain                     runtime
```

1. **Editor (HiveWE_cli)** — headless CLI reads and writes the `.w3x` map file directly.
   Query any object field, batch-edit hundreds of units, copy base objects, resolve TRIGSTR
   strings from localised maps — all from the command line, all scriptable.

2. **Map (23-Race-Legion)** — the companion Lua runtime inside the game. A Preloader-based
   **live eval channel** lets the agent execute arbitrary Lua in a running map and receive
   results back — without restarting. Combined with `probe-map`, the agent gets full runtime
   telemetry: logs, unit states, AI decisions, errors with line numbers.

3. **Agent** — Python glue scripts (`agent_bridge.py`, `build_map_lua.py`) + the CLI form
   the agent interface. The AI reads data, runs the map, observes runtime behaviour, edits
   data, and loops. No more «edit → rebuild → wait 60s for load → squint at logs».

**Why this matters.** In traditional WC3 modding, debugging is: make a change, recompile,
wait a minute for the map to load, manually trigger the scenario, read text logs. With the
triad, the agent hot-patches Lua in a running game (`agent_bridge.py exec`), instantly sees
the effect, and commits the fix to source only after verifying it works. Data changes
(balancing units, adding races) are done in bulk via CLI and tested immediately.

---

## What This Fork Adds

Everything is backward-compatible — maps edited here open in vanilla HiveWE and World Editor.

### CLI Agent Tools (`HiveWE_cli.exe`)

```bash
# Query object data
HiveWE_cli search-objects --map "23races.w3x" --type units --field-filter race:Naga
HiveWE_cli get-objects-bulk --map map.w3x --type units --ids 1,2,3
HiveWE_cli dump-objects --map map.w3x --type abilities --output units.jsonl

# Modify in bulk
HiveWE_cli batch-edit --map map.w3x --type units --ids 1,2,3 --field uhpm:500
HiveWE_cli set-field --map map.w3x --type units --id 42 --field unam:"Archer"
HiveWE_cli copy-object --map map.w3x --type units --source-id 15

# Inspect structure
HiveWE_cli list-fields --map map.w3x --type units
HiveWE_cli list-race-objects --map map.w3x --race Human
HiveWE_cli list-all-races --map map.w3x

# Search by editor metadata (TRIGSTR, suffix, comments)
HiveWE_cli search-objects --map map.w3x --type units --field-filter editorsuffix:elite
HiveWE_cli search-objects --map map.w3x --type abilities --field-filter comment:teleport

# Runtime probing and live eval
HiveWE_cli run-map --map map.w3x --warcraft "F:/Games/Warcraft III"
HiveWE_cli probe-map --map map.w3x --warcraft "F:/..." --click-after 60 --wait 220
HiveWE_cli validate-script --map map.w3x
```

### Spreadsheet Editor
- All objects as rows, configurable fields as editable columns
- **Column customization**: freeze, reorder, add editor-only text/number columns (stored per-map, never shipped to game)
- **Excel-like zoom** (Shift+wheel), icon scaling, header wrapping
- **Advanced filters**: text, field value, race, editor suffix, TRIGSTR
- **Batch edit** — modify any field across multiple selected objects at once
- **Race column** — reads `urac`/SLK race field, sortable and filterable

### Asset Manager
- Bulk-file drag-drop with Explorer-style cursor
- Dependencies toggle, expand/collapse all
- Safe-move operation for reliable asset relocation

### Engine-level fixes
- WC3 2.0+ locale auto-detection via CASC locale flags
- Object Editor crash fix, destructible loading fix
- War3map.w3i truncated table handling
- Hardened map loading with diagnostics

### Architecture
- `HiveWE_core` — Qt-free static library for headless object-data operations
- Map loading decoupled from GUI, reusable in CLI context
- TRIGSTR resolution: CP1251→UTF-8 for Russian/localised maps

---

## The Companion Map: 23-Race-Legion

The runtime half of the triad lives at **[VinerX-Games/23-Race-Legion](https://github.com/VinerX-Games/23-Race-Legion/tree/lua-rewrite)**
(`lua-rewrite` branch).

Key pieces:
- **Preloader eval channel** — execute arbitrary Lua in a running game and get results back. Hex-encoded payloads over `.pld` files. No map restart needed.
- **Probe log system** — universal logging (`[TAG] message`) to both `War3Log.txt` and `.pld` file, with runtime tag filtering (`LogEnable`, `LogDisable`).
- **`agent_bridge.py`** — Python helper for `reset`/`exec`/`exec --file` against the live eval channel.
- **`build_map_lua.py`** — assembles split Lua source sections into `war3map.lua`.
- **Debug methodology** — trace-logs, `probe-map` automation, error localisation by line number.

Typical debug loop with the triad:
```bash
# 1. Agent reads data
HiveWE_cli dump-objects --map map.w3x --type units > current_state.jsonl

# 2. Run and probe
HiveWE_cli probe-map --map map.w3x --warcraft "F:/..." --bridge-script "140:create_ai:2"

# 3. Read runtime logs
notepad "%USERPROFILE%\Documents\Warcraft III\CustomMapData\23Race_probe_log.pld"

# 4. Hot-patch in running game
python agent_bridge.py exec "return AiRace[15]"

# 5. Fix data via CLI
HiveWE_cli batch-edit --map map.w3x --type units --ids 42 --field uhpm:600

# Repeat until done.
```

---

## Download

Pre-built binaries (including `HiveWE_cli.exe`): [Releases page](https://github.com/VinerX/HiveWE/releases)

---

## Upstream HiveWE

HiveWE is a Warcraft III World Editor by **stijnherfst (eejin)** that focuses on speed and ease
of use. It improves massively on the vanilla WE, especially for large maps.

[Thread on the Hiveworkshop](https://www.hiveworkshop.com/threads/introducing-hivewe.303183/)

Some of the benefits over the vanilla WE:
- Way faster loading times (32s → 4s on a sample map)
- Renders your whole map at 120 fps
- Modern UI/UX
- Edit the pathing map directly
- Edit global tile pathing
- Import heightmaps
- Improved editing palettes (water height, 1000+ brushes, doodad variation)

![HiveWE Screenshot](/Screenshots/HiveWE.png)
![Advanced Object Editor](/Screenshots/ObjectEditor.png)
![Edit the Pathing Map](/Screenshots/PathingEditing.png)
![Edit global tile pathing](/Screenshots/GlobalPathingEditing.png)

## Build Instructions

0. Requires Visual Studio 17.14 or higher (C++20 modules)
1. Clone this fork:
   ```
   git clone https://github.com/VinerX/HiveWE.git
   ```
2. Clone [vcpkg](https://github.com/microsoft/vcpkg) somewhere central (e.g. `C:\vcpkg`)
   ```
   git clone https://github.com/Microsoft/vcpkg.git
   ```
3. Run `vcpkg/bootstrap-vcpkg.bat`
4. Add environment variable `VCPKG_ROOT` pointing to your vcpkg directory
5. Build — open the HiveWE folder in **Visual Studio as Administrator**
   (required for symlink creation). Or use CMake directly:
   ```
   cmake --preset Release
   cmake --build --preset Release
   ctest --preset Release
   ```

## License

**AGPL-3.0** — same as upstream. All changes are open-source.

## Credits

- **stijnherfst (eejin)** — original HiveWE editor. [GitHub](https://github.com/stijnherfst/HiveWE) |
  [Hiveworkshop](https://www.hiveworkshop.com/threads/introducing-hivewe.303183/) | Discord: eejin
- **VinerX** — fork author: spreadsheet editor, CLI agent tools, Preloader bridge, runtime eval channel

## Other Community Tools

- Trigger editing: [WC3 Typescript](https://cipherxof.github.io/w3ts/)
- Model editing: [3DS Max Plugin](https://github.com/TaylorMouse/warcraft_III_reforged_tools)
  or [Retera Model Studio](https://github.com/Retera/ReterasModelStudio)

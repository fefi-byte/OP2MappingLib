# Using and working with op2maplib

A practical guide: how to get the library into your map tool, how its pieces
fit together, and how to change the library itself. The C API reference is
`include/op2maplib/op2maplib.h`; the profile format is in
[profile-format.md](profile-format.md).

op2maplib carries a lot of fefi-byte's work from
[op2-codegen-editor](https://github.com/fefi-byte/op2-codegen-editor): the
blending approach, the cliff tool, the learned tile model and the whole map
generator. See the README's Credits.

**Contents**

- [Part 1 – Using the library](#part-1--using-the-library)
  - [Getting it](#getting-it)
  - [Core ideas](#core-ideas)
  - [Painting terrain](#painting-terrain)
  - [Drawing cliffs](#drawing-cliffs)
  - [Finding enclosed areas](#finding-enclosed-areas)
  - [Generating a map](#generating-a-map)
  - [Errors, threads and lifetime](#errors-threads-and-lifetime)
- [Part 2 – Working on the library](#part-2--working-on-the-library)

---

## Part 1 – Using the library

### Getting it

You need two files at run time: the native library (`op2maplib.dll`,
`libop2maplib.so` or `libop2maplib.dylib`) and the profile `profiles/op2.json`.

**Release (recommended for shipping).** Each
[release](https://github.com/OutpostUniverse/OP2MappingLib/releases) has a zip
per architecture (`windows-x64`, `windows-x86`) with `op2maplib.dll`,
`op2.json`, `LICENSE`, `THIRD-PARTY-NOTICES.md` and `SHA256SUMS.txt`. The DLLs
are built by GitHub Actions from the tagged source. Check them before you ship
them:

```
sha256sum -c SHA256SUMS.txt
gh attestation verify op2maplib.dll --repo OutpostUniverse/OP2MappingLib
```

The attestation proves the DLL came from this repository's workflow at that
tag.

**From source.** CMake 3.16+ and a C++17 compiler (MSVC, GCC or Clang):

```
cmake -S . -B build -A x64                 # Windows; -A Win32 for 32-bit, no -A elsewhere
cmake --build build --config Release
build/bin/Release/op2maplib_tests profiles/op2.json
```

Options: `-DOP2MAPLIB_BUILD_SHARED=OFF` for a static library (define
`OP2MAPLIB_STATIC` in your code then; CMake does it for you when you link the
target), `-DOP2MAPLIB_BUILD_TESTS=OFF` to skip the tests. On Windows the DLL
links the C runtime statically, so it needs no VC++ redistributable.

**As a git submodule.** op2-codegen-editor does this: the submodule provides
the sources, the Python binding and the profile; its release workflow builds
the DLL from those sources with CMake.

**Bindings.**

| Language | Where | Finds the native library |
|---|---|---|
| C / C++ | `include/op2maplib/op2maplib.h` | you link it |
| Python | `bindings/python/op2maplib` (ctypes, no build step) | `OP2MAPLIB_PATH`, next to the package, or the repo's `build/` |
| .NET (C#, VB.NET) | `bindings/dotnet/Op2MapLib.Net.csproj` (.NET Framework 4.8) | `x86\` or `x64\op2maplib.dll` next to the assembly, chosen by process bitness |

The .NET project copies `x86\op2maplib.dll`, `x64\op2maplib.dll` and
`op2maplib\op2.json` into every referencing project's output;
`BlendProfile.LoadDefault()` loads that profile.

### Core ideas

**The library never reads or writes `.map` files.** You hand it numbers and it
hands numbers back; your editor owns the map, its undo system and its file
format.

- **Profile.** JSON describing a tile set: its grounds (mud, rock, sand, ...),
  which tiles fill each ground, which tiles blend two grounds, the cliff tiles
  and the generator's stamps. `profiles/op2.json` covers the standard Outpost 2
  tilesets `well0000`–`well0012`. Load it once and share it.
- **Mapping table and mapping index.** An Outpost 2 map has a table of
  (tileset name, image) entries; each cell stores an index into it. You pass
  your map's table to the library and all tiles go in and out as indices into
  *your* table. The profile also carries the table all campaign maps share
  (`standard mapping`, 2017 entries) — use it for new maps.
- **Cells.** Row-major arrays, index `y * width + x`, `(0, 0)` top left.
- **Ground.** An index into the profile's grounds; `find_ground("san")` gives
  it. A ground with 0 fill tiles (snow, lava in `op2.json`) cannot be painted.
- **Fixed tiles.** Tiles the profile doesn't list (cliffs, walls, tubes,
  volcanoes, ...) are never touched by blending unless you paint over them.
- **Changes.** Painting, cliffs and clean-up return a list of
  `(x, y, old_mapping, new_mapping, cell_type)`. Apply them to your map as one
  undo step. `cell_type` is the Outpost 2 cell type the tile should get, or -1
  for "keep the current one".
- **Seeds.** Every random choice takes a seed. Same inputs + same seed = same
  tiles on every platform and in every binding; CI checks this with golden
  hashes.

### Painting terrain

The blend map is the library's copy of one map. Paint grounds onto cells, then
resolve; the library picks fill tiles inside areas and the game's transition
tiles along borders.

C:

```c
char err[256];
om_profile* profile = om_profile_load(json, json_len, err, sizeof err);
om_blend_map* map = om_blend_create(profile, width, height,
                                    mappings, mapping_count,   /* your tile mapping table */
                                    cells, err, sizeof err);   /* width*height mapping indices */

int32_t xy[] = {10, 10, 11, 10, 12, 10};                       /* (x, y) pairs */
om_blend_paint(map, om_profile_find_ground(profile, "san"), xy, 3);
int n = om_blend_resolve(map, NULL, seed);                     /* NULL: what was painted since last time */
om_change* changes = malloc(n * sizeof *changes);
om_blend_get_changes(map, changes, n);                         /* apply to your map as one undo step */
```

Python:

```python
from op2maplib import Profile, BlendMap

profile = Profile.from_file("profiles/op2.json")
bm = BlendMap(profile, w, h, mappings, cells)      # mappings: [(tileset, image), ...]
bm.paint(profile.find_ground("san"), [(10, 10), (11, 10), (12, 10)])
for c in bm.resolve(seed=1234):
    my_map.set_tile(c.x, c.y, c.new_mapping, c.cell_type)
```

.NET:

```csharp
using (var profile = BlendProfile.LoadDefault())
using (var map = new BlendMap(profile, w, h, mappings, cells))
{
    map.Paint(profile.FindGround("san"), strokeCells);
    foreach (BlendChange c in map.Resolve(1234))
        myMap.SetTile(c.X, c.Y, c.NewMapping, c.CellType);
}
```

Keeping the copy in step:

- **Paint during a drag, resolve on release** (or after each stroke segment):
  resolving is fast (milliseconds), but one resolve per stroke gives one undo
  step.
- **Tell the library about every tile you change yourself** — manual tile
  placement, undo, redo, pasting: `om_blend_set_tile` / `set_tile` /
  `SetTile`. Otherwise its copy and your map drift apart.
  Alternatively create a fresh blend map before each operation from your
  current map (op2-codegen-editor does this; creating one costs a few
  milliseconds even at 512×256).
- **Resolve a region** with `resolve(seed, region=(x, y, w, h))` to re-pick a
  rectangle on purpose; **resolve all** (`om_blend_resolve_all`) is a "clean up
  transitions" pass over the whole map. Tiles that already fit are kept, so
  neither reshuffles the map.

### Drawing cliffs

A cliff is drawn along a polyline of cell coordinates with a high side and a
wall height:

```c
const int32_t pts[] = {20, 6, 20, 20, 36, 20};
int n = om_cliff_draw(map, pts, 3, OM_CLIFF_HIGH_LEFT, 3.0, seed);
om_blend_get_changes(map, changes, n);      /* cliff tiles with their cell types */
```

- **High side**: `LEFT`/`RIGHT` of the drawing direction, or `NORTH`/`SOUTH`
  fixed to the map. Where the high side is north you get a wall; where it is
  south, a thin "north cliff" edge — that is how Outpost 2's cliffs look.
- **Wall height** 3 to 6 tiles.
- **Interactive drawing**: `om_cliff_snap_point` (`snap_cliff_point`,
  `BlendProfile.SnapCliffPoint`) moves the mouse point onto an allowed slope.
  Repeat the first point at the end to close a loop.
- **Families**: the cliff tiles come from the ground at the first point; grounds
  without cliff tiles of their own use mud cliffs (`cliff_ground` tells you
  which).
- **Quality**: `last_cliff_stage` is 0..7 (0 = only tile pairs seen in the
  original maps), -1 for the greedy fallback.
- Cliff cells become fixed, so painting next to a cliff never replaces it.
  Painting *over* it does.
- **Ramps and wall ends**: painting ground across a wall leaves it cut off,
  with rock pointing into the open. After applying the stroke, pass the cells
  that were cliff and are ground now to `om_cliff_close_cut`
  (`close_cliff_cut`, `BlendMap.CloseCliffCut`): the cliff cells within two
  cells of the cut are solved again so both new ends finish cleanly (a ramp),
  the rest of the wall stays as it is.

```c
/* removed[y * width + x] = 1 where the stroke turned a cliff cell into ground */
int n = om_cliff_close_cut(map, removed, seed);
om_blend_get_changes(map, changes, n);      /* re-solved cliff tiles, then blended ground */
```

### Finding enclosed areas

Regions need only the cell types (0–31), no profile:

```python
from op2maplib import Regions
regions = Regions.find(w, h, cell_types)          # Outpost 2 rules
print(len(regions), "regions;", regions.enclosed_size(), "cells unreachable")
for x, y in regions.enclosed_cells():
    ...
```

Outpost 2's rules are built in: fast/medium/slow passable, bulldozed, rubble
and tubes count as passable; 8-way connected; the bottom row is skipped; the
left and right edges join only on 512×256 world maps. Region 0 is the largest
(the main play area); everything else is enclosed.

### Generating a map

The generator builds a whole map from a seed. It is fefi-byte's generator from
op2-codegen-editor, ported to C++ (2.3 s → about 20 ms for a 128×128 map
without cliffs).

Python:

```python
from op2maplib import MapGenerator, FeatureKind, Symmetry

gen = MapGenerator("islands", width=128, height=128, seed=1234, spawns=2, cliffs=0.2)
gen.add_feature(FeatureKind.MOUNTAIN, where="c", size=0.6)
gen.add_feature(FeatureKind.VALLEY, where="w", to="o", lava=True)
result = gen.generate(profile)

result.tiles        # width*height mapping indices into profile.standard_mapping()
result.cell_types   # width*height cell types
result.spawns, result.volcanoes, result.lava_zones, result.stats
```

.NET:

```csharp
using (var gen = new MapGenSettings("islands"))
{
    gen.Set("width", 128); gen.Set("height", 128); gen.Set("spawns", 2);
    gen.Seed = 1234;
    gen.AddFeature(MapFeature.Mountain, "c", "", 0.6, false);
    GeneratedMap map = gen.Generate(profile);
}
```

C: `om_mapgen_settings_create`, `om_mapgen_set`, `om_mapgen_set_seed`,
`om_mapgen_add_feature`, `om_mapgen_generate`, then the result accessors
(`om_mapgen_get_tiles`, `om_mapgen_get_cell_types`, `om_mapgen_get_points`,
`om_mapgen_lava_zone_cells`, `om_mapgen_stat`, ...; see the header) and
`om_mapgen_result_free`.

**Templates** set the terrain mix, roughness, landscape mode and volcanoes:

| Template | Character |
|---|---|
| `plains` | soft areas, mostly mud and rock |
| `canyon` | rugged, mostly rock |
| `desert` | mostly sand |
| `islands` | rugged mix of mud, rock and sand |
| `ice` | rock and mud (snow cannot be placed with this profile, so it becomes rock) |
| `newterra` | landscape mode: a highland massif, mesas, dry riverbeds, one volcano |

**Settings** (`set(key, value)`): `width`, `height` (16–1024; Outpost 2 wants
powers of two), `roughness` (0–1), `symmetry` (none, mirror X, mirror Y,
rot180, quad), `spawns`, `spawn_clearance`, `cliffs` (plateau share), `ramps`,
`volcanoes`, `lava_zone_tiles`, `smooth`, `vegetation_spacing`, `min_biome`,
`details` (scatter, 0.5 ≈ the original maps), `landscape`, `rivers`, plus the
seed. `set_mix({"mud": 0.5, "roc": 0.3, "san": 0.2})` replaces the mix.

**Features** are placed before the random layout: valley, mountain, plateau,
clearing (kept free of cliffs and scatter), choke (a barrier with one gap).
Positions are sectors of a 3×3 grid — `nw n no / w c o / sw s so` (German
compass, `o` = east); leave them empty to let the seed decide.

**Mountains** are settings too: `mountains` (share of the map in mountain
chains, 0 = none), `mountain_density` (how tight the terraces stand, 1 = as
tight as the walls allow), `gorges` and `valleys` (cuts through the
mountains: a gorge is narrow and passable but no building ground, a valley is
building ground), `plateaus` (one-terrace areas with two ramps) and
`mountain_spread` (more, more varied mountains). They are planned on a world 32
cells larger than the map, so walls run off the edge; `world_cutout` does the
same for classic cliffs. The result's statistics say how it went:
`mountains_leaky`, `plateaus_unreachable`, and the share of building ground
(a free, reachable 7×7 square) on the map and per zone: `build`,
`build_mountain`, `build_plateau`, `build_valley`, `build_gorge`.

```python
m = MapGenerator("plains", width=128, height=128, seed=7, mountains=0.14, plateaus=2, spawns=2).generate(profile)
m.stats["mountains_leaky"], m.stats["build_mountain"]   # 0, about 0.01
```

**The learned generator** makes maps in the style of the original maps from
two small neural networks (`profiles/op2-net.bin`). It takes the same settings
object (template `learned` for suggestions) and a loaded model:

```python
model = LearnedModel.from_file("profiles/op2-net.bin")
gen = MapGenerator("learned", width=128, height=128, seed=7, spawns=2)
preview = gen.generate_learned(profile, model, LearnedQuality.SKETCH)   # about a second, sketch only
result = gen.generate_learned(profile, model, LearnedQuality.FAST, progress=lambda f: True)
```

Settings only the learned generator uses (the template sets the editor's
values): `coherence` (a height plan of the whole map whose rims become long
cliffs), `calm_terrain`, `biome_calm` (larger ground areas that change at
cliffs), `lava_rock` (lava cores in rock), `volcanoes_small`, `_medium`,
`_large`, `craters`, `cracks`, `debris`, and a `gradient` across the map with
a ground and a terrain (flat .. rugged) for each end. Details in
[learned-generator.md](learned-generator.md).

In C: `om_mapgen_model_load`, `om_mapgen_generate_learned`, `om_mapgen_get_sketch`;
in .NET: `LearnedModel.LoadDefault()`, `MapGenSettings.GenerateLearned`. A
128×128 map takes 6–10 s. Details in [learned-generator.md](learned-generator.md).

**Writing the result into a map:**

1. Use the profile's standard mapping table as the new map's tile mapping table
   (and its tilesets). Copying them from a campaign map such as `cm02.map` gives
   the same table.
2. Write `tiles[i]` and `cell_types[i]` into every cell.
3. Width must be a power of two in the map header (lgWidth); a clip rectangle
   of `(32, 0, 31 + width, height - 2)` matches what Outpost 2's own maps use.
4. Spawns, volcanoes and lava zones are for your mission or editor: lava zones
   are the cells where lava may flow when a volcano erupts — the library places
   no lava tiles.

**Same seed, same map** — but not the same map as op2-codegen-editor's Python
generator: the library has its own random numbers and builds its tiles with
its own blend engine and cliff tool. With symmetry, both halves get their own
tiles and cliffs (walls face the right way on both sides); only the scatter is
mirrored.

**Speed**: most of a map takes milliseconds; cliffs cost about 50 ms per wall,
so a canyon full of plateaus takes a few seconds.

### Errors, threads and lifetime

- **C**: functions return NULL / -1 / 0 on failure and accept NULL handles.
  Creation functions take an `err` buffer for a readable message.
- **Python** raises `Op2MapLibError` (and `ValueError`/`KeyError` for bad
  arguments); **.NET** throws `Op2MapLibException`, `ArgumentException`,
  `ObjectDisposedException`.
- **Ownership**: everything you create you free (`*_free`, `close()`,
  `Dispose()`). Free blend maps before their profile.
- **Threads**: an object is not thread-safe, but separate objects can be used
  from separate threads; a profile can be shared read-only.

---

## Part 2 – Working on the library

### Layout

| Path | What |
|---|---|
| `include/op2maplib/op2maplib.h` | the only public header: the C API |
| `src/api.cpp`, `src/handles.hpp` | C wrappers: argument checks, exceptions → error codes |
| `src/profile.cpp`, `src/json.*` | profile loader, small JSON reader |
| `src/blend.*` | blend engine (paint, resolve) |
| `src/cliff.*` | cliff solver |
| `src/regions.cpp` | passable regions |
| `src/mapgen/` | map generator (layout, relief, shapes, spawns, pipeline, C API) |
| `src/rng.hpp` | the only random number generator (xoshiro256**) |
| `tests/blend_tests.cpp`, `tests/vectors/` | C++ tests and golden vectors |
| `bindings/python/`, `bindings/dotnet/` | bindings and their tests |
| `tools/` | profile export, native DLL build, compile check |

### Build and test

```
cmake -S . -B build -A x64 && cmake --build build --config Release
build\bin\Release\op2maplib_tests.exe profiles\op2.json
python -I bindings/python/tests/test_blend.py
python -I bindings/python/tests/test_cliffs.py
python -I bindings/python/tests/test_mapgen.py
python -I bindings/python/tests/test_regions.py "path/to/Outpost 2/OPU/base/maps"
```

`.NET`: `msbuild bindings\dotnet\tests\Op2MapLib.Net.Tests.csproj
/p:Configuration=Release /p:TestArch=x64` (and `x86`), then run
`bindings\dotnet\tests\bin\<arch>\Op2MapLib.Net.Tests.exe`.

`tools\compile-check.bat file.cpp ...` compiles single files with MSVC
(`/W4 /WX /permissive-`) without linking — handy while editing.

CI (`.github/workflows/build.yml`) runs all of this on Windows x64/x86, Linux
and macOS for every push and pull request. MSVC accepts some things GCC and
Clang reject (a type and a function with the same name, a missing standard
include), so a green Windows build alone is not enough.

### Rules that keep results identical everywhere

- **Randomness only through `om::Rng`** (`src/rng.hpp`). `<random>`'s
  distributions differ between standard libraries.
- **The order of random draws is part of the output.** Drawing one number more
  or fewer anywhere changes everything after it.
- **No unordered iteration in output paths.** Iterating an `unordered_map`
  gives a different order per standard library; sort first or use vectors.
- **No exceptions across the C boundary.** Every exported function catches.
- **No fused multiply-add, own `exp`/`log`/`erf` in the learned generator.**
  The build sets `-ffp-contract=off`; the network code (`src/mapgen/net.cpp`)
  keeps its vector paths doing exactly the operations of its scalar code. See
  [learned-generator.md](learned-generator.md#same-map-everywhere).
- **Golden hashes** pin the exact output: `tests/vectors/blend_disc.json`,
  `cliff_l.json`, `mapgen_islands.json`, `mapgen_mountains.json` and
  `mapgen_learned.json`, checked by the C++, Python and .NET
  tests on every platform. When you change output *on purpose*, run the tests,
  take the new hash from the failure message, and update it in the vector file
  (the Python tests read it from there), in `tests/blend_tests.cpp` and in
  `bindings/dotnet/tests/Program.cs`. Say in the commit why the output changed.

### Adding to the C API

1. Declare it in `op2maplib.h` with `OM_API ... OM_CALL`, documented in the
   header (units, ownership, return values, NULL behaviour). Don't reuse a
   type's name for a function.
2. Implement it in `src/api.cpp` (or `src/mapgen/api_mapgen.cpp`): check
   arguments, accept NULL handles, catch every exception.
3. Python: add the signature to the table in `_load()` (argument and result
   types are required — without them ctypes truncates 64-bit pointers) and a
   wrapper method.
4. .NET: add the `DllImport` in `Native` (cdecl) and a wrapper.
5. Tests in all three.

### Regenerating the profile

`profiles/op2.json` is exported from op2-codegen-editor's learned tile model:

```
python -I tools/export_op2_profile.py <op2-codegen-editor>/mapview <a campaign .map> profiles/op2.json
```

The export is deterministic; check that only what you meant to change differs
(the script prints a summary). The format is documented in
[profile-format.md](profile-format.md).

`profiles/op2-net.bin`, the learned generator's networks, is exported from the
files op2-codegen-editor ships (needs `numpy` and `onnx`):

```
python tools/export_learned_model.py <op2-codegen-editor>/mapview profiles/op2.json profiles/op2-net.bin
```

### Releasing

1. Bump the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`),
   `src/api.cpp` (`OM_VERSION_STRING`), `bindings/dotnet/Properties/AssemblyInfo.cs`
   and the README's Status.
2. `tools/build-native.ps1` rebuilds and tests both Windows DLLs and refreshes
   `bindings/dotnet/native/` (the DLLs committed for the .NET binding).
3. Run the Python and .NET tests, commit, push, wait for CI.
4. Tag `vX.Y.Z` and push the tag. CI builds, tests and publishes the release
   with checksums and attestations; verify it as described under
   [Getting it](#getting-it).

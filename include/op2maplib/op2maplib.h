/*
 * op2maplib - reusable map-editing algorithms for Outpost 2 map tools.
 *
 * Plain C API so any language can use the library: C/C++ directly, .NET via
 * P/Invoke, Python via ctypes/cffi, Rust, Delphi, WebAssembly, ...
 *
 * Current module: terrain blending ("om_blend_*"). Paint a ground type
 * (mud, rock, sand, ...) onto cells; the library picks tiles so the borders
 * between grounds use the game's own transition tiles. Which tiles exist and
 * how they fit together comes from a profile (JSON, see docs/profile-format.md),
 * so the library itself knows nothing about a particular tileset.
 *
 * The library never reads or writes .map files. The caller passes the map's
 * tile mapping table and the mapping index of every cell, and gets back a list
 * of changes to apply in its own editor (and its own undo system).
 *
 * Conventions
 *   - Every function is safe to call with NULL handles (it does nothing / fails).
 *   - Strings are UTF-8 (in practice ASCII). Returned strings are owned by the
 *     library and stay valid while the owning object lives.
 *   - Results are deterministic: the same inputs and seed give the same tiles
 *     on every platform and in every language binding.
 *   - Not thread-safe per object; separate objects may be used from separate threads.
 *   - Objects returned by *_load / *_create / *_find are owned by the caller
 *     and must be released with the matching *_free (which accepts NULL).
 *
 * The blending approach, the cliff tool, the map generator and the Outpost 2
 * tile data come from fefi-byte's op2-codegen-editor (MIT licence,
 * github.com/fefi-byte/op2-codegen-editor; see THIRD-PARTY-NOTICES.md).
 */
#ifndef OP2MAPLIB_H
#define OP2MAPLIB_H

#include <stddef.h>
#include <stdint.h>

/*
 * OM_API marks exported functions: define OP2MAPLIB_BUILD when building the
 * DLL, OP2MAPLIB_STATIC when linking the static library, neither when using
 * the DLL. OM_CALL fixes the calling convention (cdecl) for 32-bit Windows.
 */
#if defined(_WIN32)
#  if defined(OP2MAPLIB_BUILD)
#    define OM_API __declspec(dllexport)
#  elif defined(OP2MAPLIB_STATIC)
#    define OM_API
#  else
#    define OM_API __declspec(dllimport)
#  endif
#  define OM_CALL __cdecl
#else
#  define OM_API __attribute__((visibility("default")))
#  define OM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Library                                                                   */

/** Library version as "major.minor.patch". */
OM_API const char* OM_CALL om_version(void);

/* ------------------------------------------------------------------------ */
/* Profiles                                                                  */

/** Opaque loaded profile. Read-only once loaded; one profile can serve many
    blend maps. */
typedef struct om_profile om_profile;

/** One entry of a map's tile mapping table. */
typedef struct om_mapping {
    const char* tileset;   /* tileset file name as stored in the map, e.g. "well0001" (case-insensitive) */
    int32_t     image;     /* image (graphic) index within that tileset */
} om_mapping;

/** What kind of profile tile a cell holds (om_blend_tile_role). */
typedef enum om_tile_role {
    OM_TILE_UNKNOWN    = 0,   /* not in the profile: a fixed cell (cliff, wall, ...) */
    OM_TILE_FILL       = 1,   /* plain ground the library places */
    OM_TILE_GROUND     = 2,   /* belongs to a ground but is never placed (decoration, rare tiles) */
    OM_TILE_TRANSITION = 3    /* blends two grounds */
} om_tile_role;

/**
 * Loads a blend profile from JSON text (not a file name, so callers can embed
 * it or read it however they like). Returns NULL on failure and, if err is not
 * NULL, writes a message into err (at most err_size bytes, always terminated).
 */
OM_API om_profile* OM_CALL om_profile_load(const char* json, size_t json_len,
                                           char* err, size_t err_size);

/** Frees a profile (NULL is fine). Free every blend map created from it first. */
OM_API void OM_CALL om_profile_free(om_profile* profile);

/** Profile display name, e.g. "Outpost 2 standard tilesets (...)". */
OM_API const char* OM_CALL om_profile_name(const om_profile* profile);

/** Number of grounds (terrain types) in the profile. */
OM_API int OM_CALL om_profile_ground_count(const om_profile* profile);

/** Short id of a ground, e.g. "mud". NULL if out of range. */
OM_API const char* OM_CALL om_profile_ground_id(const om_profile* profile, int ground);

/** Display name of a ground, e.g. "Mud". NULL if out of range. */
OM_API const char* OM_CALL om_profile_ground_name(const om_profile* profile, int ground);

/**
 * Number of plain fill tiles for a ground. A ground with 0 cannot be painted
 * usefully (there is nothing to fill it with); UIs should hide or disable it.
 */
OM_API int OM_CALL om_profile_ground_fill_count(const om_profile* profile, int ground);

/** Index of the ground with this id, or -1. */
OM_API int OM_CALL om_profile_find_ground(const om_profile* profile, const char* id);

/** 1 if the profile has transition tiles between the two grounds, else 0. */
OM_API int OM_CALL om_profile_grounds_blend(const om_profile* profile, int ground_a, int ground_b);

/**
 * The standard tile mapping table carried by the profile (for Outpost 2: the
 * table shared by all campaign maps). Useful for creating new maps and for
 * tests. Returns the number of entries; 0 if the profile has none.
 */
OM_API int OM_CALL om_profile_standard_mapping_count(const om_profile* profile);

/** Fills *out with standard mapping entry i. Returns 1 on success, 0 if out of range. */
OM_API int OM_CALL om_profile_standard_mapping(const om_profile* profile, int index, om_mapping* out);

/* ------------------------------------------------------------------------ */
/* Blend maps                                                                */

/** Opaque blend state of one map: a copy of its cells plus a ground per cell. */
typedef struct om_blend_map om_blend_map;

/** A rectangle in cells: x, y inclusive; width/height in cells. */
typedef struct om_rect {
    int32_t x, y, width, height;
} om_rect;

/** One tile the caller should change. */
typedef struct om_change {
    int32_t x, y;
    int32_t old_mapping;   /* mapping index before */
    int32_t new_mapping;   /* mapping index to write */
    int32_t cell_type;     /* suggested cell type for the new tile, or -1 if the profile has none */
} om_change;

/**
 * Creates the blend state for one map.
 *   mappings / mapping_count : the map's tile mapping table.
 *   cells                    : width*height mapping indices, row by row.
 * The profile must stay loaded until the map is freed.
 * Each cell's ground is worked out from its tile. Tiles the profile does not
 * know (cliffs, walls, buildings, ...) are "fixed": never changed and ignored
 * when choosing neighbours' tiles, until a cell is painted over.
 */
OM_API om_blend_map* OM_CALL om_blend_create(const om_profile* profile,
                                             int32_t width, int32_t height,
                                             const om_mapping* mappings, int32_t mapping_count,
                                             const uint16_t* cells,
                                             char* err, size_t err_size);

/** Frees a blend map (NULL is fine). */
OM_API void OM_CALL om_blend_free(om_blend_map* map);

/** Ground of a cell, or -1 for fixed/unknown cells or out-of-range coordinates. */
OM_API int OM_CALL om_blend_ground_at(const om_blend_map* map, int32_t x, int32_t y);

/** Mapping index currently held for a cell, or -1 if out of range. */
OM_API int OM_CALL om_blend_tile_at(const om_blend_map* map, int32_t x, int32_t y);

/** Role (om_tile_role) of the tile a cell holds; OM_TILE_UNKNOWN if out of range. */
OM_API int OM_CALL om_blend_tile_role(const om_blend_map* map, int32_t x, int32_t y);

/**
 * Tells the library a cell's tile was changed outside it (manual tile placement,
 * undo/redo, ...). The cell's ground is re-derived from the new tile.
 */
OM_API void OM_CALL om_blend_set_tile(om_blend_map* map, int32_t x, int32_t y, int32_t mapping);

/**
 * Paints a ground onto cells. xy holds count (x, y) pairs. Out-of-range cells
 * are skipped. Only the ground layer changes; call om_blend_resolve to pick
 * tiles. Returns the number of cells painted, or -1 for an invalid ground.
 */
OM_API int OM_CALL om_blend_paint(om_blend_map* map, int ground, const int32_t* xy, int32_t count);

/**
 * Picks tiles for a region and applies them to the library's copy of the map.
 * region: cells to re-evaluate (one extra cell around it is included, since a
 * tile depends on its neighbours). NULL = everything painted since the last
 * resolve. Returns the number of changes, retrievable with om_blend_get_changes
 * until the next resolve, or -1 on error.
 */
OM_API int OM_CALL om_blend_resolve(om_blend_map* map, const om_rect* region, uint64_t seed);

/** Whole map: re-picks every blendable cell (a "clean up transitions" pass). */
OM_API int OM_CALL om_blend_resolve_all(om_blend_map* map, uint64_t seed);

/** Copies up to max changes from the last resolve, cliff draw or cut close. Returns the number copied. */
OM_API int OM_CALL om_blend_get_changes(const om_blend_map* map, om_change* out, int32_t max);

/* ------------------------------------------------------------------------ */
/* Cliffs                                                                    */
/*                                                                           */
/* Draw a cliff along a line of points. The line is turned into a band of    */
/* rock on a 4x4-per-tile grid (a wall where the high side is north, a thin  */
/* edge where it is south, steps on steep slopes), then the profile's cliff  */
/* tiles are fitted to it. Works on an om_blend_map; the changes come back   */
/* through om_blend_get_changes, with the cliff tiles' cell types (cliff     */
/* high side, low side, north cliffs, ...). Cliff cells become fixed for     */
/* terrain blending.                                                         */

/** Which side of a cliff line is high ground. LEFT/RIGHT are relative to the
    drawing direction; NORTH/SOUTH are fixed to the map. */
typedef enum om_cliff_side {
    OM_CLIFF_HIGH_LEFT  = 0,
    OM_CLIFF_HIGH_RIGHT = 1,
    OM_CLIFF_HIGH_NORTH = 2,
    OM_CLIFF_HIGH_SOUTH = 3
} om_cliff_side;

/**
 * 1 if the ground has cliff tiles of its own, 0 if cliffs drawn on it use the
 * profile's fallback cliffs (Outpost 2: mud), -1 if the profile has no usable
 * cliffs at all.
 */
OM_API int OM_CALL om_profile_cliff_ground(const om_profile* profile, int ground);

/**
 * Draws a cliff along a polyline.
 *   xy / point_count : point_count (x, y) cell coordinates, at least 2. Repeat
 *                      the first point at the end to close a loop.
 *   high_side        : an om_cliff_side.
 *   wall_height      : wall height in tiles, 3 to 6 (clamped).
 *   seed             : same seed and input give the same cliff.
 * The cliff family comes from the ground at the first point. Returns the
 * number of changes (om_blend_get_changes), 0 if nothing could be drawn, or
 * -1 for invalid input (points outside the map, bad side).
 */
OM_API int OM_CALL om_cliff_draw(om_blend_map* map, const int32_t* xy, int32_t point_count,
                                 int high_side, double wall_height, uint64_t seed);

/**
 * Closes cut wall ends. Painting ground across a cliff (e.g. for a ramp)
 * leaves the wall cut off with rock pointing into the open; this lets both
 * new ends finish cleanly. Call it after applying the painted ground.
 *   removed : width*height bytes, row by row, nonzero = the cell was a cliff
 *             before the stroke and is ground now (cells that still hold a
 *             cliff tile are ignored).
 * Only the cliff cells within two cells of the cut are solved again; the rest
 * of the wall stays as it is. Where the cut cannot be closed as it is (e.g. a
 * diagonal wall cut straight across), the wall end recedes by up to two cells.
 * Cliff cells that keep no rock become ground of their surroundings and are
 * blended. Returns the number of changes
 * (om_blend_get_changes; cliff tiles first, then blended ground), 0 if there
 * was nothing to close or no solution (the wall stays as it is), or -1 for
 * invalid input. om_cliff_last_stage reports the solver stage.
 */
OM_API int OM_CALL om_cliff_close_cut(om_blend_map* map, const uint8_t* removed, uint64_t seed);

/**
 * How the last om_cliff_draw was solved: 0..7 = solver stage (0 is the
 * strictest: only tile pairs seen in the original maps), -1 = greedy fallback
 * (no stage could solve it; expect rougher joins), -2 = nothing drawn.
 */
OM_API int OM_CALL om_cliff_last_stage(const om_blend_map* map);

/**
 * Snaps a point for interactive drawing: moves (bx, by) so the segment from
 * (ax, ay) uses an allowed cliff slope (flat, vertical, 45 degrees, or 2:1,
 * 3:1, 4:1 either way), keeping about the same length.
 */
OM_API void OM_CALL om_cliff_snap_point(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                                        int32_t* out_x, int32_t* out_y);

/* ------------------------------------------------------------------------ */
/* Passable regions (enclosed areas)                                         */
/*                                                                           */
/* Splits a map's passable cells into connected regions, largest first. The */
/* largest region is the main play area; every other region is enclosed:     */
/* units can never reach it from the main area. Works on plain cell types    */
/* (Outpost 2 CellType values 0-31), so it needs no profile or blend map.    */

/** Opaque result of om_regions_find; holds its own copy of the regions. */
typedef struct om_regions om_regions;

/** Options for om_regions_find. */
typedef enum om_region_flags {
    OM_REGIONS_CONNECT8         = 1,   /* diagonal neighbours connect too (else only N/E/S/W) */
    OM_REGIONS_WRAP_X           = 2,   /* the left and right map edges join (Outpost 2 world maps) */
    OM_REGIONS_SKIP_BOTTOM_ROW  = 4    /* ignore the last row (Outpost 2: never shown, always impassable) */
} om_region_flags;

/**
 * The flags matching Outpost 2: 8-connected, bottom row skipped, and the left
 * and right edges joined only on the 512x256 world map size.
 */
OM_API uint32_t OM_CALL om_regions_op2_flags(int32_t width, int32_t height);

/**
 * Bit mask of the cell types that count as passable in Outpost 2 (bit n =
 * cell type n): fast, medium and slow passable, bulldozed, rubble and tubes.
 * Walls, cliffs, vents and the impassable types block.
 */
OM_API uint32_t OM_CALL om_cell_types_passable_mask(void);

/**
 * Finds the connected regions of passable cells.
 *   cell_types     : width*height cell types (0-31), row by row.
 *   passable_mask  : bit n set = cell type n is passable; 0 = om_cell_types_passable_mask().
 *   flags          : om_region_flags, e.g. om_regions_op2_flags(width, height).
 * Regions are sorted largest first (ties: the one whose first cell comes
 * first, row by row). Returns NULL for invalid input. Free with om_regions_free.
 */
OM_API om_regions* OM_CALL om_regions_find(int32_t width, int32_t height, const uint8_t* cell_types,
                                           uint32_t passable_mask, uint32_t flags);

/** Frees a regions result (NULL is fine). */
OM_API void OM_CALL om_regions_free(om_regions* regions);

/** Number of regions (0 if the map has no passable cells). */
OM_API int OM_CALL om_regions_count(const om_regions* regions);

/** Number of cells in a region, or 0 if out of range. */
OM_API int OM_CALL om_regions_size(const om_regions* regions, int region);

/**
 * Copies up to max cells of a region as (x, y) pairs into xy (2*max ints).
 * Returns the number of cells copied.
 */
OM_API int OM_CALL om_regions_cells(const om_regions* regions, int region, int32_t* xy, int32_t max);

/** Region of a cell, or -1 if the cell is not passable, skipped or outside the map. */
OM_API int OM_CALL om_regions_at(const om_regions* regions, int32_t x, int32_t y);

/** Total number of cells in all enclosed regions (every region but the largest). */
OM_API int OM_CALL om_regions_enclosed_size(const om_regions* regions);

/**
 * Copies up to max cells of all enclosed regions (every region but the
 * largest) as (x, y) pairs. Returns the number of cells copied.
 */
OM_API int OM_CALL om_regions_enclosed_cells(const om_regions* regions, int32_t* xy, int32_t max);

/* ------------------------------------------------------------------------ */
/* Map generator                                                             */
/*                                                                           */
/* Generates a whole map from a seed: a biome layout (terrain mix, roughness, */
/* symmetry), relief (plateaus with ramps, valleys, mountains, chokes,        */
/* clearings, the New Terra landscape mode with mesas and dry riverbeds),     */
/* volcanoes with lava-possible zones, scattered rocks and craters, and spawn */
/* points. Ground tiles come from the blend engine and cliffs from the cliff  */
/* solver, so the tiles follow the same rules as painting. Tiles are mapping  */
/* indices into the profile's standard mapping table                          */
/* (om_profile_standard_mapping), which a new map should use as its own.      */
/* Same profile, settings and seed give the same map.                         */

/** Generator settings (defaults, a template, mix and features). */
typedef struct om_mapgen_settings om_mapgen_settings;
/** A generated map. */
typedef struct om_mapgen_result om_mapgen_result;

/** Symmetry modes (the "symmetry" setting). Quad suits four players, the others two. */
typedef enum om_mapgen_symmetry {
    OM_SYMMETRY_NONE     = 0,
    OM_SYMMETRY_MIRROR_X = 1,   /* left half mirrored onto the right */
    OM_SYMMETRY_MIRROR_Y = 2,   /* top half mirrored onto the bottom */
    OM_SYMMETRY_ROT180   = 3,   /* point-symmetric */
    OM_SYMMETRY_QUAD     = 4    /* four-fold mirrored */
} om_mapgen_symmetry;

/** Terrain feature kinds (om_mapgen_add_feature). */
typedef enum om_mapgen_feature {
    OM_FEATURE_VALLEY   = 0,
    OM_FEATURE_MOUNTAIN = 1,
    OM_FEATURE_PLATEAU  = 2,
    OM_FEATURE_CLEARING = 3,   /* kept free of cliffs, volcanoes and scatter */
    OM_FEATURE_CHOKE    = 4    /* a barrier across the map with one gap */
} om_mapgen_feature;

/** Point lists of a result (om_mapgen_point_count / om_mapgen_get_points). */
typedef enum om_mapgen_points {
    OM_MAPGEN_SPAWNS    = 0,
    OM_MAPGEN_VOLCANOES = 1
} om_mapgen_points;

/**
 * Creates settings from a template: "plains", "canyon", "desert", "islands",
 * "ice", "newterra" or "learned" (NULL, "" or an unknown name = "plains").
 * The template sets the terrain mix, roughness, landscape mode and volcanoes
 * ("learned": suggestions for om_mapgen_generate_learned, with mountains,
 * plateaus and cliffs); everything else starts at its default. Free with
 * om_mapgen_settings_free.
 */
OM_API om_mapgen_settings* OM_CALL om_mapgen_settings_create(const char* template_name);
OM_API void OM_CALL om_mapgen_settings_free(om_mapgen_settings* settings);

/**
 * Sets a numeric setting. Returns 0, or -1 for an unknown key. Keys (default):
 *   width, height (128)     map size in cells, 16..1024 (OP2: powers of two)
 *   roughness (template)    0 = soft large areas .. 1 = rugged
 *   symmetry (0)            om_mapgen_symmetry
 *   spawns (0)              spawn points; spawn_clearance (6) free radius each
 *   cliffs (0)              plateau share; ramps (2) gaps per plateau ring
 *   volcanoes (template)    with lava_zone_tiles (300) lava-possible cells each
 *   smooth (2)              smoothing passes against specks
 *   vegetation_spacing (1)  how far vegetation stays from rock, sand and snow
 *   min_biome (0.006)       smallest biome patch, as a share of the map
 *   details (0.5)           scatter density; 0.5 is about the original maps
 *   landscape (template)    1 = New Terra relief mode
 *   rivers (-1)             dry riverbeds in landscape mode, -1 = by map size
 *   mountains (0)           share of the map in mountain chains (0 = none): tight
 *                           terraces, no building ground, closed outer wall
 *   mountain_density (0.8)  how tight the terraces stand, 1 = as tight as walls allow
 *   gorges (1), valleys (1) narrow passable cuts / wide cuts with building ground
 *                           through the mountains
 *   plateaus (0)            one-terrace areas with building ground and two ramps
 *   mountain_spread (0)     1 = more, more varied mountains over the whole map
 *   world_cutout (0)        1 = classic cliffs are planned on a larger world and
 *                           cut out, so they cross the map edge (only without
 *                           volcanoes, features, landscape mode and mountains;
 *                           mountains always do this)
 *   learned_rocks (1)       learned generator: rocks and craters where walls alone
 *                           leave building ground in mountains and gorges
 * Learned generator only (all 0 = as in library versions before them; the
 * template "learned" sets the values in brackets):
 *   coherence (0.6)         height plan of the whole map: uplands and basins
 *                           whose rims become long, stepped cliffs, nested
 *                           terraces in mountains, small closed hills; spreads
 *                           the mountains over the whole world
 *   calm_terrain (0.5)      fewer, longer cliffs, larger uplands, fewer terraces
 *                           and hills (only with coherence)
 *   biome_calm (0.5)        merges grounds into larger areas; walls are barriers,
 *                           so grounds prefer to change at cliffs
 *   lava_rock (0.5)         dark lava cores in rock areas, with transition tiles
 *                           and the rock tiles' brush patterns (0.5 = as the
 *                           original maps, 1 = more often and larger)
 *   volcanoes_small (1), volcanoes_medium (1), volcanoes_large (0)
 *                           volcanoes per size, each on a site reserved in the
 *                           sketch (rock or a lava core), with a lava-possible
 *                           zone of lava_zone_tiles cells
 *   craters (0.3), cracks (0.3), debris (0.15)
 *                           the game's tile groups on matching ground: impact
 *                           craters on dirt and sand, long cracks in rock,
 *                           crash sites (wreckage, rubble, burn marks); 1 = about
 *                           36 craters, 16 cracks, 7 crash sites per 128x128
 *   gradient (0)            ground and terrain change across the map: 0 off,
 *                           1 north->south, 2 south->north, 3 west->east,
 *                           4 east->west, 5 northwest->southeast, 6 southeast->
 *                           northwest, 7 northeast->southwest, 8 southwest->
 *                           northeast, 9 centre->edge, 10 edge->centre
 *   gradient_ground_start (-1), gradient_ground_end (-1)
 *                           ground at that end: -1 the mix, 0 mud, 1 rock, 2 sand
 *   gradient_terrain_start (0.5), gradient_terrain_end (0.5)
 *                           terrain at that end: 0 flat .. 0.5 as usual .. 1
 *                           rugged (mountains, uplands and basins move there)
 * Mountains and plateaus are planned on a world 32 cells larger on every side
 * and cut out, so their walls run off the map edge as in the original maps.
 * The seed has its own setter (64 bits do not fit a double exactly).
 */
OM_API int OM_CALL om_mapgen_set(om_mapgen_settings* settings, const char* key, double value);
/** Current value of a numeric setting, or 0 for an unknown key. */
OM_API double OM_CALL om_mapgen_get(const om_mapgen_settings* settings, const char* key);
OM_API void OM_CALL om_mapgen_set_seed(om_mapgen_settings* settings, uint64_t seed);

/** Empties the terrain mix (the template's) before setting your own shares. */
OM_API void OM_CALL om_mapgen_clear_mix(om_mapgen_settings* settings);
/**
 * Sets one ground's share of the terrain mix, by profile ground id ("mud",
 * "roc", "san", "veg", "sno"). Shares need not add up to 1. Returns 0, or -1
 * for a negative share or an empty id.
 */
OM_API int OM_CALL om_mapgen_set_mix(om_mapgen_settings* settings, const char* ground_id, double share);

/** Removes all features. */
OM_API void OM_CALL om_mapgen_clear_features(om_mapgen_settings* settings);
/**
 * Adds a terrain feature, placed before the random layout.
 *   kind  : om_mapgen_feature
 *   where : rough position, a sector of a 3x3 grid: "nw", "n", "no", "w",
 *           "c", "o", "sw", "s", "so" (German compass: o = east); NULL or "" =
 *           the seed decides
 *   to    : valley/choke end sector; NULL or "" = opposite of `where`
 *   size  : 0..1
 *   lava  : valley only, nonzero = lava-possible floor
 * Returns 0, or -1 for a bad kind.
 */
OM_API int OM_CALL om_mapgen_add_feature(om_mapgen_settings* settings, int kind, const char* where,
                                         const char* to, double size, int lava);

/**
 * Generates a map. Returns NULL on failure (bad size, profile without a
 * standard mapping table) with a message in err. Free with om_mapgen_result_free.
 */
OM_API om_mapgen_result* OM_CALL om_mapgen_generate(const om_profile* profile, const om_mapgen_settings* settings,
                                                    char* err, size_t err_size);
OM_API void OM_CALL om_mapgen_result_free(om_mapgen_result* result);

OM_API int32_t OM_CALL om_mapgen_width(const om_mapgen_result* result);
OM_API int32_t OM_CALL om_mapgen_height(const om_mapgen_result* result);
/** Copies up to max mapping indices (standard mapping table), row by row. Returns the number copied. */
OM_API int OM_CALL om_mapgen_get_tiles(const om_mapgen_result* result, uint16_t* out, int32_t max);
/** Copies up to max cell types (0-31), row by row. Returns the number copied. */
OM_API int OM_CALL om_mapgen_get_cell_types(const om_mapgen_result* result, uint8_t* out, int32_t max);

/** Number of points in a list (om_mapgen_points). */
OM_API int OM_CALL om_mapgen_point_count(const om_mapgen_result* result, int list);
/** Copies up to max points of a list as (x, y) pairs. Returns the number copied. */
OM_API int OM_CALL om_mapgen_get_points(const om_mapgen_result* result, int list, int32_t* xy, int32_t max);

/** Lava-possible zones: one per volcano, plus one per lava valley. */
OM_API int OM_CALL om_mapgen_lava_zone_count(const om_mapgen_result* result);
OM_API int OM_CALL om_mapgen_lava_zone_size(const om_mapgen_result* result, int zone);
/** Copies up to max cells of a zone as (x, y) pairs. Returns the number copied. */
OM_API int OM_CALL om_mapgen_lava_zone_cells(const om_mapgen_result* result, int zone, int32_t* xy, int32_t max);

/**
 * Statistics: "passable" (share of passable cells), "plateau" (share of cliff
 * cells), "details" (share covered by scatter), "rivers" (share of riverbed),
 * "connected" (1 if all spawns reach each other). With mountains or plateaus
 * also: "mountains", "mountains_leaky" (outer wall not closed), "plateaus",
 * "plateaus_unreachable", "build" (share of the map that is building ground:
 * covered by a free, reachable 7x7 square), "build_mountain", "build_plateau",
 * "build_valley", "build_gorge" (building ground share per zone, -1 if the map
 * has no such zone), "gorges", "gorges_passable". 0 for an unknown key.
 */
OM_API double OM_CALL om_mapgen_stat(const om_mapgen_result* result, const char* key);
/** Share of the map covered by a profile ground (by the ground of its tiles). */
OM_API double OM_CALL om_mapgen_ground_share(const om_mapgen_result* result, int ground);

/** Features as placed: count, and per feature its kind, position and size (radius or width). */
OM_API int OM_CALL om_mapgen_feature_count(const om_mapgen_result* result);
/** Fills the outputs (any may be NULL). Returns 1, or 0 if the index is out of range. */
OM_API int OM_CALL om_mapgen_get_feature(const om_mapgen_result* result, int index,
                                     int32_t* kind, int32_t* x, int32_t* y, int32_t* size);
/** Per feature kind: cliff cells it produced (clearings: how many were placed). */
OM_API int OM_CALL om_mapgen_feature_report(const om_mapgen_result* result, int kind);

/* ------------------------------------------------------------------------ */
/* Learned map generator                                                     */
/*                                                                           */
/* Maps in the style of the original maps, from two small neural networks    */
/* trained on 61 maps (originals and OPU): a sketch network lays out walls   */
/* and grounds on a half-resolution grid, guided by zones (mountains and     */
/* gorges: no building ground; plateaus and valleys: building ground), and a */
/* tile network paints the map tile by tile, using only tile pairs that      */
/* occur in the training maps. The networks run on this library's own        */
/* inference code (CPU, no dependencies), the same on every platform and for */
/* any thread count. The network file for the Outpost 2 profile ships as     */
/* profiles/op2-net.bin. Uses the same settings as om_mapgen_generate         */
/* (template "learned" for suggestions, see om_mapgen_set for the settings   */
/* only it uses); features, "volcanoes" (use the per-size keys), landscape   */
/* mode and roughness are not used. Width and height must be multiples of 8. */

/** The learned generator's networks and tile tables (profiles/op2-net.bin). */
typedef struct om_mapgen_model om_mapgen_model;

/** Quality levels of om_mapgen_generate_learned. */
typedef enum om_learned_quality {
    OM_LEARNED_SKETCH = 0,   /* only the sketch (about a second): om_mapgen_get_sketch */
    OM_LEARNED_FAST   = 1,   /* 6 painting rounds */
    OM_LEARNED_GOOD   = 2    /* 24 painting rounds, fewer odd spots, about 1.5x slower */
} om_learned_quality;

/**
 * Progress callback: fraction 0..1 of the painting (0 while sketching).
 * Return 0 to go on, nonzero to cancel (the call then fails with "cancelled").
 */
typedef int (OM_CALL *om_progress_fn)(double fraction, void* user);

/** Loads a network file from memory (copied). Returns NULL with a message in err on bad data. */
OM_API om_mapgen_model* OM_CALL om_mapgen_model_load(const void* data, size_t len, char* err, size_t err_size);
OM_API void OM_CALL om_mapgen_model_free(om_mapgen_model* model);

/**
 * Generates a map with the learned generator. threads: 0 = automatic (half the
 * cores, at most 8); the map does not depend on it. progress may be NULL. A
 * sketch-only result (OM_LEARNED_SKETCH) has no tiles (width and height 0).
 * The result works with all om_mapgen_* result functions, including the zone
 * statistics. Returns NULL with a message in err on failure or cancel.
 * Same profile, model, settings and seed give the same map.
 */
OM_API om_mapgen_result* OM_CALL om_mapgen_generate_learned(const om_profile* profile, const om_mapgen_model* model,
                                                            const om_mapgen_settings* settings, int quality, int threads,
                                                            om_progress_fn progress, void* user, char* err,
                                                            size_t err_size);
/** Size of the result's sketch (half the map size; 0 x 0 for other results). Returns width * height. */
OM_API int OM_CALL om_mapgen_sketch_size(const om_mapgen_result* result, int32_t* width, int32_t* height);
/**
 * Copies up to max sketch cells, row by row: walls 0 = none, 1..8 = the
 * direction the wall falls towards (E, SE, S, SW, W, NW, N, NE); grounds 0 mud,
 * 1 rock, 2 sand, 3 vegetation, 4 other. Either buffer may be NULL. Returns the
 * number copied.
 */
OM_API int OM_CALL om_mapgen_get_sketch(const om_mapgen_result* result, uint8_t* walls, uint8_t* grounds, int32_t max);

#ifdef __cplusplus
}
#endif

#endif /* OP2MAPLIB_H */

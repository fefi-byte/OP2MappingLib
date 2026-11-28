"""Python binding for op2maplib (ctypes, no compile step).

Loads the op2maplib shared library and wraps its C API
(include/op2maplib/op2maplib.h) in three classes:

    Profile   - a blend profile (which tiles exist and how they fit)
    BlendMap  - the blend state of one map: paint grounds, resolve tiles,
                draw cliffs
    Regions   - connected passable regions of a map (enclosed areas)
    MapGenerator / GeneratedMap - generate a whole map from a seed

plus the helpers version() and snap_cliff_point().

The library never reads or writes .map files: you pass in the map's tile
mapping table and the mapping index of every cell, and get back a list of
Change records to apply in your own editor (and its own undo system).

Units and conventions:
    - Coordinates are in cells (tiles), x to the east, y to the south, with
      (0, 0) the top-left cell. Cell arrays are row by row (index y*width + x).
    - A "mapping index" is an index into the map's tile mapping table; each
      entry is a (tileset name, image index) pair, e.g. ("well0001", 12).
    - A "ground" is an index into the profile's list of terrain types
      (see Profile.find_ground).
    - Results are deterministic: the same inputs and seed give the same tiles
      on every platform and in every language binding.
    - Objects are not thread-safe; separate objects may be used from separate
      threads.

The library is loaded once, when this package is imported. Finding it, in order:
    1. the OP2MAPLIB_PATH environment variable (full path to the .dll/.so/.dylib)
    2. next to this package (op2maplib.dll / libop2maplib.so / libop2maplib.dylib)
    3. the repository's CMake build output (build/bin/Release, build/bin, build/lib)

Example:
    from op2maplib import Profile, BlendMap

    profile = Profile.from_file("profiles/op2.json")
    mappings = profile.standard_mapping()               # or the map's own table
    bm = BlendMap(profile, 64, 64, mappings, cells)     # cells: 64*64 mapping indices
    bm.paint(profile.find_ground("san"), [(10, 10), (11, 10)])
    for change in bm.resolve(seed=1234):
        ...  # write change.new_mapping / change.cell_type into your map

Importing raises Op2MapLibError if the shared library cannot be found.
The tests in bindings/python/tests show complete, runnable examples.
"""
from __future__ import annotations

import ctypes
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

__all__ = ["LearnedModel", "LearnedQuality", "Profile", "BlendMap", "Change", "TileRole", "CliffSide", "Op2MapLibError", "version",
           "snap_cliff_point", "Regions", "MapGenerator", "GeneratedMap", "Symmetry", "FeatureKind"]


class Op2MapLibError(RuntimeError):
    """Raised when the native library reports an error (or cannot be found)."""


class TileRole:
    """What kind of profile tile a cell holds (om_tile_role).

    Returned by BlendMap.tile_role().
    """
    UNKNOWN = 0     # not in the profile (a fixed cell: cliff, wall, ...)
    FILL = 1         # plain ground the library places
    GROUND = 2       # belongs to a ground but is never placed (decoration)
    TRANSITION = 3   # blends two grounds


class CliffSide:
    """Which side of a cliff line is high ground (om_cliff_side).

    LEFT and RIGHT are relative to the drawing direction (walking from the
    first point to the last); NORTH and SOUTH are fixed to the map. Used as
    the high_side argument of BlendMap.draw_cliff().
    """
    LEFT = 0     # relative to the drawing direction
    RIGHT = 1
    NORTH = 2    # fixed to the map
    SOUTH = 3


# --- native types ------------------------------------------------------------
# ctypes mirrors of the C structs; field order and types must match op2maplib.h.

class _Mapping(ctypes.Structure):
    """om_mapping: one tile mapping table entry (tileset name, image index)."""
    _fields_ = [("tileset", ctypes.c_char_p), ("image", ctypes.c_int32)]


class _Rect(ctypes.Structure):
    """om_rect: a rectangle in cells (x, y of the top-left cell; width, height)."""
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32),
                ("width", ctypes.c_int32), ("height", ctypes.c_int32)]


class _Change(ctypes.Structure):
    """om_change: one tile the caller should change (copied into Change)."""
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32),
                ("old_mapping", ctypes.c_int32), ("new_mapping", ctypes.c_int32),
                ("cell_type", ctypes.c_int32)]


@dataclass(frozen=True)
class Change:
    """One tile to change: write new_mapping (and cell_type, if not -1) at (x, y).

    Attributes:
        x, y:         cell coordinates.
        old_mapping:  mapping index the cell held before.
        new_mapping:  mapping index to write.
        cell_type:    suggested Outpost 2 cell type (0-31) for the new tile, or
                      -1 if the profile has none for it (keep the old one).
    """
    x: int
    y: int
    old_mapping: int
    new_mapping: int
    cell_type: int


# --- loading -----------------------------------------------------------------

def _library_names() -> list[str]:
    """File names the shared library has on this platform."""
    if sys.platform == "win32":
        return ["op2maplib.dll"]
    if sys.platform == "darwin":
        return ["libop2maplib.dylib"]
    return ["libop2maplib.so"]


def _find_library() -> str:
    """Path of the shared library (search order: see the module docstring)."""
    env = os.environ.get("OP2MAPLIB_PATH")
    if env:
        return env   # taken as given; ctypes reports it if the file is missing
    here = Path(__file__).resolve().parent
    # This file is bindings/python/op2maplib/__init__.py, so three levels up
    # is the repository root (where CMake's build/ directory lives).
    repo = here.parent.parent.parent
    dirs = [here, repo / "build" / "bin" / "Release", repo / "build" / "bin",
            repo / "build" / "lib" / "Release", repo / "build" / "lib"]
    for d in dirs:
        for name in _library_names():
            if (d / name).is_file():
                return str(d / name)
    raise Op2MapLibError(
        "op2maplib shared library not found; build it with CMake or set OP2MAPLIB_PATH")


# om_progress_fn: int (double fraction, void* user), cdecl on every platform.
_PROGRESS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_double, ctypes.c_void_p)


def _load():
    """Loads the library and declares every C function's signature.

    Declaring restype/argtypes is required: without them ctypes assumes int
    results, which truncates the 64-bit handles and string pointers.
    """
    lib = ctypes.CDLL(_find_library())
    P = ctypes.c_void_p   # opaque handles (om_profile*, om_blend_map*, om_regions*)
    # name: (result type, [argument types]), in the order of op2maplib.h
    sig = {
        "om_version": (ctypes.c_char_p, []),
        "om_profile_load": (P, [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]),
        "om_profile_free": (None, [P]),
        "om_profile_name": (ctypes.c_char_p, [P]),
        "om_profile_ground_count": (ctypes.c_int, [P]),
        "om_profile_ground_id": (ctypes.c_char_p, [P, ctypes.c_int]),
        "om_profile_ground_name": (ctypes.c_char_p, [P, ctypes.c_int]),
        "om_profile_ground_fill_count": (ctypes.c_int, [P, ctypes.c_int]),
        "om_profile_find_ground": (ctypes.c_int, [P, ctypes.c_char_p]),
        "om_profile_grounds_blend": (ctypes.c_int, [P, ctypes.c_int, ctypes.c_int]),
        "om_profile_standard_mapping_count": (ctypes.c_int, [P]),
        "om_profile_standard_mapping": (ctypes.c_int, [P, ctypes.c_int, ctypes.POINTER(_Mapping)]),
        "om_blend_create": (P, [P, ctypes.c_int32, ctypes.c_int32, ctypes.POINTER(_Mapping), ctypes.c_int32,
                                ctypes.POINTER(ctypes.c_uint16), ctypes.c_char_p, ctypes.c_size_t]),
        "om_blend_free": (None, [P]),
        "om_blend_ground_at": (ctypes.c_int, [P, ctypes.c_int32, ctypes.c_int32]),
        "om_blend_tile_at": (ctypes.c_int, [P, ctypes.c_int32, ctypes.c_int32]),
        "om_blend_tile_role": (ctypes.c_int, [P, ctypes.c_int32, ctypes.c_int32]),
        "om_blend_set_tile": (None, [P, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32]),
        "om_blend_paint": (ctypes.c_int, [P, ctypes.c_int, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32]),
        "om_blend_resolve": (ctypes.c_int, [P, ctypes.POINTER(_Rect), ctypes.c_uint64]),
        "om_blend_resolve_all": (ctypes.c_int, [P, ctypes.c_uint64]),
        "om_blend_get_changes": (ctypes.c_int, [P, ctypes.POINTER(_Change), ctypes.c_int32]),
        "om_profile_cliff_ground": (ctypes.c_int, [P, ctypes.c_int]),
        "om_cliff_draw": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
                                         ctypes.c_int, ctypes.c_double, ctypes.c_uint64]),
        "om_cliff_last_stage": (ctypes.c_int, [P]),
        "om_cliff_close_cut": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint64]),
        "om_cliff_snap_point": (None, [ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
                                       ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)]),
        "om_regions_op2_flags": (ctypes.c_uint32, [ctypes.c_int32, ctypes.c_int32]),
        "om_cell_types_passable_mask": (ctypes.c_uint32, []),
        "om_regions_find": (P, [ctypes.c_int32, ctypes.c_int32, ctypes.POINTER(ctypes.c_uint8),
                                ctypes.c_uint32, ctypes.c_uint32]),
        "om_regions_free": (None, [P]),
        "om_regions_count": (ctypes.c_int, [P]),
        "om_regions_size": (ctypes.c_int, [P, ctypes.c_int]),
        "om_regions_cells": (ctypes.c_int, [P, ctypes.c_int, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32]),
        "om_regions_at": (ctypes.c_int, [P, ctypes.c_int32, ctypes.c_int32]),
        "om_regions_enclosed_size": (ctypes.c_int, [P]),
        "om_regions_enclosed_cells": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32]),
        "om_mapgen_settings_create": (P, [ctypes.c_char_p]),
        "om_mapgen_settings_free": (None, [P]),
        "om_mapgen_set": (ctypes.c_int, [P, ctypes.c_char_p, ctypes.c_double]),
        "om_mapgen_get": (ctypes.c_double, [P, ctypes.c_char_p]),
        "om_mapgen_set_seed": (None, [P, ctypes.c_uint64]),
        "om_mapgen_clear_mix": (None, [P]),
        "om_mapgen_set_mix": (ctypes.c_int, [P, ctypes.c_char_p, ctypes.c_double]),
        "om_mapgen_clear_features": (None, [P]),
        "om_mapgen_add_feature": (ctypes.c_int, [P, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p,
                                                 ctypes.c_double, ctypes.c_int]),
        "om_mapgen_generate": (P, [P, P, ctypes.c_char_p, ctypes.c_size_t]),
        "om_mapgen_result_free": (None, [P]),
        "om_mapgen_width": (ctypes.c_int32, [P]),
        "om_mapgen_height": (ctypes.c_int32, [P]),
        "om_mapgen_get_tiles": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_uint16), ctypes.c_int32]),
        "om_mapgen_get_cell_types": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32]),
        "om_mapgen_point_count": (ctypes.c_int, [P, ctypes.c_int]),
        "om_mapgen_get_points": (ctypes.c_int, [P, ctypes.c_int, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32]),
        "om_mapgen_lava_zone_count": (ctypes.c_int, [P]),
        "om_mapgen_lava_zone_size": (ctypes.c_int, [P, ctypes.c_int]),
        "om_mapgen_lava_zone_cells": (ctypes.c_int, [P, ctypes.c_int, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32]),
        "om_mapgen_stat": (ctypes.c_double, [P, ctypes.c_char_p]),
        "om_mapgen_ground_share": (ctypes.c_double, [P, ctypes.c_int]),
        "om_mapgen_feature_count": (ctypes.c_int, [P]),
        "om_mapgen_get_feature": (ctypes.c_int, [P, ctypes.c_int] + [ctypes.POINTER(ctypes.c_int32)] * 4),
        "om_mapgen_feature_report": (ctypes.c_int, [P, ctypes.c_int]),
        "om_mapgen_model_load": (P, [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]),
        "om_mapgen_model_free": (None, [P]),
        "om_mapgen_generate_learned": (P, [P, P, P, ctypes.c_int, ctypes.c_int, _PROGRESS, P,
                                           ctypes.c_char_p, ctypes.c_size_t]),
        "om_mapgen_sketch_size": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)]),
        "om_mapgen_get_sketch": (ctypes.c_int, [P, ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint8),
                                                ctypes.c_int32]),
    }
    for name, (res, args) in sig.items():
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = args
    return lib


_lib = _load()


def version() -> str:
    """Native library version as "major.minor.patch", e.g. '0.1.0'."""
    return _lib.om_version().decode()


def snap_cliff_point(ax: int, ay: int, bx: int, by: int) -> tuple[int, int]:
    """Snaps a point for interactive cliff drawing (om_cliff_snap_point).

    Moves (bx, by) so the segment from (ax, ay) uses an allowed cliff slope
    (flat, vertical, 45 degrees, or 2:1, 3:1, 4:1 either way), keeping about
    the same length.

    Args:
        ax, ay: the fixed start point, in cells.
        bx, by: the point to snap (e.g. the mouse position), in cells.

    Returns:
        The snapped point as (x, y), in cells.
    """
    ox, oy = ctypes.c_int32(), ctypes.c_int32()
    _lib.om_cliff_snap_point(ax, ay, bx, by, ctypes.byref(ox), ctypes.byref(oy))
    return ox.value, oy.value


# --- Profile -----------------------------------------------------------------

class Profile:
    """A blend profile: which tiles exist and how they fit together.

    The profile is JSON (format: docs/profile-format.md); the library itself
    knows nothing about a particular tileset. profiles/op2.json is the one for
    the standard Outpost 2 tilesets.

    Keep it alive as long as any BlendMap made from it (BlendMap holds a
    reference for you). The native object is freed by close() or when the
    Profile is garbage collected.
    """

    def __init__(self, json_text: str | bytes):
        """Loads a profile from JSON text (om_profile_load).

        Args:
            json_text: the profile's JSON, as str or UTF-8 bytes (not a file
                name; use from_file for that).

        Raises:
            Op2MapLibError: the JSON is invalid or not a usable profile; the
                message includes the library's reason.
        """
        data = json_text.encode("utf-8") if isinstance(json_text, str) else json_text
        self._free = _lib.om_profile_free   # see close()
        err = ctypes.create_string_buffer(512)   # the library writes its error message here
        self._h = _lib.om_profile_load(data, len(data), err, len(err))
        if not self._h:
            raise Op2MapLibError(f"could not load profile: {err.value.decode(errors='replace')}")

    @classmethod
    def from_file(cls, path: str | os.PathLike) -> "Profile":
        """Loads a profile from a JSON file.

        Raises:
            OSError: the file cannot be read.
            Op2MapLibError: the file is not a usable profile.
        """
        return cls(Path(path).read_bytes())

    def close(self) -> None:
        """Frees the native profile. Safe to call more than once.

        Close any BlendMap made from this profile first.
        """
        # getattr: __init__ may have raised before _h was set.
        # self._free, not _lib: at interpreter shutdown the module's globals
        # may already be None when __del__ runs; the bound function stays.
        if getattr(self, "_h", None):
            self._free(self._h)
            self._h = None

    def __del__(self):
        self.close()

    @property
    def name(self) -> str:
        """Display name, e.g. "Outpost 2 standard tilesets (...)"."""
        return _lib.om_profile_name(self._h).decode()

    @property
    def ground_count(self) -> int:
        """Number of grounds (terrain types); valid ground indices are 0..count-1."""
        return _lib.om_profile_ground_count(self._h)

    def ground_id(self, ground: int) -> str:
        """Short id of a ground, e.g. "mud".

        Raises:
            IndexError: ground is out of range.
        """
        v = _lib.om_profile_ground_id(self._h, ground)
        if v is None:
            raise IndexError(ground)
        return v.decode()

    def ground_name(self, ground: int) -> str:
        """Display name of a ground, e.g. "Mud".

        Raises:
            IndexError: ground is out of range.
        """
        v = _lib.om_profile_ground_name(self._h, ground)
        if v is None:
            raise IndexError(ground)
        return v.decode()

    def ground_fill_count(self, ground: int) -> int:
        """Number of plain fill tiles for a ground.

        0 means the ground can't be painted usefully (nothing to fill it
        with); UIs should hide or disable it.
        """
        return _lib.om_profile_ground_fill_count(self._h, ground)

    def find_ground(self, ground_id: str) -> int:
        """Index of the ground with this id (e.g. "mud", "san", "roc"), or -1."""
        return _lib.om_profile_find_ground(self._h, ground_id.encode())

    def grounds_blend(self, a: int, b: int) -> bool:
        """True if the profile has transition tiles between grounds a and b."""
        return bool(_lib.om_profile_grounds_blend(self._h, a, b))

    def cliff_ground(self, ground: int) -> int:
        """Which cliffs BlendMap.draw_cliff uses on this ground.

        Returns:
            1 if the ground has cliff tiles of its own, 0 if cliffs drawn on it
            use the profile's fallback cliffs (Outpost 2: mud), -1 if the
            profile has no usable cliffs at all.
        """
        return _lib.om_profile_cliff_ground(self._h, ground)

    def standard_mapping(self) -> list[tuple[str, int]]:
        """The profile's standard tile mapping table.

        For Outpost 2 this is the table shared by all campaign maps. Useful for
        creating new maps and for tests.

        Returns:
            One (tileset name, image index) pair per mapping index; an empty
            list if the profile has none.
        """
        out = []
        m = _Mapping()   # reused; the tileset string is copied out by decode()
        for i in range(_lib.om_profile_standard_mapping_count(self._h)):
            _lib.om_profile_standard_mapping(self._h, i, ctypes.byref(m))
            out.append((m.tileset.decode(), m.image))
        return out


# --- BlendMap ----------------------------------------------------------------

class BlendMap:
    """Blend state for one map: paint grounds, resolve tiles, draw cliffs.

    The library keeps its own copy of the map's tiles. Typical use: paint()
    some cells, then resolve() and write the returned changes into your map.
    If your editor changes tiles by other means (manual placement, undo/redo),
    report them with set_tile() so the copy stays in sync.

    Each cell's ground is worked out from its tile. Tiles the profile does not
    know (cliffs, walls, buildings, ...) are "fixed": never changed and ignored
    when choosing neighbours' tiles, until a cell is painted over.
    """

    def __init__(self, profile: Profile, width: int, height: int,
                 mappings: Sequence[tuple[str, int]], cells: Sequence[int]):
        """Creates the blend state for one map (om_blend_create).

        Args:
            profile:  the blend profile; kept alive by this object.
            width, height: map size in cells.
            mappings: the map's tile mapping table, as (tileset name, image
                index) per mapping index. Tileset names are matched
                case-insensitively, e.g. "well0001".
            cells:    width*height mapping indices (0-65535), row by row.

        Raises:
            ValueError: cells does not hold width*height entries.
            Op2MapLibError: the library rejected the input; the message
                includes its reason.
        """
        if len(cells) != width * height:
            raise ValueError("cells must hold width*height entries")
        self._profile = profile   # keeps the native profile alive (it must outlive the map)
        self._free = _lib.om_blend_free   # see Profile.close()
        self.width, self.height = width, height
        # The C structs point into these bytes objects; keep them for the call.
        self._names = [str(ts).encode() for ts, _ in mappings]
        arr = (_Mapping * len(mappings))(*[_Mapping(n, img) for n, (_, img) in zip(self._names, mappings)])
        cell_arr = (ctypes.c_uint16 * len(cells))(*cells)
        err = ctypes.create_string_buffer(512)
        self._h = _lib.om_blend_create(profile._h, width, height, arr, len(mappings), cell_arr, err, len(err))
        if not self._h:
            raise Op2MapLibError(f"could not create blend map: {err.value.decode(errors='replace')}")

    def close(self) -> None:
        """Frees the native blend map. Safe to call more than once."""
        # getattr: __init__ may have raised before _h was set.
        if getattr(self, "_h", None):
            self._free(self._h)
            self._h = None

    def __del__(self):
        self.close()

    def ground_at(self, x: int, y: int) -> int:
        """Ground index of a cell, or -1 for a fixed/unknown cell or outside the map."""
        return _lib.om_blend_ground_at(self._h, x, y)

    def tile_at(self, x: int, y: int) -> int:
        """Mapping index the library's copy holds for a cell, or -1 outside the map."""
        return _lib.om_blend_tile_at(self._h, x, y)

    def tile_role(self, x: int, y: int) -> int:
        """Role of the tile a cell holds, as a TileRole value.

        TileRole.UNKNOWN for tiles not in the profile and outside the map.
        """
        return _lib.om_blend_tile_role(self._h, x, y)

    def set_tile(self, x: int, y: int, mapping: int) -> None:
        """Tells the library a cell's tile was changed outside it (om_blend_set_tile).

        Use for manual tile placement, undo/redo, ... The cell's ground is
        re-derived from the new tile. mapping is a mapping index.
        """
        _lib.om_blend_set_tile(self._h, x, y, mapping)

    def paint(self, ground: int, cells: Iterable[tuple[int, int]]) -> int:
        """Paints a ground onto cells (om_blend_paint).

        Only the ground layer changes; call resolve() afterwards to pick tiles.

        Args:
            ground: a ground index (see Profile.find_ground).
            cells:  (x, y) cell coordinates; cells outside the map are skipped.

        Returns:
            The number of cells painted.

        Raises:
            Op2MapLibError: ground is not a valid ground index.
        """
        # The C API takes a flat x0, y0, x1, y1, ... array.
        flat = [v for xy in cells for v in xy]
        arr = (ctypes.c_int32 * len(flat))(*flat)
        n = _lib.om_blend_paint(self._h, ground, arr, len(flat) // 2)
        if n < 0:
            raise Op2MapLibError(f"invalid ground {ground}")
        return n

    def resolve(self, seed: int, region: tuple[int, int, int, int] | None = None) -> list[Change]:
        """Picks tiles for a region and applies them to the library's copy (om_blend_resolve).

        Args:
            seed:   random seed for tile variation; the same seed and input
                give the same tiles. Passed to C as an unsigned 64-bit value.
            region: (x, y, width, height) in cells of the cells to re-evaluate,
                or None for everything painted since the last resolve. One
                extra cell around the region is included, since a tile depends
                on its neighbours.

        Returns:
            The changes to apply to your map (possibly empty).

        Raises:
            Op2MapLibError: the library reported an error.
        """
        # None is passed to C as a NULL pointer ("everything painted").
        rect = ctypes.byref(_Rect(*region)) if region else None
        n = _lib.om_blend_resolve(self._h, rect, ctypes.c_uint64(seed))
        return self._changes(n)

    def draw_cliff(self, points: Sequence[tuple[int, int]], high_side: int,
                   wall_height: float = 3.0, seed: int = 0) -> list[Change]:
        """Draws a cliff along a polyline (om_cliff_draw).

        The line is turned into a band of rock (a wall where the high side is
        north, a thin edge where it is south, steps on steep slopes), then the
        profile's cliff tiles are fitted to it. The cliff family comes from the
        ground at the first point (see Profile.cliff_ground). Cliff cells
        become fixed for terrain blending.

        Args:
            points:      at least 2 (x, y) cell coordinates. Repeat the first
                point at the end to close a loop. snap_cliff_point() helps
                keep segments on allowed slopes.
            high_side:   a CliffSide value.
            wall_height: wall height in tiles, 3 to 6 (clamped by the library).
            seed:        the same seed and input give the same cliff.

        Returns:
            The changes to apply to your map, with the cliff tiles' cell types;
            empty if nothing could be drawn. See last_cliff_stage for how well
            it was solved.

        Raises:
            Op2MapLibError: invalid input (points outside the map, bad side).
        """
        flat = [v for p in points for v in p]
        arr = (ctypes.c_int32 * len(flat))(*flat)
        n = _lib.om_cliff_draw(self._h, arr, len(points), high_side, float(wall_height), ctypes.c_uint64(seed))
        if n < 0:
            raise Op2MapLibError("invalid cliff input")
        return self._changes(n)

    def close_cliff_cut(self, removed, seed: int = 0) -> list[Change]:
        """Closes cut wall ends after ground was painted across a cliff (om_cliff_close_cut).

        Args:
            removed: width*height flags, row by row (a sequence, bytes or a
                numpy array): nonzero = the cell was a cliff before the
                stroke and is ground now.
            seed:    seed for blending the cells that become ground.

        Returns:
            The changes to apply to your map: re-solved cliff tiles first,
            then the blended ground of cliff cells that keep no rock (where a
            cut cannot be closed as it is, the wall end recedes by up to two
            cells). Empty if there was nothing to close or no solution (the
            wall stays as is).

        Raises:
            ValueError:     removed has the wrong size.
            Op2MapLibError: the library reported an error.
        """
        flags = bytes(int(bool(v)) for v in (removed.ravel() if hasattr(removed, "ravel") else removed))
        if len(flags) != self.width * self.height:
            raise ValueError("removed needs width*height entries")
        arr = (ctypes.c_uint8 * len(flags)).from_buffer_copy(flags)
        n = _lib.om_cliff_close_cut(self._h, arr, ctypes.c_uint64(seed))
        if n < 0:
            raise Op2MapLibError("closing the cliff cut failed")
        return self._changes(n)

    @property
    def last_cliff_stage(self) -> int:
        """How the last draw_cliff was solved (om_cliff_last_stage).

        0..7 = solver stage (0 is the strictest: only tile pairs seen in the
        original maps), -1 = greedy fallback (no stage could solve it; expect
        rougher joins), -2 = nothing drawn.
        """
        return _lib.om_cliff_last_stage(self._h)

    def resolve_all(self, seed: int) -> list[Change]:
        """Re-picks every blendable cell of the whole map (om_blend_resolve_all).

        A "clean up transitions" pass. Returns the changes to apply; raises
        Op2MapLibError if the library reports an error. seed: as in resolve().
        """
        return self._changes(_lib.om_blend_resolve_all(self._h, ctypes.c_uint64(seed)))

    def _changes(self, n: int) -> list[Change]:
        """Fetches the n changes of the last resolve/cliff draw as Change objects.

        n is the count the C call returned; negative means it failed.
        """
        if n < 0:
            raise Op2MapLibError("resolve failed")
        if n == 0:
            return []
        buf = (_Change * n)()
        got = _lib.om_blend_get_changes(self._h, buf, n)
        return [Change(c.x, c.y, c.old_mapping, c.new_mapping, c.cell_type) for c in buf[:got]]


# --- Passable regions --------------------------------------------------------

class Regions:
    """Connected regions of passable cells, largest first (om_regions_*).

    The largest region is the main play area; every other region is enclosed.

        regions = Regions.find(width, height, cell_types)   # Outpost 2 rules
        for x, y in regions.enclosed_cells():
            ...

    Works on plain Outpost 2 cell types (CellType values 0-31), so it needs
    no profile or BlendMap. Create one with Regions.find(), not the
    constructor. Region indices are 0..len(regions)-1; region 0 is the largest.
    """

    # om_region_flags; combine with |
    CONNECT8 = 1          # diagonal neighbours connect too (else only N/E/S/W)
    WRAP_X = 2            # the left and right map edges join (Outpost 2 world maps)
    SKIP_BOTTOM_ROW = 4   # ignore the last row (Outpost 2: never shown, always impassable)

    def __init__(self, handle, width: int, height: int):
        """Wraps a native om_regions handle; use Regions.find() instead."""
        self._free = _lib.om_regions_free   # see Profile.close()
        self._h = handle
        self.width, self.height = width, height

    @classmethod
    def find(cls, width: int, height: int, cell_types: Sequence[int],
             passable_mask: int = 0, flags: int | None = None) -> "Regions":
        """Finds the connected regions of passable cells (om_regions_find).

        Args:
            width, height: map size in cells.
            cell_types:    width*height cell types (0-31), row by row.
            passable_mask: bit n set = cell type n is passable; 0 = Outpost 2's
                passable types (see passable_mask()).
            flags:         combination of CONNECT8, WRAP_X, SKIP_BOTTOM_ROW;
                None = Outpost 2's rules for this map size (see op2_flags()).

        Returns:
            The regions, sorted largest first (ties: the one whose first cell
            comes first, row by row).

        Raises:
            ValueError: cell_types does not hold width*height entries.
            Op2MapLibError: the library rejected the input.
        """
        if len(cell_types) != width * height:
            raise ValueError("cell_types must hold width*height entries")
        if flags is None:
            flags = _lib.om_regions_op2_flags(width, height)
        arr = (ctypes.c_uint8 * len(cell_types))(*cell_types)
        h = _lib.om_regions_find(width, height, arr, passable_mask, flags)
        if not h:
            raise Op2MapLibError("invalid region input")
        return cls(h, width, height)

    @staticmethod
    def op2_flags(width: int, height: int) -> int:
        """The flags matching Outpost 2 for a map of this size (in cells).

        8-connected, bottom row skipped, and the left and right edges joined
        only on the 512x256 world map size.
        """
        return _lib.om_regions_op2_flags(width, height)

    @staticmethod
    def passable_mask() -> int:
        """Bit mask of the cell types that count as passable in Outpost 2.

        Bit n = cell type n: fast, medium and slow passable, bulldozed, rubble
        and tubes. Walls, cliffs, vents and the impassable types block.
        """
        return _lib.om_cell_types_passable_mask()

    def close(self) -> None:
        """Frees the native regions object. Safe to call more than once."""
        if getattr(self, "_h", None):
            self._free(self._h)
            self._h = None

    def __del__(self):
        self.close()

    def __len__(self) -> int:
        """Number of regions (0 if the map has no passable cells)."""
        return _lib.om_regions_count(self._h)

    def size(self, region: int) -> int:
        """Number of cells in a region, or 0 if region is out of range."""
        return _lib.om_regions_size(self._h, region)

    def cells(self, region: int) -> list[tuple[int, int]]:
        """The (x, y) cells of a region; empty if region is out of range."""
        n = self.size(region)
        # At least one pair so ctypes never builds a zero-length array.
        buf = (ctypes.c_int32 * (2 * max(1, n)))()
        got = _lib.om_regions_cells(self._h, region, buf, n)
        return [(buf[2 * i], buf[2 * i + 1]) for i in range(got)]

    def region_at(self, x: int, y: int) -> int:
        """Region index of a cell, or -1 if it is not passable, skipped or outside the map."""
        return _lib.om_regions_at(self._h, x, y)

    def enclosed_size(self) -> int:
        """Total number of cells in all enclosed regions (every region but the largest)."""
        return _lib.om_regions_enclosed_size(self._h)

    def enclosed_cells(self) -> list[tuple[int, int]]:
        """The (x, y) cells of all enclosed regions (every region but the largest).

        These are the cells units can never reach from the main play area.
        """
        n = self.enclosed_size()
        buf = (ctypes.c_int32 * (2 * max(1, n)))()
        got = _lib.om_regions_enclosed_cells(self._h, buf, n)
        return [(buf[2 * i], buf[2 * i + 1]) for i in range(got)]


# --- Map generator -------------------------------------------------------------

class Symmetry:
    """Symmetry modes of the map generator (om_mapgen_symmetry)."""
    NONE = 0
    MIRROR_X = 1    # left half mirrored onto the right (2 players)
    MIRROR_Y = 2    # top half mirrored onto the bottom (2 players)
    ROT180 = 3      # point-symmetric (2 players)
    QUAD = 4        # four-fold mirrored (4 players)


class FeatureKind:
    """Terrain feature kinds (om_mapgen_feature)."""
    VALLEY = 0
    MOUNTAIN = 1
    PLATEAU = 2
    CLEARING = 3    # kept free of cliffs, volcanoes and scatter
    CHOKE = 4       # a barrier across the map with one gap


# Keys of om_mapgen_stat. The mountain keys are 0 (counts) or -1 (shares of a
# zone the map does not have) without mountains.
_MAPGEN_STATS = ("passable", "plateau", "details", "rivers", "connected",
                 "mountains", "mountains_leaky", "plateaus", "plateaus_unreachable",
                 "build", "build_mountain", "build_plateau", "build_valley", "build_gorge",
                 "gorges", "gorges_passable")


class MapGenerator:
    """Settings for generating a map (om_mapgen_settings_*).

    The map generator is a C++ port of fefi-byte's generator from
    op2-codegen-editor (MIT, see THIRD-PARTY-NOTICES.md).

        gen = MapGenerator("islands", width=128, height=128, seed=1234, spawns=2)
        gen.add_feature(FeatureKind.MOUNTAIN, where="c", size=0.6)
        result = gen.generate(profile)

    Templates: plains, canyon, desert, islands, ice, newterra, learned
    (unknown = plains; "learned" holds suggestions for generate_learned()).
    Keyword arguments are numeric settings (see set()), plus seed.
    """

    def __init__(self, template: str = "plains", **settings):
        self._h = _lib.om_mapgen_settings_create(template.encode())
        if not self._h:
            raise Op2MapLibError("could not create generator settings")
        for key, value in settings.items():
            self.set(key, value)

    def close(self) -> None:
        """Frees the native settings. Safe to call more than once."""
        if getattr(self, "_h", None):
            _lib.om_mapgen_settings_free(self._h)
            self._h = None

    def __del__(self):
        self.close()

    def set(self, key: str, value) -> None:
        """Sets a numeric setting (om_mapgen_set) or the seed.

        Keys: width, height, seed, roughness, symmetry (Symmetry), spawns,
        spawn_clearance, cliffs, ramps, volcanoes, lava_zone_tiles, smooth,
        vegetation_spacing, min_biome, details, landscape, rivers; mountains,
        mountain_density, gorges, valleys, plateaus, mountain_spread,
        world_cutout, learned_rocks; learned generator only: biome_calm,
        coherence, calm_terrain, lava_rock, volcanoes_small, volcanoes_medium,
        volcanoes_large, craters, cracks, debris, gradient,
        gradient_ground_start, gradient_ground_end, gradient_terrain_start,
        gradient_terrain_end (see op2maplib.h, om_mapgen_set).

        Raises:
            KeyError: unknown key.
        """
        if key == "seed":
            _lib.om_mapgen_set_seed(self._h, ctypes.c_uint64(int(value)))
            return
        if _lib.om_mapgen_set(self._h, key.encode(), float(value)) != 0:
            raise KeyError(key)

    def get(self, key: str) -> float:
        """Current value of a numeric setting (0 for an unknown key)."""
        return _lib.om_mapgen_get(self._h, key.encode())

    def set_mix(self, mix: dict[str, float]) -> None:
        """Replaces the terrain mix: ground id -> share, e.g. {"mud": 0.5, "roc": 0.5}."""
        _lib.om_mapgen_clear_mix(self._h)
        for ground_id, share in mix.items():
            if _lib.om_mapgen_set_mix(self._h, ground_id.encode(), float(share)) != 0:
                raise ValueError(f"bad mix entry {ground_id!r}: {share}")

    def add_feature(self, kind: int, where: str = "", to: str = "", size: float = 0.5,
                    lava: bool = False) -> None:
        """Adds a terrain feature (FeatureKind). `where`/`to` are sectors
        "nw","n","no","w","c","o","sw","s","so" ("" = the seed decides)."""
        if _lib.om_mapgen_add_feature(self._h, int(kind), where.encode(), to.encode(),
                                      float(size), int(bool(lava))) != 0:
            raise ValueError(f"bad feature kind {kind}")

    def clear_features(self) -> None:
        _lib.om_mapgen_clear_features(self._h)

    def generate(self, profile: Profile) -> "GeneratedMap":
        """Generates the map (om_mapgen_generate).

        Raises:
            Op2MapLibError: the library rejected the settings; the message says why.
        """
        err = ctypes.create_string_buffer(512)
        h = _lib.om_mapgen_generate(profile._h, self._h, err, len(err))
        if not h:
            raise Op2MapLibError(f"could not generate map: {err.value.decode(errors='replace')}")
        try:
            return GeneratedMap._from_handle(h, profile.ground_count)
        finally:
            _lib.om_mapgen_result_free(h)

    def generate_learned(self, profile: Profile, model: "LearnedModel", quality: int = 1, threads: int = 0,
                         progress=None) -> "GeneratedMap":
        """Generates the map with the learned generator (om_mapgen_generate_learned).

        quality: LearnedQuality.SKETCH (only the sketch, no tiles), FAST or GOOD.
        threads: 0 = automatic; the map does not depend on it. progress: called
        with the painted fraction 0..1; return False to cancel. Width and height
        must be multiples of 8.

        Raises:
            Op2MapLibError: bad settings, or cancelled; the message says why.
        """
        err = ctypes.create_string_buffer(512)
        failure = []

        def report(fraction, _user):
            if progress is None:
                return 0
            try:
                return 1 if progress(fraction) is False else 0
            except BaseException as e:   # stop the native call, raise afterwards
                failure.append(e)
                return 1
        cb = _PROGRESS(report)
        h = _lib.om_mapgen_generate_learned(profile._h, model._h, self._h, int(quality), int(threads), cb,
                                            None, err, len(err))
        if failure:
            if h:
                _lib.om_mapgen_result_free(h)
            raise failure[0]
        if not h:
            raise Op2MapLibError(f"could not generate map: {err.value.decode(errors='replace')}")
        try:
            return GeneratedMap._from_handle(h, profile.ground_count)
        finally:
            _lib.om_mapgen_result_free(h)


class LearnedQuality:
    """Quality levels of the learned generator (om_learned_quality)."""
    SKETCH = 0   # only the sketch, about a second
    FAST = 1     # 6 painting rounds
    GOOD = 2     # 24 painting rounds, about 1.5x slower


class LearnedModel:
    """The learned generator's networks (om_mapgen_model): profiles/op2-net.bin.

        model = LearnedModel.from_file("profiles/op2-net.bin")
        result = MapGenerator("learned", width=128, height=128, seed=7).generate_learned(profile, model)
    """

    def __init__(self, data: bytes):
        err = ctypes.create_string_buffer(512)
        self._h = _lib.om_mapgen_model_load(data, len(data), err, len(err))
        if not self._h:
            raise Op2MapLibError(f"could not load network file: {err.value.decode(errors='replace')}")

    @classmethod
    def from_file(cls, path) -> "LearnedModel":
        return cls(Path(path).read_bytes())

    def close(self) -> None:
        """Frees the native model. Safe to call more than once."""
        if getattr(self, "_h", None):
            _lib.om_mapgen_model_free(self._h)
            self._h = None

    def __del__(self):
        self.close()


@dataclass
class GeneratedMap:
    """A generated map, copied out of the native result.

    tiles are mapping indices into the profile's standard mapping table
    (Profile.standard_mapping()), which a new map should use as its own;
    cell_types are Outpost 2 cell types. Both are row by row.
    """
    width: int
    height: int
    tiles: list[int]
    cell_types: list[int]
    spawns: list[tuple[int, int]]
    volcanoes: list[tuple[int, int]]
    lava_zones: list[list[tuple[int, int]]]
    stats: dict[str, float]
    ground_shares: list[float]                 # per profile ground
    features: list[tuple[int, int, int, int]]   # (kind, x, y, size)
    feature_report: list[int]                   # per FeatureKind
    # Learned generator only: its sketch at half resolution, row by row (walls
    # 0 none / 1..8 fall direction E..NE; grounds 0 mud, 1 rock, 2 sand, 3 vegetation, 4 other).
    sketch_width: int = 0
    sketch_height: int = 0
    sketch_walls: list[int] | None = None
    sketch_grounds: list[int] | None = None

    @classmethod
    def _from_handle(cls, h, ground_count: int) -> "GeneratedMap":
        w, ht = _lib.om_mapgen_width(h), _lib.om_mapgen_height(h)
        n = w * ht
        tiles = (ctypes.c_uint16 * n)()
        cells = (ctypes.c_uint8 * n)()
        _lib.om_mapgen_get_tiles(h, tiles, n)
        _lib.om_mapgen_get_cell_types(h, cells, n)

        def pts(count, fetch):
            buf = (ctypes.c_int32 * (2 * max(1, count)))()
            got = fetch(buf, count)
            return [(buf[2 * i], buf[2 * i + 1]) for i in range(got)]

        lists = [pts(_lib.om_mapgen_point_count(h, k), lambda b, c, k=k: _lib.om_mapgen_get_points(h, k, b, c))
                 for k in (0, 1)]
        zones = [pts(_lib.om_mapgen_lava_zone_size(h, z), lambda b, c, z=z: _lib.om_mapgen_lava_zone_cells(h, z, b, c))
                 for z in range(_lib.om_mapgen_lava_zone_count(h))]
        stats = {k: _lib.om_mapgen_stat(h, k.encode())
                 for k in _MAPGEN_STATS}
        shares = [_lib.om_mapgen_ground_share(h, g) for g in range(ground_count)]
        feats = []
        for i in range(_lib.om_mapgen_feature_count(h)):
            v = [ctypes.c_int32() for _ in range(4)]
            _lib.om_mapgen_get_feature(h, i, *[ctypes.byref(x) for x in v])
            feats.append(tuple(x.value for x in v))
        sw, sh = ctypes.c_int32(), ctypes.c_int32()
        sn = _lib.om_mapgen_sketch_size(h, ctypes.byref(sw), ctypes.byref(sh))
        walls = grounds = None
        if sn > 0:
            wb, gb = (ctypes.c_uint8 * sn)(), (ctypes.c_uint8 * sn)()
            _lib.om_mapgen_get_sketch(h, wb, gb, sn)
            walls, grounds = list(wb), list(gb)
        return cls(w, ht, list(tiles), list(cells), lists[0], lists[1], zones, stats,
                   shares, feats, [_lib.om_mapgen_feature_report(h, k) for k in range(5)],
                   sw.value, sh.value, walls, grounds)

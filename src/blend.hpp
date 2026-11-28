// Terrain blending - internal C++ types behind the om_profile / om_blend_* API.
//
// Profile is the loaded tile profile (profile.cpp): grounds, the tiles that
// belong to them and the transition tiles between pairs of grounds, indexed for
// fast lookup. BlendMap is the per-map state (blend.cpp): the caller's mapping
// table, each cell's tile and ground, and the paint/resolve logic. api.cpp
// wraps both in the opaque C handles.
#pragma once

#include "op2maplib/op2maplib.h"
#include "cliff.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace om::json { class Value; }

namespace om {

/// Corner key bits. A set bit means that corner belongs to the pair's second
/// ground ("b"). Written as a string in the profile in the order NW NE SE SW.
enum CornerBit : uint8_t { kNW = 8, kNE = 4, kSE = 2, kSW = 1 };

/// How the library uses a profile tile. Mirrors om_tile_role (minus Unknown).
enum class TileRole : uint8_t {
    Fill,        ///< plain ground, used to fill painted areas ("plain" in JSON)
    Ground,      ///< belongs to a ground but is never placed by the library (decoration, rare tiles)
    Transition,  ///< blends two grounds
};

/// One tile listed in the profile, identified by (tileset, image).
struct ProfileTile {
    int tileset = 0;          ///< index into Profile::tilesets
    int image = 0;
    TileRole role = TileRole::Fill;
    int ground = -1;          ///< Fill/Ground: its ground. Transition: ground a.
    int groundB = -1;         ///< Transition only: ground b.
    uint8_t corners = 0;      ///< Transition only: CornerBit mask.
    uint32_t weight = 1;      ///< relative chance when picking among candidates
    int cellType = -1;        ///< suggested cell type, -1 if unknown
};

/// One tile of a generator stamp: (tileset, image) plus its cell type.
struct StampTile {
    int tileset = 0;
    int image = 0;
    int cellType = -1;        ///< -1 if unknown
};

/// A ready-made block of tiles the map generator places in one piece
/// (scatter detail or volcano).
struct Stamp {
    std::string name;         ///< volcanoes and decorations, e.g. "rocvolc1", "mudcrk3"
    std::string kind;         ///< decorations only: "crater", "crack" or "debris"
    int ground = -1;          ///< details: the ground it belongs on, -1 = any
    int w = 0, h = 0;
    std::vector<StampTile> tiles;   ///< w * h, row by row
};

/// The profile's optional "generator" section: stamps and the tile pairs seen
/// side by side in the original maps (for checking a stamp's seam).
struct GeneratorData {
    std::vector<Stamp> details;
    std::vector<Stamp> volcanoes;
    std::vector<Stamp> decorations;   ///< named tile groups for the learned generator's craters, cracks, debris

    /// Packs (tileset, image) as the profile does: tileset * 4096 + image.
    static uint32_t pack(int tileset, int image) { return static_cast<uint32_t>(tileset) * 4096u + static_cast<uint32_t>(image); }
    /// True if `b` was seen right of `a` (horizontal) or below `a` (vertical).
    bool seen(bool horizontal, uint32_t a, uint32_t b) const;
    /// True if `a` has any recorded neighbour in that direction; tiles without
    /// any are not checked (Python: nachbarn() returning None).
    bool known(bool horizontal, uint32_t a) const;

    // pair key: a << 32 | b; known: a
    std::unordered_map<uint64_t, uint8_t> right, down;
    std::unordered_map<uint32_t, uint8_t> rightKnown, downKnown;
};

/// A terrain type such as mud or rock.
struct Ground {
    std::string id;
    std::string name;
    std::vector<int> fill;    ///< profile tile indices with role Fill
};

/// A loaded blend profile. Immutable after fromJson, so one profile can be
/// shared by any number of BlendMaps (which keep a reference to it).
class Profile {
public:
    /// Throws std::runtime_error with a readable message on bad input.
    static Profile fromJson(const char* text, size_t len);

    std::string name;                    ///< display name, may be empty
    std::vector<std::string> tilesets;   ///< lower-case file names
    std::vector<Ground> grounds;
    /// All profile tiles; "profile tile index" elsewhere means an index into this.
    std::vector<ProfileTile> tiles;
    /// Standard mapping table: (tileset index, image) per mapping index. May be empty.
    std::vector<std::pair<int, int>> standardMapping;
    /// Cliff tiles and their rules (empty if the profile has no "cliffs" section).
    CliffData cliffs;
    /// Map generator stamps and seam pairs (empty if no "generator" section).
    GeneratorData generator;

    /// Index of the ground with this id (case-sensitive), or -1.
    int findGround(const std::string& id) const;
    /// Index of a tileset by its lower-case name, or -1.
    int findTileset(const std::string& lowerName) const;
    /// Profile tile index for (tileset, image), or -1.
    int findTile(int tileset, int image) const;

    /// Transition tiles placed on a cell of ground `a` that borders ground `b`,
    /// indexed by corner key (0..15). Empty if the pair does not blend.
    const std::array<std::vector<int>, 16>& transitions(int a, int b) const;
    bool hasTransitions(int a, int b) const;
    /// True if the grounds blend in either direction.
    bool blends(int x, int y) const { return hasTransitions(x, y) || hasTransitions(y, x); }

private:
    /// Reads the optional "generator" section into `generator` (profile.cpp).
    static void parseGenerator(Profile& p, const json::Value& g);

    std::unordered_map<uint64_t, int> tileIndex_;            // (tileset << 32 | image) -> tile index
    std::vector<std::array<std::vector<int>, 16>> byPair_;   // [a * groundCount + b]
    std::array<std::vector<int>, 16> empty_{};               // returned for out-of-range pairs
};

/// Blend state of one map. Holds a copy of the cells' mapping indices and a
/// ground per cell; the caller's map is only changed by applying the change
/// lists that resolve() and drawCliff() return. The profile must outlive it.
class BlendMap {
public:
    /// mappings: the map's tile mapping table; cells: width*height mapping
    /// indices, row by row (copied).
    BlendMap(const Profile& profile, int width, int height,
             const om_mapping* mappings, int mappingCount, const uint16_t* cells);

    int width() const { return width_; }
    int height() const { return height_; }
    /// Ground of a cell, -1 if fixed or outside the map.
    int groundAt(int x, int y) const;
    /// Mapping index held by a cell, -1 if outside the map.
    int tileAt(int x, int y) const;
    /// om_tile_role of the cell's tile.
    int roleAt(int x, int y) const;
    /// Records a tile changed by the caller and re-derives the cell's ground.
    void setTile(int x, int y, int mapping);
    /// Sets the ground of count (x, y) cells and grows the dirty area. Returns
    /// the cells painted (outside ones are skipped), or -1 for a bad ground.
    int paint(int ground, const int32_t* xy, int count);
    /// Picks tiles for region plus a one-cell border and applies them to this
    /// copy. region == nullptr: the dirty area since the last resolve. Returns
    /// the number of changes (see changes()).
    int resolve(const om_rect* region, uint64_t seed);
    /// Changes made by the last resolve() or drawCliff().
    const std::vector<om_change>& changes() const { return changes_; }

    /// Draws a cliff along a polyline (cell coordinates). Returns the number of
    /// changes (see changes()), or -1 for invalid input.
    int drawCliff(const int32_t* xy, int count, int highSide, double wallHeight, uint64_t seed);
    /// Closes the cut ends of walls after the caller painted ground across
    /// them. removed: width*height flags, nonzero = the cell was cliff and is
    /// ground now. Returns the number of changes (see changes()), 0 if there
    /// was nothing to close, or -1 for invalid input.
    int closeCliffCut(const uint8_t* removed, uint64_t seed);
    /// Solver stage of the last drawCliff or closeCliffCut: 0..7, -1 = greedy
    /// fallback, -2 = nothing drawn.
    int lastCliffStage() const { return lastCliffStage_; }

private:
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    int idx(int x, int y) const { return y * width_ + x; }
    /// Ground of a cell for neighbour checks: -1 outside the map or fixed.
    int groundOrNone(int x, int y) const { return inside(x, y) ? ground_[idx(x, y)] : -1; }
    /// Ground of a mapping index's tile, -1 if the profile does not know it.
    int groundOfMapping(int mapping) const;
    /// The tile a cell should hold, or -1 to leave it as it is.
    int chooseMapping(int x, int y, uint64_t seed) const;
    /// Weighted random choice among the candidates this map can use (see
    /// usable), seeded per cell. Returns a profile tile index, or -1 if none.
    int pick(const std::vector<int>& candidates, uint64_t seed, int x, int y) const;
    /// True if the map's mapping table contains this profile tile.
    bool usable(int profileTile) const { return mappingOfTile_[profileTile] >= 0; }

    const Profile& profile_;
    int width_, height_;
    std::vector<int> tileOfMapping_;    ///< map mapping index -> profile tile (-1 unknown)
    std::vector<int> mappingOfTile_;    ///< profile tile -> map mapping index (-1 not in this map)
    std::vector<int> mappingOfCliff_;   ///< cliff tile -> map mapping index (-1 not in this map)
    std::vector<int> cliffOfMapping_;   ///< map mapping index -> cliff tile (-1 not a cliff tile)
    int lastCliffStage_ = -2;
    std::vector<int32_t> mapping_;      ///< per cell
    std::vector<int8_t> ground_;        ///< per cell, -1 = fixed
    /// Per cell: painted since the last resolve. A painted cell that needs plain
    /// ground gets a fill tile even if it holds decoration of that ground;
    /// unpainted cells keep their decoration.
    std::vector<uint8_t> painted_;
    bool dirty_ = false;                // something was painted since the last resolve
    int dirtyX0_ = 0, dirtyY0_ = 0, dirtyX1_ = -1, dirtyY1_ = -1;   // inclusive bounding box of painted cells
    std::vector<om_change> changes_;
};

/// Deterministic per-cell random number (splitmix64 of seed, x, y). Fully
/// specified so every binding and platform picks the same tiles.
uint64_t cellRandom(uint64_t seed, int x, int y);

}  // namespace om

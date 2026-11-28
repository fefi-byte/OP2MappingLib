// Cliff drawing - data loaded from the profile's "cliffs" section, and the
// entry point used by BlendMap::drawCliff. The algorithm lives in cliff.cpp.
//
// Ported from op2-codegen-editor's cliff tool (MIT licence, by fefi-byte;
// mapview/klippe_gelaende.py and klippe_algo.py). Terms used in the code:
//   fine grid  - each tile split into 4x4 sub-cells; true = rock
//   edge codes - per tile side, 4 segments of BIOM (ground), FELS (rock) or
//                EGAL (either); sides in the order N, E, S, W; N/S segments
//                read left to right, E/W top to bottom
//   inner      - the tile's own 4x4 rock image
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace om {

namespace json { class Value; }

/// Side indices used throughout the cliff code.
enum Side : int { kN = 0, kE = 1, kS = 2, kW = 3 };

/// Edge segment values: ground, rock, or either (matches both).
enum EdgeCode : int8_t { kBiom = 0, kFels = 1, kEgal = 2 };

/// One cliff tile from the profile: which image it is, how its edges look on
/// the fine grid, and what was found next to it in the original maps.
struct CliffTile {
    int tileset = 0;                        ///< index into the profile's tilesets
    int image = 0;                          ///< tile image within that tileset
    int family = -1;                        ///< index into CliffData::families
    std::array<std::array<int8_t, 4>, 4> edges{};   ///< [side][segment]
    double rockShare = 0;                   ///< share of FELS among the 16 edge codes
    std::array<std::array<bool, 4>, 4> inner{};     ///< [row][col], true = rock
    bool hasInner = false;                  ///< false = no inner image in the profile
    int cellType = -1;                      ///< suggested cell type, -1 = unknown
    std::array<std::vector<int>, 4> seen;   ///< cliff tile indices seen beside it, per side
    std::array<std::vector<uint8_t>, 4> rims;       ///< 32 RGB pixels per side (empty = unknown)
};

/// The cliff tiles for one ground. Walls are always built from one family.
struct CliffFamily {
    std::string id;                         ///< ground id, e.g. "mud"
    std::vector<int> tiles;                 ///< cliff tile indices
};

struct CliffTable;   // per-family lookup tables, built on first use (cliff.cpp)

/// All cliff data of a profile. Owned by the profile; read-only after parse()
/// apart from the lazily built lookup tables.
struct CliffData {
    std::vector<CliffTile> tiles;           ///< in profile order; "seen" lists index into this
    std::vector<CliffFamily> families;
    int minTiles = 40;                      ///< a family needs this many tiles to be usable
    std::string fallback = "mud";           ///< family used for grounds without a usable one

    bool empty() const { return tiles.empty(); }
    /// Family index for a ground id, -1 if none.
    int findFamily(const std::string& id) const;
    /// True if this family has enough tiles to build walls.
    bool usable(int family) const;
    /// Lookup tables for a family, built once.
    const CliffTable& table(int family) const;

    /// Reads the profile's "cliffs" section; throws std::runtime_error on bad
    /// data. tilesetCount bounds the tileset indices.
    void parse(const json::Value& section, int tilesetCount);

    CliffData() = default;
    CliffData(CliffData&&) noexcept = default;
    CliffData& operator=(CliffData&&) noexcept = default;
    ~CliffData();

private:
    // Per family, built on first use by table(); shared_ptr keeps CliffTable
    // an incomplete type here.
    mutable std::vector<std::shared_ptr<CliffTable>> tables_;
};

/// The high side of a cliff line (matches om_cliff_side in the C API).
enum class CliffHighSide { Left = 0, Right = 1, North = 2, South = 3 };

/// Result of solving a cliff in a local area: tile per cell (-1 = none).
struct CliffResult {
    int width = 0, height = 0;
    std::vector<int> tile;          ///< cliff tile index per local cell, -1 = unchanged
    int stage = -1;                 ///< solver stage that succeeded, -1 = greedy fallback
    int searchSteps = 0;            ///< solver steps used, summed over the stages tried
};

/// Builds a cliff along a polyline (cell coordinates, relative to a local area
/// of width x height) for one family. wallHeight 3..6 tiles (clamped).
/// A polyline whose first and last points are equal is a closed loop.
/// Deterministic: the same inputs and seed always give the same result.
CliffResult solveCliffLine(const CliffData& data, int family,
                           const std::vector<std::pair<int, int>>& points,
                           CliffHighSide highSide, double wallHeight,
                           int width, int height, uint64_t seed);

/// Result of closing a cut in a wall (solveCliffCut), per local cell.
struct CliffCutResult {
    int width = 0, height = 0;
    std::vector<int> tile;          ///< new cliff tile index, -1 = unchanged
    std::vector<uint8_t> ground;    ///< 1 = was a cliff cell, keeps no rock: becomes plain ground
    int stage = -2;                 ///< solver stage that succeeded, -2 = nothing solved
};

/// Lets a wall end cleanly where cells were cut out of it (painting ground
/// across a cliff, e.g. for a ramp). cliffTiles: per local cell the cliff
/// tile standing there (-1 = none; tiles of other families are ignored).
/// removed: cells that were cliff and are ground now. The wall's rock band is
/// recovered from the tiles' inner images and opened towards the cut; only
/// the cliff cells within `zone` cells of the cut are solved again, and the
/// rest of the wall stays, constraining them as fixed neighbours. If no
/// solver stage succeeds, the wall end recedes: the cut grows by one cell into
/// the wall (at most twice), and those cells become ground too. If even that
/// fails, nothing changes (stage -2). Deterministic.
CliffCutResult solveCliffCut(const CliffData& data, int family, const std::vector<int>& cliffTiles,
                             const std::vector<uint8_t>& removed, int width, int height, int zone = 2);

/// Snaps (bx, by) so the segment from (ax, ay) uses an allowed cliff slope:
/// flat, vertical, 45 degrees, or 2:1, 3:1, 4:1 either way.
void snapCliffPoint(int ax, int ay, int bx, int by, int& outX, int& outY);

}  // namespace om

// Terrain blending engine.
//
// The model: every cell has a ground (mud, rock, ...) or is "fixed" (a tile the
// profile doesn't know, such as a cliff or wall). Painting changes grounds;
// resolving picks each cell's tile from the grounds around it.
//
// Corner rule (after op2-codegen-editor's _uebergaenge_aus_ecken): a cell of
// ground A that touches ground B gets a transition tile of the pair (A, B).
// Each of its four corners belongs to B if any cell meeting at that corner is
// B. Neighbouring cells share the two corners of their common edge, so their
// tiles agree along it without any further matching. Transition tiles always
// sit on the A side of a border (A is the pair's first ground in the profile);
// B cells stay plain.
//
// A cell's tile depends only on the ground grid, never on the tiles already
// chosen around it, so cells can be resolved in any order and the result is
// reproducible. A cell that already holds a fitting tile keeps it, so
// re-resolving an area doesn't reshuffle it.
//
// Cliff drawing also lives on BlendMap (drawCliff, at the end): it picks the
// cliff family, cuts out the work area and hands it to the solver in cliff.cpp,
// then writes the result back as changes like resolve does.
#include "blend.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace om {

namespace {

// Lower-case ASCII copy; NULL gives "". Tileset names are matched case-insensitively.
std::string lower(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Number of set bits in a 4-bit corner key.
int popcount4(int v) {
    return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1);
}

}  // namespace

// Part of the cross-binding contract: changing any constant here changes
// every blended map and the golden test hashes.
uint64_t cellRandom(uint64_t seed, int x, int y) {
    // splitmix64 over the seed mixed with the coordinates.
    uint64_t z = seed ^ (static_cast<uint64_t>(static_cast<uint32_t>(x)) * 0x9E3779B97F4A7C15ULL)
                      ^ (static_cast<uint64_t>(static_cast<uint32_t>(y)) * 0xC2B2AE3D27D4EB4FULL);
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

BlendMap::BlendMap(const Profile& profile, int width, int height,
                   const om_mapping* mappings, int mappingCount, const uint16_t* cells)
    : profile_(profile), width_(width), height_(height) {
    // Map mapping index <-> profile tile, matched by (tileset name, image).
    tileOfMapping_.assign(static_cast<size_t>(mappingCount), -1);
    mappingOfTile_.assign(profile.tiles.size(), -1);
    // Cliff tiles are not profile tiles (they are never blended), so they are
    // matched separately below; this table remembers the first mapping index
    // for every (tileset, image) the map uses, known to the profile or not.
    std::unordered_map<uint64_t, int> firstMapping;    // (tileset, image) -> mapping index
    for (int m = 0; m < mappingCount; ++m) {
        const int ts = profile.findTileset(lower(mappings[m].tileset));
        // Tilesets the profile does not list stay unknown: their cells are fixed.
        if (ts < 0) continue;
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(ts)) << 32) | static_cast<uint32_t>(mappings[m].image);
        // emplace keeps the existing entry, so the first mapping wins here too.
        firstMapping.emplace(key, m);
        const int tile = profile.findTile(ts, mappings[m].image);
        if (tile < 0) continue;
        tileOfMapping_[m] = tile;
        if (mappingOfTile_[tile] < 0) mappingOfTile_[tile] = m;   // first mapping wins
    }
    mappingOfCliff_.assign(profile.cliffs.tiles.size(), -1);
    for (size_t i = 0; i < profile.cliffs.tiles.size(); ++i) {
        const CliffTile& c = profile.cliffs.tiles[i];
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(c.tileset)) << 32) | static_cast<uint32_t>(c.image);
        auto it = firstMapping.find(key);
        if (it != firstMapping.end()) mappingOfCliff_[i] = it->second;
    }
    // And back: which cliff tile a mapping index shows (the first cliff tile
    // with that image wins).
    cliffOfMapping_.assign(static_cast<size_t>(mappingCount), -1);
    {
        std::unordered_map<uint64_t, int> cliffOfKey;
        for (size_t i = 0; i < profile.cliffs.tiles.size(); ++i) {
            const CliffTile& c = profile.cliffs.tiles[i];
            cliffOfKey.emplace((static_cast<uint64_t>(static_cast<uint32_t>(c.tileset)) << 32) | static_cast<uint32_t>(c.image),
                               static_cast<int>(i));
        }
        for (int m = 0; m < mappingCount; ++m) {
            const int ts = profile.findTileset(lower(mappings[m].tileset));
            if (ts < 0) continue;
            auto it = cliffOfKey.find((static_cast<uint64_t>(static_cast<uint32_t>(ts)) << 32) | static_cast<uint32_t>(mappings[m].image));
            if (it != cliffOfKey.end()) cliffOfMapping_[m] = it->second;
        }
    }

    // Copy the cells and derive each cell's ground from its tile. Mapping
    // indices beyond the table give ground -1 (fixed), like unknown tiles.
    const size_t n = static_cast<size_t>(width) * height;
    mapping_.resize(n);
    ground_.resize(n);
    painted_.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        mapping_[i] = cells[i];
        ground_[i] = static_cast<int8_t>(groundOfMapping(cells[i]));
    }
}

int BlendMap::groundOfMapping(int mapping) const {
    if (mapping < 0 || mapping >= static_cast<int>(tileOfMapping_.size())) return -1;
    const int tile = tileOfMapping_[mapping];
    // Transition tiles sit on the A side of their border, so their cell is ground A.
    return tile < 0 ? -1 : profile_.tiles[tile].ground;
}

// Accessors; all return -1 / OM_TILE_UNKNOWN for cells outside the map.
int BlendMap::groundAt(int x, int y) const { return inside(x, y) ? ground_[idx(x, y)] : -1; }

int BlendMap::tileAt(int x, int y) const { return inside(x, y) ? mapping_[idx(x, y)] : -1; }

int BlendMap::roleAt(int x, int y) const {
    if (!inside(x, y)) return OM_TILE_UNKNOWN;
    const int m = mapping_[idx(x, y)];
    if (m < 0 || m >= static_cast<int>(tileOfMapping_.size()) || tileOfMapping_[m] < 0) return OM_TILE_UNKNOWN;
    switch (profile_.tiles[tileOfMapping_[m]].role) {
    case TileRole::Fill: return OM_TILE_FILL;
    case TileRole::Ground: return OM_TILE_GROUND;
    case TileRole::Transition: return OM_TILE_TRANSITION;
    }
    return OM_TILE_UNKNOWN;
}

// The caller changed a tile itself. Only the tile and ground are updated: the
// cell is not marked painted or dirty, and neighbours are not re-resolved.
void BlendMap::setTile(int x, int y, int mapping) {
    if (!inside(x, y)) return;
    mapping_[idx(x, y)] = mapping;
    ground_[idx(x, y)] = static_cast<int8_t>(groundOfMapping(mapping));
}

// Only the ground layer changes here; tiles are picked by resolve(). Painting
// over a fixed cell turns it into a normal cell of that ground.
int BlendMap::paint(int ground, const int32_t* xy, int count) {
    if (ground < 0 || ground >= static_cast<int>(profile_.grounds.size())) return -1;
    int painted = 0;
    for (int i = 0; i < count; ++i) {
        const int x = xy[2 * i], y = xy[2 * i + 1];
        if (!inside(x, y)) continue;
        ground_[idx(x, y)] = static_cast<int8_t>(ground);
        painted_[idx(x, y)] = 1;
        ++painted;
        // Grow the dirty box that resolve(nullptr) will work on.
        if (!dirty_) {
            dirty_ = true;
            dirtyX0_ = dirtyX1_ = x;
            dirtyY0_ = dirtyY1_ = y;
        } else {
            dirtyX0_ = std::min(dirtyX0_, x); dirtyX1_ = std::max(dirtyX1_, x);
            dirtyY0_ = std::min(dirtyY0_, y); dirtyY1_ = std::max(dirtyY1_, y);
        }
    }
    return painted;
}

// The random number depends only on (seed, x, y), not on the candidates, so
// the choice is reproducible and independent of resolve order. Candidates
// the map's mapping table lacks are skipped, as if absent from the profile.
int BlendMap::pick(const std::vector<int>& candidates, uint64_t seed, int x, int y) const {
    uint64_t total = 0;
    for (int t : candidates)
        if (usable(t)) total += profile_.tiles[t].weight;
    if (total == 0) return -1;
    uint64_t r = cellRandom(seed, x, y) % total;
    for (int t : candidates) {
        if (!usable(t)) continue;
        const uint64_t w = profile_.tiles[t].weight;
        if (r < w) return t;
        r -= w;
    }
    return -1;   // not reached
}

// The heart of the blender: works out the tile for one cell from the grounds
// of the cell and its 8 neighbours. Returns a mapping index, or -1 to keep
// the current tile (fixed cell, current tile already fits, or nothing usable).
int BlendMap::chooseMapping(int x, int y, uint64_t seed) const {
    const int g = ground_[idx(x, y)];
    if (g < 0) return -1;   // fixed cell
    const int currentMapping = mapping_[idx(x, y)];
    const int current = (currentMapping >= 0 && currentMapping < static_cast<int>(tileOfMapping_.size()))
                            ? tileOfMapping_[currentMapping] : -1;   // profile tile, -1 unknown

    // Which foreign ground does this cell blend into? Among the 8 neighbours,
    // the most common ground B for which (g, B) has transition tiles.
    static const int dx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int partner = -1, best = 0;
    std::vector<int> count(profile_.grounds.size(), 0);
    for (int i = 0; i < 8; ++i) {
        const int k = groundOrNone(x + dx8[i], y + dy8[i]);
        if (k < 0 || k == g || !profile_.hasTransitions(g, k)) continue;
        // Ties go to the lower ground index so the result is order-independent.
        if (++count[k] > best || (count[k] == best && k < partner)) {
            best = count[k];
            partner = k;
        }
    }

    // Fallback for a cell with no blending partner: a plain fill tile of g.
    auto plain = [&]() -> int {
        // Keep a fill tile of this ground (no reshuffling). Keep decoration of
        // this ground too, unless the cell was just painted: painting means
        // "make this plain ground".
        if (current >= 0 && profile_.tiles[current].ground == g) {
            const TileRole role = profile_.tiles[current].role;
            if (role == TileRole::Fill) return -1;
            if (role == TileRole::Ground && !painted_[idx(x, y)]) return -1;
        }
        const int t = pick(profile_.grounds[g].fill, seed, x, y);
        return t < 0 ? -1 : mappingOfTile_[t];
    };

    // No neighbour blends with g. This also covers the B side of a border:
    // (B, A) normally has no transitions, so B cells stay plain.
    if (partner < 0) return plain();

    // Corner key: a corner belongs to the partner if any cell meeting there is it.
    // Each corner is shared by this cell, its two edge neighbours on that side
    // and the diagonal one; the corner is B if any of those three is B.
    auto is = [&](int cx, int cy) { return groundOrNone(cx, cy) == partner; };
    int key = 0;
    if (is(x - 1, y - 1) || is(x, y - 1) || is(x - 1, y)) key |= kNW;
    if (is(x, y - 1) || is(x + 1, y - 1) || is(x + 1, y)) key |= kNE;
    if (is(x + 1, y) || is(x + 1, y + 1) || is(x, y + 1)) key |= kSE;
    if (is(x, y + 1) || is(x - 1, y + 1) || is(x - 1, y)) key |= kSW;

    // Already a fitting tile: keep it.
    if (current >= 0) {
        const ProfileTile& c = profile_.tiles[current];
        if (c.role == TileRole::Transition && c.ground == g && c.groundB == partner && c.corners == key)
            return -1;
    }

    // Exact key, else the nearest keys (fewest differing corners). Some keys
    // have no tile in a set (e.g. the two diagonals NW+SE / NE+SW).
    const auto& sets = profile_.transitions(g, partner);
    // distance = number of corners that differ from the wanted key; the
    // first distance with any usable tile wins.
    for (int distance = 0; distance <= 4; ++distance) {
        std::vector<int> candidates;
        for (int k = 0; k < 16; ++k)
            if (popcount4(k ^ key) == distance)
                candidates.insert(candidates.end(), sets[k].begin(), sets[k].end());
        // Never fall back to an all-A transition tile for a cell that touches B,
        // nor to all-B: those read as plain ground with a seam.
        if (distance > 0)
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](int t) {
                const int c = profile_.tiles[t].corners;
                return c == 0 || c == (kNW | kNE | kSE | kSW);
            }), candidates.end());
        if (current >= 0 && std::find(candidates.begin(), candidates.end(), current) != candidates.end())
            return -1;   // already one of the best available
        const int t = pick(candidates, seed, x, y);
        if (t >= 0) return mappingOfTile_[t];
    }
    return plain();
}

// Re-picks the tiles of a rectangle (or the dirty box) plus one cell around
// it, applies them to this copy and records them as changes for the caller.
// An explicit region is clamped to the map; one that is empty returns 0.
int BlendMap::resolve(const om_rect* region, uint64_t seed) {
    changes_.clear();
    int x0, y0, x1, y1;
    if (region) {
        if (region->width <= 0 || region->height <= 0) return 0;
        x0 = region->x; y0 = region->y;
        x1 = region->x + region->width - 1; y1 = region->y + region->height - 1;
    } else {
        if (!dirty_) return 0;
        x0 = dirtyX0_; y0 = dirtyY0_; x1 = dirtyX1_; y1 = dirtyY1_;
    }
    // Even an explicit region clears the dirty box; the caller chose what to resolve.
    dirty_ = false;

    // One extra cell around: a tile depends on its neighbours' grounds.
    x0 = std::max(0, x0 - 1); y0 = std::max(0, y0 - 1);
    x1 = std::min(width_ - 1, x1 + 1); y1 = std::min(height_ - 1, y1 + 1);

    // Decide first, then apply: choices depend only on grounds, but keeping
    // the two steps apart makes that independent of the loop order for sure.
    std::vector<om_change> pending;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int m = chooseMapping(x, y, seed);
            if (m < 0 || m == mapping_[idx(x, y)]) continue;
            const int tile = tileOfMapping_[m];
            pending.push_back({x, y, mapping_[idx(x, y)], m, tile >= 0 ? profile_.tiles[tile].cellType : -1});
        }
    }
    for (const om_change& c : pending) mapping_[idx(c.x, c.y)] = c.new_mapping;
    // The resolved area is no longer "just painted": its decoration is kept
    // from now on.
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) painted_[idx(x, y)] = 0;
    changes_ = std::move(pending);
    return static_cast<int>(changes_.size());
}

// Draws a cliff along the polyline: chooses the cliff family, runs the cliff
// solver (cliff.cpp) on a local window around the line, and records the cliff
// tiles as changes. Cliff cells become fixed (ground -1). Returns -1 for bad
// input (no points, fewer than 2, a point off the map, bad side) and 0 if the
// profile has no usable cliffs.
int BlendMap::drawCliff(const int32_t* xy, int count, int highSide, double wallHeight, uint64_t seed) {
    changes_.clear();
    lastCliffStage_ = -2;
    if (!xy || count < 2 || highSide < 0 || highSide > 3) return -1;
    std::vector<std::pair<int, int>> pts;
    for (int i = 0; i < count; ++i) {
        const int x = xy[2 * i], y = xy[2 * i + 1];
        if (!inside(x, y)) return -1;
        pts.push_back({x, y});
    }
    const CliffData& cliffs = profile_.cliffs;
    if (cliffs.empty()) return 0;

    // Cliff family from the ground at the first point (the most common ground
    // around it if that cell is fixed); grounds without cliffs of their own
    // use the profile's fallback family (Outpost 2: mud).
    int ground = groundOrNone(pts[0].first, pts[0].second);
    if (ground < 0) {
        std::vector<int> count5(profile_.grounds.size(), 0);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int g = groundOrNone(pts[0].first + dx, pts[0].second + dy);
                if (g >= 0) ++count5[g];
            }
        int best = 0;
        for (size_t g = 0; g < count5.size(); ++g)
            if (count5[g] > best) {
                best = count5[g];
                ground = static_cast<int>(g);
            }
    }
    int family = ground >= 0 ? cliffs.findFamily(profile_.grounds[ground].id) : -1;
    if (!cliffs.usable(family)) family = cliffs.findFamily(cliffs.fallback);
    if (!cliffs.usable(family)) return 0;

    // Work in the area around the line, with room for the wall and its wandering.
    // z: wall height clamped to 3..6 tiles. The margin covers the wall height
    // plus up to three times the wall top's wandering (0.35 tiles, kUnruhe in
    // cliff.cpp), with a few cells spare, so the solver's window never cuts
    // off part of the cliff.
    const double z = std::min(6.0, std::max(3.0, wallHeight));
    const int margin = static_cast<int>(std::ceil(std::max(z - 0.5, 0.5) + 3 * 0.35)) + 3;
    int x0 = pts[0].first, x1 = x0, y0 = pts[0].second, y1 = y0;
    for (const auto& p : pts) {
        x0 = std::min(x0, p.first); x1 = std::max(x1, p.first);
        y0 = std::min(y0, p.second); y1 = std::max(y1, p.second);
    }
    x0 = std::max(0, x0 - margin); y0 = std::max(0, y0 - margin);
    x1 = std::min(width_ - 1, x1 + margin); y1 = std::min(height_ - 1, y1 + margin);
    // Points relative to the work window, which is what the solver sees.
    std::vector<std::pair<int, int>> local;
    for (const auto& p : pts) local.push_back({p.first - x0, p.second - y0});

    const CliffResult res = solveCliffLine(cliffs, family, local, static_cast<CliffHighSide>(highSide), z,
                                           x1 - x0 + 1, y1 - y0 + 1, seed);
    lastCliffStage_ = res.stage;
    // Copy the solver's grid back. -1 in res.tile means "no cliff here". "any"
    // tracks whether the solver placed anything, even tiles this map lacks.
    bool any = false;
    for (int ly = 0; ly < res.height; ++ly)
        for (int lx = 0; lx < res.width; ++lx) {
            const int t = res.tile[static_cast<size_t>(ly) * res.width + lx];
            if (t < 0) continue;
            any = true;
            const int m = mappingOfCliff_[t];
            if (m < 0) continue;      // this map's mapping table lacks the tile
            const int x = lx + x0, y = ly + y0;
            if (m == mapping_[idx(x, y)]) continue;
            changes_.push_back({x, y, mapping_[idx(x, y)], m, cliffs.tiles[t].cellType});
        }
    if (!any) lastCliffStage_ = -2;
    for (const om_change& c : changes_) {
        mapping_[idx(c.x, c.y)] = c.new_mapping;
        ground_[idx(c.x, c.y)] = static_cast<int8_t>(groundOfMapping(c.new_mapping));   // cliffs are fixed (-1)
        painted_[idx(c.x, c.y)] = 0;
    }
    return static_cast<int>(changes_.size());
}

// Closes cut wall ends: finds the cliff family around the cut, lets the cliff
// code (solveCliffCut) re-solve the wall cells near it, and turns cliff cells
// that keep no rock into ground of their surroundings, which a resolve of
// that area then gives proper tiles. The cliff changes come first in the
// change list, then the resolve's.
int BlendMap::closeCliffCut(const uint8_t* removed, uint64_t seed) {
    changes_.clear();
    lastCliffStage_ = -2;
    if (!removed) return -1;
    const CliffData& cliffs = profile_.cliffs;
    if (cliffs.empty()) return 0;
    auto cliffAt = [&](int x, int y) {
        const int m = mapping_[idx(x, y)];
        return (m >= 0 && m < static_cast<int>(cliffOfMapping_.size())) ? cliffOfMapping_[m] : -1;
    };

    // The cut: flagged cells that hold no cliff tile now. The family is the
    // most common one among the cliff cells touching it (ties: lowest index).
    std::vector<int> famCount(cliffs.families.size(), 0);
    int bx0 = width_, by0 = height_, bx1 = -1, by1 = -1;
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            if (!removed[idx(x, y)] || cliffAt(x, y) >= 0) continue;
            bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
            by0 = std::min(by0, y); by1 = std::max(by1, y);
            static const int dx4[4] = {0, 1, 0, -1}, dy4[4] = {-1, 0, 1, 0};
            for (int s = 0; s < 4; ++s) {
                const int nx = x + dx4[s], ny = y + dy4[s];
                if (!inside(nx, ny)) continue;
                const int t = cliffAt(nx, ny);
                if (t >= 0 && cliffs.tiles[t].family >= 0) ++famCount[cliffs.tiles[t].family];
            }
        }
    int family = -1, best = 0;
    for (size_t f = 0; f < famCount.size(); ++f)
        if (famCount[f] > best) {
            best = famCount[f];
            family = static_cast<int>(f);
        }
    if (family < 0 || !cliffs.usable(family)) return 0;

    // Work window: the cut plus the re-solved zone (2 cells) and room for the
    // fixed neighbours around it.
    const int zone = 2, margin = zone + 4;
    const int x0 = std::max(0, bx0 - margin), y0 = std::max(0, by0 - margin);
    const int x1 = std::min(width_ - 1, bx1 + margin), y1 = std::min(height_ - 1, by1 + margin);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<int> tiles(static_cast<size_t>(w) * h, -1);
    std::vector<uint8_t> cut(static_cast<size_t>(w) * h, 0);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y - y0) * w + (x - x0);
            tiles[i] = cliffAt(x, y);
            cut[i] = removed[idx(x, y)] && tiles[i] < 0;
        }
    // Two cells around the cut first; if the solver finds nothing there
    // (e.g. a thin north edge), a wider zone gives it more room.
    CliffCutResult res;
    for (int z = zone; z <= zone + 2 && res.stage < 0; ++z) res = solveCliffCut(cliffs, family, tiles, cut, w, h, z);
    lastCliffStage_ = res.stage;
    if (res.stage < 0) return 0;

    std::vector<om_change> mine;
    for (int ly = 0; ly < h; ++ly)
        for (int lx = 0; lx < w; ++lx) {
            const int t = res.tile[static_cast<size_t>(ly) * w + lx];
            if (t < 0) continue;
            const int m = mappingOfCliff_[t];
            const int x = lx + x0, y = ly + y0;
            if (m < 0 || m == mapping_[idx(x, y)]) continue;   // tile missing in this map, or unchanged
            mine.push_back({x, y, mapping_[idx(x, y)], m, cliffs.tiles[t].cellType});
        }
    for (const om_change& c : mine) {
        mapping_[idx(c.x, c.y)] = c.new_mapping;
        ground_[idx(c.x, c.y)] = static_cast<int8_t>(groundOfMapping(c.new_mapping));
        painted_[idx(c.x, c.y)] = 0;
    }

    // Former cliff cells without rock: the most common ground within two
    // cells (not counting fixed cells or other such cells), then a resolve.
    int gx0 = width_, gy0 = height_, gx1 = -1, gy1 = -1;
    std::vector<std::pair<std::pair<int, int>, int>> grounds;
    for (int ly = 0; ly < h; ++ly)
        for (int lx = 0; lx < w; ++lx) {
            if (!res.ground[static_cast<size_t>(ly) * w + lx]) continue;
            const int x = lx + x0, y = ly + y0;
            std::vector<int> count(profile_.grounds.size(), 0);
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int qx = x + dx, qy = y + dy;
                    if (!inside(qx, qy)) continue;
                    const int lqx = qx - x0, lqy = qy - y0;
                    if (lqx >= 0 && lqy >= 0 && lqx < w && lqy < h && res.ground[static_cast<size_t>(lqy) * w + lqx]) continue;
                    const int g = ground_[idx(qx, qy)];
                    if (g >= 0) ++count[g];
                }
            int g = -1, n = 0;
            for (size_t k = 0; k < count.size(); ++k)
                if (count[k] > n) {
                    n = count[k];
                    g = static_cast<int>(k);
                }
            if (g < 0) continue;
            grounds.push_back({{x, y}, g});
            gx0 = std::min(gx0, x); gx1 = std::max(gx1, x);
            gy0 = std::min(gy0, y); gy1 = std::max(gy1, y);
        }
    for (const auto& [p, g] : grounds) {
        ground_[idx(p.first, p.second)] = static_cast<int8_t>(g);
        painted_[idx(p.first, p.second)] = 1;
    }
    std::vector<om_change> blended;
    if (!grounds.empty()) {
        const om_rect r{gx0, gy0, gx1 - gx0 + 1, gy1 - gy0 + 1};
        resolve(&r, seed);
        blended = changes_;
    }
    changes_ = std::move(mine);
    changes_.insert(changes_.end(), blended.begin(), blended.end());
    return static_cast<int>(changes_.size());
}

}  // namespace om

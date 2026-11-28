// Tests for op2maplib (terrain blending, cliffs, passable regions), through
// the public C API only.
//
// Usage: op2maplib_tests <path to profiles/op2.json>
//
// No test framework: each CHECK prints the failing condition and the test
// exits non-zero if any failed.
//
// The golden hashes below are shared with the language bindings (via
// tests/vectors/), so they also check that every binding gets identical output.
#include "op2maplib/op2maplib.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Number of failed CHECKs so far; non-zero makes main return 1.
static int g_failures = 0;

// Records a failure and carries on, so one run reports every broken check.
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  FAILED: %s  (%s:%d)\n", #cond, __FILE__, __LINE__);      \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

namespace {

// Size of every blend test map, and the default seed.
constexpr int W = 40;
constexpr int H = 40;
constexpr uint64_t kSeed = 1234;

// Whole file as a string; empty if it cannot be read (the profile load then fails).
std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// A map using the profile's standard mapping table, every cell holding `mapping`.
struct TestMap {
    std::vector<om_mapping> mappings;
    om_blend_map* map = nullptr;

    TestMap(const om_profile* p, int mapping) {
        const int n = om_profile_standard_mapping_count(p);
        mappings.resize(n);
        for (int i = 0; i < n; ++i) om_profile_standard_mapping(p, i, &mappings[i]);
        std::vector<uint16_t> cells(W * H, static_cast<uint16_t>(mapping));
        char err[256];
        map = om_blend_create(p, W, H, mappings.data(), n, cells.data(), err, sizeof err);
        if (!map) std::printf("  om_blend_create failed: %s\n", err);
    }
    // om_blend_free accepts NULL, so a failed create needs no special case.
    ~TestMap() { om_blend_free(map); }
};

// (x, y) pairs of the cells within radius r of (cx, cy), row by row.
std::vector<int32_t> disc(int cx, int cy, int r) {
    std::vector<int32_t> xy;
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                xy.push_back(x);
                xy.push_back(y);
            }
    return xy;
}

// Mapping index of every cell, row by row.
std::vector<int> snapshot(const om_blend_map* m) {
    std::vector<int> out(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) out[y * W + x] = om_blend_tile_at(m, x, y);
    return out;
}

/// FNV-1a over the tile grid - the golden value pins the exact output.
uint64_t hashGrid(const std::vector<int>& g) {
    uint64_t h = 1469598103934665603ULL;
    for (int v : g) {
        for (int b = 0; b < 4; ++b) {
            h ^= static_cast<uint8_t>(v >> (8 * b));
            h *= 1099511628211ULL;
        }
    }
    return h;
}

/// Paints the whole map with `ground` and resolves it.
void fillWith(TestMap& tm, int ground) {
    std::vector<int32_t> all;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) { all.push_back(x); all.push_back(y); }
    om_blend_paint(tm.map, ground, all.data(), W * H);
    om_blend_resolve(tm.map, nullptr, kSeed);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: op2maplib_tests <profiles/op2.json>\n");
        return 2;
    }
    std::printf("op2maplib %s\n", om_version());

    const std::string json = readFile(argv[1]);
    char err[256];
    om_profile* p = om_profile_load(json.data(), json.size(), err, sizeof err);
    if (!p) {
        std::printf("FAILED to load profile: %s\n", err);
        return 1;
    }

    // --- Profile -----------------------------------------------------------
    std::printf("profile: %s\n", om_profile_name(p));
    const int mud = om_profile_find_ground(p, "mud");
    const int san = om_profile_find_ground(p, "san");
    const int roc = om_profile_find_ground(p, "roc");
    const int veg = om_profile_find_ground(p, "veg");
    CHECK(mud >= 0 && san >= 0 && roc >= 0 && veg >= 0);
    CHECK(om_profile_grounds_blend(p, mud, san) == 1);
    CHECK(om_profile_grounds_blend(p, san, mud) == 1);
    CHECK(om_profile_grounds_blend(p, mud, roc) == 1);
    CHECK(om_profile_grounds_blend(p, mud, veg) == 0);
    CHECK(om_profile_ground_fill_count(p, mud) > 0);
    CHECK(om_profile_standard_mapping_count(p) == 2017);
    for (int g = 0; g < om_profile_ground_count(p); ++g)
        std::printf("  ground %d: %s (%s), %d fill tiles\n", g, om_profile_ground_id(p, g),
                    om_profile_ground_name(p, g), om_profile_ground_fill_count(p, g));

    // Bad input is reported, not crashed on.
    CHECK(om_profile_load("{", 1, err, sizeof err) == nullptr && err[0] != '\0');
    CHECK(om_profile_load("{\"format\":\"x\",\"version\":1}", 27, err, sizeof err) == nullptr);

    // --- Fill a map with mud ----------------------------------------------
    std::printf("fill\n");
    TestMap a(p, 0);
    CHECK(a.map != nullptr);
    if (!a.map) return 1;
    fillWith(a, mud);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            CHECK(om_blend_ground_at(a.map, x, y) == mud);
            CHECK(om_blend_tile_role(a.map, x, y) == OM_TILE_FILL);
        }
    // Resolving again changes nothing.
    CHECK(om_blend_resolve_all(a.map, kSeed) == 0);
    // Fill uses a variety of tiles, not one repeated tile.
    {
        std::vector<int> g = snapshot(a.map);
        int distinct = 0;
        std::vector<bool> seen(4096, false);
        for (int v : g) if (v >= 0 && v < 4096 && !seen[v]) { seen[v] = true; ++distinct; }
        CHECK(distinct > 5);
    }

    // --- Paint a sand disc ---------------------------------------------------
    std::printf("sand disc\n");
    const std::vector<int> before = snapshot(a.map);
    const std::vector<int32_t> sand = disc(20, 20, 6);
    CHECK(om_blend_paint(a.map, san, sand.data(), static_cast<int32_t>(sand.size() / 2)) ==
          static_cast<int>(sand.size() / 2));
    const int changed = om_blend_resolve(a.map, nullptr, kSeed);
    CHECK(changed > 0);
    {
        std::vector<om_change> changes(changed);
        CHECK(om_blend_get_changes(a.map, changes.data(), changed) == changed);
        for (const om_change& c : changes) {
            CHECK(c.old_mapping == before[c.y * W + c.x]);
            CHECK(c.new_mapping == om_blend_tile_at(a.map, c.x, c.y));
            CHECK(c.cell_type >= -1 && c.cell_type < 32);
        }
    }
    const std::vector<int> after = snapshot(a.map);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool inDisc = (x - 20) * (x - 20) + (y - 20) * (y - 20) <= 36;
            const int ground = om_blend_ground_at(a.map, x, y);
            const int role = om_blend_tile_role(a.map, x, y);
            CHECK(ground == (inDisc ? san : mud));
            // Sand is the pair's second ground: sand cells stay plain.
            if (inDisc) CHECK(role == OM_TILE_FILL);
            // Every mud cell touching sand holds a transition tile.
            bool touchesSand = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (om_blend_ground_at(a.map, x + dx, y + dy) == san) touchesSand = true;
            if (!inDisc && touchesSand) CHECK(role == OM_TILE_TRANSITION);
            if (!inDisc && !touchesSand) CHECK(role == OM_TILE_FILL);
            // Nothing changes outside the painted area plus one cell.
            if (x < 13 || x > 27 || y < 13 || y > 27) CHECK(after[y * W + x] == before[y * W + x]);
        }
    }
    // Stable: resolving the same area again changes nothing.
    {
        om_rect r{13, 13, 15, 15};
        CHECK(om_blend_resolve(a.map, &r, kSeed) == 0);
        CHECK(om_blend_resolve_all(a.map, 99) == 0);
    }

    // --- Determinism -------------------------------------------------------
    std::printf("determinism\n");
    {
        TestMap b(p, 0);
        fillWith(b, mud);
        om_blend_paint(b.map, san, sand.data(), static_cast<int32_t>(sand.size() / 2));
        om_blend_resolve(b.map, nullptr, kSeed);
        CHECK(snapshot(b.map) == after);
        const uint64_t h = hashGrid(after);
        std::printf("  grid hash: %016llx\n", static_cast<unsigned long long>(h));
        // Golden value: pins the exact output for this profile and seed so every
        // platform and binding can be checked against it (tests/vectors/blend_disc.json).
        // If an intended algorithm or profile change moves it, update both places.
        CHECK(h == 0xd33d22343ac4ecfdULL);
    }

    // --- Rock next to the sand: three grounds meeting ------------------------
    std::printf("three grounds\n");
    {
        std::vector<int32_t> rock;
        for (int y = 14; y <= 26; ++y)
            for (int x = 26; x <= 32; ++x) { rock.push_back(x); rock.push_back(y); }
        om_blend_paint(a.map, roc, rock.data(), static_cast<int32_t>(rock.size() / 2));
        CHECK(om_blend_resolve(a.map, nullptr, kSeed) > 0);
        // Every non-fixed cell still holds a profile tile.
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) CHECK(om_blend_tile_role(a.map, x, y) != OM_TILE_UNKNOWN);
    }

    // --- Fixed cells are never changed -------------------------------------
    std::printf("fixed cells\n");
    {
        TestMap c(p, 0);
        fillWith(c, mud);
        // Find a standard mapping the profile doesn't know (e.g. a cliff or wall).
        int unknown = -1;
        for (int m = 0; m < om_profile_standard_mapping_count(p) && unknown < 0; ++m) {
            om_blend_set_tile(c.map, 0, 0, m);
            if (om_blend_tile_role(c.map, 0, 0) == OM_TILE_UNKNOWN) unknown = m;
        }
        CHECK(unknown >= 0);
        om_blend_set_tile(c.map, 0, 0, om_blend_tile_at(a.map, 0, 0));
        om_blend_set_tile(c.map, 10, 10, unknown);
        CHECK(om_blend_ground_at(c.map, 10, 10) == -1);
        const std::vector<int32_t> around = disc(10, 10, 3);
        std::vector<int32_t> ring;
        for (size_t i = 0; i < around.size(); i += 2)
            if (!(around[i] == 10 && around[i + 1] == 10)) { ring.push_back(around[i]); ring.push_back(around[i + 1]); }
        om_blend_paint(c.map, san, ring.data(), static_cast<int32_t>(ring.size() / 2));
        om_blend_resolve(c.map, nullptr, kSeed);
        CHECK(om_blend_tile_at(c.map, 10, 10) == unknown);
        // Painting over it makes it a normal cell again.
        const int32_t one[2] = {10, 10};
        om_blend_paint(c.map, san, one, 1);
        om_blend_resolve(c.map, nullptr, kSeed);
        CHECK(om_blend_tile_role(c.map, 10, 10) == OM_TILE_FILL);
    }

    // --- Cliffs -----------------------------------------------------------------
    std::printf("cliffs\n");
    {
        CHECK(om_profile_cliff_ground(p, mud) == 1);
        CHECK(om_profile_cliff_ground(p, san) == 1);
        CHECK(om_profile_cliff_ground(p, roc) == 0);   // rock uses the fallback (mud) cliffs

        int32_t sx, sy;
        om_cliff_snap_point(10, 10, 30, 11, &sx, &sy);
        CHECK(sx == 30 && sy == 10);

        // The L shape from op2-codegen-editor's screenshot: down, then right; high side left.
        const int32_t pts[] = {20, 6, 20, 20, 36, 20};
        TestMap c(p, 0);
        fillWith(c, mud);
        const std::vector<int> cliffBefore = snapshot(c.map);
        const int n = om_blend_resolve_all(c.map, kSeed);   // settle, then draw
        CHECK(n == 0);
        const int drawn = om_cliff_draw(c.map, pts, 3, OM_CLIFF_HIGH_LEFT, 3.0, 7);
        CHECK(drawn > 20);
        CHECK(om_cliff_last_stage(c.map) >= 0);              // solved, not the greedy fallback
        std::vector<om_change> changes(drawn > 0 ? drawn : 1);
        om_blend_get_changes(c.map, changes.data(), drawn);
        int high = 0, low = 0, north = 0;
        for (int i = 0; i < drawn; ++i) {
            const om_change& ch = changes[i];
            CHECK(ch.old_mapping == cliffBefore[ch.y * W + ch.x]);
            CHECK(om_blend_ground_at(c.map, ch.x, ch.y) == -1);   // cliffs are fixed
            high += ch.cell_type == 9;
            low += ch.cell_type == 10;
            north += ch.cell_type == 8;
            // Cliff stays near the line
            CHECK(ch.x >= 14 && ch.x <= 40 && ch.y >= 2 && ch.y <= 27);
        }
        std::printf("  L shape: %d tiles (high %d, low %d, north %d), stage %d\n", drawn, high, low, north,
                    om_cliff_last_stage(c.map));
        CHECK(high > 0 && low > 0);

        // Deterministic: the same draw on a fresh map gives the same grid.
        TestMap d(p, 0);
        fillWith(d, mud);
        om_cliff_draw(d.map, pts, 3, OM_CLIFF_HIGH_LEFT, 3.0, 7);
        const std::vector<int> grid = snapshot(c.map);
        CHECK(snapshot(d.map) == grid);
        const uint64_t h = hashGrid(grid);
        std::printf("  cliff grid hash: %016llx\n", static_cast<unsigned long long>(h));
        // Golden value, also in tests/vectors/cliff_l.json for the bindings.
        CHECK(h == 0x5e0acf226271c020ULL);

        // Painting sand around the cliff leaves the cliff alone.
        std::vector<int32_t> around;
        for (int y = 0; y < 30; ++y)
            for (int x = 10; x < 40; ++x)
                if (om_blend_ground_at(c.map, x, y) >= 0) { around.push_back(x); around.push_back(y); }
        om_blend_paint(c.map, san, around.data(), static_cast<int32_t>(around.size() / 2));
        om_blend_resolve(c.map, nullptr, kSeed);
        for (int i = 0; i < drawn; ++i)
            CHECK(om_blend_tile_at(c.map, changes[i].x, changes[i].y) == changes[i].new_mapping);

        // Bad input
        const int32_t outside[] = {0, 0, 500, 0};
        CHECK(om_cliff_draw(c.map, outside, 2, OM_CLIFF_HIGH_NORTH, 3.0, 1) == -1);
        CHECK(om_cliff_draw(c.map, pts, 1, OM_CLIFF_HIGH_NORTH, 3.0, 1) == -1);
        CHECK(om_cliff_draw(c.map, pts, 3, 9, 3.0, 1) == -1);

        // Closing a cut: paint mud across the L's vertical leg (rows 11-13),
        // then let the wall ends finish cleanly.
        TestMap e(p, 0);
        fillWith(e, mud);
        om_cliff_draw(e.map, pts, 3, OM_CLIFF_HIGH_LEFT, 3.0, 7);
        const std::vector<int> wall = snapshot(e.map);
        std::vector<int32_t> cutCells;
        std::vector<uint8_t> removed(W * H, 0);
        for (int y = 11; y <= 13; ++y)
            for (int x = 0; x < W; ++x)
                if (om_blend_ground_at(e.map, x, y) == -1) {
                    cutCells.push_back(x);
                    cutCells.push_back(y);
                    removed[y * W + x] = 1;
                }
        CHECK(!cutCells.empty());
        om_blend_paint(e.map, mud, cutCells.data(), static_cast<int32_t>(cutCells.size() / 2));
        om_blend_resolve(e.map, nullptr, kSeed);
        const int closed = om_cliff_close_cut(e.map, removed.data(), kSeed);
        CHECK(closed > 0);
        CHECK(om_cliff_last_stage(e.map) >= 0);
        std::vector<om_change> cc(closed > 0 ? closed : 1);
        om_blend_get_changes(e.map, cc.data(), closed);
        for (int i = 0; i < closed; ++i) CHECK(cc[i].y >= 11 - 4 && cc[i].y <= 13 + 4);   // only near the cut
        // Far from the cut the wall stands as it was.
        const std::vector<int> after = snapshot(e.map);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (y < 11 - 4 || y > 13 + 4) CHECK(after[y * W + x] == wall[y * W + x]);
        std::printf("  cut closed: %d changes, stage %d\n", closed, om_cliff_last_stage(e.map));
        // Nothing cut: nothing to do. Bad input: -1.
        const std::vector<uint8_t> none(W * H, 0);
        CHECK(om_cliff_close_cut(e.map, none.data(), kSeed) == 0);
        CHECK(om_cliff_close_cut(e.map, nullptr, kSeed) == -1);
        CHECK(om_cliff_close_cut(nullptr, none.data(), kSeed) == -1);
    }

    // --- Passable regions (enclosed areas) ----------------------------------------
    std::printf("regions\n");
    {
        constexpr uint8_t kPass = 0, kWall = 23, kCliff = 9, kTube = 26;
        const int rw = 20, rh = 12;
        std::vector<uint8_t> cells(rw * rh, kPass);
        // A wall ring around a 3x3 pocket at (3..5, 3..5)
        for (int y = 2; y <= 6; ++y)
            for (int x = 2; x <= 6; ++x)
                if (x == 2 || x == 6 || y == 2 || y == 6) cells[y * rw + x] = kWall;
        // A cliff ring around a 2x2 pocket at (12..13, 4..5); one ring corner is a wall instead.
        for (int y = 3; y <= 6; ++y)
            for (int x = 11; x <= 14; ++x)
                if (x == 11 || x == 14 || y == 3 || y == 6) cells[y * rw + x] = kCliff;
        cells[6 * rw + 14] = kWall;
        // Tubes are passable: a tube cell inside the first pocket stays part of it.
        cells[4 * rw + 4] = kTube;

        CHECK(om_cell_types_passable_mask() & (1u << 26));
        CHECK(!(om_cell_types_passable_mask() & (1u << 23)));
        CHECK(om_regions_op2_flags(512, 256) & OM_REGIONS_WRAP_X);
        CHECK(!(om_regions_op2_flags(256, 128) & OM_REGIONS_WRAP_X));

        om_regions* reg = om_regions_find(rw, rh, cells.data(), 0, om_regions_op2_flags(rw, rh));
        CHECK(reg != nullptr);
        CHECK(om_regions_count(reg) == 3);                     // main area + 2 pockets
        CHECK(om_regions_size(reg, 1) == 9);                   // 3x3 pocket (largest enclosed)
        CHECK(om_regions_size(reg, 2) == 4);                   // 2x2 pocket
        // Main area: 20 x 11 usable rows = 220, minus the wall ring (16), the cliff ring (12) and both pockets (13).
        CHECK(om_regions_size(reg, 0) == 179);
        CHECK(om_regions_at(reg, 4, 4) == 1);                  // the tube cell
        CHECK(om_regions_at(reg, 2, 2) == -1);                 // wall
        CHECK(om_regions_at(reg, 0, rh - 1) == -1);            // bottom row skipped
        CHECK(om_regions_enclosed_size(reg) == 13);
        std::vector<int32_t> xy(2 * 13);
        CHECK(om_regions_enclosed_cells(reg, xy.data(), 13) == 13);
        om_regions_free(reg);

        // Diagonal gap: open the pocket's corner diagonally. 8-connected joins it, 4-connected doesn't.
        std::vector<uint8_t> diag(rw * rh, kWall);
        diag[2 * rw + 2] = kPass;                              // cell A
        diag[3 * rw + 3] = kPass;                              // cell B, touching A only at a corner
        om_regions* d8 = om_regions_find(rw, rh, diag.data(), 0, OM_REGIONS_CONNECT8);
        om_regions* d4 = om_regions_find(rw, rh, diag.data(), 0, 0);
        CHECK(om_regions_count(d8) == 1);
        CHECK(om_regions_count(d4) == 2);
        om_regions_free(d8);
        om_regions_free(d4);

        // World map: a strip touching both side edges is one region only when wrapping.
        std::vector<uint8_t> world(512 * 256, kWall);
        for (int x = 0; x < 3; ++x) world[100 * 512 + x] = kPass;
        for (int x = 509; x < 512; ++x) world[100 * 512 + x] = kPass;
        om_regions* wrap = om_regions_find(512, 256, world.data(), 0, om_regions_op2_flags(512, 256));
        om_regions* nowrap = om_regions_find(512, 256, world.data(), 0, OM_REGIONS_CONNECT8 | OM_REGIONS_SKIP_BOTTOM_ROW);
        CHECK(om_regions_count(wrap) == 1);
        CHECK(om_regions_count(nowrap) == 2);
        om_regions_free(wrap);
        om_regions_free(nowrap);

        CHECK(om_regions_find(0, 5, cells.data(), 0, 0) == nullptr);
        CHECK(om_regions_count(nullptr) == 0);
    }

    // --- Map generator -------------------------------------------------------
    std::printf("mapgen\n");
    {
        // Every template generates a plausible map.
        for (const char* tpl : {"plains", "canyon", "desert", "islands", "ice", "newterra"}) {
            om_mapgen_settings* s = om_mapgen_settings_create(tpl);
            om_mapgen_set(s, "width", 64);
            om_mapgen_set(s, "height", 64);
            om_mapgen_set(s, "spawns", 2);
            om_mapgen_set_seed(s, 5);
            om_mapgen_result* r = om_mapgen_generate(p, s, err, sizeof err);
            CHECK(r != nullptr);
            if (r) {
                CHECK(om_mapgen_width(r) == 64 && om_mapgen_height(r) == 64);
                CHECK(om_mapgen_stat(r, "passable") > 0.85);
                CHECK(om_mapgen_point_count(r, OM_MAPGEN_SPAWNS) == 2);
                std::printf("  %-9s passable %.3f, plateau %.3f, details %.3f, volcanoes %d\n", tpl,
                            om_mapgen_stat(r, "passable"), om_mapgen_stat(r, "plateau"),
                            om_mapgen_stat(r, "details"), om_mapgen_point_count(r, OM_MAPGEN_VOLCANOES));
            }
            om_mapgen_result_free(r);
            om_mapgen_settings_free(s);
        }

        // Golden map (tests/vectors/mapgen_islands.json): islands 64x64, seed 1234,
        // cliffs 0.25, 2 spawns, one mountain feature. Same seed = same map.
        auto golden = [&](std::vector<int>& tiles, std::vector<uint8_t>& cells) {
            om_mapgen_settings* s = om_mapgen_settings_create("islands");
            om_mapgen_set(s, "width", 64);
            om_mapgen_set(s, "height", 64);
            om_mapgen_set(s, "cliffs", 0.25);
            om_mapgen_set(s, "spawns", 2);
            om_mapgen_set_seed(s, 1234);
            om_mapgen_add_feature(s, OM_FEATURE_MOUNTAIN, "c", "", 0.6, 0);
            om_mapgen_result* r = om_mapgen_generate(p, s, err, sizeof err);
            CHECK(r != nullptr);
            if (r) {
                std::vector<uint16_t> t(64 * 64);
                cells.assign(64 * 64, 0);
                om_mapgen_get_tiles(r, t.data(), 64 * 64);
                om_mapgen_get_cell_types(r, cells.data(), 64 * 64);
                tiles.assign(t.begin(), t.end());
                CHECK(om_mapgen_stat(r, "connected") == 1.0);
            }
            om_mapgen_result_free(r);
            om_mapgen_settings_free(s);
        };
        std::vector<int> t1, t2;
        std::vector<uint8_t> c1, c2;
        golden(t1, c1);
        golden(t2, c2);
        CHECK(t1 == t2 && c1 == c2);
        int cliffCells = 0;
        for (uint8_t c : c1) cliffCells += c == 8 || c == 9 || c == 10;
        CHECK(cliffCells > 0);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hashGrid(t1)));
        std::printf("  golden map hash: %s, %d cliff cells\n", hex, cliffCells);
        CHECK(std::string(hex) == "6c45f417758ec9c7");

        // Mountains, gorges, valleys and plateaus (tests/vectors/mapgen_mountains.json):
        // closed mountains, reachable plateaus, same seed = same map.
        {
            om_mapgen_settings* ms = om_mapgen_settings_create("plains");
            om_mapgen_set(ms, "width", 64);
            om_mapgen_set(ms, "height", 64);
            om_mapgen_set(ms, "mountains", 0.14);
            om_mapgen_set(ms, "plateaus", 1);
            om_mapgen_set(ms, "spawns", 2);
            om_mapgen_set_seed(ms, 7);
            om_mapgen_result* r = om_mapgen_generate(p, ms, err, sizeof err);
            CHECK(r != nullptr);
            if (r) {
                std::vector<uint16_t> t(64 * 64);
                om_mapgen_get_tiles(r, t.data(), 64 * 64);
                std::snprintf(hex, sizeof hex, "%016llx",
                              static_cast<unsigned long long>(hashGrid(std::vector<int>(t.begin(), t.end()))));
                std::printf("  mountains: %d (leaky %d), plateaus %d (unreachable %d), build in mountains %.3f, hash %s\n",
                            int(om_mapgen_stat(r, "mountains")), int(om_mapgen_stat(r, "mountains_leaky")),
                            int(om_mapgen_stat(r, "plateaus")), int(om_mapgen_stat(r, "plateaus_unreachable")),
                            om_mapgen_stat(r, "build_mountain"), hex);
                CHECK(om_mapgen_stat(r, "mountains") >= 1 && om_mapgen_stat(r, "mountains_leaky") == 0);
                CHECK(om_mapgen_stat(r, "plateaus") == 1 && om_mapgen_stat(r, "plateaus_unreachable") == 0);
                CHECK(om_mapgen_stat(r, "build_mountain") < 0.1);
                CHECK(std::string(hex) == "683ac10cc3dfc740");
            }
            om_mapgen_result_free(r);
            // Classic cliffs as a world cutout cross the map edge.
            om_mapgen_settings* ws = om_mapgen_settings_create("plains");
            om_mapgen_set(ws, "width", 64);
            om_mapgen_set(ws, "height", 64);
            om_mapgen_set(ws, "cliffs", 0.2);
            om_mapgen_set(ws, "world_cutout", 1);
            om_mapgen_set_seed(ws, 7);
            r = om_mapgen_generate(p, ws, err, sizeof err);
            CHECK(r != nullptr && om_mapgen_width(r) == 64);
            if (r) {
                std::vector<uint8_t> c(64 * 64);
                om_mapgen_get_cell_types(r, c.data(), 64 * 64);
                int edge = 0;
                for (int i = 0; i < 64; ++i)
                    for (int k : {i, 63 * 64 + i, i * 64, i * 64 + 63}) edge += c[size_t(k)] >= 8 && c[size_t(k)] <= 10;
                CHECK(edge > 0);
            }
            om_mapgen_result_free(r);
            om_mapgen_settings_free(ws);
            om_mapgen_settings_free(ms);
        }

        // Settings API.
        om_mapgen_settings* s = om_mapgen_settings_create(nullptr);
        CHECK(om_mapgen_set(s, "no_such_key", 1) == -1);
        CHECK(om_mapgen_set(s, "symmetry", 9) == -1);
        CHECK(om_mapgen_set(s, "cliffs", 0.5) == 0 && om_mapgen_get(s, "cliffs") == 0.5);
        CHECK(om_mapgen_add_feature(s, 7, "", "", 0.5, 0) == -1);
        om_mapgen_set(s, "width", 8);
        CHECK(om_mapgen_generate(p, s, err, sizeof err) == nullptr && err[0] != '\0');
        om_mapgen_settings_free(s);
        CHECK(om_mapgen_generate(nullptr, nullptr, err, sizeof err) == nullptr);
    }

    // --- Learned map generator ----------------------------------------------
    std::printf("learned generator\n");
    {
        // The network file sits next to the profile (profiles/op2-net.bin).
        std::string netPath = argv[1];
        const size_t slash = netPath.find_last_of("/\\");
        netPath = (slash == std::string::npos ? std::string() : netPath.substr(0, slash + 1)) + "op2-net.bin";
        const std::string net = readFile(netPath.c_str());
        om_mapgen_model* m = om_mapgen_model_load(net.data(), net.size(), err, sizeof err);
        CHECK(m != nullptr);
        CHECK(om_mapgen_model_load("nope", 4, err, sizeof err) == nullptr && err[0] != '\0');
        om_mapgen_settings* s = om_mapgen_settings_create("learned");
        om_mapgen_set(s, "width", 64);
        om_mapgen_set(s, "height", 64);
        om_mapgen_set(s, "spawns", 2);
        om_mapgen_set_seed(s, 3);
        // The additions (coherence, calm biomes and terrain, lava rock, volcano
        // sizes, craters, cracks, debris) all off: the generator as before.
        const char* additions[] = {"biome_calm", "coherence", "calm_terrain", "lava_rock", "volcanoes_small",
                                   "volcanoes_medium", "volcanoes_large", "craters", "cracks", "debris"};
        for (const char* k : additions) CHECK(om_mapgen_set(s, k, 0) == 0);
        // Golden map (tests/vectors/mapgen_learned.json), the same for any thread count.
        std::string hashes[2];
        for (int i = 0; i < 2 && m; ++i) {
            om_mapgen_result* r = om_mapgen_generate_learned(p, m, s, OM_LEARNED_FAST, i == 0 ? 1 : 3, nullptr, nullptr,
                                                             err, sizeof err);
            CHECK(r != nullptr);
            if (!r) continue;
            std::vector<uint16_t> t(64 * 64);
            om_mapgen_get_tiles(r, t.data(), 64 * 64);
            char hx[17];
            std::snprintf(hx, sizeof hx, "%016llx",
                          static_cast<unsigned long long>(hashGrid(std::vector<int>(t.begin(), t.end()))));
            hashes[i] = hx;
            CHECK(om_mapgen_stat(r, "connected") == 1.0 && om_mapgen_point_count(r, OM_MAPGEN_SPAWNS) == 2);
            int32_t sw = 0, sh = 0;
            CHECK(om_mapgen_sketch_size(r, &sw, &sh) == 32 * 32 && sw == 32 && sh == 32);
            if (i == 0)
                std::printf("  passable %.3f, plateau %.3f, build %.3f, hash %s\n", om_mapgen_stat(r, "passable"),
                            om_mapgen_stat(r, "plateau"), om_mapgen_stat(r, "build"), hx);
            om_mapgen_result_free(r);
        }
        CHECK(hashes[0] == hashes[1]);
        CHECK(hashes[0] == "05a14ace030553ef");
        // The template with all additions (tests/vectors/mapgen_learned_full.json), 128 x 128.
        {
            om_mapgen_settings* f = om_mapgen_settings_create("learned");
            CHECK(om_mapgen_get(f, "coherence") == 0.6 && om_mapgen_get(f, "volcanoes_medium") == 1);
            om_mapgen_set(f, "width", 128);
            om_mapgen_set(f, "height", 128);
            om_mapgen_set(f, "spawns", 2);
            om_mapgen_set_seed(f, 3);
            std::string full[2];
            for (int i = 0; i < 2 && m; ++i) {
                om_mapgen_result* r = om_mapgen_generate_learned(p, m, f, OM_LEARNED_FAST, i == 0 ? 1 : 8, nullptr,
                                                                 nullptr, err, sizeof err);
                CHECK(r != nullptr);
                if (!r) continue;
                std::vector<uint16_t> t(128 * 128);
                om_mapgen_get_tiles(r, t.data(), 128 * 128);
                char hx[17];
                std::snprintf(hx, sizeof hx, "%016llx",
                              static_cast<unsigned long long>(hashGrid(std::vector<int>(t.begin(), t.end()))));
                full[i] = hx;
                CHECK(om_mapgen_stat(r, "connected") == 1.0);
                const int volcanoes = om_mapgen_point_count(r, OM_MAPGEN_VOLCANOES);
                CHECK(volcanoes >= 1 && om_mapgen_lava_zone_count(r) >= 1);
                if (i == 0)
                    std::printf("  all additions: passable %.3f, plateau %.3f, %d volcanoes, hash %s\n",
                                om_mapgen_stat(r, "passable"), om_mapgen_stat(r, "plateau"), volcanoes, hx);
                om_mapgen_result_free(r);
            }
            CHECK(full[0] == full[1]);
            CHECK(full[0] == "394c43ade7da8a3d");
            // A gradient: flat sand in the north, rugged in the south.
            om_mapgen_set(f, "gradient", 1);
            om_mapgen_set(f, "gradient_ground_start", 2);
            om_mapgen_set(f, "gradient_terrain_start", 0.05);
            om_mapgen_set(f, "gradient_terrain_end", 0.95);
            om_mapgen_result* r = m ? om_mapgen_generate_learned(p, m, f, OM_LEARNED_FAST, 0, nullptr, nullptr, err, sizeof err)
                                    : nullptr;
            CHECK(r != nullptr);
            if (r) {
                std::vector<uint8_t> cells(128 * 128);
                om_mapgen_get_cell_types(r, cells.data(), 128 * 128);
                int north = 0, south = 0;
                for (int y = 0; y < 128; ++y)
                    for (int x = 0; x < 128; ++x) {
                        const uint8_t c = cells[size_t(y) * 128 + x];
                        const int cliff = c == 8 || c == 9 || c == 10;
                        (y < 32 ? north : y >= 96 ? south : north) += y < 32 || y >= 96 ? cliff : 0;
                    }
                std::printf("  gradient: cliff cells north %d, south %d\n", north, south);
                CHECK(north < south);
                om_mapgen_result_free(r);
            }
            om_mapgen_settings_free(f);
        }
        // Sketch only: no tiles, a sketch with walls.
        om_mapgen_result* r = om_mapgen_generate_learned(p, m, s, OM_LEARNED_SKETCH, 0, nullptr, nullptr, err, sizeof err);
        CHECK(r != nullptr && om_mapgen_width(r) == 0);
        if (r) {
            std::vector<uint8_t> walls(32 * 32), grounds(32 * 32);
            CHECK(om_mapgen_get_sketch(r, walls.data(), grounds.data(), 32 * 32) == 32 * 32);
            int any = 0;
            for (uint8_t w : walls) any += w > 0 && w <= 8;
            CHECK(any > 0);
        }
        om_mapgen_result_free(r);
        // Cancel from the progress callback.
        auto stop = [](double, void*) -> int { return 1; };
        CHECK(om_mapgen_generate_learned(p, m, s, OM_LEARNED_FAST, 0, stop, nullptr, err, sizeof err) == nullptr);
        CHECK(std::string(err) == "cancelled");
        // Sizes must be multiples of 8.
        om_mapgen_set(s, "width", 60);
        CHECK(om_mapgen_generate_learned(p, m, s, OM_LEARNED_FAST, 0, nullptr, nullptr, err, sizeof err) == nullptr);
        CHECK(om_mapgen_generate_learned(p, nullptr, s, OM_LEARNED_FAST, 0, nullptr, nullptr, err, sizeof err) == nullptr);
        om_mapgen_settings_free(s);
        om_mapgen_model_free(m);
    }

    // --- Null safety ---------------------------------------------------------
    CHECK(om_blend_resolve(nullptr, nullptr, 0) == -1);
    CHECK(om_blend_ground_at(nullptr, 0, 0) == -1);
    CHECK(om_blend_paint(a.map, 999, nullptr, 0) == -1);

    om_profile_free(p);
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

// Cliff drawing: polyline -> rock band on a 4x fine grid -> cliff tiles.
//
// Input is a polyline in cell coordinates of a local area, the side the high
// ground is on, a wall height and a seed. Output (CliffResult) is one cliff
// tile index per local cell, or -1 where nothing is placed. The caller
// (BlendMap::drawCliff) maps the local area onto the map and applies the tiles.
//
// A port of op2-codegen-editor's cliff tool (MIT licence, by fefi-byte):
// mapview/klippe_gelaende.py for the band, mapview/klippe_algo.py for the
// rules and the solver. Many names and comments refer to the German
// identifiers of the original (given in parentheses) so the two can be read
// side by side. The steps:
//
//   1. band     - draw a rock band along the line on a grid of 4x4 sub-cells
//                 per tile. Walls whose high side is north become a thick band
//                 (thickness from the wall height, wandering randomly); lines
//                 whose high side is south ("north cliffs") become a single
//                 sub-row; steep ones become stairs.
//   2. fill rule - notch full rock cells so a wall has 0-2 filler tiles.
//   3. terrain  - make the band buildable: no rock on an edge facing open
//                 ground, neighbouring cliff cells share their edge rock, and
//                 cells no tile can match are adjusted to the nearest tile.
//   4. solver   - backtracking search with forward checking, in stages from
//                 strict (only tile pairs seen in the original maps, exact
//                 edges) to looser, preferring tiles whose inner image matches
//                 the band, good-looking seams and little repetition.
//   5. fallback - if no stage solves it, a greedy per-cell choice.
//
// Differences from the Python original:
//   - Random numbers come from om::Rng, so shapes match the original in kind
//     but not tile for tile.
//   - The solver stops after a number of search steps instead of 0.15 s of
//     wall-clock time, so results never depend on machine speed.
//   - naht_haelt picks the widest point of the shared edge along the edge
//     (the original indexed it across, which looks like a transposition).
//
// Determinism: every random draw goes through one Rng seeded by the caller,
// in a fixed order, and every sort breaks ties on the tile index. Changing
// the order of draws or the tie-breaks changes the output (and the golden
// test hashes).
#include "cliff.hpp"

#include "json.hpp"
#include "rng.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace om {

namespace {

// --- constants (names after the Python originals) ---------------------------
//
// Tuning values copied from the original. Lengths are in tiles unless noted.
// "Straight" means an axis-parallel segment, "side wall" a segment whose band
// grows sideways (high side east or west), "north line" a segment whose high
// side is south. Some values are 0 or 1 and so have no effect; they are kept
// so the formulas stay the same as the original's.

constexpr int F = 4;                          // FEIN: sub-cells per tile side
constexpr double kUnruhe = 0.35;              // UNRUHE: how much the wall top wanders (tiles)
constexpr double kGeradeUnruhe = 0.0;         // GERADE_UNRUHE: share of the wandering kept on straight segments
constexpr double kGeradeDicke = 0.15;         // GERADE_DICKE: share of the thickness variation kept on straight segments
constexpr double kGeradeUebergang = 2.0;      // GERADE_UEBERGANG (tiles): blend length between straight and sloped parts
constexpr double kNordMin = 0.25, kNordMax = 0.5;   // north line thickness (tiles)
constexpr double kEndeLaenge = 3.5;           // taper length at line ends (tiles)
constexpr double kEndeAnteilMax = 0.25;       // ... at most this share of the line
constexpr double kEndeRest = 0.0;             // thickness left at the very end
constexpr double kSeiteDicke = 1.0;           // side walls: thickness factor
constexpr double kSeiteUnruhe = 0.4;          // side walls: wandering factor
constexpr double kSchraegeStueckLo = 2.0, kSchraegeStueckHi = 3.0;   // 45-degree piece length
constexpr double kSteilStueckLo = 1.0, kSteilStueckHi = 1.0;          // steep side walls: 45-degree piece length
constexpr double kSteilGleichmass = 4.0;      // steep side walls: Dirichlet evenness of the straight runs
constexpr double kNordStufeLo = 1.0, kNordStufeHi = 2.0;   // north stair step height (cells)
constexpr double kNordGleichmass = 20.0;      // north stairs: Dirichlet evenness of the tread widths
constexpr double kNordHaken = 0.0;            // north stairs: horizontal "hook" per step (0 = vertical risers)
constexpr double kNordAbsatzDicke = 1.5;      // rock thickness of a stair's small side wall
constexpr double kNordTreppeAb = 0.6;         // stairs from this slope (rows per column)
constexpr int kNordZeile = 1;                 // north line sits in the cell's second sub-row
constexpr int kOffenFelsMax = 2;              // OFFEN_FELS_MAX: inner-image rock pixels on an edge that count as rock
constexpr int kHart = 99;                     // mismatch score for rock facing open ground
constexpr double kNahtSpielraum = 3.0;        // NAHT_SPIELRAUM: greedy may take a seam this much worse to avoid repeats
constexpr int kNahtRunden = 4;                // NAHT_RUNDEN: greedy seam-improvement rounds
constexpr double kWallMin = 3.0, kWallMax = 6.0;   // allowed wall height (tiles); input is clamped

/// Solver stages: (only pairs seen in the originals, max edge mismatches).
/// Tried in order; the first stage that finds a full assignment wins. Stage 0
/// is the strictest in pairing (only neighbour pairs seen in the campaign
/// maps); later stages allow any pair whose edge codes fit and tolerate more
/// cells whose tile edges differ from the band.
const std::pair<bool, int> kStages[] = {
    {true, 4}, {false, 0}, {false, 1}, {false, 2}, {false, 3}, {false, 4}, {false, 5}, {false, 6}};

/// Search-step budget per stage (replaces the original's 0.15 s per stage).
int stepBudget(int cells) { return 20000 + 200 * cells; }

// Neighbour offsets per side (N, E, S, W; y grows downwards) and the
// opposite side, i.e. the side of the neighbour that touches side s.
const int DX[4] = {0, 1, 0, -1};
const int DY[4] = {-1, 0, 1, 0};
const int OPP[4] = {kS, kW, kN, kE};

// --- fine grid ----------------------------------------------------------------

/// The rock band: a bitmap of F x F sub-cells per map cell. A cell "is a cliff
/// cell" when any of its sub-cells is rock. A cell's edge segments are its
/// outermost sub-cells on each side, so they line up with a tile's edge codes.
struct Fine {
    int w = 0, h = 0;                  // in cells
    std::vector<uint8_t> v;            // (h*F) rows x (w*F) columns, 1 = rock

    Fine() = default;
    Fine(int w_, int h_) : w(w_), h(h_), v(static_cast<size_t>(w_) * F * h_ * F, 0) {}
    int fw() const { return w * F; }   // width in sub-cells
    int fh() const { return h * F; }   // height in sub-cells
    // at: by fine (sub-cell) coordinates. sub/setSub: by cell (x, y) and the
    // sub-cell's row r and column c inside it. No bounds checks.
    uint8_t& at(int fy, int fx) { return v[static_cast<size_t>(fy) * fw() + fx]; }
    uint8_t at(int fy, int fx) const { return v[static_cast<size_t>(fy) * fw() + fx]; }
    bool sub(int x, int y, int r, int c) const { return at(y * F + r, x * F + c) != 0; }
    void setSub(int x, int y, int r, int c, bool b) { at(y * F + r, x * F + c) = b ? 1 : 0; }

    // Per-cell summaries: any rock, all rock (a "filler" cell), share of rock.
    bool cellAny(int x, int y) const {
        for (int r = 0; r < F; ++r)
            for (int c = 0; c < F; ++c)
                if (sub(x, y, r, c)) return true;
        return false;
    }
    bool cellAll(int x, int y) const {
        for (int r = 0; r < F; ++r)
            for (int c = 0; c < F; ++c)
                if (!sub(x, y, r, c)) return false;
        return true;
    }
    double cellMean(int x, int y) const {
        int n = 0;
        for (int r = 0; r < F; ++r)
            for (int c = 0; c < F; ++c) n += sub(x, y, r, c);
        return n / double(F * F);
    }
    bool anyRock() const {
        return std::any_of(v.begin(), v.end(), [](uint8_t b) { return b != 0; });
    }

    /// Edge segment i of side s (N/S left to right, E/W top to bottom), as the
    /// sub-cell (r, c) inside the cell. Same order as CliffTile::edges, so
    /// segment i of side s faces segment i of side OPP[s] in the neighbour.
    static void edgePos(int s, int i, int& r, int& c) {
        switch (s) {
        case kN: r = 0; c = i; break;
        case kE: r = i; c = F - 1; break;
        case kS: r = F - 1; c = i; break;
        default: r = i; c = 0; break;
        }
    }
    bool edge(int x, int y, int s, int i) const {
        int r, c;
        edgePos(s, i, r, c);
        return sub(x, y, r, c);
    }
    void setEdge(int x, int y, int s, int i, bool b) {
        int r, c;
        edgePos(s, i, r, c);
        setSub(x, y, r, c, b);
    }
};

/// Cells holding any rock, as a w*h row-major mask ("hat" = has rock). The
/// rules below recompute it whenever the band may have changed.
std::vector<uint8_t> cellsWithRock(const Fine& f) {
    std::vector<uint8_t> hat(static_cast<size_t>(f.w) * f.h, 0);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) hat[static_cast<size_t>(y) * f.w + x] = f.cellAny(x, y);
    return hat;
}

/// Side s of cell (x, y) faces open ground: the neighbour has no rock or is outside.
bool isOpen(const std::vector<uint8_t>& hat, int w, int h, int x, int y, int s) {
    const int nx = x + DX[s], ny = y + DY[s];
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return true;
    return hat[static_cast<size_t>(ny) * w + nx] == 0;
}

// --- 1. band ------------------------------------------------------------------

/// numpy.convolve(a, ones(L)/L, mode="same"): a box blur of width L that keeps
/// the length of a, treating values outside a as 0. The centring offset
/// matches numpy for even L as well.
std::vector<double> boxSame(const std::vector<double>& a, int L) {
    const int n = static_cast<int>(a.size());
    std::vector<double> out(a.size(), 0.0);
    const int shift = (L - 1) / 2;
    for (int i = 0; i < n; ++i) {
        double s = 0;
        for (int j = 0; j < L; ++j) {
            const int k = i + shift - j;
            if (k >= 0 && k < n) s += a[k];
        }
        out[i] = s / L;
    }
    return out;
}

/// Smooth random profile of n values with standard deviation `strength`
/// (_glatt: white noise box-blurred twice with window `len`).
std::vector<double> smoothNoise(int n, Rng& rng, double strength, int len) {
    len = std::max(1, len);
    // Generate at least 8 windows' worth, plus a margin of len on each side
    // that is cut off, so the zero padding of boxSame barely shows and the
    // standard deviation is measured on enough samples.
    const int m = std::max(n, 8 * len);
    std::vector<double> r(static_cast<size_t>(m + 2 * len));
    for (double& v : r) v = rng.normal();
    std::vector<double> g = boxSame(boxSame(r, len), len);
    g = std::vector<double>(g.begin() + len, g.begin() + len + m);
    const double mean = std::accumulate(g.begin(), g.end(), 0.0) / m;
    double var = 0;
    for (double v : g) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / m);
    g.resize(static_cast<size_t>(n));
    if (sd > 1e-9)
        for (double& v : g) v = v / sd * strength;
    return g;
}

/// numpy.interp(x, xs, ys): piecewise linear, clamped at both ends. xs must
/// be ascending; where two xs are equal (a vertical step) the later y wins.
double interp(double x, const std::vector<double>& xs, const std::vector<double>& ys) {
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    const auto it = std::upper_bound(xs.begin(), xs.end(), x);
    const size_t i = static_cast<size_t>(it - xs.begin()) - 1;
    const double dx = xs[i + 1] - xs[i];
    if (dx <= 0) return ys[i + 1];
    return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / dx;
}

/// Python's round(): halves go to the even neighbour. nearbyint does this in
/// the default floating-point rounding mode, which the library never changes.
int roundHalfEven(double v) { return static_cast<int>(std::nearbyint(v)); }

/// A piecewise linear profile as (xs, ys) breakpoints, evaluated with interp.
using Profile2 = std::pair<std::vector<double>, std::vector<double>>;

/// A slope split into flat runs and 45-degree pieces (_schraege_profil).
/// lx is the run along the main axis, ly the rise (both >= 0, in tiles).
/// Returns a profile from (0, 0) to (lx, ly): k pieces of 45 degrees, each
/// about pieceLo..pieceHi long, with k + 1 flat runs between them whose
/// lengths are drawn from a Dirichlet distribution (larger `evenness` = more
/// equal runs). Nearly flat or nearly 45-degree slopes stay a straight line.
Profile2 slopeProfile(double lx, double ly, Rng& rng, double pieceLo, double pieceHi, double evenness) {
    if (ly < 0.75 || ly >= 0.9 * lx) return {{0.0, lx}, {0.0, ly}};
    const int k = std::max(1, roundHalfEven(ly / rng.uniform(pieceLo, pieceHi)));
    const double piece = ly / k;
    const std::vector<double> share = rng.dirichlet(k + 1, evenness);
    std::vector<double> xs{0.0}, ys{0.0};
    for (int i = 0; i <= k; ++i) {
        xs.push_back(xs.back() + (lx - ly) * share[i]);
        ys.push_back(ys.back());
        if (i < k) {
            xs.push_back(xs.back() + piece);
            ys.push_back(ys.back() + piece);
        }
    }
    xs.back() = lx;   // absorb rounding so the profile ends exactly at lx
    return {xs, ys};
}

/// A steep north line as stairs (_nord_treppe). Same shape of result as
/// slopeProfile, but the rises are vertical steps (risers) of 1..2 cells with
/// flat treads between them, because a north line can't run diagonally.
/// drawBand puts a small side wall at each riser.
Profile2 northStairs(double lx, double ly, Rng& rng) {
    const int steps = std::max(1, roundHalfEven(ly));
    // Split the rise into whole-cell step heights.
    std::vector<int> heights;
    int rest = steps;
    while (rest > 0) {
        const int h = std::max(1, static_cast<int>(std::min<double>(rest, roundHalfEven(rng.uniform(kNordStufeLo, kNordStufeHi)))));
        heights.push_back(h);
        rest -= h;
    }
    const int k = static_cast<int>(heights.size());
    std::vector<double> hooks;
    for (int h : heights) hooks.push_back(std::min(kNordHaken * h, 0.9 * lx / k));
    const double hookSum = std::accumulate(hooks.begin(), hooks.end(), 0.0);
    const std::vector<double> share = rng.dirichlet(k + 1, kNordGleichmass);
    std::vector<double> xs{0.0}, ys{0.0};
    double y = 0;
    for (int i = 0; i <= k; ++i) {
        xs.push_back(xs.back() + std::max(0.0, lx - hookSum) * share[i]);
        ys.push_back(y);
        if (i < k) {
            y += heights[i] * ly / steps;
            // Keep xs strictly ascending even for a vertical riser, so interp
            // has a defined step.
            xs.push_back(xs.back() + std::max(1e-6, hooks[i]));
            ys.push_back(y);
        }
    }
    xs.back() = std::max(xs.back(), lx);
    return {xs, ys};
}

/// Which way a segment's wall faces (_wand_achse): axis 'v' (band runs
/// vertically from the line: wall faces N/S) or 'h', and the direction the
/// band grows from the line (towards the low side).
/// dir is +1 (south / east) or -1 (north / west). On a 'v' segment dir < 0
/// means the high side is south, which drawBand turns into a north line.
void wallAxis(double x0, double y0, double x1, double y1, CliffHighSide side, char& axis, int& dir) {
    const double dx = x1 - x0, dy = y1 - y0;
    const bool flat = std::abs(dx) >= std::abs(dy);
    if (side == CliffHighSide::North || side == CliffHighSide::South) {
        // Map-fixed sides: a steep segment always becomes a side wall
        // growing east.
        if (flat) {
            axis = 'v';
            dir = side == CliffHighSide::North ? 1 : -1;
        } else {
            axis = 'h';
            dir = 1;
        }
        return;
    }
    // Left/Right are relative to the drawing direction; the normal points at
    // the high side, and the band grows the other way.
    double lx = dy, ly = -dx;       // left normal in screen coordinates (y down)
    if (side == CliffHighSide::Right) {
        lx = -lx;
        ly = -ly;
    }
    if (flat) {
        axis = 'v';
        dir = ly > 0 ? -1 : 1;
    } else {
        axis = 'h';
        dir = lx > 0 ? -1 : 1;
    }
}

/// One end of a run of north line: solveCliffLine later extends the line to
/// the cell's edge on `side` and allows that edge to face open ground.
struct NorthEnd {
    int cx, cy;     // cell
    int side;       // kW or kE
    int row;        // fine row
};

/// Turns the north-line rock into one sub-row per column, joined where the row
/// changes, and records where each run ends (_nordlinie_durchziehen).
/// northLine marks the sub-cells drawBand painted for north segments. Each
/// such column is cleared and replaced by a single sub-row (kNordZeile of the
/// cell its centre falls in). Adjacent columns whose rows differ are joined
/// with a vertical piece so the line has no gaps. axis gets 1 for every
/// cell the line passes. ends receives both ends of every run of columns.
void pullNorthLine(Fine& fine, std::vector<uint8_t>& axis, const std::vector<uint8_t>& northLine,
                   std::vector<NorthEnd>& ends) {
    const int fw = fine.fw(), fh = fine.fh();
    std::map<int, std::pair<int, int>> columns;   // fine x -> (first row, last row), ascending x
    for (int x = 0; x < fw; ++x) {
        int lo = -1, hi = -1;
        for (int y = 0; y < fh; ++y)
            if (northLine[static_cast<size_t>(y) * fw + x]) {
                if (lo < 0) lo = y;
                hi = y;
            }
        if (lo < 0) continue;
        for (int y = lo; y <= hi; ++y) fine.at(y, x) = 0;
        const double mid = (lo + hi) / 2.0;
        const int row = std::min(fh - 1, static_cast<int>(mid) / F * F + kNordZeile);
        columns[x] = {row, row};
    }
    bool havePrev = false;
    int prevX = 0;
    std::pair<int, int> prev{0, 0};
    std::vector<std::vector<int>> runs;
    for (auto& [x, range] : columns) {
        int y0 = range.first, y1 = range.second;
        if (havePrev && prevX == x - 1) {
            // Continues the previous column: stretch this column towards the
            // previous one's row so the two touch.
            if (y0 > prev.second + 1) y0 = prev.second + 1;
            else if (y1 < prev.first - 1) y1 = prev.first - 1;
            runs.back().push_back(x);
        } else {
            runs.push_back({x});
        }
        for (int y = y0; y <= y1; ++y) fine.at(y, x) = 1;
        for (int cy = y0 / F; cy <= y1 / F; ++cy) axis[static_cast<size_t>(cy) * fine.w + x / F] = 1;
        havePrev = true;
        prevX = x;
        prev = {y0, y1};
    }
    // Each run's ends: fill the row out to the cell's west / east edge, so the
    // line ends on a cell boundary, and remember the end.
    for (const auto& run : runs) {
        for (int k = 0; k < 2; ++k) {
            const int x = k == 0 ? run.front() : run.back();
            const int side = k == 0 ? kW : kE;
            const int row = columns[x].first;
            const int cx = x / F;
            if (side == kW)
                for (int fx = cx * F; fx <= x; ++fx) fine.at(row, fx) = 1;
            else
                for (int fx = x; fx < (cx + 1) * F; ++fx) fine.at(row, fx) = 1;
            ends.push_back({cx, row / F, side, row});
        }
    }
}

/// Draws the rock band along the polyline (band_aus_linie). axis per cell:
/// 1 = band runs vertically (wall faces N/S), 2 = horizontally.
///
/// pts are cell coordinates; the line goes through cell centres. thickMin /
/// thickMax are the wall band thickness range in tiles. taperEnds thins the
/// band towards both ends (off for closed loops). Outputs: fine (the band,
/// sized width x height cells), axis (per cell, 0 = no band) and northEnds.
///
/// The line is walked in steps of one sub-cell. At each step a strip of rock
/// is painted across the line: vertically for 'v' segments, horizontally for
/// 'h' segments, starting at the line (plus wander) and growing `thickness`
/// towards the low side. Random draws happen in a fixed order: wander, thickness,
/// then per segment the slope / stair profiles.
void drawBand(const std::vector<std::pair<int, int>>& pts, CliffHighSide side, int width, int height,
              Rng& rng, double thickMin, double thickMax, bool taperEnds,
              Fine& fine, std::vector<uint8_t>& axis, std::vector<NorthEnd>& northEnds) {
    fine = Fine(width, height);
    axis.assign(static_cast<size_t>(width) * height, 0);
    const int fw = fine.fw(), fh = fine.fh();

    auto segSteps = [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return static_cast<int>(std::max(std::abs(b.first - a.first), std::abs(b.second - a.second)) * F) + 1;
    };
    int steps = 0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) steps += segSteps(pts[i], pts[i + 1]);
    if (steps < 2) return;

    // Per step along the whole polyline: an offset of the wall top (wander)
    // and a 0..1 position within the thickness range (share).
    std::vector<double> wander = smoothNoise(steps, rng, kUnruhe, 6 * F);
    std::vector<double> thickR = smoothNoise(steps, rng, 1.0, 10 * F);
    std::vector<double> share(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) share[i] = 0.5 + 0.5 * std::tanh(thickR[i]);

    // Straight (axis-parallel) segments wander less and keep a steadier thickness.
    std::vector<double> sloped(static_cast<size_t>(steps), 1.0);
    {
        int s = 0;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const int n = segSteps(pts[i], pts[i + 1]);
            if (pts[i].first == pts[i + 1].first || pts[i].second == pts[i + 1].second)
                for (int k = 0; k < n; ++k) sloped[s + k] = 0.0;
            s += n;
        }
        const bool anyStraight = std::any_of(sloped.begin(), sloped.end(), [](double v) { return v == 0.0; });
        const bool anySloped = std::any_of(sloped.begin(), sloped.end(), [](double v) { return v != 0.0; });
        if (anyStraight && anySloped) {
            // Blur the 0/1 mask (edge-padded) so the change between straight
            // and sloped parts is gradual.
            const int b = std::max(1, static_cast<int>(kGeradeUebergang * F));
            std::vector<double> padded;
            for (int k = 0; k < b; ++k) padded.push_back(sloped.front());
            padded.insert(padded.end(), sloped.begin(), sloped.end());
            for (int k = 0; k < b; ++k) padded.push_back(sloped.back());
            const std::vector<double> blurred = boxSame(padded, 2 * b + 1);
            sloped = std::vector<double>(blurred.begin() + b, blurred.begin() + b + steps);
        }
    }
    std::vector<double> thick(static_cast<size_t>(steps)), thickNorth(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) {
        wander[i] *= kGeradeUnruhe + (1 - kGeradeUnruhe) * sloped[i];
        const double a = 0.5 + (share[i] - 0.5) * (kGeradeDicke + (1 - kGeradeDicke) * sloped[i]);
        thick[i] = thickMin + (thickMax - thickMin) * a;
        thickNorth[i] = kNordMin + (kNordMax - kNordMin) * a;
    }

    std::vector<uint8_t> northLine(fine.v.size(), 0);   // sub-cells painted by north segments
    struct Ledge { double x, ya, yb; int dir; };          // a stair riser: x, rows ya..yb, side its wall grows to
    std::vector<Ledge> ledges;

    int s = 0;   // step index along the whole polyline
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double x0 = pts[i].first, y0 = pts[i].second, x1 = pts[i + 1].first, y1 = pts[i + 1].second;
        char ax;
        int ri;
        wallAxis(x0, y0, x1, y1, side, ax, ri);
        const int n = segSteps(pts[i], pts[i + 1]);
        const double adx = std::abs(x1 - x0), ady = std::abs(y1 - y0);

        // 'v' segments: y as a function of x. A wall slope becomes flat runs
        // and 45-degree pieces; a steep north line becomes stairs, and each
        // riser is recorded as a ledge.
        bool hasProfile = false;
        Profile2 profile;
        if (ax == 'v' && ri > 0 && ady > 0) {
            profile = slopeProfile(adx, ady, rng, kSchraegeStueckLo, kSchraegeStueckHi, 2.0);
            hasProfile = true;
        } else if (ax == 'v' && ri < 0 && ady >= kNordTreppeAb * adx) {
            profile = northStairs(adx, ady, rng);
            hasProfile = true;
            const int sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1;
            for (size_t j = 0; j + 1 < profile.first.size(); ++j) {
                if (profile.second[j + 1] != profile.second[j]) {
                    const double ya = y0 + sy * profile.second[j], yb = y0 + sy * profile.second[j + 1];
                    ledges.push_back({x0 + sx * profile.first[j], ya, yb, yb > ya ? sx : -sx});
                }
            }
        }
        // 'h' segments (side walls): x as a function of y, same idea.
        bool hasSteep = false;
        Profile2 steep;
        if (ax == 'h' && adx > 0) {
            steep = slopeProfile(ady, adx, rng, kSteilStueckLo, kSteilStueckHi, kSteilGleichmass);
            hasSteep = true;
        }
        const double sgnX = (x1 > x0) - (x1 < x0), sgnY = (y1 > y0) - (y1 < y0);

        for (int k = 0; k < n; ++k) {
            // (px, py): this step's point on the line, in fine coordinates
            // (+0.5 puts cell coordinates at the cell centre).
            const double u = static_cast<double>(k) / std::max(1, n - 1);
            const double px = hasSteep ? (x0 + sgnX * interp(ady * u, steep.first, steep.second) + 0.5) * F
                                       : (x0 + (x1 - x0) * u + 0.5) * F;
            const double py = hasProfile ? (y0 + sgnY * interp(adx * u, profile.first, profile.second) + 0.5) * F
                                         : (y0 + (y1 - y0) * u + 0.5) * F;
            const int si = std::min(s, steps - 1);
            double w = wander[si] * F;
            double d = thick[si] * F;

            // Taper: thickness falls off with the square root of the distance
            // to the nearer end of the whole polyline.
            const double edgeDist = std::min(s, steps - 1 - s) / double(F);
            const double taperLen = std::min(kEndeLaenge, kEndeAnteilMax * steps / F);
            if (taperEnds && edgeDist < taperLen) d *= kEndeRest + (1 - kEndeRest) * std::sqrt(edgeDist / taperLen);
            if (ax == 'h') {
                d *= kSeiteDicke;
                w *= kSeiteUnruhe;
            }
            const bool north = ax == 'v' && ri < 0;
            if (north) d = thickNorth[si] * F;   // north lines: thin, no taper
            ++s;

            // Paint the strip [from, to) across the line; ri picks the side.
            if (ax == 'v') {
                const int fx = static_cast<int>(px);
                if (fx < 0 || fx >= fw) continue;
                const double a = py + w;
                const double from = ri > 0 ? a : a + d * ri, to = ri > 0 ? a + d * ri : a;
                const int v0 = std::max(0, roundHalfEven(from)), v1 = std::min(fh, roundHalfEven(to));
                if (v1 > v0) {
                    for (int fy = v0; fy < v1; ++fy) {
                        fine.at(fy, fx) = 1;
                        if (north) northLine[static_cast<size_t>(fy) * fw + fx] = 1;
                    }
                    for (int cy = v0 / F; cy <= (v1 - 1) / F; ++cy) axis[static_cast<size_t>(cy) * width + fx / F] = 1;
                }
            } else {
                const int fy = static_cast<int>(py);
                if (fy < 0 || fy >= fh) continue;
                const double a = px + w;
                const double from = ri > 0 ? a : a + d * ri, to = ri > 0 ? a + d * ri : a;
                const int v0 = std::max(0, roundHalfEven(from)), v1 = std::min(fw, roundHalfEven(to));
                if (v1 > v0) {
                    for (int fx = v0; fx < v1; ++fx) fine.at(fy, fx) = 1;
                    for (int cx = v0 / F; cx <= (v1 - 1) / F; ++cx) axis[static_cast<size_t>(fy / F) * width + cx] = 2;
                }
            }
        }
    }

    pullNorthLine(fine, axis, northLine, northEnds);

    // Small side walls at each stair step: a block kNordAbsatzDicke wide
    // beside the riser, covering the north-line rows of both treads. These
    // cells count as horizontal band (axis 2) for the fill rule.
    for (const Ledge& l : ledges) {
        int r0 = static_cast<int>(std::min(l.ya, l.yb)) * F + kNordZeile;
        int r1 = static_cast<int>(std::max(l.ya, l.yb)) * F + kNordZeile + 1;
        const double fx = (l.x + 0.5) * F, d = kNordAbsatzDicke * F;
        double c0d = l.dir > 0 ? fx : fx - d, c1d = l.dir > 0 ? fx + d : fx;
        const int c0 = std::max(0, roundHalfEven(c0d)), c1 = std::min(fw, roundHalfEven(c1d));
        r0 = std::max(0, r0);
        r1 = std::min(fh, r1);
        if (c1 > c0 && r1 > r0) {
            for (int fy = r0; fy < r1; ++fy)
                for (int fxx = c0; fxx < c1; ++fxx) fine.at(fy, fxx) = 1;
            for (int cy = r0 / F; cy <= (r1 - 1) / F; ++cy)
                for (int cx = c0 / F; cx <= (c1 - 1) / F; ++cx) axis[static_cast<size_t>(cy) * width + cx] = 2;
        }
    }
}

// --- 2. fill rule -----------------------------------------------------------------

/// A wall of height h may have h-2..h-1 filler (all-rock) tiles, at most two:
/// notch the top corner of the extra ones (fuellregel / _lauf_kerben).
/// A run is an unbroken line of band cells across the band: a column of
/// vertical-band cells or a row of horizontal-band cells, n cells long. The
/// run keeps a random number of all-rock cells (1..2 for long runs, fewer
/// for short ones), the last ones in run order (north / west first); the
/// other all-rock cells lose one or three sub-cells at a random top corner,
/// so the solver must give them an edge tile instead of a filler tile.
void fillRule(Fine& fine, const std::vector<uint8_t>& axis, Rng& rng) {
    const int w = fine.w, h = fine.h;
    const std::vector<uint8_t> hat = cellsWithRock(fine);   // taken once, before any notching
    auto notchRun = [&](const std::vector<std::pair<int, int>>& run) {
        const int n = static_cast<int>(run.size());
        std::vector<std::pair<int, int>> full;
        for (const auto& c : run)
            if (fine.cellAll(c.first, c.second)) full.push_back(c);
        const int lo = std::min(std::max(0, n - 2), 1);
        const int hi = std::min(std::max(0, n - 1), 2);
        const int target = static_cast<int>(rng.integers(lo, hi + 1));
        if (static_cast<int>(full.size()) <= target) return;
        for (size_t k = 0; k < full.size() - static_cast<size_t>(target); ++k) {
            const int x = full[k].first, y = full[k].second;
            const bool left = rng.integers(0, 2) != 0;   // which top corner
            const bool big = rng.integers(0, 2) != 0;    // 1 sub-cell, or an L of 3
            const int fx = x * F + (left ? 0 : F - 1), fy = y * F;
            fine.at(fy, fx) = 0;
            if (big) {
                fine.at(fy + 1, fx) = 0;
                fine.at(fy, fx + (left ? 1 : -1)) = 0;
            }
        }
    };
    // Columns of vertical-band cells, then rows of horizontal-band cells.
    // b runs one past the end so the last run is flushed.
    for (int pass = 0; pass < 2; ++pass) {
        const bool rows = pass == 1;
        const int across = rows ? h : w, along = rows ? w : h;
        for (int a = 0; a < across; ++a) {
            std::vector<std::pair<int, int>> run;
            for (int b = 0; b <= along; ++b) {
                const int x = rows ? b : a, y = rows ? a : b;
                const bool in = b < along && hat[static_cast<size_t>(y) * w + x] &&
                                axis[static_cast<size_t>(y) * w + x] == (rows ? 2 : 1);
                if (in) {
                    run.push_back({x, y});
                    continue;
                }
                if (!run.empty()) notchRun(run);
                run.clear();
            }
        }
    }
}

}  // namespace

// --- per-family tables -------------------------------------------------------------

/// Precomputed data for one cliff family (one ground), so the rules and the
/// solver never touch CliffTile directly. Tiles are addressed by table row
/// 0..n-1; ids maps a row back to the CliffData::tiles index. Pair tables
/// are n*n per side: entry [s][i*n+j] describes tile j placed on side s of
/// tile i.
struct CliffTable {
    int n = 0;
    std::vector<int> ids;                                    ///< cliff tile index per table row
    std::vector<std::array<std::array<int8_t, 4>, 4>> E;     ///< edge codes [side][segment]
    std::vector<double> fa;                                  ///< rock share of the edge codes
    std::vector<std::array<std::array<bool, 4>, 4>> I;       ///< inner image
    std::vector<uint8_t> hasInner;
    std::vector<std::array<bool, 4>> rockSide;               ///< side has any FELS
    std::vector<std::array<bool, 4>> pixRock;                ///< inner image edge has >= 2 rock
    std::vector<std::array<bool, 4>> groundSide;             ///< side: >= 3 BIOM and no FELS
    // seen: the pair occurs in the original maps. fits: the facing edge codes
    // agree (EGAL matches anything). hole: >= 3 facing segments are ground on
    // both sides, which would open a gap in the cliff.
    std::array<std::vector<uint8_t>, 4> seen, fits, hole;    ///< [side][i*n+j]
    // Mean absolute difference of the two tiles' rim pixels along the shared
    // edge (lower = better-looking seam).
    std::array<std::vector<float>, 4> seam;                  ///< [side][i*n+j], NaN = unknown
    std::array<std::vector<uint8_t>, 4> allowSeen, allowAny; ///< allowed pairs per solver mode

    /// May tile row j sit on side `side` of tile row i in this solver mode?
    uint8_t allowed(bool onlySeen, int side, int i, int j) const {
        return (onlySeen ? allowSeen : allowAny)[side][static_cast<size_t>(i) * n + j];
    }
};

CliffData::~CliffData() = default;

int CliffData::findFamily(const std::string& id) const {
    for (size_t i = 0; i < families.size(); ++i)
        if (families[i].id == id) return static_cast<int>(i);
    return -1;
}

bool CliffData::usable(int family) const {
    return family >= 0 && family < static_cast<int>(families.size()) &&
           static_cast<int>(families[family].tiles.size()) >= minTiles;
}

/// Builds the family's CliffTable on first use and caches it. Not thread-safe:
/// the cache is filled lazily through a const method. The family must be valid.
const CliffTable& CliffData::table(int family) const {
    if (tables_.size() < families.size()) tables_.resize(families.size());
    if (tables_[family]) return *tables_[family];

    auto t = std::make_shared<CliffTable>();
    const std::vector<int>& ids = families[family].tiles;
    const int n = static_cast<int>(ids.size());
    t->n = n;
    t->ids = ids;
    std::vector<int> row(tiles.size(), -1);   // tile index -> table row, -1 = other family
    for (int i = 0; i < n; ++i) row[ids[i]] = i;
    // Per-tile, per-side summaries used to score a tile against open sides.
    for (int i = 0; i < n; ++i) {
        const CliffTile& c = tiles[ids[i]];
        t->E.push_back(c.edges);
        t->fa.push_back(c.rockShare);
        t->I.push_back(c.inner);
        t->hasInner.push_back(c.hasInner);
        std::array<bool, 4> rock{}, pix{}, ground{};
        for (int s = 0; s < 4; ++s) {
            int biom = 0;
            for (int k = 0; k < 4; ++k) {
                if (c.edges[s][k] == kFels) rock[s] = true;
                if (c.edges[s][k] == kBiom) ++biom;
            }
            ground[s] = biom >= 3 && !rock[s];
            int edgeRock = 0;
            for (int k = 0; k < 4; ++k) {
                int r, col;
                Fine::edgePos(s, k, r, col);
                edgeRock += c.inner[r][col];
            }
            pix[s] = c.hasInner && edgeRock >= kOffenFelsMax;
        }
        t->rockSide.push_back(rock);
        t->pixRock.push_back(pix);
        t->groundSide.push_back(ground);
    }
    // Pair tables. Pairs "seen" in the originals are stored by tile index in
    // the profile; ones pointing at other families are dropped. A pair is
    // never allowed if it would leave a hole, even when seen.
    const size_t nn = static_cast<size_t>(n) * n;
    for (int s = 0; s < 4; ++s) {
        const int o = OPP[s];
        t->seen[s].assign(nn, 0);
        t->fits[s].assign(nn, 0);
        t->hole[s].assign(nn, 0);
        t->seam[s].assign(nn, std::numeric_limits<float>::quiet_NaN());
        t->allowSeen[s].assign(nn, 0);
        t->allowAny[s].assign(nn, 0);
        for (int i = 0; i < n; ++i) {
            const CliffTile& a = tiles[ids[i]];
            for (int u : a.seen[s])
                if (u >= 0 && u < static_cast<int>(row.size()) && row[u] >= 0) t->seen[s][static_cast<size_t>(i) * n + row[u]] = 1;
            for (int j = 0; j < n; ++j) {
                const CliffTile& b = tiles[ids[j]];
                bool fit = true;
                int bothGround = 0;
                for (int k = 0; k < 4; ++k) {
                    const int8_t x = a.edges[s][k], y = b.edges[o][k];
                    if (!(x == y || x == kEgal || y == kEgal)) fit = false;
                    if (x == kBiom && y == kBiom) ++bothGround;
                }
                const size_t ij = static_cast<size_t>(i) * n + j;
                t->fits[s][ij] = fit;
                t->hole[s][ij] = bothGround >= 3;
                if (!a.rims[s].empty() && !b.rims[o].empty()) {
                    int sum = 0;
                    for (size_t k = 0; k < a.rims[s].size(); ++k) sum += std::abs(int(a.rims[s][k]) - int(b.rims[o][k]));
                    t->seam[s][ij] = static_cast<float>(sum / double(a.rims[s].size()));
                }
            }
        }
        for (size_t ij = 0; ij < nn; ++ij) {
            t->allowSeen[s][ij] = t->seen[s][ij] && !t->hole[s][ij];
            t->allowAny[s][ij] = (t->seen[s][ij] || t->fits[s][ij]) && !t->hole[s][ij];
        }
    }
    tables_[family] = t;
    return *t;
}

/// Reads the profile's "cliffs" section. Each entry of "tiles" has: ts
/// (tileset index), img, family (ground id; families are created in order of
/// first appearance), edges (16 codes), and optionally inner (16 chars '0'/'1',
/// row-major), cell (cell type), seen ({n,e,s,w}: lists of tile indices into
/// this same list) and rims ({n,e,s,w}: hex RGB strings). Throws
/// std::runtime_error on malformed data.
void CliffData::parse(const json::Value& sec, int tilesetCount) {
    if (const json::Value* v = sec.find("minTiles")) minTiles = static_cast<int>(v->integer());
    if (const json::Value* v = sec.find("fallback")) fallback = v->string();
    const json::Array& list = sec.at("tiles").array();
    static const char* sideKeys[4] = {"n", "e", "s", "w"};
    for (const json::Value& e : list) {
        CliffTile c;
        c.tileset = static_cast<int>(e.at("ts").integer());
        c.image = static_cast<int>(e.at("img").integer());
        if (c.tileset < 0 || c.tileset >= tilesetCount) throw std::runtime_error("cliff tile refers to an unknown tileset index");
        const std::string fam = e.at("family").string();
        int f = findFamily(fam);
        if (f < 0) {
            families.push_back({fam, {}});
            f = static_cast<int>(families.size()) - 1;
        }
        c.family = f;
        // "edges": "NNNN EEEE SSSS WWWW", digits 0 = ground, 1 = rock, 2 = either
        const std::string& ed = e.at("edges").string();
        int s = 0, k = 0, rock = 0;
        for (char ch : ed) {
            if (ch == ' ') continue;
            if (ch < '0' || ch > '2' || s >= 4) throw std::runtime_error("bad cliff \"edges\" string");
            c.edges[s][k] = static_cast<int8_t>(ch - '0');
            rock += ch == '1';
            if (++k == 4) {
                k = 0;
                ++s;
            }
        }
        if (s != 4) throw std::runtime_error("cliff \"edges\" needs 16 codes");
        c.rockShare = rock / 16.0;
        if (const json::Value* in = e.find("inner"); in && in->isString()) {
            const std::string& str = in->string();
            if (str.size() != 16) throw std::runtime_error("cliff \"inner\" needs 16 characters");
            for (int i = 0; i < 16; ++i) c.inner[i / 4][i % 4] = str[i] == '1';
            c.hasInner = true;
        }
        if (const json::Value* cell = e.find("cell"); cell && cell->isNumber()) c.cellType = static_cast<int>(cell->integer());
        if (const json::Value* seen = e.find("seen")) {
            for (int sd = 0; sd < 4; ++sd)
                if (const json::Value* l = seen->find(sideKeys[sd]))
                    for (const json::Value& v : l->array()) c.seen[sd].push_back(static_cast<int>(v.integer()));
        }
        if (const json::Value* rims = e.find("rims"); rims && rims->isObject()) {
            for (int sd = 0; sd < 4; ++sd) {
                const std::string& hex = rims->at(sideKeys[sd]).string();
                std::vector<uint8_t> bytes;
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                    bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
                c.rims[sd] = std::move(bytes);
            }
        }
        families[f].tiles.push_back(static_cast<int>(tiles.size()));
        tiles.push_back(std::move(c));
    }
}

namespace {

// --- 3. terrain rules ----------------------------------------------------------------

/// Clears every edge line of a cliff cell that faces open ground (rand_dem_biom).
/// Repeats because clearing can empty a cell, which opens its neighbours' sides.
void clearOpenEdges(Fine& fine, int rounds = 3) {
    for (int r = 0; r < rounds; ++r) {
        const std::vector<uint8_t> hat = cellsWithRock(fine);
        bool changed = false;
        for (int y = 0; y < fine.h; ++y)
            for (int x = 0; x < fine.w; ++x) {
                if (!hat[static_cast<size_t>(y) * fine.w + x]) continue;
                for (int s = 0; s < 4; ++s) {
                    if (!isOpen(hat, fine.w, fine.h, x, y, s)) continue;
                    for (int k = 0; k < F; ++k)
                        if (fine.edge(x, y, s, k)) {
                            fine.setEdge(x, y, s, k, false);
                            changed = true;
                        }
                }
            }
        if (!changed) break;
    }
}

/// Where two neighbouring cliff cells both have an empty shared edge, joins
/// them with one rock segment at the widest point (naht_haelt).
/// Without this, two cliff cells touching along an all-ground edge would need
/// tiles that leave a gap between them. Pairs are collected before any change,
/// so a joint made here never stops another pair from qualifying.
void keepSeams(Fine& fine) {
    const std::vector<uint8_t> hat = cellsWithRock(fine);
    struct Pair { int x, y, s; };   // cell and side (E or S only, so each pair once)
    std::vector<Pair> pairs;
    for (int y = 0; y < fine.h; ++y)
        for (int x = 0; x < fine.w; ++x) {
            if (!hat[static_cast<size_t>(y) * fine.w + x]) continue;
            for (int s : {kE, kS}) {
                const int nx = x + DX[s], ny = y + DY[s];
                if (nx >= fine.w || ny >= fine.h || !hat[static_cast<size_t>(ny) * fine.w + nx]) continue;
                bool emptyA = true, emptyB = true;
                for (int k = 0; k < F; ++k) {
                    if (fine.edge(x, y, s, k)) emptyA = false;
                    if (fine.edge(nx, ny, OPP[s], k)) emptyB = false;
                }
                if (emptyA && emptyB) pairs.push_back({x, y, s});
            }
        }
    for (const Pair& p : pairs) {
        const int nx = p.x + DX[p.s], ny = p.y + DY[p.s];
        // Rock per position along the shared edge, in both cells. Ties go to
        // the lowest position.
        int best = 0, bestSum = -1;
        for (int k = 0; k < F; ++k) {
            int sum = 0;
            for (int j = 0; j < F; ++j) {
                if (p.s == kE) sum += fine.sub(p.x, p.y, k, j) + fine.sub(nx, ny, k, j);   // row k
                else sum += fine.sub(p.x, p.y, j, k) + fine.sub(nx, ny, j, k);             // column k
            }
            if (sum > bestSum) {
                bestSum = sum;
                best = k;
            }
        }
        fine.setEdge(p.x, p.y, p.s, best, true);
        fine.setEdge(nx, ny, OPP[p.s], best, true);
    }
}

/// Neighbouring cliff cells share their edge rock: both facing edge lines
/// become the union of the two (naht_teilen). Tile edge codes on facing sides
/// must agree, so the band has to agree too. East seams are done first, and
/// the mask is recomputed before the south seams.
void shareSeams(Fine& fine) {
    for (int s : {kE, kS}) {
        const std::vector<uint8_t> hat = cellsWithRock(fine);
        struct Set { int x, y, k; bool v; };
        std::vector<Set> sets;
        for (int y = 0; y < fine.h; ++y)
            for (int x = 0; x < fine.w; ++x) {
                const int nx = x + DX[s], ny = y + DY[s];
                if (nx >= fine.w || ny >= fine.h) continue;
                if (!hat[static_cast<size_t>(y) * fine.w + x] || !hat[static_cast<size_t>(ny) * fine.w + nx]) continue;
                for (int k = 0; k < F; ++k) {
                    const bool v = fine.edge(x, y, s, k) || fine.edge(nx, ny, OPP[s], k);
                    sets.push_back({x, y, k, v});
                }
            }
        for (const Set& st : sets) {
            fine.setEdge(st.x, st.y, s, st.k, st.v);
            fine.setEdge(st.x + DX[s], st.y + DY[s], OPP[s], st.k, st.v);
        }
    }
}

/// Edge mismatches of tile row t against a cell's own edges: how many of the
/// 16 edge segments the tile codes as rock where the band has ground, or the
/// other way round. EGAL segments never count. 0 = exact match.
int mismatches(const CliffTable& tab, int t, const Fine& fine, int x, int y) {
    int n = 0;
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < F; ++k) {
            const int8_t e = tab.E[t][s][k];
            if (e != kEgal && e != (fine.edge(x, y, s, k) ? kFels : kBiom)) ++n;
        }
    return n;
}

/// Cells no tile matches exactly take the edges of the closest tile (angleichen).
/// "Closest" ranks tiles by, in order: fewest rock segments on open sides,
/// fewest mismatches, nearest rock share to the cell's, lowest tile index.
/// Only edge segments change, never the cell's interior; open sides are
/// always set to ground. Changing a cell's edges can break its neighbours'
/// matches, hence the rounds.
void matchToTiles(Fine& fine, const CliffTable& tab, int rounds = 6) {
    for (int round = 0; round < rounds; ++round) {
        const std::vector<uint8_t> hat = cellsWithRock(fine);
        // All bad cells are found first, then fixed, so the round's fixes
        // don't affect which cells it picks. fehl = mismatches per tile row.
        struct Bad { int x, y; std::vector<int> fehl; std::array<bool, 4> open; };
        std::vector<Bad> bad;
        for (int y = 0; y < fine.h; ++y)
            for (int x = 0; x < fine.w; ++x) {
                if (!hat[static_cast<size_t>(y) * fine.w + x]) continue;
                std::vector<int> fehl(static_cast<size_t>(tab.n));
                bool exact = false;
                for (int t = 0; t < tab.n; ++t) {
                    fehl[t] = mismatches(tab, t, fine, x, y);
                    if (fehl[t] == 0) exact = true;
                }
                if (exact) continue;
                std::array<bool, 4> open{};
                for (int s = 0; s < 4; ++s) open[s] = isOpen(hat, fine.w, fine.h, x, y, s);
                bad.push_back({x, y, std::move(fehl), open});
            }
        if (bad.empty()) break;
        for (const Bad& b : bad) {
            const double share = fine.cellMean(b.x, b.y);
            int best = -1;
            std::tuple<int, int, double, int> bestKey{};
            for (int t = 0; t < tab.n; ++t) {
                int hard = 0;
                for (int s = 0; s < 4; ++s)
                    if (b.open[s])
                        for (int k = 0; k < F; ++k) hard += tab.E[t][s][k] == kFels;
                const auto key = std::make_tuple(hard, b.fehl[t], std::abs(tab.fa[t] - share), tab.ids[t]);
                if (best < 0 || key < bestKey) {
                    best = t;
                    bestKey = key;
                }
            }
            for (int s = 0; s < 4; ++s)
                for (int k = 0; k < F; ++k) {
                    const int8_t e = tab.E[best][s][k];
                    if (e == kEgal) continue;
                    fine.setEdge(b.x, b.y, s, k, e == kFels && !b.open[s]);
                }
        }
    }
}

/// gelaende_angleichen: repeat the rules until the band stops changing.
/// Takes the band by value and returns the conditioned copy. share = false
/// skips shareSeams (used for the greedy fallback, as in the original).
/// The round limits keep it finite if the rules keep undoing each other.
Fine conditionTerrain(Fine fine, const CliffTable& tab, bool share) {
    for (int r = 0; r < (share ? 8 : 4); ++r) {
        const std::vector<uint8_t> before = fine.v;
        clearOpenEdges(fine);
        keepSeams(fine);
        if (share) shareSeams(fine);
        matchToTiles(fine, tab);
        if (fine.v == before) break;
    }
    return fine;
}

// --- 4. solver ---------------------------------------------------------------------------

/// The solver's input: the cliff cells of the conditioned band and per
/// (cell, tile row) scores, all lower = better. n = tiles, m = cliff cells.
///   fehl  - edge mismatches, or kHart if the tile has rock on an open side
///   ofs   - open sides where the tile's inner image shows rock at the edge
///   bin   - closed sides (cliff neighbour) where the tile is plain ground
///   inn   - inner-image pixels differing from the band (16 if no image)
///   share - |tile rock share - cell rock share|
struct Prepared {
    int w = 0, h = 0, n = 0, m = 0;
    std::vector<std::pair<int, int>> cells;            ///< (x, y), row-major
    std::vector<int> cellAt;                           ///< w*h -> cell index or -1
    std::vector<std::vector<std::pair<int, int>>> nbrs;  ///< per cell: (side, cell index)
    std::vector<int> fehl, ofs, bin, inn;              ///< [cell*n + tile]
    std::vector<double> share;                         ///< |fa - cell rock share|
    /// Per cell: (side, tile row) of fixed neighbours -- tiles that stay where
    /// they are and that the cell's tile has to fit (closing a cut).
    std::vector<std::vector<std::pair<int, int>>> fixed;
};

/// Scores every tile for every cliff cell of the band. openOk lists cells
/// (north-line ends) whose given side may face open ground: that side is
/// treated as closed, so a tile with rock there isn't rejected.
/// only (w*h, optional): solve just these cells; every other rock cell still
/// counts as cliff (a closed side). fixedRow (w*h, optional): table row of a
/// tile that stays in place, -1 = none; it becomes a fixed neighbour of the
/// solved cells next to it.
Prepared prepare(const CliffTable& tab, const Fine& fine, const std::map<std::pair<int, int>, int>& openOk,
                 const std::vector<uint8_t>* only = nullptr, const std::vector<int>* fixedRow = nullptr) {
    Prepared p;
    p.w = fine.w;
    p.h = fine.h;
    p.n = tab.n;
    const std::vector<uint8_t> hat = cellsWithRock(fine);
    p.cellAt.assign(hat.size(), -1);
    for (int y = 0; y < fine.h; ++y)
        for (int x = 0; x < fine.w; ++x)
            if (hat[static_cast<size_t>(y) * fine.w + x] && (!only || (*only)[static_cast<size_t>(y) * fine.w + x])) {
                p.cellAt[static_cast<size_t>(y) * fine.w + x] = static_cast<int>(p.cells.size());
                p.cells.push_back({x, y});
            }
    p.m = static_cast<int>(p.cells.size());
    const size_t mn = static_cast<size_t>(p.m) * p.n;
    p.fehl.resize(mn);
    p.ofs.resize(mn);
    p.bin.resize(mn);
    p.inn.resize(mn);
    p.share.resize(mn);
    p.nbrs.resize(static_cast<size_t>(p.m));
    p.fixed.resize(static_cast<size_t>(p.m));
    for (int k = 0; k < p.m; ++k) {
        const int x = p.cells[k].first, y = p.cells[k].second;
        std::array<bool, 4> open{};
        for (int s = 0; s < 4; ++s) open[s] = isOpen(hat, fine.w, fine.h, x, y, s);
        auto ok = openOk.find({x, y});
        if (ok != openOk.end()) open[ok->second] = false;
        const double mean = fine.cellMean(x, y);
        for (int t = 0; t < p.n; ++t) {
            const size_t kt = static_cast<size_t>(k) * p.n + t;
            int f = mismatches(tab, t, fine, x, y);
            bool hard = false;
            int ofs = 0, bin = 0;
            for (int s = 0; s < 4; ++s) {
                if (open[s] && tab.rockSide[t][s]) hard = true;
                if (open[s] && tab.pixRock[t][s]) ++ofs;
                if (!open[s] && tab.groundSide[t][s]) ++bin;
            }
            p.fehl[kt] = hard ? kHart : f;
            p.ofs[kt] = ofs;
            p.bin[kt] = bin;
            int inn = F * F;
            if (tab.hasInner[t]) {
                inn = 0;
                for (int r = 0; r < F; ++r)
                    for (int c = 0; c < F; ++c) inn += tab.I[t][r][c] != fine.sub(x, y, r, c);
            }
            p.inn[kt] = inn;
            p.share[kt] = std::abs(tab.fa[t] - mean);
        }
        for (int s = 0; s < 4; ++s) {
            const int nx = x + DX[s], ny = y + DY[s];
            if (nx < 0 || ny < 0 || nx >= fine.w || ny >= fine.h) continue;
            const int q = p.cellAt[static_cast<size_t>(ny) * fine.w + nx];
            if (q >= 0) p.nbrs[k].push_back({s, q});
            else if (fixedRow) {
                const int f = (*fixedRow)[static_cast<size_t>(ny) * fine.w + nx];
                if (f >= 0) p.fixed[k].push_back({s, f});
            }
        }
    }
    return p;
}

/// One solver stage: backtracking search with forward checking over the
/// cliff cells. Each cell's domain starts as the tiles with fehl <= maxDev
/// (pre-sorted by score); placing a tile prunes the unassigned neighbours'
/// domains to tiles allowed beside it (CliffTable::allowed for onlySeen).
/// The next cell is the one with the smallest domain. The search gives up
/// after `budget` steps. On success, assignment() holds a tile row per cell.
class Solver {
public:
    Solver(const CliffTable& tab, const Prepared& p, bool onlySeen, int maxDev, int budget)
        : tab_(tab), p_(p), onlySeen_(onlySeen), budget_(budget), assigned_(static_cast<size_t>(p.m), -1) {
        dom_.resize(static_cast<size_t>(p.m));
        for (int k = 0; k < p.m; ++k) {
            std::vector<int> cand;
            for (int t = 0; t < p.n; ++t) {
                if (p.fehl[idx(k, t)] > maxDev) continue;
                // A fixed neighbour f on side s: only tiles it may stand beside.
                bool fits = true;
                for (const auto& [s, f] : p.fixed[k])
                    if (!tab.allowed(onlySeen, s, t, f)) fits = false;
                if (fits) cand.push_back(t);
            }
            if (cand.empty()) {   // some cell has no tile at all: stage can't succeed
                empty_ = true;
                return;
            }
            std::stable_sort(cand.begin(), cand.end(), [&](int a, int b) {
                const size_t ka = idx(k, a), kb = idx(k, b);
                return std::make_tuple(p.ofs[ka] + p.bin[ka], p.fehl[ka], p.inn[ka], p.share[ka], tab.ids[a]) <
                       std::make_tuple(p.ofs[kb] + p.bin[kb], p.fehl[kb], p.inn[kb], p.share[kb], tab.ids[b]);
            });
            dom_[k] = std::move(cand);
        }
    }

    bool solve() {
        if (empty_) return false;
        return step();
    }
    const std::vector<int>& assignment() const { return assigned_; }
    int steps() const { return steps_; }

private:
    size_t idx(int k, int t) const { return static_cast<size_t>(k) * p_.n + t; }

    /// Cell k's remaining candidates in the order to try them. Key, most
    /// important first: open-side and ground-side problems (ofs + bin), edge
    /// mismatches, repetition (2 if a direct neighbour already has this tile,
    /// +1 if one 2-3 cells away in a straight line does), inner-image
    /// difference, whether every placed neighbour pair was seen in the
    /// originals, mean seam difference to placed neighbours, and finally the
    /// domain order (which keeps the sort deterministic).
    std::vector<int> order(int k) const {
        const int x = p_.cells[k].first, y = p_.cells[k].second;
        std::vector<std::pair<int, int>> placed;     // (side, tile) of assigned direct neighbours
        std::set<int> direct;
        for (const auto& [s, q] : p_.nbrs[k])
            if (assigned_[q] >= 0) {
                placed.push_back({s, assigned_[q]});
                direct.insert(assigned_[q]);
            }
        std::set<int> further;
        for (int s = 0; s < 4; ++s)
            for (int d = 2; d <= 3; ++d) {
                const int nx = x + d * DX[s], ny = y + d * DY[s];
                if (nx < 0 || ny < 0 || nx >= p_.w || ny >= p_.h) continue;
                const int q = p_.cellAt[static_cast<size_t>(ny) * p_.w + nx];
                if (q >= 0 && assigned_[q] >= 0) further.insert(assigned_[q]);
            }
        const std::vector<int>& cand = dom_[k];
        using Key = std::tuple<int, int, int, int, int, double, int>;
        std::vector<std::pair<Key, int>> keyed;
        keyed.reserve(cand.size());
        for (size_t i = 0; i < cand.size(); ++i) {
            const int t = cand[i];
            const size_t kt = idx(k, t);
            int rank = 0;
            double seamSum = 0;
            int seamN = 0;
            for (const auto& [s, b] : placed) {
                if (!tab_.seen[s][static_cast<size_t>(t) * tab_.n + b]) rank = 1;
                const float d = tab_.seam[s][static_cast<size_t>(t) * tab_.n + b];
                if (d == d) {   // skip NaN (no rim data)
                    seamSum += d;
                    ++seamN;
                }
            }
            const int same = (direct.count(t) ? 2 : 0) + (further.count(t) ? 1 : 0);
            keyed.push_back({Key(p_.ofs[kt] + p_.bin[kt], p_.fehl[kt], same, p_.inn[kt], rank,
                                 seamN ? seamSum / seamN : 0.0, static_cast<int>(i)),
                             t});
        }
        std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<int> out;
        out.reserve(keyed.size());
        for (const auto& kv : keyed) out.push_back(kv.second);
        return out;
    }

    /// One level of the search; true when every cell is assigned. Each call
    /// counts as one step against the budget.
    bool step() {
        if (++steps_ > budget_) {
            outOfTime_ = true;
            return false;
        }
        // Most constrained unassigned cell; ties go to the lowest index.
        int best = -1;
        for (int k = 0; k < p_.m; ++k) {
            if (assigned_[k] >= 0) continue;
            if (best < 0 || dom_[k].size() < dom_[best].size()) {
                best = k;
                if (dom_[k].size() <= 1) break;
            }
        }
        if (best < 0) return true;
        for (int t : order(best)) {
            assigned_[best] = t;
            // Forward check: prune neighbours' domains, saving the old ones
            // so they can be restored when backtracking.
            std::vector<std::pair<int, std::vector<int>>> saved;
            bool dead = false;
            for (const auto& [s, q] : p_.nbrs[best]) {
                if (assigned_[q] >= 0) continue;
                std::vector<int> kept;
                for (int u : dom_[q])
                    if (tab_.allowed(onlySeen_, s, t, u)) kept.push_back(u);
                saved.push_back({q, std::move(dom_[q])});
                dom_[q] = std::move(kept);
                if (dom_[q].empty()) {
                    dead = true;
                    break;
                }
            }
            if (!dead && step()) return true;
            for (auto& [q, d] : saved) dom_[q] = std::move(d);
            assigned_[best] = -1;
            if (outOfTime_) return false;   // unwind all the way once the budget is spent
        }
        return false;
    }

    const CliffTable& tab_;
    const Prepared& p_;
    bool onlySeen_;
    int budget_;
    int steps_ = 0;
    bool outOfTime_ = false;
    bool empty_ = false;
    std::vector<int> assigned_;            // tile row per cell, -1 = unassigned
    std::vector<std::vector<int>> dom_;    // remaining candidate tile rows per cell
};

// --- 5. greedy fallback -------------------------------------------------------------------

/// Per cell the exact-matching tile closest in rock share, then a few rounds
/// of swapping for better seams and less repetition (setzen).
/// Returns a tile row per cell (-1 = none). Cells no tile matches exactly stay
/// empty, and neighbour pairs aren't checked, so the result can have holes
/// or invalid pairs; it is only used when every solver stage failed.
std::vector<int> greedy(const CliffTable& tab, const Fine& fine) {
    const int w = fine.w, h = fine.h;
    std::vector<int> grid(static_cast<size_t>(w) * h, -1);
    std::map<std::pair<int, int>, std::vector<int>> choices;   // (x, y) -> candidates, best first
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!fine.cellAny(x, y)) continue;
            const double share = fine.cellMean(x, y);
            std::vector<std::pair<double, int>> fit;
            for (int t = 0; t < tab.n; ++t)
                if (mismatches(tab, t, fine, x, y) == 0) fit.push_back({std::abs(tab.fa[t] - share), t});
            if (fit.empty()) continue;
            std::sort(fit.begin(), fit.end(), [&](const auto& a, const auto& b) {
                return std::make_pair(a.first, tab.ids[a.second]) < std::make_pair(b.first, tab.ids[b.second]);
            });
            grid[static_cast<size_t>(y) * w + x] = fit[0].second;
            std::vector<int> list;
            for (const auto& f : fit) list.push_back(f.second);
            choices[{x, y}] = std::move(list);
        }
    // Mean seam difference of tile t at (x, y) to its placed neighbours; false
    // if no neighbour has rim data.
    auto seamHere = [&](int t, int x, int y, double& out) {
        double sum = 0;
        int n = 0;
        for (int s = 0; s < 4; ++s) {
            const int nx = x + DX[s], ny = y + DY[s];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const int b = grid[static_cast<size_t>(ny) * w + nx];
            if (b < 0) continue;
            const float d = tab.seam[s][static_cast<size_t>(t) * tab.n + b];
            if (d == d) {
                sum += d;
                ++n;
            }
        }
        if (!n) return false;
        out = sum / n;
        return true;
    };
    for (int round = 0; round < kNahtRunden; ++round) {
        int changed = 0;
        for (const auto& [pos, cand] : choices) {      // ordered by (x, y)
            const int x = pos.first, y = pos.second;
            std::vector<std::pair<double, int>> rated;
            for (int t : cand) {
                double d;
                if (seamHere(t, x, y, d)) rated.push_back({d, t});
            }
            if (rated.empty()) continue;
            std::sort(rated.begin(), rated.end(), [&](const auto& a, const auto& b) {
                return std::make_pair(a.first, tab.ids[a.second]) < std::make_pair(b.first, tab.ids[b.second]);
            });
            std::set<int> neighbours;
            for (int s = 0; s < 4; ++s) {
                const int nx = x + DX[s], ny = y + DY[s];
                if (nx >= 0 && ny >= 0 && nx < w && ny < h && grid[static_cast<size_t>(ny) * w + nx] >= 0)
                    neighbours.insert(grid[static_cast<size_t>(ny) * w + nx]);
            }
            // Best seam, unless a tile within kNahtSpielraum of it differs
            // from all neighbours. Cells update in place, so later cells in
            // the round see the new choice.
            int pick = rated[0].second;
            const double limit = rated[0].first + kNahtSpielraum;
            for (const auto& [d, t] : rated) {
                if (d > limit) break;
                if (!neighbours.count(t)) {
                    pick = t;
                    break;
                }
            }
            int& cell = grid[static_cast<size_t>(y) * w + x];
            if (pick != cell) {
                cell = pick;
                ++changed;
            }
        }
        if (!changed) break;
    }
    return grid;
}

}  // namespace

// --- entry points ---------------------------------------------------------------------------

/// See cliff.hpp. Runs the whole pipeline: band, fill rule, terrain rules,
/// north-line ends, solver stages, greedy fallback. Returns an all -1 result
/// for fewer than two points, an unusable family, or a band with no rock.
/// The search steps of all tried stages are summed in searchSteps.
CliffResult solveCliffLine(const CliffData& data, int family,
                           const std::vector<std::pair<int, int>>& points,
                           CliffHighSide highSide, double wallHeight,
                           int width, int height, uint64_t seed) {
    CliffResult res;
    res.width = width;
    res.height = height;
    res.tile.assign(static_cast<size_t>(width) * height, -1);
    if (points.size() < 2 || !data.usable(family)) return res;
    const CliffTable& tab = data.table(family);

    // Wall height in tiles -> rock thickness range (dicke_aus_hoehe).
    const double z = std::min(kWallMax, std::max(kWallMin, wallHeight));
    const double thickMin = z - 1.5, thickMax = z - 0.5;

    Rng rng(seed);   // the only source of randomness; draws happen in band, then fill rule
    const bool closed = points.size() > 2 && points.front() == points.back();   // loops get no taper
    Fine fine;
    std::vector<uint8_t> axis;
    std::vector<NorthEnd> northEnds;
    drawBand(points, highSide, width, height, rng, thickMin, thickMax, !closed, fine, axis, northEnds);
    if (!fine.anyRock()) return res;
    fillRule(fine, axis, rng);

    Fine gel = conditionTerrain(fine, tab, true);

    // North-line ends: carry the line to the cell edge and let it touch open ground there.
    // clearOpenEdges may have cut the end back; this restores it on the
    // conditioned band (gel), and prepare() then treats that side as closed.
    std::map<std::pair<int, int>, int> openOk;
    for (const NorthEnd& e : northEnds) {
        if (e.cx < 0 || e.cy < 0 || e.cx >= gel.w || e.cy >= gel.h || !gel.cellAny(e.cx, e.cy)) continue;
        int lo = -1, hi = -1;
        for (int c = 0; c < F; ++c)
            if (gel.at(e.row, e.cx * F + c)) {
                if (lo < 0) lo = c;
                hi = c;
            }
        if (lo < 0) continue;
        if (e.side == kW)
            for (int c = 0; c <= lo; ++c) gel.at(e.row, e.cx * F + c) = 1;
        else
            for (int c = hi; c < F; ++c) gel.at(e.row, e.cx * F + c) = 1;
        openOk[{e.cx, e.cy}] = e.side;
    }

    const Prepared prep = prepare(tab, gel, openOk);
    if (prep.m > 0) {
        int stage = 0;
        for (const auto& [onlySeen, dev] : kStages) {
            Solver solver(tab, prep, onlySeen, dev, stepBudget(prep.m));
            const bool ok = solver.solve();
            res.searchSteps += solver.steps();
            if (ok) {
                for (int k = 0; k < prep.m; ++k) {
                    const auto& c = prep.cells[k];
                    res.tile[static_cast<size_t>(c.second) * width + c.first] = tab.ids[solver.assignment()[k]];
                }
                res.stage = stage;
                return res;
            }
            ++stage;
        }
    }

    // No stage solved it: greedy, from the band before the north-end changes.
    const std::vector<int> g = greedy(tab, conditionTerrain(fine, tab, false));
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i] >= 0) res.tile[i] = tab.ids[g[i]];
    res.stage = -1;
    return res;
}

/// See cliff.hpp. Ported from the editor's schnitt_schliessen (Python):
/// band from the inner images, the edge lines towards the cut cleared, the
/// terrain rules applied, the old band restored outside the zone, then the
/// solver stages on the zone's rock cells with the standing tiles fixed.
CliffCutResult solveCliffCut(const CliffData& data, int family, const std::vector<int>& cliffTiles,
                             const std::vector<uint8_t>& removed, int width, int height, int zone) {
    CliffCutResult res;
    res.width = width;
    res.height = height;
    const size_t n = static_cast<size_t>(width) * height;
    res.tile.assign(n, -1);
    res.ground.assign(n, 0);
    if (width <= 0 || height <= 0 || cliffTiles.size() != n || removed.size() != n || !data.usable(family))
        return res;
    const CliffTable& tab = data.table(family);
    std::vector<int> row(data.tiles.size(), -1);   // cliff tile index -> table row of this family
    for (int i = 0; i < tab.n; ++i) row[tab.ids[i]] = i;
    auto rowOf = [&](size_t i) {
        const int t = cliffTiles[i];
        return (t >= 0 && t < static_cast<int>(row.size())) ? row[t] : -1;
    };
    // Grows a mask by one cell (4-neighbours), within the area.
    auto grow = [&](std::vector<uint8_t>& m) {
        const std::vector<uint8_t> prev = m;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                if (prev[static_cast<size_t>(y) * width + x]) continue;
                for (int s = 0; s < 4; ++s) {
                    const int nx = x + DX[s], ny = y + DY[s];
                    if (nx >= 0 && ny >= 0 && nx < width && ny < height && prev[static_cast<size_t>(ny) * width + nx]) {
                        m[static_cast<size_t>(y) * width + x] = 1;
                        break;
                    }
                }
            }
    };

    // One try with a given cut. True and res filled if a solver stage worked.
    auto tryCut = [&](const std::vector<uint8_t>& cut) -> bool {
        // The standing wall (this family, not cut), the zone around the cut,
        // and the wall cells inside it, which are solved again.
        std::vector<uint8_t> wall(n, 0), near(n, 0), redo(n, 0);
        for (size_t i = 0; i < n; ++i) {
            near[i] = cut[i] ? 1 : 0;
            wall[i] = !cut[i] && rowOf(i) >= 0;
        }
        for (int r = 0; r < zone; ++r) grow(near);
        bool anyRedo = false;
        for (size_t i = 0; i < n; ++i) {
            redo[i] = near[i] && wall[i];
            anyRedo = anyRedo || redo[i];
        }
        if (!anyRedo) return false;

        // Rock band of the standing wall from the tiles' inner images (all
        // rock where a tile has none).
        Fine fine(width, height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const size_t i = static_cast<size_t>(y) * width + x;
                if (!wall[i]) continue;
                const CliffTile& c = data.tiles[cliffTiles[i]];
                for (int r = 0; r < F; ++r)
                    for (int col = 0; col < F; ++col) fine.setSub(x, y, r, col, c.hasInner ? c.inner[r][col] : true);
            }
        const Fine before = fine;
        // Give way at the cut: in the cells to solve, clear the edge line
        // facing a cut cell, so no rock points into the open there.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                if (!redo[static_cast<size_t>(y) * width + x]) continue;
                for (int s = 0; s < 4; ++s) {
                    const int nx = x + DX[s], ny = y + DY[s];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height || !cut[static_cast<size_t>(ny) * width + nx])
                        continue;
                    for (int k = 0; k < F; ++k) fine.setEdge(x, y, s, k, false);
                }
            }
        Fine gel = conditionTerrain(fine, tab, true);
        // Outside the zone the old band holds: the old tiles stay there.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                if (redo[static_cast<size_t>(y) * width + x]) continue;
                for (int r = 0; r < F; ++r)
                    for (int col = 0; col < F; ++col) gel.setSub(x, y, r, col, before.sub(x, y, r, col));
            }

        const std::vector<uint8_t> hat = cellsWithRock(gel);
        std::vector<uint8_t> only(n, 0);
        std::vector<int> fixedRow(n, -1);
        bool anyOnly = false;
        for (size_t i = 0; i < n; ++i) {
            only[i] = redo[i] && hat[i];
            anyOnly = anyOnly || only[i];
            if (wall[i] && !redo[i]) fixedRow[i] = rowOf(i);
        }
        std::fill(res.tile.begin(), res.tile.end(), -1);
        int stage = 0;
        if (anyOnly) {
            const Prepared prep = prepare(tab, gel, {}, &only, &fixedRow);
            bool solved = false;
            for (const auto& [onlySeen, dev] : kStages) {
                Solver solver(tab, prep, onlySeen, dev, stepBudget(prep.m));
                if (solver.solve()) {
                    for (int k = 0; k < prep.m; ++k) {
                        const auto& c = prep.cells[k];
                        res.tile[static_cast<size_t>(c.second) * width + c.first] = tab.ids[solver.assignment()[k]];
                    }
                    solved = true;
                    break;
                }
                ++stage;
            }
            if (!solved) return false;
        }
        res.stage = stage;
        // Ground: zone cells left without rock, and wall cells the cut was
        // widened over (see below).
        for (size_t i = 0; i < n; ++i)
            res.ground[i] = (redo[i] && !hat[i]) || (cut[i] && !removed[i] && rowOf(i) >= 0);
        return true;
    };

    // If the cut has no solution (e.g. a diagonal wall cut straight across),
    // let the wall end recede: widen the cut by one cell into the wall, at
    // most twice.
    std::vector<uint8_t> cut(n, 0);
    for (size_t i = 0; i < n; ++i) cut[i] = removed[i] ? 1 : 0;
    for (int widen = 0; widen <= 2; ++widen) {
        if (widen > 0) {
            std::vector<uint8_t> wider = cut;
            grow(wider);
            for (size_t i = 0; i < n; ++i)
                if (wider[i] && !cut[i] && rowOf(i) >= 0) cut[i] = 1;   // only wall cells join the cut
        }
        if (tryCut(cut)) return res;
    }
    std::fill(res.tile.begin(), res.tile.end(), -1);
    res.stage = -2;
    return res;   // no solution: leave the wall as it is
}

/// See cliff.hpp. Picks the allowed direction closest in angle to a -> b
/// (largest cosine), then the whole number of its steps nearest to the
/// segment's length. The direction set is a std::set, so ties resolve the
/// same way everywhere.
void snapCliffPoint(int ax, int ay, int bx, int by, int& outX, int& outY) {
    outX = bx;
    outY = by;
    const int vx = bx - ax, vy = by - ay;
    if (vx == 0 && vy == 0) return;
    // (dx, dy) per step for each allowed slope, mirrored into all quadrants.
    static const int slopes[][2] = {{1, 0}, {4, 1}, {3, 1}, {2, 1}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {0, 1}};
    std::set<std::pair<int, int>> dirs;
    for (const auto& s : slopes)
        for (int sx : {1, -1})
            for (int sy : {1, -1}) dirs.insert({s[0] * sx, s[1] * sy});
    const double len = std::sqrt(double(vx) * vx + double(vy) * vy);
    double bestCos = -2;
    int bdx = 1, bdy = 0;
    double bn = 1;
    for (const auto& [dx, dy] : dirs) {
        const double n = std::sqrt(double(dx) * dx + double(dy) * dy);
        const double c = (vx * dx + vy * dy) / (len * n);
        if (c > bestCos) {
            bestCos = c;
            bdx = dx;
            bdy = dy;
            bn = n;
        }
    }
    const int steps = std::max(1, roundHalfEven(len / bn));
    outX = ax + bdx * steps;
    outY = ay + bdy * steps;
}

}  // namespace om

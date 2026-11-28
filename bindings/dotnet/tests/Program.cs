// Tests for the .NET binding: reproduces the shared test vector
// (tests/vectors/blend_disc.json) and checks the binding's basics.
// Build for x86 and x64 (PlatformTarget) to test both native DLLs.
//
// The hashes checked below are the same ones the C++ and Python tests expect:
// a match proves the binding passes data through unchanged and that the
// library is deterministic across bitness and language. A plain console
// program (no test framework) so it builds with no NuGet downloads.
// Exit code 0 = all checks passed, 1 = at least one failed.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using Op2MapLib;

internal static class Program
{
    private static int _failures;

    // Records a failed check and keeps going, so one run reports every failure.
    private static void Check(bool cond, string what)
    {
        if (cond) return;
        _failures++;
        Console.WriteLine("  FAILED: " + what);
    }

    // 64-bit FNV-1a over each value as 4 little-endian bytes, printed as 16 hex
    // digits. Must match the hash in the C++ and Python tests byte for byte.
    private static string Fnv1a64(IEnumerable<int> values)
    {
        ulong h = 1469598103934665603UL;   // FNV-1a 64-bit offset basis
        foreach (int v in values)
            for (int b = 0; b < 4; b++)
            {
                h ^= (byte)((uint)v >> (8 * b));
                h *= 1099511628211UL;      // FNV 64-bit prime (wraps, unchecked)
            }
        return h.ToString("x16");
    }

    private static int Main()
    {
        // Also confirms the native DLL of the right bitness loaded.
        Console.WriteLine($"op2maplib {BlendProfile.LibraryVersion} (.NET binding, {(IntPtr.Size == 8 ? "64" : "32")}-bit)");

        // Uses the profile copied to op2maplib\op2.json by the library's project file.
        using (var profile = BlendProfile.LoadDefault())
        {
            Console.WriteLine("profile: " + profile.Name);
            // Profile queries; mud/sand blend in Outpost 2, mud/vegetation don't.
            int mud = profile.FindGround("mud"), san = profile.FindGround("san"), veg = profile.FindGround("veg");
            Check(mud >= 0 && san >= 0 && veg >= 0, "grounds exist");
            Check(profile.GroundName(mud) == "Mud", "ground name");
            Check(profile.GroundsBlend(mud, san), "mud blends with sand");
            Check(!profile.GroundsBlend(mud, veg), "mud does not blend with vegetation");
            TileMapping[] std = profile.StandardMapping();
            Check(std.Length == 2017, "standard mapping has 2017 entries");
            Check(std[0].Tileset == "well0000", "standard mapping names");

            // Shared vector: tests/vectors/blend_disc.json
            // A 40x40 map filled with mud, then a sand disc of radius 6 painted at
            // (20, 20) and resolved with seed 1234.
            const int w = 40, h = 40;
            using (var map = new BlendMap(profile, w, h, std, Enumerable.Repeat(0, w * h).ToList()))
            {
                var all = new List<Point>();
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) all.Add(new Point(x, y));
                // Paint and resolve everything as mud first, so the starting state is
                // the library's own mud fill, whatever tile mapping 0 happens to be.
                map.Paint(mud, all);
                map.Resolve(1234);

                var disc = all.Where(p => (p.X - 20) * (p.X - 20) + (p.Y - 20) * (p.Y - 20) <= 36).ToList();
                Check(map.Paint(san, disc) == disc.Count, "paint count");
                BlendChange[] changes = map.Resolve(1234);
                Check(changes.Length > 0, "resolve returns changes");
                // The library applies its changes to its own copy of the map.
                Check(changes.All(c => c.NewMapping == map.TileAt(c.X, c.Y)), "changes are applied");

                // Hash every cell's mapping index, row by row.
                string hash = Fnv1a64(all.Select(p => map.TileAt(p.X, p.Y)));
                Console.WriteLine("vector hash: " + hash);
                Check(hash == "d33d22343ac4ecfd", "vector hash matches C++ and Python");

                Check(map.TileRoleAt(20, 20) == TileRole.Fill, "sand centre is fill");
                Check(map.TileRoleAt(20, 13) == TileRole.Transition, "mud next to sand is a transition");
                // A second resolve over the same area must be stable (no flicker).
                Check(map.Resolve(new Rectangle(0, 0, w, h), 1234).Length == 0, "re-resolving changes nothing");
            }

            // Cliff vector: tests/vectors/cliff_l.json (same as the C++ and Python tests)
            Check(profile.CliffGround(mud) == 1, "mud has its own cliffs");
            Check(profile.CliffGround(profile.FindGround("roc")) == 0, "rock uses the fallback cliffs");
            Check(BlendProfile.SnapCliffPoint(new Point(10, 10), new Point(30, 11)) == new Point(30, 10), "snap to flat");
            using (var map = new BlendMap(profile, w, h, std, Enumerable.Repeat(0, w * h).ToList()))
            {
                var all = new List<Point>();
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) all.Add(new Point(x, y));
                map.Paint(mud, all);
                map.Resolve(1234);
                // An L-shaped cliff: down from (20, 6), then right to (36, 20),
                // high ground on the left, 3-tile wall, seed 7.
                BlendChange[] cliff = map.DrawCliff(new[] { new Point(20, 6), new Point(20, 20), new Point(36, 20) },
                                                    CliffSide.Left, 3.0, 7);
                Check(cliff.Length > 20, "cliff drawn");
                Check(map.LastCliffStage >= 0, "cliff solved by a stage");
                Check(cliff.All(c => map.GroundAt(c.X, c.Y) == -1), "cliff cells are fixed");
                string hash = Fnv1a64(all.Select(p => map.TileAt(p.X, p.Y)));
                Console.WriteLine("cliff vector hash: " + hash);
                Check(hash == "5e0acf226271c020", "cliff vector hash matches C++ and Python");

                // Closing a cut: paint mud across the vertical leg (rows 11-13), then close.
                var removed = new bool[w * h];
                var cut = new List<Point>();
                for (int y = 11; y <= 13; y++)
                    for (int x = 0; x < w; x++)
                        if (map.GroundAt(x, y) == -1)
                        {
                            removed[y * w + x] = true;
                            cut.Add(new Point(x, y));
                        }
                map.Paint(mud, cut);
                map.Resolve(1234);
                BlendChange[] closed = map.CloseCliffCut(removed, 1234);
                Check(closed.Length > 0 && map.LastCliffStage >= 0, "cliff cut closed");
                Check(closed.All(c => c.Y >= 7 && c.Y <= 17), "cut closing stays near the cut");
                Check(map.CloseCliffCut(new bool[w * h]).Length == 0, "nothing cut, nothing to close");
            }

            // Regions: a wall ring around a 3x3 pocket.
            {
                const int rw = 20, rh = 12;
                var cellTypes = new byte[rw * rh];                   // all FastPassible1 (0)
                for (int y = 2; y <= 6; y++)
                    for (int x = 2; x <= 6; x++)
                        if (x == 2 || x == 6 || y == 2 || y == 6) cellTypes[y * rw + x] = 23;   // NormalWall
                // Default mask and flags: Outpost 2's passable types and rules.
                using (var regions = MapRegions.Find(rw, rh, cellTypes))
                {
                    Check(regions.Count == 2, "regions: main area + pocket");
                    Check(regions.Size(1) == 9 && regions.EnclosedSize == 9, "regions: pocket size");
                    Check(regions.EnclosedCells().All(p => p.X >= 3 && p.X <= 5 && p.Y >= 3 && p.Y <= 5), "regions: pocket cells");
                    Check(regions.RegionAt(0, rh - 1) == -1, "regions: bottom row skipped");
                }
            }

            // Map generator: the shared golden map (tests/vectors/mapgen_islands.json).
            using (var gen = new MapGenSettings("islands"))
            {
                gen.Set("width", 64);
                gen.Set("height", 64);
                gen.Set("cliffs", 0.25);
                gen.Set("spawns", 2);
                gen.Seed = 1234;
                gen.AddFeature(MapFeature.Mountain, "c", "", 0.6, false);
                GeneratedMap map = gen.Generate(profile);
                Check(map.Width == 64 && map.Height == 64 && map.Tiles.Length == 64 * 64, "mapgen: size");
                Check(map.Spawns.Length == 2 && map.Connected, "mapgen: two connected spawns");
                Check(map.CellTypes.Any(c => c == 8 || c == 9 || c == 10), "mapgen: cliffs drawn");
                string hash = Fnv1a64(map.Tiles.Select(t => (int)t));
                Console.WriteLine("mapgen golden hash: " + hash);
                Check(hash == "6c45f417758ec9c7", "mapgen golden hash matches C++ and Python");
                try
                {
                    gen.Set("width", 8);
                    gen.Generate(profile);
                    Check(false, "too small map throws");
                }
                catch (Op2MapLibException e)
                {
                    Check(e.Message.Contains("size"), "mapgen error mentions the size");
                }
            }
            using (var gen = new MapGenSettings("newterra"))
            {
                GeneratedMap map = gen.Generate(profile);
                Check(map.Volcanoes.Length == 1 && map.LavaZones.Length >= 1 && map.RiverShare > 0, "mapgen: newterra volcano and rivers");
            }
            // Mountains (tests/vectors/mapgen_mountains.json): closed, reachable plateaus.
            using (var gen = new MapGenSettings("plains"))
            {
                gen.Set("width", 64);
                gen.Set("height", 64);
                gen.Set("mountains", 0.14);
                gen.Set("plateaus", 1);
                gen.Set("spawns", 2);
                gen.Seed = 7;
                GeneratedMap map = gen.Generate(profile);
                string hash = Fnv1a64(map.Tiles.Select(t => (int)t));
                Console.WriteLine("mountains golden hash: " + hash);
                Check(hash == "683ac10cc3dfc740", "mountains golden hash matches C++ and Python");
                Check(map.Stats["mountains"] >= 1 && map.Stats["mountains_leaky"] == 0, "mountains closed");
                Check(map.Stats["plateaus_unreachable"] == 0, "plateaus reachable");
            }
            // Learned generator (tests/vectors/mapgen_learned.json), the same for any thread count.
            using (var model = LearnedModel.LoadDefault())
            using (var gen = new MapGenSettings("learned"))
            {
                gen.Set("width", 64);
                gen.Set("height", 64);
                gen.Set("spawns", 2);
                foreach (string k in new[] { "biome_calm", "coherence", "calm_terrain", "lava_rock", "volcanoes_small",
                                             "volcanoes_medium", "volcanoes_large", "craters", "cracks", "debris" })
                    gen.Set(k, 0);   // the additions off: the golden map of tests/vectors/mapgen_learned.json
                gen.Seed = 3;
                double last = 0;
                GeneratedMap map = gen.GenerateLearned(profile, model, LearnedQuality.Fast, 1, f => { last = f; return true; });
                string hash = Fnv1a64(map.Tiles.Select(t => (int)t));
                Console.WriteLine("learned golden hash: " + hash);
                Check(hash == "05a14ace030553ef", "learned golden hash matches C++ and Python");
                Check(Fnv1a64(gen.GenerateLearned(profile, model, LearnedQuality.Fast, 3).Tiles.Select(t => (int)t)) == hash,
                      "learned: same map with 3 threads");
                Check(last == 1.0 && map.SketchWidth == 32 && map.SketchWalls.Length == 32 * 32, "learned: progress and sketch");
                GeneratedMap sketch = gen.GenerateLearned(profile, model, LearnedQuality.Sketch);
                Check(sketch.Width == 0 && sketch.SketchWalls.SequenceEqual(map.SketchWalls), "learned: sketch only");
                try
                {
                    gen.GenerateLearned(profile, model, LearnedQuality.Fast, 0, f => false);
                    Check(false, "learned: cancel throws");
                }
                catch (Op2MapLibException e)
                {
                    Check(e.Message.Contains("cancelled"), "learned: cancel reported");
                }
            }

            // Native errors surface as Op2MapLibException carrying the library's message.
            try
            {
                new BlendProfile("{");
                Check(false, "bad JSON throws");
            }
            catch (Op2MapLibException e)
            {
                Check(e.Message.Contains("JSON"), "error message mentions JSON");
            }
        }

        if (_failures > 0)
        {
            Console.WriteLine($"{_failures} check(s) FAILED");
            return 1;
        }
        Console.WriteLine("all checks passed");
        return 0;
    }
}

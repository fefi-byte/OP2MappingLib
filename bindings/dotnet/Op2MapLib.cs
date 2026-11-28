// .NET binding for op2maplib (P/Invoke over the C API in include/op2maplib/op2maplib.h).
//
// Works from C# and VB.NET, .NET Framework 4.x and modern .NET.
//
// Public types:
//     BlendProfile  a loaded blend profile (JSON): grounds, fill and transition
//                   tiles, cliff tiles, standard mapping table.
//     BlendMap      blend state for one map: paint grounds, resolve them into
//                   tile changes, draw cliffs.
//     MapRegions    connected passable regions of a map (finds enclosed areas).
// Each wraps a native handle; dispose them (Using / using) when done. The
// finalizers free the handle too, but only at the garbage collector's pace.
//
// The library never touches .map files: the caller passes the map's mapping
// table and cell tiles in, and applies the returned changes in its own editor
// (and undo system). Results are deterministic for the same input and seed.
// Objects are not thread-safe; separate objects may be used on separate threads.
//
// Native library loading (Windows): the binding looks for
//     <this assembly's folder>\x64\op2maplib.dll   (64-bit process)
//     <this assembly's folder>\x86\op2maplib.dll   (32-bit process)
// and loads the one matching the process before the first call, so AnyCPU
// hosts work either way. If neither exists, the normal DLL search applies
// (op2maplib.dll next to the executable or on PATH). On Linux/macOS, put
// libop2maplib.so / .dylib where the runtime can find it.
//
// Example (VB.NET):
//     Using profile = BlendProfile.LoadDefault()
//         Using map = New BlendMap(profile, width, height, mappings, cells)
//             map.Paint(profile.FindGround("san"), cellsToPaint)
//             For Each c In map.Resolve(seed)
//                 ' write c.NewMapping (and c.CellType if >= 0) at c.X, c.Y
//             Next
//         End Using
//     End Using
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Op2MapLib
{
    /// <summary>
    /// Error reported by the native library, e.g. an invalid profile or a map the
    /// library could not set up. The message carries the native error text when
    /// the C API provides one.
    /// </summary>
    public sealed class Op2MapLibException : Exception
    {
        /// <summary>Creates the exception with the given message.</summary>
        /// <param name="message">Description of the error.</param>
        public Op2MapLibException(string message) : base(message) { }
    }

    /// <summary>
    /// What kind of profile tile a cell holds (see <see cref="BlendMap.TileRoleAt"/>).
    /// Values match the C enum om_tile_role.
    /// </summary>
    public enum TileRole
    {
        /// <summary>Not in the profile: a fixed cell (cliff, wall, ...). Never changed unless painted.</summary>
        Unknown = 0,
        /// <summary>Plain ground the library places.</summary>
        Fill = 1,
        /// <summary>Belongs to a ground but is never placed (decoration, rare tiles).</summary>
        Ground = 2,
        /// <summary>Blends two grounds.</summary>
        Transition = 3,
    }

    /// <summary>
    /// Which side of a cliff line is high ground (see <see cref="BlendMap.DrawCliff"/>).
    /// Values match the C enum om_cliff_side.
    /// </summary>
    public enum CliffSide
    {
        /// <summary>Left of the drawing direction.</summary>
        Left = 0,
        /// <summary>Right of the drawing direction.</summary>
        Right = 1,
        /// <summary>North (top of the map), whatever the drawing direction.</summary>
        North = 2,
        /// <summary>South (bottom of the map).</summary>
        South = 3,
    }

    /// <summary>
    /// One entry of a map's tile mapping table: a cell stores a mapping index,
    /// and the entry says which tileset image that index shows.
    /// </summary>
    /// <remarks>
    /// Managed-only type; it is converted to the native layout (Native.MappingIn /
    /// Native.MappingOut) when crossing the P/Invoke boundary.
    /// </remarks>
    public struct TileMapping
    {
        /// <summary>Tileset file name as stored in the map, e.g. "well0001" (case-insensitive).</summary>
        public string Tileset;
        /// <summary>Image (graphic) index within that tileset.</summary>
        public int Image;

        /// <summary>Creates a mapping entry.</summary>
        /// <param name="tileset">Tileset file name, e.g. "well0001".</param>
        /// <param name="image">Image index within the tileset.</param>
        public TileMapping(string tileset, int image)
        {
            Tileset = tileset;
            Image = image;
        }

        /// <summary>Returns "tileset:image", e.g. "well0001:12".</summary>
        public override string ToString() => Tileset + ":" + Image;
    }

    /// <summary>
    /// One tile the editor should change, as returned by <see cref="BlendMap.Resolve(ulong)"/>,
    /// <see cref="BlendMap.ResolveAll"/> and <see cref="BlendMap.DrawCliff"/>.
    /// </summary>
    /// <remarks>
    /// Blittable and laid out exactly like the C struct om_change (five int32
    /// fields), so arrays of it are filled directly by om_blend_get_changes.
    /// Do not reorder or add fields.
    /// </remarks>
    [StructLayout(LayoutKind.Sequential)]
    public struct BlendChange
    {
        /// <summary>Cell column (0 = left edge).</summary>
        public int X;
        /// <summary>Cell row (0 = top edge).</summary>
        public int Y;
        /// <summary>Mapping index before the change.</summary>
        public int OldMapping;
        /// <summary>Mapping index to write.</summary>
        public int NewMapping;
        /// <summary>Suggested cell type for the new tile, or -1 if the profile has none.</summary>
        public int CellType;

        /// <summary>Returns a readable form, e.g. "(3,4) 12 -> 40 cell 0".</summary>
        public override string ToString() => $"({X},{Y}) {OldMapping} -> {NewMapping} cell {CellType}";
    }

    /// <summary>
    /// A blend profile: which tiles exist for each ground and how transitions
    /// fit. Keep it alive (not disposed) while any BlendMap made from it is in use.
    /// </summary>
    /// <remarks>
    /// Wraps a native om_profile handle, freed by <see cref="Dispose"/> (or the
    /// finalizer). Grounds are addressed by index, 0 to <see cref="GroundCount"/> - 1.
    /// </remarks>
    public sealed class BlendProfile : IDisposable
    {
        // Native om_profile*, owned by this object; IntPtr.Zero once disposed.
        internal IntPtr Handle;

        /// <summary>Loads a profile from its JSON text (not a file name).</summary>
        /// <param name="json">The profile JSON (format: docs/profile-format.md).</param>
        /// <exception cref="ArgumentNullException"><paramref name="json"/> is null.</exception>
        /// <exception cref="Op2MapLibException">The JSON is invalid or is not a usable profile; the message says why.</exception>
        public BlendProfile(string json)
        {
            if (json == null) throw new ArgumentNullException(nameof(json));
            // The C API takes UTF-8 bytes plus a length, so pass a byte array rather
            // than a marshalled string (no terminator needed, no ANSI code page involved).
            byte[] bytes = Encoding.UTF8.GetBytes(json);
            // Buffer the native side writes its error message into (always terminated).
            var err = new StringBuilder(512);
            Handle = Native.om_profile_load(bytes, (UIntPtr)bytes.Length, err, (UIntPtr)err.Capacity);
            if (Handle == IntPtr.Zero)
                throw new Op2MapLibException("Could not load blend profile: " + err);
        }

        /// <summary>Loads a profile from a JSON file (read as UTF-8).</summary>
        /// <param name="path">Path of the profile JSON file.</param>
        /// <returns>The loaded profile; dispose it when done.</returns>
        /// <exception cref="IOException">The file could not be read (e.g. <see cref="FileNotFoundException"/>).</exception>
        /// <exception cref="Op2MapLibException">The file is not a valid profile.</exception>
        public static BlendProfile FromFile(string path) => new BlendProfile(File.ReadAllText(path, Encoding.UTF8));

        /// <summary>
        /// Loads the Outpost 2 profile shipped with the binding
        /// (op2maplib\op2.json next to this assembly; the older
        /// op2maplib\profiles\op2.json is still accepted).
        /// </summary>
        /// <returns>The Outpost 2 profile; dispose it when done.</returns>
        /// <exception cref="FileNotFoundException">Neither profile location exists.</exception>
        /// <exception cref="Op2MapLibException">The file is not a valid profile.</exception>
        public static BlendProfile LoadDefault()
        {
            // The project file copies the profile to op2maplib\op2.json in the output
            // folder of every project that references this binding.
            string path = Path.Combine(Native.AssemblyFolder, "op2maplib", "op2.json");
            if (!File.Exists(path))
            {
                // Layout used by earlier releases of the binding.
                string old = Path.Combine(Native.AssemblyFolder, "op2maplib", "profiles", "op2.json");
                if (File.Exists(old)) return FromFile(old);
                throw new FileNotFoundException("The op2maplib profile was not found next to the program.", path);
            }
            return FromFile(path);
        }

        // The strings returned by the native functions in this class point into
        // memory owned by the library (static, or owned by the profile), so they
        // are copied with PtrToStringAnsi and never freed here.

        /// <summary>Native library version as "major.minor.patch", e.g. "0.1.0".</summary>
        /// <remarks>Reading this loads the native DLL if it is not loaded yet.</remarks>
        public static string LibraryVersion => Marshal.PtrToStringAnsi(Native.om_version());

        /// <summary>Display name of the profile, e.g. "Outpost 2 standard tilesets (...)".</summary>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public string Name => Marshal.PtrToStringAnsi(Native.om_profile_name(Live));

        /// <summary>Number of grounds (terrain types) in the profile.</summary>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public int GroundCount => Native.om_profile_ground_count(Live);

        /// <summary>Short id of a ground, e.g. "mud".</summary>
        /// <param name="ground">Ground index, 0 to <see cref="GroundCount"/> - 1.</param>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="ground"/> is out of range.</exception>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public string GroundId(int ground) => Str(Native.om_profile_ground_id(Live, ground), ground);

        /// <summary>Display name of a ground, e.g. "Mud".</summary>
        /// <param name="ground">Ground index, 0 to <see cref="GroundCount"/> - 1.</param>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="ground"/> is out of range.</exception>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public string GroundName(int ground) => Str(Native.om_profile_ground_name(Live, ground), ground);

        /// <summary>
        /// Number of plain fill tiles for a ground; 0 means the ground can't be painted
        /// usefully (there is nothing to fill it with), so UIs should hide or disable it.
        /// </summary>
        /// <param name="ground">Ground index.</param>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public int GroundFillCount(int ground) => Native.om_profile_ground_fill_count(Live, ground);

        /// <summary>Index of the ground with this id, or -1.</summary>
        /// <param name="id">Short ground id, e.g. "mud", "san", "roc".</param>
        /// <returns>The ground index, or -1 if the profile has no such ground.</returns>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public int FindGround(string id) => Native.om_profile_find_ground(Live, id);

        /// <summary>True if the profile has transition tiles between the two grounds.</summary>
        /// <param name="a">First ground index.</param>
        /// <param name="b">Second ground index.</param>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public bool GroundsBlend(int a, int b) => Native.om_profile_grounds_blend(Live, a, b) != 0;

        /// <summary>
        /// 1 if the ground has its own cliff tiles, 0 if cliffs drawn on it use the
        /// fallback cliffs (Outpost 2: mud), -1 if the profile has no usable cliffs.
        /// </summary>
        /// <param name="ground">Ground index.</param>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public int CliffGround(int ground) => Native.om_profile_cliff_ground(Live, ground);

        /// <summary>
        /// Snaps a point for interactive cliff drawing: moves <paramref name="to"/> so the
        /// segment from <paramref name="from"/> uses an allowed cliff slope (flat, vertical,
        /// 45 degrees, or 2:1, 3:1, 4:1 either way), keeping about the same length.
        /// </summary>
        /// <param name="from">Fixed start of the segment (cell coordinates).</param>
        /// <param name="to">Point to snap, e.g. the mouse position.</param>
        /// <returns>The snapped end point.</returns>
        /// <remarks>Pure geometry: needs no profile instance.</remarks>
        public static System.Drawing.Point SnapCliffPoint(System.Drawing.Point from, System.Drawing.Point to)
        {
            Native.om_cliff_snap_point(from.X, from.Y, to.X, to.Y, out int x, out int y);   // writes the snapped point
            return new System.Drawing.Point(x, y);
        }

        /// <summary>The profile's standard mapping table (for Outpost 2: the one all campaign maps share).</summary>
        /// <returns>
        /// A new array with the table's entries (empty if the profile has none). Useful
        /// for creating new maps and for tests.
        /// </returns>
        /// <exception cref="ObjectDisposedException">The profile was disposed.</exception>
        public TileMapping[] StandardMapping()
        {
            int n = Native.om_profile_standard_mapping_count(Live);
            var result = new TileMapping[n];
            for (int i = 0; i < n; i++)
            {
                Native.om_profile_standard_mapping(Live, i, out Native.MappingOut m);
                // m.tileset points into the profile's memory: copy it, don't free it.
                result[i] = new TileMapping(Marshal.PtrToStringAnsi(m.tileset), m.image);
            }
            return result;
        }

        // The handle for native calls; throws instead of passing a freed (zero)
        // handle, which the C API would silently treat as "do nothing".
        internal IntPtr Live
        {
            get
            {
                if (Handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(BlendProfile));
                return Handle;
            }
        }

        // Copies a library-owned string; the C API returns NULL for an
        // out-of-range ground index, reported here as an argument error.
        private static string Str(IntPtr p, int ground)
        {
            if (p == IntPtr.Zero) throw new ArgumentOutOfRangeException(nameof(ground));
            return Marshal.PtrToStringAnsi(p);
        }

        /// <summary>
        /// Frees the native profile. Dispose every <see cref="BlendMap"/> made from it
        /// first. Calling Dispose more than once is harmless.
        /// </summary>
        public void Dispose()
        {
            if (Handle != IntPtr.Zero)
            {
                Native.om_profile_free(Handle);
                Handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Frees the native profile if <see cref="Dispose"/> was not called.</summary>
        ~BlendProfile()
        {
            if (Handle != IntPtr.Zero) Native.om_profile_free(Handle);
        }
    }

    /// <summary>
    /// Blend state for one map: paint grounds onto cells, then Resolve to get
    /// the tile changes to apply (as one undo step) in the editor.
    /// </summary>
    /// <remarks>
    /// The library keeps its own copy of the map's tiles. Every change it returns
    /// is already applied to that copy; tell it about edits made elsewhere with
    /// <see cref="SetTile"/>. Cells whose tile the profile does not know (cliffs,
    /// walls, ...) are fixed: never changed and ignored when choosing neighbours'
    /// tiles, until painted over. Wraps a native om_blend_map handle, freed by
    /// <see cref="Dispose"/> (or the finalizer).
    /// </remarks>
    public sealed class BlendMap : IDisposable
    {
        // Native om_blend_map*, owned by this object; IntPtr.Zero once disposed.
        private IntPtr _handle;
        private readonly BlendProfile _profile;   // keeps the profile reachable while this map lives

        /// <summary>Map width in cells.</summary>
        public int Width { get; }
        /// <summary>Map height in cells.</summary>
        public int Height { get; }

        /// <summary>Creates the blend state for one map.</summary>
        /// <param name="profile">The blend profile; must stay undisposed while this map is used.</param>
        /// <param name="width">Map width in cells.</param>
        /// <param name="height">Map height in cells.</param>
        /// <param name="mappings">The map's tile mapping table.</param>
        /// <param name="cells">Width*Height mapping indices, row by row.</param>
        /// <exception cref="ArgumentNullException">An argument is null.</exception>
        /// <exception cref="ArgumentException"><paramref name="cells"/> does not hold width*height entries.</exception>
        /// <exception cref="ObjectDisposedException"><paramref name="profile"/> was disposed.</exception>
        /// <exception cref="Op2MapLibException">The native library rejected the input; the message says why.</exception>
        public BlendMap(BlendProfile profile, int width, int height, IList<TileMapping> mappings, IList<int> cells)
        {
            if (profile == null) throw new ArgumentNullException(nameof(profile));
            if (mappings == null) throw new ArgumentNullException(nameof(mappings));
            if (cells == null) throw new ArgumentNullException(nameof(cells));
            if (cells.Count != width * height) throw new ArgumentException("cells must hold width*height entries", nameof(cells));

            // Convert to the native layouts. The marshaller makes temporary ANSI
            // copies of the tileset names for the duration of the call; the native
            // side copies what it needs, so nothing has to outlive the call.
            var native = new Native.MappingIn[mappings.Count];
            for (int i = 0; i < native.Length; i++)
                native[i] = new Native.MappingIn { tileset = mappings[i].Tileset ?? "", image = mappings[i].Image };
            // The C API takes uint16 mapping indices (as stored in .map files).
            var cellArray = new ushort[cells.Count];
            for (int i = 0; i < cellArray.Length; i++) cellArray[i] = (ushort)cells[i];

            var err = new StringBuilder(512);   // receives the native error message
            _handle = Native.om_blend_create(profile.Live, width, height, native, native.Length, cellArray,
                                             err, (UIntPtr)err.Capacity);
            if (_handle == IntPtr.Zero)
                throw new Op2MapLibException("Could not create blend map: " + err);
            _profile = profile;
            Width = width;
            Height = height;
        }

        /// <summary>Ground index of a cell, or -1 for a fixed cell or outside the map.</summary>
        /// <param name="x">Cell column.</param>
        /// <param name="y">Cell row.</param>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public int GroundAt(int x, int y) => Native.om_blend_ground_at(Live, x, y);

        /// <summary>Mapping index the cell holds, or -1 outside the map.</summary>
        /// <param name="x">Cell column.</param>
        /// <param name="y">Cell row.</param>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public int TileAt(int x, int y) => Native.om_blend_tile_at(Live, x, y);

        /// <summary>
        /// What kind of profile tile the cell holds; <see cref="TileRole.Unknown"/> for
        /// a fixed cell or outside the map.
        /// </summary>
        /// <param name="x">Cell column.</param>
        /// <param name="y">Cell row.</param>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public TileRole TileRoleAt(int x, int y) => (TileRole)Native.om_blend_tile_role(Live, x, y);

        /// <summary>
        /// Tell the library a cell was changed outside it (manual edit, undo, redo).
        /// The cell's ground is re-derived from the new tile.
        /// </summary>
        /// <param name="x">Cell column.</param>
        /// <param name="y">Cell row.</param>
        /// <param name="mapping">The mapping index the cell now holds.</param>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public void SetTile(int x, int y, int mapping) => Native.om_blend_set_tile(Live, x, y, mapping);

        /// <summary>
        /// Paints a ground onto cells. Only the ground layer changes; call Resolve
        /// afterwards to pick tiles. Cells outside the map are skipped.
        /// </summary>
        /// <param name="ground">Ground index from the profile.</param>
        /// <param name="cells">Cells to paint.</param>
        /// <returns>The number of cells painted.</returns>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="ground"/> is not a valid ground.</exception>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public int Paint(int ground, IEnumerable<System.Drawing.Point> cells)
        {
            // Flatten to the (x, y) int pairs the C API expects.
            var xy = new List<int>();
            foreach (var p in cells) { xy.Add(p.X); xy.Add(p.Y); }
            int n = Native.om_blend_paint(Live, ground, xy.ToArray(), xy.Count / 2);
            if (n < 0) throw new ArgumentOutOfRangeException(nameof(ground));
            return n;
        }

        /// <summary>
        /// Picks tiles for everything painted since the last resolve (plus one
        /// cell around it) and returns the changes to apply.
        /// </summary>
        /// <param name="seed">Random seed; the same input and seed give the same tiles.</param>
        /// <returns>The tile changes (possibly empty), already applied to the library's copy of the map.</returns>
        /// <exception cref="Op2MapLibException">The native resolve failed.</exception>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        /// <remarks>Passes a NULL region (IntPtr.Zero) to om_blend_resolve, which means "everything painted since the last resolve".</remarks>
        public BlendChange[] Resolve(ulong seed) => Changes(Native.om_blend_resolve(Live, IntPtr.Zero, seed));

        /// <summary>
        /// Picks tiles for a region (plus one cell around it, since a tile depends
        /// on its neighbours) and returns the changes to apply.
        /// </summary>
        /// <param name="region">Cells to re-evaluate.</param>
        /// <param name="seed">Random seed; the same input and seed give the same tiles.</param>
        /// <returns>The tile changes (possibly empty), already applied to the library's copy of the map.</returns>
        /// <exception cref="Op2MapLibException">The native resolve failed.</exception>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public BlendChange[] Resolve(System.Drawing.Rectangle region, ulong seed)
        {
            var r = new Native.Rect { x = region.X, y = region.Y, width = region.Width, height = region.Height };
            return Changes(Native.om_blend_resolve(Live, ref r, seed));
        }

        /// <summary>Re-picks every blendable cell of the whole map ("clean up transitions").</summary>
        /// <param name="seed">Random seed; the same input and seed give the same tiles.</param>
        /// <returns>The tile changes (possibly empty), already applied to the library's copy of the map.</returns>
        /// <exception cref="Op2MapLibException">The native resolve failed.</exception>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public BlendChange[] ResolveAll(ulong seed) => Changes(Native.om_blend_resolve_all(Live, seed));

        /// <summary>
        /// Draws a cliff along the points (repeat the first point at the end to close a
        /// loop) and returns the tile changes to apply, with the cliff tiles' cell types.
        /// The cliff family comes from the ground at the first point. Cliff cells become
        /// fixed for terrain blending.
        /// </summary>
        /// <param name="points">Cell coordinates of the cliff line, at least 2.</param>
        /// <param name="highSide">Which side of the line is high ground.</param>
        /// <param name="wallHeight">Wall height in tiles, 3 to 6 (clamped by the library).</param>
        /// <param name="seed">Random seed; the same input and seed give the same cliff.</param>
        /// <returns>
        /// The tile changes, already applied to the library's copy of the map; empty if
        /// nothing could be drawn (see <see cref="LastCliffStage"/>).
        /// </returns>
        /// <exception cref="ArgumentNullException"><paramref name="points"/> is null.</exception>
        /// <exception cref="ArgumentException">Points outside the map, fewer than 2 points, or an invalid side.</exception>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public BlendChange[] DrawCliff(IList<System.Drawing.Point> points, CliffSide highSide,
                                       double wallHeight, ulong seed)
        {
            if (points == null) throw new ArgumentNullException(nameof(points));
            var xy = new int[points.Count * 2];
            for (int i = 0; i < points.Count; i++)
            {
                xy[2 * i] = points[i].X;
                xy[2 * i + 1] = points[i].Y;
            }
            int n = Native.om_cliff_draw(Live, xy, points.Count, (int)highSide, wallHeight, seed);
            if (n < 0) throw new ArgumentException("Invalid cliff: points outside the map, fewer than 2 points, or a bad side.");
            return Changes(n);
        }

        /// <summary>
        /// Closes cut wall ends after ground was painted across a cliff (e.g. for a ramp).
        /// </summary>
        /// <param name="removed">Width*Height flags, row by row: true = the cell was a cliff before the stroke and is ground now.</param>
        /// <param name="seed">Seed for blending the cells that become ground.</param>
        /// <returns>The changes to apply: re-solved cliff tiles first, then blended ground. Empty if there was nothing to close or no solution.</returns>
        /// <exception cref="ArgumentException">removed has the wrong length.</exception>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public BlendChange[] CloseCliffCut(bool[] removed, ulong seed = 0)
        {
            if (removed == null) throw new ArgumentNullException(nameof(removed));
            if (removed.Length != Width * Height) throw new ArgumentException("removed needs Width*Height entries.", nameof(removed));
            var flags = new byte[removed.Length];
            for (int i = 0; i < removed.Length; i++) flags[i] = removed[i] ? (byte)1 : (byte)0;
            int n = Native.om_cliff_close_cut(Live, flags, seed);
            if (n < 0) throw new InvalidOperationException("Closing the cliff cut failed.");
            return Changes(n);
        }

        /// <summary>
        /// How the last DrawCliff was solved: 0..7 = solver stage (0 strictest), -1 = greedy
        /// fallback (rougher joins), -2 = nothing drawn.
        /// </summary>
        /// <remarks>Stage 0 uses only tile pairs seen in the original game maps; higher stages relax that.</remarks>
        /// <exception cref="ObjectDisposedException">The map was disposed.</exception>
        public int LastCliffStage => Native.om_cliff_last_stage(Live);

        // Turns a native change count into an array. The changes of the last
        // resolve or cliff draw are held inside the native map until the next
        // call, so they are copied out straight away into a managed array
        // (BlendChange is blittable, so the marshaller fills it in place).
        private BlendChange[] Changes(int n)
        {
            if (n < 0) throw new Op2MapLibException("Resolve failed.");
            var result = new BlendChange[n];
            if (n > 0) Native.om_blend_get_changes(Live, result, n);
            return result;
        }

        // The handle for native calls; throws once disposed rather than passing a
        // zero handle, which the C API would silently ignore.
        private IntPtr Live
        {
            get
            {
                if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(BlendMap));
                return _handle;
            }
        }

        /// <summary>Frees the native map. Calling Dispose more than once is harmless.</summary>
        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.om_blend_free(_handle);
                _handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Frees the native map if <see cref="Dispose"/> was not called.</summary>
        ~BlendMap()
        {
            // Safe even if the profile was finalized first in the same GC pass:
            // freeing a map never touches its profile.
            if (_handle != IntPtr.Zero) Native.om_blend_free(_handle);
        }
    }

    /// <summary>Options for <see cref="MapRegions.Find"/>. Values match the C enum om_region_flags.</summary>
    [Flags]
    public enum RegionFlags
    {
        /// <summary>4-connected, no wrapping, every row included.</summary>
        None = 0,
        /// <summary>Diagonal neighbours connect too (otherwise only N/E/S/W).</summary>
        Connect8 = 1,
        /// <summary>The left and right map edges join (Outpost 2 world maps).</summary>
        WrapX = 2,
        /// <summary>Ignore the last row (Outpost 2: never shown, always impassable).</summary>
        SkipBottomRow = 4,
    }

    /// <summary>
    /// Connected regions of passable cells, largest first. The largest region is
    /// the main play area; every other region is enclosed (unreachable from it).
    /// Works on plain cell types (Outpost 2 CellType values 0-31), so it needs no
    /// profile or blend map. Create one with <see cref="Find"/>.
    /// </summary>
    /// <remarks>
    /// A snapshot: it does not follow later edits to the map; call Find again.
    /// Wraps a native om_regions handle, freed by <see cref="Dispose"/> (or the finalizer).
    /// </remarks>
    public sealed class MapRegions : IDisposable
    {
        // Native om_regions*, owned by this object; IntPtr.Zero once disposed.
        private IntPtr _handle;

        /// <summary>Map width in cells.</summary>
        public int Width { get; }
        /// <summary>Map height in cells.</summary>
        public int Height { get; }

        // Takes ownership of a handle returned by om_regions_find.
        private MapRegions(IntPtr handle, int width, int height)
        {
            _handle = handle;
            Width = width;
            Height = height;
        }

        /// <summary>Outpost 2's rules for a map size: 8-connected, bottom row skipped, edges joined on 512x256.</summary>
        /// <param name="width">Map width in cells.</param>
        /// <param name="height">Map height in cells.</param>
        /// <returns>The flags to pass to <see cref="Find"/>.</returns>
        public static RegionFlags Op2Flags(int width, int height) => (RegionFlags)Native.om_regions_op2_flags(width, height);

        /// <summary>
        /// Bit n set = cell type n is passable in Outpost 2: fast, medium and slow
        /// passable, bulldozed, rubble and tubes. Walls, cliffs, vents and the
        /// impassable types block.
        /// </summary>
        public static uint Op2PassableMask => Native.om_cell_types_passable_mask();

        /// <summary>
        /// Finds the regions. cellTypes: width*height cell types, row by row.
        /// passableMask 0 = Outpost 2's passable types; flags null = Outpost 2's rules.
        /// </summary>
        /// <param name="width">Map width in cells.</param>
        /// <param name="height">Map height in cells.</param>
        /// <param name="cellTypes">Width*height cell types (0-31), row by row.</param>
        /// <param name="passableMask">Bit n set = cell type n is passable; 0 = <see cref="Op2PassableMask"/>.</param>
        /// <param name="flags">Connectivity options; null = <see cref="Op2Flags"/> for this size.</param>
        /// <returns>
        /// The regions, sorted largest first (ties: the one whose first cell comes first,
        /// row by row). Dispose it when done.
        /// </returns>
        /// <exception cref="ArgumentNullException"><paramref name="cellTypes"/> is null.</exception>
        /// <exception cref="ArgumentException"><paramref name="cellTypes"/> does not hold width*height entries.</exception>
        /// <exception cref="Op2MapLibException">The native library rejected the input.</exception>
        public static MapRegions Find(int width, int height, byte[] cellTypes, uint passableMask = 0, RegionFlags? flags = null)
        {
            if (cellTypes == null) throw new ArgumentNullException(nameof(cellTypes));
            if (cellTypes.Length != width * height) throw new ArgumentException("cellTypes must hold width*height entries", nameof(cellTypes));
            var f = flags ?? Op2Flags(width, height);
            IntPtr h = Native.om_regions_find(width, height, cellTypes, passableMask, (uint)f);
            if (h == IntPtr.Zero) throw new Op2MapLibException("Could not analyse the map's regions.");
            return new MapRegions(h, width, height);
        }

        /// <summary>Number of regions (0 if no cell is passable).</summary>
        /// <exception cref="ObjectDisposedException">The object was disposed.</exception>
        public int Count => Native.om_regions_count(Live);

        /// <summary>Number of cells in a region (0 = the main play area), or 0 if out of range.</summary>
        /// <param name="region">Region index, 0 to <see cref="Count"/> - 1.</param>
        /// <exception cref="ObjectDisposedException">The object was disposed.</exception>
        public int Size(int region) => Native.om_regions_size(Live, region);

        /// <summary>The cells of a region.</summary>
        /// <param name="region">Region index, 0 to <see cref="Count"/> - 1 (0 = the main play area).</param>
        /// <returns>A new array of cell coordinates; empty if the region is out of range.</returns>
        /// <exception cref="ObjectDisposedException">The object was disposed.</exception>
        public System.Drawing.Point[] Cells(int region) => ToPoints(Size(region), (buf, n) => Native.om_regions_cells(Live, region, buf, n));

        /// <summary>Region of a cell, or -1 if it is not passable, skipped or outside the map.</summary>
        /// <param name="x">Cell column.</param>
        /// <param name="y">Cell row.</param>
        /// <exception cref="ObjectDisposedException">The object was disposed.</exception>
        public int RegionAt(int x, int y) => Native.om_regions_at(Live, x, y);

        /// <summary>Total cells in all enclosed regions (every region but the largest).</summary>
        /// <exception cref="ObjectDisposedException">The object was disposed.</exception>
        public int EnclosedSize => Native.om_regions_enclosed_size(Live);

        /// <summary>All cells of all enclosed regions (every region but the largest).</summary>
        /// <returns>A new array of cell coordinates; empty if nothing is enclosed.</returns>
        /// <exception cref="ObjectDisposedException">The object was disposed.</exception>
        public System.Drawing.Point[] EnclosedCells() => ToPoints(EnclosedSize, (buf, n) => Native.om_regions_enclosed_cells(Live, buf, n));

        // Calls a native "copy up to n cells as (x, y) pairs" function into a
        // managed buffer and converts the result to points. The caller supplies
        // the expected count, so one call is enough; the number actually copied
        // is what counts.
        private static System.Drawing.Point[] ToPoints(int n, Func<int[], int, int> fill)
        {
            // Defensive minimum of one pair; fill is only called when n > 0.
            var buf = new int[Math.Max(1, n) * 2];
            int got = n > 0 ? fill(buf, n) : 0;
            var result = new System.Drawing.Point[got];
            for (int i = 0; i < got; i++) result[i] = new System.Drawing.Point(buf[2 * i], buf[2 * i + 1]);
            return result;
        }

        // The handle for native calls; throws once disposed rather than passing a
        // zero handle, which the C API would silently ignore.
        private IntPtr Live
        {
            get
            {
                if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(MapRegions));
                return _handle;
            }
        }

        /// <summary>Frees the native regions. Calling Dispose more than once is harmless.</summary>
        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.om_regions_free(_handle);
                _handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Frees the native regions if <see cref="Dispose"/> was not called.</summary>
        ~MapRegions()
        {
            if (_handle != IntPtr.Zero) Native.om_regions_free(_handle);
        }
    }

    /// <summary>Symmetry modes of the map generator (om_mapgen_symmetry).</summary>
    public enum MapSymmetry
    {
        /// <summary>No symmetry.</summary>
        None = 0,
        /// <summary>Left half mirrored onto the right (2 players).</summary>
        MirrorX = 1,
        /// <summary>Top half mirrored onto the bottom (2 players).</summary>
        MirrorY = 2,
        /// <summary>Point-symmetric (2 players).</summary>
        Rot180 = 3,
        /// <summary>Four-fold mirrored (4 players).</summary>
        Quad = 4,
    }

    /// <summary>Terrain features the map generator can place (om_mapgen_feature).</summary>
    public enum MapFeature
    {
        /// <summary>A valley between two walls, optionally with a lava-possible floor.</summary>
        Valley = 0,
        /// <summary>A rugged highland blob.</summary>
        Mountain = 1,
        /// <summary>A flatter, rounder highland to build on.</summary>
        Plateau = 2,
        /// <summary>An area kept free of cliffs, volcanoes and scatter.</summary>
        Clearing = 3,
        /// <summary>A barrier across the map with one gap.</summary>
        Choke = 4,
    }

    /// <summary>
    /// Settings for generating a whole map (om_mapgen_settings_*). Start from a
    /// template, change settings, add features, then call <see cref="Generate"/>.
    /// The map generator is a C++ port of fefi-byte's generator from
    /// op2-codegen-editor (MIT, see THIRD-PARTY-NOTICES.md).
    /// </summary>
    /// <remarks>
    /// <code>
    /// using (var gen = new MapGenSettings("islands"))
    /// {
    ///     gen.Set("width", 128); gen.Set("height", 128); gen.Set("spawns", 2);
    ///     gen.Seed = 1234;
    ///     gen.AddFeature(MapFeature.Mountain, "c", "", 0.6, false);
    ///     GeneratedMap map = gen.Generate(profile);
    /// }
    /// </code>
    /// The result's tiles are mapping indices into the profile's standard mapping
    /// table (<see cref="BlendProfile.StandardMapping"/>), which a new map should
    /// use as its own. Same profile, settings and seed give the same map.
    /// </remarks>
    public sealed class MapGenSettings : IDisposable
    {
        private IntPtr _handle;
        private ulong _seed = 1;

        /// <summary>Creates settings from a template: plains, canyon, desert, islands, ice, newterra or learned
        /// (unknown = plains; learned holds suggestions for <see cref="GenerateLearned"/>).</summary>
        /// <param name="template">Template name; null or "" = plains.</param>
        /// <exception cref="Op2MapLibException">The native library could not create the settings.</exception>
        public MapGenSettings(string template = "plains")
        {
            _handle = Native.om_mapgen_settings_create(template ?? "");
            if (_handle == IntPtr.Zero) throw new Op2MapLibException("Could not create map generator settings.");
        }

        /// <summary>
        /// Sets a numeric setting: width, height, roughness, symmetry (<see cref="MapSymmetry"/>),
        /// spawns, spawn_clearance, cliffs, ramps, volcanoes, lava_zone_tiles, smooth,
        /// vegetation_spacing, min_biome, details, landscape, rivers; mountains, mountain_density, gorges,
        /// valleys, plateaus, mountain_spread, world_cutout, learned_rocks; learned generator only: biome_calm,
        /// coherence, calm_terrain, lava_rock, volcanoes_small, volcanoes_medium, volcanoes_large, craters, cracks,
        /// debris, gradient, gradient_ground_start, gradient_ground_end, gradient_terrain_start, gradient_terrain_end.
        /// See op2maplib.h for meanings and defaults.
        /// </summary>
        /// <exception cref="ArgumentException">Unknown key or invalid value.</exception>
        public void Set(string key, double value)
        {
            if (Native.om_mapgen_set(Live, key, value) != 0) throw new ArgumentException("Unknown map generator setting or bad value: " + key, nameof(key));
        }

        /// <summary>Current value of a numeric setting (0 for an unknown key).</summary>
        public double Get(string key) => Native.om_mapgen_get(Live, key);

        /// <summary>The random seed (default 1).</summary>
        public ulong Seed
        {
            get => _seed;
            set { Native.om_mapgen_set_seed(Live, value); _seed = value; }
        }

        /// <summary>Replaces the terrain mix: ground id ("mud", "roc", "san", "veg", "sno") to share.</summary>
        /// <exception cref="ArgumentException">A share is negative or an id is empty.</exception>
        public void SetMix(IDictionary<string, double> mix)
        {
            if (mix == null) throw new ArgumentNullException(nameof(mix));
            Native.om_mapgen_clear_mix(Live);
            foreach (var kv in mix)
                if (Native.om_mapgen_set_mix(Live, kv.Key, kv.Value) != 0) throw new ArgumentException("Bad mix entry " + kv.Key, nameof(mix));
        }

        /// <summary>
        /// Adds a terrain feature. where/to are sectors of a 3x3 grid: "nw", "n", "no",
        /// "w", "c", "o", "sw", "s", "so" (o = east); "" = the seed decides (to: opposite of where).
        /// </summary>
        public void AddFeature(MapFeature kind, string where = "", string to = "", double size = 0.5, bool lava = false)
        {
            if (Native.om_mapgen_add_feature(Live, (int)kind, where ?? "", to ?? "", size, lava ? 1 : 0) != 0)
                throw new ArgumentException("Bad feature kind", nameof(kind));
        }

        /// <summary>Removes all features.</summary>
        public void ClearFeatures() => Native.om_mapgen_clear_features(Live);

        /// <summary>Generates the map.</summary>
        /// <exception cref="Op2MapLibException">The settings were rejected (for example a bad map size); the message says why.</exception>
        public GeneratedMap Generate(BlendProfile profile)
        {
            if (profile == null) throw new ArgumentNullException(nameof(profile));
            var err = new StringBuilder(512);
            IntPtr r = Native.om_mapgen_generate(profile.Live, Live, err, (UIntPtr)err.Capacity);
            if (r == IntPtr.Zero) throw new Op2MapLibException("Could not generate the map: " + err);
            try { return new GeneratedMap(r, profile.GroundCount); }
            finally { Native.om_mapgen_result_free(r); }
        }

        /// <summary>
        /// Generates the map with the learned generator (om_mapgen_generate_learned): maps in the
        /// style of the original maps from two small neural networks (<see cref="LearnedModel"/>).
        /// Width and height must be multiples of 8. The map does not depend on <paramref name="threads"/>.
        /// </summary>
        /// <param name="profile">The Outpost 2 profile (profiles/op2.json).</param>
        /// <param name="model">The network file (profiles/op2-net.bin).</param>
        /// <param name="quality">Sketch only, fast or good.</param>
        /// <param name="threads">Compute threads, 0 = automatic.</param>
        /// <param name="progress">Called with the painted share 0..1; return false to cancel. May be null.</param>
        /// <exception cref="Op2MapLibException">Bad settings, or cancelled; the message says why.</exception>
        public GeneratedMap GenerateLearned(BlendProfile profile, LearnedModel model, LearnedQuality quality = LearnedQuality.Fast,
                                            int threads = 0, Func<double, bool> progress = null)
        {
            if (profile == null) throw new ArgumentNullException(nameof(profile));
            if (model == null) throw new ArgumentNullException(nameof(model));
            Exception failure = null;
            Native.ProgressFn cb = (fraction, user) =>
            {
                if (progress == null) return 0;
                try { return progress(fraction) ? 0 : 1; }
                catch (Exception e) { failure = e; return 1; }   // stop the native call, rethrow below
            };
            var err = new StringBuilder(512);
            IntPtr r = Native.om_mapgen_generate_learned(profile.Live, model.Live, Live, (int)quality, threads, cb, IntPtr.Zero,
                                                         err, (UIntPtr)err.Capacity);
            GC.KeepAlive(cb);
            if (failure != null)
            {
                if (r != IntPtr.Zero) Native.om_mapgen_result_free(r);
                throw failure;
            }
            if (r == IntPtr.Zero) throw new Op2MapLibException("Could not generate the map: " + err);
            try { return new GeneratedMap(r, profile.GroundCount); }
            finally { Native.om_mapgen_result_free(r); }
        }

        private IntPtr Live
        {
            get
            {
                if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(MapGenSettings));
                return _handle;
            }
        }

        /// <summary>Frees the native settings. Calling Dispose more than once is harmless.</summary>
        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.om_mapgen_settings_free(_handle);
                _handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Frees the native settings if <see cref="Dispose"/> was not called.</summary>
        ~MapGenSettings()
        {
            if (_handle != IntPtr.Zero) Native.om_mapgen_settings_free(_handle);
        }
    }

    /// <summary>Quality levels of the learned generator (om_learned_quality).</summary>
    public enum LearnedQuality
    {
        /// <summary>Only the sketch (about a second); the result has no tiles.</summary>
        Sketch = 0,
        /// <summary>6 painting rounds.</summary>
        Fast = 1,
        /// <summary>24 painting rounds, fewer odd spots, about 1.5x slower.</summary>
        Good = 2,
    }

    /// <summary>
    /// The learned generator's networks and tile tables (om_mapgen_model), loaded from
    /// profiles/op2-net.bin. Thread-safe to share between generator calls.
    /// </summary>
    public sealed class LearnedModel : IDisposable
    {
        private IntPtr _handle;

        /// <summary>Loads a network file from memory.</summary>
        /// <exception cref="Op2MapLibException">The data is not a usable network file.</exception>
        public LearnedModel(byte[] data)
        {
            if (data == null) throw new ArgumentNullException(nameof(data));
            var err = new StringBuilder(512);
            _handle = Native.om_mapgen_model_load(data, (UIntPtr)data.Length, err, (UIntPtr)err.Capacity);
            if (_handle == IntPtr.Zero) throw new Op2MapLibException("Could not load the network file: " + err);
        }

        /// <summary>Loads a network file (profiles/op2-net.bin).</summary>
        public static LearnedModel FromFile(string path) => new LearnedModel(System.IO.File.ReadAllBytes(path));

        /// <summary>Loads op2maplib\op2-net.bin next to this assembly (the project file copies it there).</summary>
        /// <exception cref="FileNotFoundException">The file is missing.</exception>
        public static LearnedModel LoadDefault() => FromFile(Path.Combine(Native.AssemblyFolder, "op2maplib", "op2-net.bin"));

        internal IntPtr Live
        {
            get
            {
                if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(LearnedModel));
                return _handle;
            }
        }

        /// <summary>Frees the native model. Calling Dispose more than once is harmless.</summary>
        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.om_mapgen_model_free(_handle);
                _handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Frees the native model if <see cref="Dispose"/> was not called.</summary>
        ~LearnedModel()
        {
            if (_handle != IntPtr.Zero) Native.om_mapgen_model_free(_handle);
        }
    }

    /// <summary>A generated map, copied out of the native result (no handle to dispose).</summary>
    public sealed class GeneratedMap
    {
        /// <summary>Map width in cells.</summary>
        public int Width { get; }
        /// <summary>Map height in cells.</summary>
        public int Height { get; }
        /// <summary>Mapping index per cell (standard mapping table), row by row.</summary>
        public ushort[] Tiles { get; }
        /// <summary>Outpost 2 cell type per cell, row by row.</summary>
        public byte[] CellTypes { get; }
        /// <summary>Spawn points.</summary>
        public System.Drawing.Point[] Spawns { get; }
        /// <summary>Volcano centres.</summary>
        public System.Drawing.Point[] Volcanoes { get; }
        /// <summary>Lava-possible cells, one list per volcano or lava valley.</summary>
        public System.Drawing.Point[][] LavaZones { get; }
        /// <summary>Share of passable cells.</summary>
        public double PassableShare { get; }
        /// <summary>Share of cliff cells.</summary>
        public double PlateauShare { get; }
        /// <summary>Share covered by scattered rocks and craters.</summary>
        public double DetailShare { get; }
        /// <summary>Share of dry riverbed (landscape mode).</summary>
        public double RiverShare { get; }
        /// <summary>True if all spawn points reach each other over passable cells.</summary>
        public bool Connected { get; }
        /// <summary>Share of the map per profile ground.</summary>
        public double[] GroundShares { get; }
        /// <summary>Per <see cref="MapFeature"/>: cliff cells it produced (clearings: how many were placed).</summary>
        public int[] FeatureReport { get; }
        /// <summary>Features as placed: kind, centre/start and size (radius or width).</summary>
        public (MapFeature Kind, int X, int Y, int Size)[] Features { get; }
        /// <summary>
        /// Every statistic of om_mapgen_stat by key, including the zone statistics of
        /// mountains and plateaus ("mountains", "mountains_leaky", "build", "build_mountain", ...).
        /// </summary>
        public IReadOnlyDictionary<string, double> Stats { get; }
        /// <summary>Learned generator only: sketch width (half the map width), else 0.</summary>
        public int SketchWidth { get; }
        /// <summary>Learned generator only: sketch height, else 0.</summary>
        public int SketchHeight { get; }
        /// <summary>Sketch walls row by row: 0 none, 1..8 fall direction E, SE, S, SW, W, NW, N, NE.</summary>
        public byte[] SketchWalls { get; }
        /// <summary>Sketch grounds row by row: 0 mud, 1 rock, 2 sand, 3 vegetation, 4 other.</summary>
        public byte[] SketchGrounds { get; }

        private static readonly string[] StatKeys =
        {
            "passable", "plateau", "details", "rivers", "connected", "mountains", "mountains_leaky", "plateaus",
            "plateaus_unreachable", "build", "build_mountain", "build_plateau", "build_valley", "build_gorge",
            "gorges", "gorges_passable",
        };

        internal GeneratedMap(IntPtr r, int groundCount)
        {
            Width = Native.om_mapgen_width(r);
            Height = Native.om_mapgen_height(r);
            int n = Width * Height;
            Tiles = new ushort[n];
            CellTypes = new byte[n];
            Native.om_mapgen_get_tiles(r, Tiles, n);
            Native.om_mapgen_get_cell_types(r, CellTypes, n);
            Spawns = Points(Native.om_mapgen_point_count(r, 0), (b, c) => Native.om_mapgen_get_points(r, 0, b, c));
            Volcanoes = Points(Native.om_mapgen_point_count(r, 1), (b, c) => Native.om_mapgen_get_points(r, 1, b, c));
            LavaZones = new System.Drawing.Point[Native.om_mapgen_lava_zone_count(r)][];
            for (int z = 0; z < LavaZones.Length; z++)
                LavaZones[z] = Points(Native.om_mapgen_lava_zone_size(r, z), (b, c) => Native.om_mapgen_lava_zone_cells(r, z, b, c));
            PassableShare = Native.om_mapgen_stat(r, "passable");
            PlateauShare = Native.om_mapgen_stat(r, "plateau");
            DetailShare = Native.om_mapgen_stat(r, "details");
            RiverShare = Native.om_mapgen_stat(r, "rivers");
            Connected = Native.om_mapgen_stat(r, "connected") != 0;
            GroundShares = new double[groundCount];
            for (int g = 0; g < groundCount; g++) GroundShares[g] = Native.om_mapgen_ground_share(r, g);
            FeatureReport = new int[5];
            for (int k = 0; k < 5; k++) FeatureReport[k] = Native.om_mapgen_feature_report(r, k);
            var stats = new Dictionary<string, double>();
            foreach (string k in StatKeys) stats[k] = Native.om_mapgen_stat(r, k);
            Stats = stats;
            int sn = Native.om_mapgen_sketch_size(r, out int sw, out int sh);
            SketchWidth = sw;
            SketchHeight = sh;
            SketchWalls = new byte[sn];
            SketchGrounds = new byte[sn];
            if (sn > 0) Native.om_mapgen_get_sketch(r, SketchWalls, SketchGrounds, sn);
            Features = new (MapFeature, int, int, int)[Native.om_mapgen_feature_count(r)];
            for (int i = 0; i < Features.Length; i++)
            {
                Native.om_mapgen_get_feature(r, i, out int kind, out int x, out int y, out int size);
                Features[i] = ((MapFeature)kind, x, y, size);
            }
        }

        // Copies a native (x, y) pair list into points.
        private static System.Drawing.Point[] Points(int n, Func<int[], int, int> fill)
        {
            var buf = new int[Math.Max(1, n) * 2];
            int got = n > 0 ? fill(buf, n) : 0;
            var result = new System.Drawing.Point[got];
            for (int i = 0; i < got; i++) result[i] = new System.Drawing.Point(buf[2 * i], buf[2 * i + 1]);
            return result;
        }
    }

    /// <summary>Raw P/Invoke declarations and native library loading.</summary>
    /// <remarks>
    /// Signatures mirror include/op2maplib/op2maplib.h one to one (cdecl). Handles
    /// (om_profile*, om_blend_map*, om_regions*) travel as IntPtr and are owned by
    /// the wrapper classes, which free them with the matching *_free function.
    /// Returned const char* strings are owned by the library and are only copied,
    /// never freed. Arrays are managed arrays sized by the count argument next to
    /// them ([In] = read by the native side, [Out] = filled by it); blittable ones
    /// (int, ushort, byte, BlendChange) are pinned for the call, MappingIn[] is copied.
    /// </remarks>
    internal static class Native
    {
        // Library name without extension: op2maplib.dll on Windows; the runtime
        // probes libop2maplib.so / .dylib on Linux and macOS.
        private const string Lib = "op2maplib";

        // Folder of this assembly (not the host exe): the x86\, x64\ and op2maplib\
        // folders are copied next to it. Falls back to the AppDomain base folder
        // when Location is empty (e.g. assembly loaded from bytes).
        internal static readonly string AssemblyFolder =
            Path.GetDirectoryName(typeof(Native).Assembly.Location) ?? AppDomain.CurrentDomain.BaseDirectory;

        static Native()
        {
            // Load x86\ or x64\op2maplib.dll for this process's bitness. Once the
            // module is loaded, the DllImports below bind to it by name.
            // A static constructor runs before the first P/Invoke through this
            // class, so the right DLL is always in place in time.
            if (Environment.OSVersion.Platform != PlatformID.Win32NT) return;
            string path = Path.Combine(AssemblyFolder, IntPtr.Size == 8 ? "x64" : "x86", "op2maplib.dll");
            // The module handle is deliberately never freed: the library stays
            // loaded for the life of the process. If the file is missing (or fails
            // to load), the normal DLL search is used on the first call instead.
            if (File.Exists(path)) LoadLibrary(path);
        }

        // Win32 LoadLibraryW; returns the module handle, or IntPtr.Zero on failure.
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibrary(string path);

        // om_mapping as passed in to om_blend_create. The marshaller converts
        // tileset to a temporary ANSI string for the duration of the call.
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        internal struct MappingIn
        {
            [MarshalAs(UnmanagedType.LPStr)] public string tileset;
            public int image;
        }

        // om_mapping as filled by om_profile_standard_mapping. tileset is left as a
        // raw pointer because it is owned by the profile: marshalling it as a
        // string could make the runtime try to free the library's memory.
        [StructLayout(LayoutKind.Sequential)]
        internal struct MappingOut
        {
            public IntPtr tileset;
            public int image;
        }

        // om_rect: x, y of the top-left cell; width/height in cells.
        [StructLayout(LayoutKind.Sequential)]
        internal struct Rect
        {
            public int x, y, width, height;
        }

        // OM_CALL is __cdecl on Windows (matters for 32-bit, where the default is stdcall).
        private const CallingConvention CC = CallingConvention.Cdecl;

        // Functions returning const char* are declared as IntPtr (not string) so the
        // marshaller doesn't try to free library-owned memory.
        [DllImport(Lib, CallingConvention = CC)] internal static extern IntPtr om_version();

        // --- Profiles --------------------------------------------------------
        // json is UTF-8 bytes with an explicit length (size_t = UIntPtr); err is a
        // caller-owned buffer of errSize chars the library writes a message into.
        // Returns an om_profile* to free with om_profile_free, or IntPtr.Zero.
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern IntPtr om_profile_load(byte[] json, UIntPtr len, StringBuilder err, UIntPtr errSize);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_profile_free(IntPtr p);
        [DllImport(Lib, CallingConvention = CC)] internal static extern IntPtr om_profile_name(IntPtr p);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_profile_ground_count(IntPtr p);
        [DllImport(Lib, CallingConvention = CC)] internal static extern IntPtr om_profile_ground_id(IntPtr p, int g);
        [DllImport(Lib, CallingConvention = CC)] internal static extern IntPtr om_profile_ground_name(IntPtr p, int g);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_profile_ground_fill_count(IntPtr p, int g);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern int om_profile_find_ground(IntPtr p, [MarshalAs(UnmanagedType.LPStr)] string id);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_profile_grounds_blend(IntPtr p, int a, int b);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_profile_standard_mapping_count(IntPtr p);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_profile_standard_mapping(IntPtr p, int index, out MappingOut m);

        // --- Blend maps ------------------------------------------------------
        // cells: width*height uint16 mapping indices. The native map copies all
        // input. Returns an om_blend_map* to free with om_blend_free, or IntPtr.Zero.
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern IntPtr om_blend_create(IntPtr profile, int width, int height,
            [In] MappingIn[] mappings, int mappingCount, [In] ushort[] cells, StringBuilder err, UIntPtr errSize);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_blend_free(IntPtr m);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_ground_at(IntPtr m, int x, int y);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_tile_at(IntPtr m, int x, int y);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_tile_role(IntPtr m, int x, int y);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_blend_set_tile(IntPtr m, int x, int y, int mapping);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_paint(IntPtr m, int ground, [In] int[] xy, int count);
        // Two overloads of the same entry point: IntPtr.Zero for a NULL region
        // ("everything painted since the last resolve"), or a pointer to a Rect.
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_resolve(IntPtr m, IntPtr region, ulong seed);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_resolve(IntPtr m, ref Rect region, ulong seed);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_resolve_all(IntPtr m, ulong seed);
        // Copies up to max changes of the last resolve / cliff draw into output.
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_blend_get_changes(IntPtr m, [Out] BlendChange[] output, int max);

        // --- Cliffs ----------------------------------------------------------
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_profile_cliff_ground(IntPtr p, int ground);
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern int om_cliff_draw(IntPtr m, [In] int[] xy, int count, int highSide, double wallHeight, ulong seed);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_cliff_last_stage(IntPtr m);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_cliff_close_cut(IntPtr m, [In] byte[] removed, ulong seed);
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern void om_cliff_snap_point(int ax, int ay, int bx, int by, out int outX, out int outY);

        // --- Passable regions ------------------------------------------------
        // om_regions_find returns an om_regions* to free with om_regions_free, or
        // IntPtr.Zero. The *_cells functions fill xy with up to max (x, y) pairs
        // (so xy needs 2*max ints) and return the number of cells copied.
        [DllImport(Lib, CallingConvention = CC)] internal static extern uint om_regions_op2_flags(int width, int height);
        [DllImport(Lib, CallingConvention = CC)] internal static extern uint om_cell_types_passable_mask();
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern IntPtr om_regions_find(int width, int height, [In] byte[] cellTypes, uint passableMask, uint flags);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_regions_free(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_regions_count(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_regions_size(IntPtr r, int region);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_regions_cells(IntPtr r, int region, [Out] int[] xy, int max);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_regions_at(IntPtr r, int x, int y);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_regions_enclosed_size(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_regions_enclosed_cells(IntPtr r, [Out] int[] xy, int max);

        // --- Map generator ---------------------------------------------------
        // om_mapgen_settings_create returns a handle to free with
        // om_mapgen_settings_free; om_mapgen_generate one to free with
        // om_mapgen_result_free. Keys and ids are ASCII strings.
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern IntPtr om_mapgen_settings_create([MarshalAs(UnmanagedType.LPStr)] string template);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_mapgen_settings_free(IntPtr s);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern int om_mapgen_set(IntPtr s, [MarshalAs(UnmanagedType.LPStr)] string key, double value);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern double om_mapgen_get(IntPtr s, [MarshalAs(UnmanagedType.LPStr)] string key);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_mapgen_set_seed(IntPtr s, ulong seed);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_mapgen_clear_mix(IntPtr s);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern int om_mapgen_set_mix(IntPtr s, [MarshalAs(UnmanagedType.LPStr)] string ground, double share);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_mapgen_clear_features(IntPtr s);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern int om_mapgen_add_feature(IntPtr s, int kind, [MarshalAs(UnmanagedType.LPStr)] string where,
            [MarshalAs(UnmanagedType.LPStr)] string to, double size, int lava);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern IntPtr om_mapgen_generate(IntPtr profile, IntPtr settings, StringBuilder err, UIntPtr errSize);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_mapgen_result_free(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_width(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_height(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_get_tiles(IntPtr r, [Out] ushort[] output, int max);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_get_cell_types(IntPtr r, [Out] byte[] output, int max);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_point_count(IntPtr r, int list);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_get_points(IntPtr r, int list, [Out] int[] xy, int max);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_lava_zone_count(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_lava_zone_size(IntPtr r, int zone);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_lava_zone_cells(IntPtr r, int zone, [Out] int[] xy, int max);
        [DllImport(Lib, CallingConvention = CC, CharSet = CharSet.Ansi)]
        internal static extern double om_mapgen_stat(IntPtr r, [MarshalAs(UnmanagedType.LPStr)] string key);
        [DllImport(Lib, CallingConvention = CC)] internal static extern double om_mapgen_ground_share(IntPtr r, int ground);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_feature_count(IntPtr r);
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern int om_mapgen_get_feature(IntPtr r, int index, out int kind, out int x, out int y, out int size);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_feature_report(IntPtr r, int kind);

        // Learned generator. The progress delegate must stay alive for the call
        // (GenerateLearned keeps it); it returns nonzero to cancel.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int ProgressFn(double fraction, IntPtr user);
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern IntPtr om_mapgen_model_load([In] byte[] data, UIntPtr len, StringBuilder err, UIntPtr errSize);
        [DllImport(Lib, CallingConvention = CC)] internal static extern void om_mapgen_model_free(IntPtr m);
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern IntPtr om_mapgen_generate_learned(IntPtr profile, IntPtr model, IntPtr settings, int quality, int threads,
                                                                ProgressFn progress, IntPtr user, StringBuilder err, UIntPtr errSize);
        [DllImport(Lib, CallingConvention = CC)] internal static extern int om_mapgen_sketch_size(IntPtr r, out int width, out int height);
        [DllImport(Lib, CallingConvention = CC)]
        internal static extern int om_mapgen_get_sketch(IntPtr r, [Out] byte[] walls, [Out] byte[] grounds, int max);
    }
}

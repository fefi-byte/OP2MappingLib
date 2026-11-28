// C API - thin wrappers that turn exceptions into return codes / error text.
//
// Every exported om_* function lives here except the passable-regions ones
// (regions.cpp). The real work is done by om::Profile (profile.cpp),
// om::BlendMap (blend.cpp) and the cliff solver (cliff.cpp); this file only
// checks arguments, guards against NULL handles and makes sure no C++
// exception ever crosses the C boundary.
#include "handles.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>

// Returned by om_version. Keep in step with project(VERSION) in CMakeLists.txt.
#define OM_VERSION_STRING "0.3.5"

using om::setError;

extern "C" {

OM_API const char* OM_CALL om_version(void) { return OM_VERSION_STRING; }

// Parses the JSON into a new om_profile owned by the caller (free with
// om_profile_free). Parse and validation errors arrive as exceptions from
// Profile::fromJson and are turned into NULL plus the message in err; err is
// cleared first so it is empty on success.
OM_API om_profile* OM_CALL om_profile_load(const char* json, size_t json_len, char* err, size_t err_size) {
    setError(err, err_size, "");
    if (!json) {
        setError(err, err_size, "no JSON text given");
        return nullptr;
    }
    try {
        auto* p = new om_profile{om::Profile::fromJson(json, json_len)};
        return p;
    } catch (const std::exception& e) {
        setError(err, err_size, e.what());
    } catch (...) {
        setError(err, err_size, "unknown error");
    }
    return nullptr;
}

// Accepts NULL. Free every blend map created from the profile first.
OM_API void OM_CALL om_profile_free(om_profile* profile) { delete profile; }

OM_API const char* OM_CALL om_profile_name(const om_profile* profile) {
    return profile ? profile->profile.name.c_str() : nullptr;
}

OM_API int OM_CALL om_profile_ground_count(const om_profile* profile) {
    return profile ? static_cast<int>(profile->profile.grounds.size()) : 0;
}

// The ground with this index, or nullptr for a NULL profile or an index out of range.
static const om::Ground* groundOf(const om_profile* profile, int ground) {
    if (!profile || ground < 0 || ground >= static_cast<int>(profile->profile.grounds.size())) return nullptr;
    return &profile->profile.grounds[ground];
}

OM_API const char* OM_CALL om_profile_ground_id(const om_profile* profile, int ground) {
    const om::Ground* g = groundOf(profile, ground);
    return g ? g->id.c_str() : nullptr;
}

OM_API const char* OM_CALL om_profile_ground_name(const om_profile* profile, int ground) {
    const om::Ground* g = groundOf(profile, ground);
    return g ? g->name.c_str() : nullptr;
}

OM_API int OM_CALL om_profile_ground_fill_count(const om_profile* profile, int ground) {
    const om::Ground* g = groundOf(profile, ground);
    return g ? static_cast<int>(g->fill.size()) : 0;
}

OM_API int OM_CALL om_profile_find_ground(const om_profile* profile, const char* id) {
    return (profile && id) ? profile->profile.findGround(id) : -1;
}

OM_API int OM_CALL om_profile_grounds_blend(const om_profile* profile, int ground_a, int ground_b) {
    return (profile && profile->profile.blends(ground_a, ground_b)) ? 1 : 0;
}

OM_API int OM_CALL om_profile_standard_mapping_count(const om_profile* profile) {
    return profile ? static_cast<int>(profile->profile.standardMapping.size()) : 0;
}

OM_API int OM_CALL om_profile_standard_mapping(const om_profile* profile, int index, om_mapping* out) {
    if (!profile || !out || index < 0 || index >= static_cast<int>(profile->profile.standardMapping.size()))
        return 0;
    const auto& e = profile->profile.standardMapping[index];
    // Points at the profile's lower-case copy of the name; valid while the profile lives.
    out->tileset = profile->profile.tilesets[e.first].c_str();
    out->image = e.second;
    return 1;
}

// Builds a new om_blend_map owned by the caller (free with om_blend_free). The
// map copies the cells but keeps a reference to the profile, so the profile
// must outlive it. Returns NULL with a message in err on bad arguments or
// allocation failure.
OM_API om_blend_map* OM_CALL om_blend_create(const om_profile* profile, int32_t width, int32_t height,
                                             const om_mapping* mappings, int32_t mapping_count,
                                             const uint16_t* cells, char* err, size_t err_size) {
    setError(err, err_size, "");
    if (!profile || !cells || width <= 0 || height <= 0 || mapping_count < 0 || (mapping_count > 0 && !mappings)) {
        setError(err, err_size, "invalid arguments");
        return nullptr;
    }
    try {
        auto* m = new om_blend_map;
        m->map = std::make_unique<om::BlendMap>(profile->profile, width, height, mappings, mapping_count, cells);
        return m;
    } catch (const std::bad_alloc&) {
        setError(err, err_size, "out of memory");
    } catch (const std::exception& e) {
        setError(err, err_size, e.what());
    }
    return nullptr;
}

// Accepts NULL.
OM_API void OM_CALL om_blend_free(om_blend_map* map) { delete map; }

// Simple queries and edits: forward to BlendMap, with a safe value for a NULL map.

OM_API int OM_CALL om_blend_ground_at(const om_blend_map* map, int32_t x, int32_t y) {
    return map ? map->map->groundAt(x, y) : -1;
}

OM_API int OM_CALL om_blend_tile_at(const om_blend_map* map, int32_t x, int32_t y) {
    return map ? map->map->tileAt(x, y) : -1;
}

OM_API int OM_CALL om_blend_tile_role(const om_blend_map* map, int32_t x, int32_t y) {
    return map ? map->map->roleAt(x, y) : OM_TILE_UNKNOWN;
}

OM_API void OM_CALL om_blend_set_tile(om_blend_map* map, int32_t x, int32_t y, int32_t mapping) {
    if (map) map->map->setTile(x, y, mapping);
}

// -1 for a NULL map, a negative count or missing points; BlendMap::paint
// rejects an invalid ground the same way.
OM_API int OM_CALL om_blend_paint(om_blend_map* map, int ground, const int32_t* xy, int32_t count) {
    if (!map || (count > 0 && !xy) || count < 0) return -1;
    return map->map->paint(ground, xy, count);
}

// Picks tiles for region (NULL = the dirty area). Any exception (in practice
// only out of memory) becomes -1.
OM_API int OM_CALL om_blend_resolve(om_blend_map* map, const om_rect* region, uint64_t seed) {
    if (!map) return -1;
    try {
        return map->map->resolve(region, seed);
    } catch (...) {
        return -1;
    }
}

// The whole map as one explicit region. Unlike resolve(NULL) this does not
// depend on what was painted, so it also tidies tiles placed by other tools.
OM_API int OM_CALL om_blend_resolve_all(om_blend_map* map, uint64_t seed) {
    if (!map) return -1;
    om_rect all{0, 0, map->map->width(), map->map->height()};
    return om_blend_resolve(map, &all, seed);
}

// Copies the change list of the last resolve or cliff draw into the caller's
// buffer. Callers usually pass the count that call returned as max.
OM_API int OM_CALL om_blend_get_changes(const om_blend_map* map, om_change* out, int32_t max) {
    if (!map || !out || max <= 0) return 0;
    const auto& changes = map->map->changes();
    const int n = std::min<int>(max, static_cast<int>(changes.size()));
    std::memcpy(out, changes.data(), sizeof(om_change) * static_cast<size_t>(n));
    return n;
}

// Cliff families are matched to grounds by id. 1 = the ground's own family is
// usable, 0 = only the fallback family is, -1 = neither (also for a NULL
// profile or a bad ground index).
OM_API int OM_CALL om_profile_cliff_ground(const om_profile* profile, int ground) {
    if (!profile || ground < 0 || ground >= static_cast<int>(profile->profile.grounds.size())) return -1;
    const om::CliffData& c = profile->profile.cliffs;
    if (c.usable(c.findFamily(profile->profile.grounds[ground].id))) return 1;
    return c.usable(c.findFamily(c.fallback)) ? 0 : -1;
}

// Argument checks are done by BlendMap::drawCliff; any exception becomes -1.
OM_API int OM_CALL om_cliff_draw(om_blend_map* map, const int32_t* xy, int32_t point_count,
                                 int high_side, double wall_height, uint64_t seed) {
    if (!map) return -1;
    try {
        return map->map->drawCliff(xy, point_count, high_side, wall_height, seed);
    } catch (...) {
        return -1;
    }
}

OM_API int OM_CALL om_cliff_close_cut(om_blend_map* map, const uint8_t* removed, uint64_t seed) {
    if (!map || !removed) return -1;
    try {
        return map->map->closeCliffCut(removed, seed);
    } catch (...) {
        return -1;
    }
}

OM_API int OM_CALL om_cliff_last_stage(const om_blend_map* map) {
    return map ? map->map->lastCliffStage() : -2;
}

// Pure geometry: needs no map or profile. Either output pointer may be NULL.
OM_API void OM_CALL om_cliff_snap_point(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                                        int32_t* out_x, int32_t* out_y) {
    int x, y;
    om::snapCliffPoint(ax, ay, bx, by, x, y);
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

}  // extern "C"

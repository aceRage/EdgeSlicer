#pragma once

// Bookkeeping for gizmos that keep their own pointers into the canvas' GLVolumes.
//
// The Measure / Assembly gizmo (and Brim ears) ray-cast the selected objects themselves: they keep a
// map GLVolume* -> SceneRaycasterItem, and every item holds a raw pointer to that volume's
// GLVolume::mesh_raycaster (whose AABB tree is what the ray query walks). GLCanvas3D::reload_scene()
// deletes every GLVolume whose ModelVolume has gone (assemble, merge, split, delete, undo / redo,
// reload from disk) and GLCanvas3D::reset_volumes() deletes all of them, so a cached entry can
// outlive the volume it points at. Ray-casting it then reads freed memory (Sentry EDGESLICER-8:
// EXCEPTION_ACCESS_VIOLATION_READ in AABBMesh::AABBImpl::intersect_ray from GLGizmoMeasure::on_render).
//
// These helpers answer "is every cached pointer still a live volume, and does it still own the same
// raycaster?" without dereferencing a cached pointer: a stale pointer is only ever compared as a value
// against the canvas' current volume list. They are header-only and free of GL so the unit tests
// (tests/slic3rutils/gizmo_volume_refs_tests.cpp) can drive them with plain structs.

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace GUI { namespace GizmoVolumeRefs {

// Snapshot of the canvas' volumes: which GLVolume pointers are alive right now and which raycaster
// each one owns. Build it from GLCanvas3D::get_volumes().volumes.
template<class Volume, class Raycaster>
class LiveVolumes
{
public:
    // raycaster_of(const Volume&) -> const Raycaster* (may return nullptr).
    template<class VolumePtrRange, class RaycasterOf>
    LiveVolumes(const VolumePtrRange &volumes, RaycasterOf raycaster_of)
    {
        for (const Volume *v : volumes)
            if (v != nullptr)
                m_raycasters.emplace(v, raycaster_of(*v));
    }

    bool contains(const Volume *v) const { return v != nullptr && m_raycasters.count(v) != 0; }

    // The raycaster the live volume owns now, or nullptr when v is not a live volume.
    const Raycaster *raycaster_of(const Volume *v) const
    {
        auto it = m_raycasters.find(v);
        return it == m_raycasters.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<const Volume *, const Raycaster *> m_raycasters;
};

// Keys of a GLVolume* -> pick item cache that no longer match the canvas: the volume is gone, or it
// now owns a different raycaster than the one the item points at, or the item is empty.
// Item needs get_raycaster() returning a pointer comparable with the live raycaster.
template<class Volume, class Raycaster, class Item>
std::vector<Volume *> stale_pick_keys(const std::map<Volume *, std::shared_ptr<Item>> &cache,
                                      const LiveVolumes<Volume, Raycaster>        &live)
{
    std::vector<Volume *> stale;
    for (const auto &[volume, item] : cache) {
        const Raycaster *now = live.raycaster_of(volume);
        if (now == nullptr || item == nullptr || item->get_raycaster() != now)
            stale.push_back(volume);
    }
    return stale;
}

// True when any key of a GLVolume*-keyed map is not a live volume.
template<class Volume, class Raycaster, class Value>
bool has_dead_key(const std::map<Volume *, Value> &map, const LiveVolumes<Volume, Raycaster> &live)
{
    for (const auto &kv : map)
        if (!live.contains(kv.first))
            return true;
    return false;
}

// True when any non-null pointer in the range is not a live volume (nullptr means "nothing picked").
template<class Volume, class Raycaster, class VolumePtrRange>
bool has_dead_pointer(const VolumePtrRange &pointers, const LiveVolumes<Volume, Raycaster> &live)
{
    for (const Volume *v : pointers)
        if (v != nullptr && !live.contains(v))
            return true;
    return false;
}

}}} // namespace Slic3r::GUI::GizmoVolumeRefs

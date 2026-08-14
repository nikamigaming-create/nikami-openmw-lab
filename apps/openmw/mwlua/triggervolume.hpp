#ifndef MWLUA_TRIGGERVOLUME_H
#define MWLUA_TRIGGERVOLUME_H

#include <set>
#include <utility>

#include <osg/Vec3f>

#include <components/esm/position.hpp>
#include <components/esm4/loadrefr.hpp>

#include "object.hpp"

namespace MWLua
{
    class EngineEvents;
    class ObjectLists;

    bool intersectsTriggerBox(const ESM4::Primitive& primitive, float scale, const ESM::Position& triggerPosition,
        const osg::Vec3f& actorCenter, const osg::Vec3f& actorHalfExtents);

    class TriggerVolumeTracker
    {
    public:
        void update(const ObjectLists& objects, EngineEvents& events);
        void clear() { mOverlaps.clear(); }

    private:
        using Overlap = std::pair<ObjectId, ObjectId>;
        std::set<Overlap> mOverlaps;
    };
}

#endif

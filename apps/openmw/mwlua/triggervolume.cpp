#include "triggervolume.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <components/misc/convert.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/worldmodel.hpp"

#include "engineevents.hpp"
#include "objectlists.hpp"

namespace
{
    constexpr float sSatEpsilon = 1e-5f;

    bool objectsShareSpace(const MWWorld::Ptr& lhs, const MWWorld::Ptr& rhs)
    {
        if (!lhs.isInCell() || !rhs.isInCell())
            return false;
        if (lhs.getCell() == rhs.getCell())
            return true;
        return lhs.getCell()->getCell()->getWorldSpace() == rhs.getCell()->getCell()->getWorldSpace();
    }

    bool isUsable(const MWWorld::Ptr& ptr)
    {
        return !ptr.isEmpty() && ptr.isInCell() && ptr.getRefData().isEnabled() && ptr.getCellRef().getCount() > 0
            && !ptr.mRef->isDeleted();
    }
}

namespace MWLua
{
    bool intersectsTriggerBox(const ESM4::Primitive& primitive, float scale, const ESM::Position& triggerPosition,
        const osg::Vec3f& actorCenter, const osg::Vec3f& actorHalfExtents)
    {
        if (primitive.mType != ESM4::Primitive::Box || scale <= 0.f)
            return false;

        const osg::Quat orientation = Misc::Convert::makeOsgQuat(triggerPosition);
        const std::array<osg::Vec3f, 3> axes{
            orientation * osg::Vec3f(1.f, 0.f, 0.f),
            orientation * osg::Vec3f(0.f, 1.f, 0.f),
            orientation * osg::Vec3f(0.f, 0.f, 1.f),
        };
        const std::array<float, 3> triggerHalf{
            std::abs(primitive.mBounds[0] * scale) * 0.5f,
            std::abs(primitive.mBounds[1] * scale) * 0.5f,
            std::abs(primitive.mBounds[2] * scale) * 0.5f,
        };
        const std::array<float, 3> actorHalf{
            std::abs(actorHalfExtents.x()),
            std::abs(actorHalfExtents.y()),
            std::abs(actorHalfExtents.z()),
        };
        if (triggerHalf[0] == 0.f || triggerHalf[1] == 0.f || triggerHalf[2] == 0.f)
            return false;

        float rotation[3][3]{};
        float absoluteRotation[3][3]{};
        for (std::size_t i = 0; i < 3; ++i)
        {
            rotation[i][0] = axes[i].x();
            rotation[i][1] = axes[i].y();
            rotation[i][2] = axes[i].z();
            for (std::size_t j = 0; j < 3; ++j)
                absoluteRotation[i][j] = std::abs(rotation[i][j]) + sSatEpsilon;
        }

        const osg::Vec3f worldDelta = actorCenter - triggerPosition.asVec3();
        const std::array<float, 3> translation{
            worldDelta * axes[0],
            worldDelta * axes[1],
            worldDelta * axes[2],
        };

        // The trigger's three local axes.
        for (std::size_t i = 0; i < 3; ++i)
        {
            const float projectedActor = actorHalf[0] * absoluteRotation[i][0]
                + actorHalf[1] * absoluteRotation[i][1] + actorHalf[2] * absoluteRotation[i][2];
            if (std::abs(translation[i]) > triggerHalf[i] + projectedActor)
                return false;
        }

        // The actor AABB's three world axes.
        for (std::size_t j = 0; j < 3; ++j)
        {
            const float projectedTranslation = std::abs(
                translation[0] * rotation[0][j] + translation[1] * rotation[1][j]
                + translation[2] * rotation[2][j]);
            const float projectedTrigger = triggerHalf[0] * absoluteRotation[0][j]
                + triggerHalf[1] * absoluteRotation[1][j] + triggerHalf[2] * absoluteRotation[2][j];
            if (projectedTranslation > actorHalf[j] + projectedTrigger)
                return false;
        }

        // The nine pairwise cross-product axes from the OBB/AABB separating-axis test.
        for (std::size_t i = 0; i < 3; ++i)
        {
            const std::size_t i1 = (i + 1) % 3;
            const std::size_t i2 = (i + 2) % 3;
            for (std::size_t j = 0; j < 3; ++j)
            {
                const std::size_t j1 = (j + 1) % 3;
                const std::size_t j2 = (j + 2) % 3;
                const float projectedTranslation
                    = std::abs(translation[i2] * rotation[i1][j] - translation[i1] * rotation[i2][j]);
                const float projectedTrigger = triggerHalf[i1] * absoluteRotation[i2][j]
                    + triggerHalf[i2] * absoluteRotation[i1][j];
                const float projectedActor = actorHalf[j1] * absoluteRotation[i][j2]
                    + actorHalf[j2] * absoluteRotation[i][j1];
                if (projectedTranslation > projectedTrigger + projectedActor)
                    return false;
            }
        }
        return true;
    }

    void TriggerVolumeTracker::update(const ObjectLists& objects, EngineEvents& events)
    {
        MWWorld::WorldModel* worldModel = MWBase::Environment::get().getWorldModel();
        MWBase::World* world = MWBase::Environment::tryGetWorld();
        if (worldModel == nullptr || world == nullptr)
            return;

        std::set<Overlap> current;
        const ObjectIdList activators = objects.getActivatorsInScene();
        const ObjectIdList actors = objects.getActorsInScene();
        struct ActiveActor
        {
            ObjectId mId;
            MWWorld::Ptr mPtr;
            osg::Vec3f mCenter;
            osg::Vec3f mHalfExtents;
        };
        std::vector<ActiveActor> activeActors;
        activeActors.reserve(actors->size());
        for (const ObjectId actorId : *actors)
        {
            MWWorld::Ptr actor = worldModel->getPtr(actorId);
            if (!isUsable(actor))
                continue;
            activeActors.push_back(
                { actorId, actor, world->getActorCollisionPosition(actor), world->getHalfExtents(actor) });
        }

        for (const ObjectId triggerId : *activators)
        {
            const MWWorld::Ptr trigger = worldModel->getPtr(triggerId);
            if (!isUsable(trigger) || trigger.getRefData().getLuaScripts() == nullptr)
                continue;
            const ESM4::Primitive* primitive = trigger.getCellRef().getEsm4Primitive();
            if (primitive == nullptr || primitive->mType != ESM4::Primitive::Box)
                continue;

            for (const ActiveActor& actor : activeActors)
            {
                if (!objectsShareSpace(trigger, actor.mPtr))
                    continue;
                if (!intersectsTriggerBox(*primitive, trigger.getCellRef().getScale(),
                        trigger.getRefData().getPosition(), actor.mCenter, actor.mHalfExtents))
                    continue;

                const Overlap overlap{ triggerId, actor.mId };
                current.insert(overlap);
                if (!mOverlaps.contains(overlap))
                    events.addToQueue(EngineEvents::OnTriggerEnter{ actor.mId, triggerId });
            }
        }

        for (const Overlap& overlap : mOverlaps)
        {
            if (current.contains(overlap))
                continue;
            const MWWorld::Ptr trigger = worldModel->getPtr(overlap.first);
            const MWWorld::Ptr actor = worldModel->getPtr(overlap.second);
            if (isUsable(trigger) && isUsable(actor))
                events.addToQueue(EngineEvents::OnTriggerLeave{ overlap.second, overlap.first });
        }
        mOverlaps = std::move(current);
    }
}

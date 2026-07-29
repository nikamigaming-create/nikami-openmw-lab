#ifndef GAME_MWWORLD_CELLVISITORS_H
#define GAME_MWWORLD_CELLVISITORS_H

#include <string>
#include <vector>

#include <components/sceneutil/positionattitudetransform.hpp>

#include "ptr.hpp"

namespace MWWorld
{
    template <class DoorRange, class Visitor>
    void forEachTeleportDoor(const DoorRange& doors, Visitor&& visitor)
    {
        for (const auto& door : doors)
        {
            if (door.mRef.getTeleport())
                visitor(door);
        }
    }

    struct ListAndResetObjectsVisitor
    {
        std::vector<MWWorld::Ptr> mObjects;

        bool operator()(const MWWorld::Ptr& ptr)
        {
            if (ptr.getRefData().getBaseNode())
            {
                ptr.getRefData().setBaseNode(nullptr);
            }
            mObjects.push_back(ptr);

            return true;
        }
    };

}

#endif

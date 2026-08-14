#ifndef GAME_MWWORLD_CELLVISITORS_H
#define GAME_MWWORLD_CELLVISITORS_H

#include <string>
#include <vector>

<<<<<<< HEAD
=======
#include <components/sceneutil/positionattitudetransform.hpp>

>>>>>>> origin/main
#include "ptr.hpp"

namespace MWWorld
{
<<<<<<< HEAD
=======
    template <class DoorRange, class Visitor>
    void forEachTeleportDoor(const DoorRange& doors, Visitor&& visitor)
    {
        for (const auto& door : doors)
        {
            if (door.mRef.getTeleport())
                visitor(door);
        }
    }

>>>>>>> origin/main
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

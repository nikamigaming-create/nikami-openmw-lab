#ifndef GAME_MWCLASS_FNVFURNITUREPLACEMENT_H
#define GAME_MWCLASS_FNVFURNITUREPLACEMENT_H

#include "esm4npc.hpp"

namespace ESM4
{
    struct Reference;
}

namespace MWWorld
{
    class ESMStore;
}

namespace MWClass
{
    // Resolve the selected NIF furniture marker into the same runtime placement
    // used by both scene insertion and an in-flight EvaluatePackage handoff.
    // Keeping this here prevents a script-driven package change from silently
    // degrading to the furniture origin after a valid seated claim is released.
    FalloutFurniturePlacement makeFalloutFurniturePlacement(
        const MWWorld::ESMStore& store, const ESM4::Reference& furniture);
}

#endif // GAME_MWCLASS_FNVFURNITUREPLACEMENT_H

#include "gamecontent.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/esmstore.hpp"

namespace MWGui
{
    bool usesFalloutNewVegasInterface(MWWorld::ESM4Game game) noexcept
    {
        return game == MWWorld::ESM4Game::FalloutNewVegas;
    }

    bool usesFalloutNewVegasInterface() noexcept
    {
        const MWBase::World* const world = MWBase::Environment::tryGetWorld();
        return world != nullptr && usesFalloutNewVegasInterface(world->getStore().getESM4Game());
    }
}

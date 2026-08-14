#ifndef OPENMW_COMPONENTS_DETOURNAVIGATOR_RECASTMESHPROVIDER_H
#define OPENMW_COMPONENTS_DETOURNAVIGATOR_RECASTMESHPROVIDER_H

<<<<<<< HEAD
#include "tileposition.hpp"

#include <components/esm/refid.hpp>

=======
#include "recastmesh.hpp"
#include "tilecachedrecastmeshmanager.hpp"
#include "tileposition.hpp"

#include <functional>
>>>>>>> origin/main
#include <memory>

namespace DetourNavigator
{
    class RecastMesh;

<<<<<<< HEAD
    struct RecastMeshProvider
    {
        virtual ~RecastMeshProvider() = default;

        virtual std::shared_ptr<RecastMesh> getMesh(ESM::RefId worldspace, const TilePosition& tilePosition) const = 0;
=======
    class RecastMeshProvider
    {
    public:
        RecastMeshProvider(TileCachedRecastMeshManager& impl)
            : mImpl(impl)
        {
        }

        std::shared_ptr<RecastMesh> getMesh(ESM::RefId worldspace, const TilePosition& tilePosition) const
        {
            return mImpl.get().getNewMesh(worldspace, tilePosition);
        }

    private:
        std::reference_wrapper<TileCachedRecastMeshManager> mImpl;
>>>>>>> origin/main
    };
}

#endif

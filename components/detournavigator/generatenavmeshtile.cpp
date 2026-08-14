#include "generatenavmeshtile.hpp"

#include "dbrefgeometryobject.hpp"
#include "makenavmesh.hpp"
<<<<<<< HEAD
#include "navmeshdata.hpp"
=======
>>>>>>> origin/main
#include "preparednavmeshdata.hpp"
#include "serialization.hpp"
#include "settings.hpp"

#include <components/debug/debuglog.hpp>

<<<<<<< HEAD
#include <DetourNavMesh.h>

=======
>>>>>>> origin/main
#include <osg/io_utils>

#include <memory>
#include <optional>
#include <vector>

namespace DetourNavigator
{
    namespace
    {
        struct Ignore
        {
            ESM::RefId mWorldspace;
            const TilePosition& mTilePosition;
            std::shared_ptr<NavMeshTileConsumer> mConsumer;

            ~Ignore() noexcept
            {
                if (mConsumer != nullptr)
                    mConsumer->ignore(mWorldspace, mTilePosition);
            }
        };
    }

    GenerateNavMeshTile::GenerateNavMeshTile(ESM::RefId worldspace, const TilePosition& tilePosition,
<<<<<<< HEAD
        std::weak_ptr<const RecastMeshProvider> recastMeshProvider, const AgentBounds& agentBounds,
        const DetourNavigator::Settings& settings, bool collectStats, std::weak_ptr<NavMeshTileConsumer> consumer)
        : mWorldspace(worldspace)
        , mTilePosition(tilePosition)
        , mRecastMeshProvider(std::move(recastMeshProvider))
        , mAgentBounds(agentBounds)
        , mSettings(settings)
        , mCollectStats(collectStats)
=======
        RecastMeshProvider recastMeshProvider, const AgentBounds& agentBounds,
        const DetourNavigator::Settings& settings, std::weak_ptr<NavMeshTileConsumer> consumer)
        : mWorldspace(worldspace)
        , mTilePosition(tilePosition)
        , mRecastMeshProvider(recastMeshProvider)
        , mAgentBounds(agentBounds)
        , mSettings(settings)
>>>>>>> origin/main
        , mConsumer(std::move(consumer))
    {
    }

    void GenerateNavMeshTile::doWork()
    {
        impl();
    }

    void GenerateNavMeshTile::impl() noexcept
    {
<<<<<<< HEAD
        try
        {
            const std::shared_ptr<NavMeshTileConsumer> consumer = mConsumer.lock();

            if (consumer == nullptr)
                return;

            Ignore ignore{ mWorldspace, mTilePosition, consumer };

            const std::shared_ptr<const RecastMeshProvider> recastMeshProvider = mRecastMeshProvider.lock();

            if (recastMeshProvider == nullptr)
                return;

            const std::shared_ptr<RecastMesh> recastMesh = recastMeshProvider->getMesh(mWorldspace, mTilePosition);
=======
        const auto consumer = mConsumer.lock();

        if (consumer == nullptr)
            return;

        try
        {
            Ignore ignore{ mWorldspace, mTilePosition, consumer };

            const std::shared_ptr<RecastMesh> recastMesh = mRecastMeshProvider.getMesh(mWorldspace, mTilePosition);
>>>>>>> origin/main

            if (recastMesh == nullptr || isEmpty(*recastMesh))
                return;

            const std::vector<DbRefGeometryObject> objects = makeDbRefGeometryObjects(
                recastMesh->getMeshSources(), [&](const MeshSource& v) { return consumer->resolveMeshSource(v); });
            std::vector<std::byte> input = serialize(mSettings.mRecast, mAgentBounds, *recastMesh, objects);
            const std::optional<NavMeshTileInfo> info = consumer->find(mWorldspace, mTilePosition, input);

            if (info.has_value() && info->mVersion == navMeshFormatVersion)
            {
                consumer->identity(mWorldspace, mTilePosition, info->mTileId);
<<<<<<< HEAD

                if (mCollectStats)
                {
                    const NavMeshData tileData
                        = makeNavMeshTileData(*info->mData, {}, mAgentBounds, mTilePosition, mSettings.mRecast);
                    const dtMeshHeader& header = *reinterpret_cast<dtMeshHeader*>(tileData.mValue.get());

                    consumer->updateStats(NavMeshTileConsumerStats{
                        .mPolyCount = header.polyCount,
                    });
                }

=======
>>>>>>> origin/main
                ignore.mConsumer = nullptr;
                return;
            }

            const std::unique_ptr<PreparedNavMeshData> data
                = prepareNavMeshTileData(*recastMesh, mWorldspace, mTilePosition, mAgentBounds, mSettings.mRecast);

            if (data == nullptr)
                return;

<<<<<<< HEAD
            // Verify generated data before consuming.
            const NavMeshData tileData = makeNavMeshTileData(*data, {}, mAgentBounds, mTilePosition, mSettings.mRecast);

=======
>>>>>>> origin/main
            if (info.has_value())
                consumer->update(mWorldspace, mTilePosition, info->mTileId, navMeshFormatVersion, *data);
            else
                consumer->insert(mWorldspace, mTilePosition, navMeshFormatVersion, input, *data);

<<<<<<< HEAD
            if (mCollectStats)
            {
                const dtMeshHeader& header = *reinterpret_cast<dtMeshHeader*>(tileData.mValue.get());

                consumer->updateStats(NavMeshTileConsumerStats{
                    .mPolyCount = header.polyCount,
                });
            }

=======
>>>>>>> origin/main
            ignore.mConsumer = nullptr;
        }
        catch (const std::exception& e)
        {
<<<<<<< HEAD
            if (const std::shared_ptr<NavMeshTileConsumer> consumer = mConsumer.lock())
            {
                Log(Debug::Warning) << "Failed to generate navmesh for worldspace \"" << mWorldspace << "\" tile "
                                    << mTilePosition << ": " << e.what();
                consumer->cancel(e.what());
            }
=======
            Log(Debug::Warning) << "Failed to generate navmesh for worldspace \"" << mWorldspace << "\" tile "
                                << mTilePosition << ": " << e.what();
            consumer->cancel(e.what());
>>>>>>> origin/main
        }
    }
}

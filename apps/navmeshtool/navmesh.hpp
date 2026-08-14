#ifndef OPENMW_NAVMESHTOOL_NAVMESH_H
#define OPENMW_NAVMESHTOOL_NAVMESH_H

#include <cstddef>

namespace DetourNavigator
{
    class NavMeshDb;
    struct Settings;
    struct AgentBounds;
}

<<<<<<< HEAD
namespace SceneUtil
{
    class WorkQueue;
}

=======
>>>>>>> origin/main
namespace NavMeshTool
{
    struct WorldspaceData;

<<<<<<< HEAD
    struct GenerateAllNavMeshTilesOptions
    {
        bool mRemoveUnusedTiles;
        bool mWriteBinaryLog;
        bool mCollectStats;
    };

=======
>>>>>>> origin/main
    enum class Status
    {
        Ok,
        Cancelled,
        NotEnoughSpace,
    };

<<<<<<< HEAD
    struct GenerateTilesStats
    {
        int mMaxPolyCountPerTile = 0;
    };

    struct GenerateTilesResult
    {
        Status mStatus;
        std::size_t mProvided;
        std::size_t mInserted;
        std::size_t mUpdated;
        std::size_t mDeleted;
        GenerateTilesStats mStats;
    };

    GenerateTilesResult generateAllNavMeshTiles(const DetourNavigator::AgentBounds& agentBounds,
        const DetourNavigator::Settings& settings, const GenerateAllNavMeshTilesOptions& options,
        const WorldspaceData& data, DetourNavigator::NavMeshDb& db, SceneUtil::WorkQueue& workQueue);
=======
    Status generateAllNavMeshTiles(const DetourNavigator::AgentBounds& agentBounds,
        const DetourNavigator::Settings& settings, std::size_t threadsNumber, bool removeUnusedTiles,
        bool writeBinaryLog, WorldspaceData& cellsData, DetourNavigator::NavMeshDb&& db);
>>>>>>> origin/main
}

#endif

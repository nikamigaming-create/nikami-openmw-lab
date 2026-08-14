#include "terrainstorage.hpp"

#include <sstream>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm4/loadltex.hpp>
#include <components/esm4/loadtxst.hpp>
#include <components/esm4/loadwrld.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"

#include "landmanager.hpp"

namespace MWRender
{

    TerrainStorage::TerrainStorage(Resource::ResourceSystem* resourceSystem, std::string_view normalMapPattern,
        std::string_view normalHeightMapPattern, bool autoUseNormalMaps, std::string_view specularMapPattern,
        bool autoUseSpecularMaps)
        : ESMTerrain::Storage(resourceSystem->getVFS(), normalMapPattern, normalHeightMapPattern, autoUseNormalMaps,
            specularMapPattern, autoUseSpecularMaps)
        , mLandManager(new LandManager(
              ESM::Land::DATA_VCLR | ESM::Land::DATA_VHGT | ESM::Land::DATA_VNML | ESM::Land::DATA_VTEX))
        , mResourceSystem(resourceSystem)
    {
        mResourceSystem->addResourceManager(mLandManager.get());
    }

    TerrainStorage::~TerrainStorage()
    {
        mResourceSystem->removeResourceManager(mLandManager.get());
    }

    bool TerrainStorage::hasData(ESM::ExteriorCellLocation cellLocation)
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        if (ESM::isEsm4Ext(cellLocation.mWorldspace))
        {
            const ESM4::World* worldspace = esmStore.get<ESM4::World>().find(cellLocation.mWorldspace);
            if (!worldspace->mParent.isZeroOrUnset() && worldspace->mParentUseFlags & ESM4::World::UseFlag_Land)
                cellLocation.mWorldspace = worldspace->mParent;

            return esmStore.get<ESM4::Land>().search(cellLocation) != nullptr;
        }
        else
        {
            return esmStore.get<ESM::Land>().search(cellLocation.mX, cellLocation.mY) != nullptr;
        }
    }

    static void BoundUnion(float& minX, float& maxX, float& minY, float& maxY, float x, float y)
    {
        if (x < minX)
            minX = x;
        if (x > maxX)
            maxX = x;
        if (y < minY)
            minY = y;
        if (y > maxY)
            maxY = y;
    }

    void TerrainStorage::getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId worldspace)
    {
        minX = 0;
        minY = 0;
        maxX = 0;
        maxY = 0;

        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        if (ESM::isEsm4Ext(worldspace))
        {
            const ESM4::World* worldRec = esmStore.get<ESM4::World>().find(worldspace);
            if (!worldRec->mParent.isZeroOrUnset() && worldRec->mParentUseFlags & ESM4::World::UseFlag_Land)
                worldspace = worldRec->mParent;

            const auto& lands = esmStore.get<ESM4::Land>().getLands();
            for (const auto& [landPos, _] : lands)
            {
                if (landPos.mWorldspace == worldspace)
                {
                    BoundUnion(minX, maxX, minY, maxY, static_cast<float>(landPos.mX), static_cast<float>(landPos.mY));
                }
            }
        }
        else
        {
            MWWorld::Store<ESM::Land>::iterator it = esmStore.get<ESM::Land>().begin();
            for (; it != esmStore.get<ESM::Land>().end(); ++it)
            {
                BoundUnion(minX, maxX, minY, maxY, static_cast<float>(it->mX), static_cast<float>(it->mY));
            }
        }
        // since grid coords are at cell origin, we need to add 1 cell
        maxX += 1;
        maxY += 1;
    }

    LandManager* TerrainStorage::getLandManager() const
    {
        return mLandManager.get();
    }

    osg::ref_ptr<const ESMTerrain::LandObject> TerrainStorage::getLand(ESM::ExteriorCellLocation cellLocation)
    {
        return mLandManager->getLand(cellLocation);
    }

    const std::string* TerrainStorage::getLandTexture(std::uint16_t index, int plugin)
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        return esmStore.get<ESM::LandTexture>().search(index, plugin);
    }

    const ESM4::LandTexture* TerrainStorage::getEsm4LandTexture(ESM::RefId ltexId) const
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        const MWWorld::Store<ESM4::LandTexture>& ltexStore = esmStore.get<ESM4::LandTexture>();
        if (const ESM4::LandTexture* direct = ltexStore.search(ltexId))
            return direct;

        const ESM::FormId* requestedFormId = ltexId.getIf<ESM::FormId>();
        for (auto it = ltexStore.begin(); requestedFormId != nullptr && it != ltexStore.end(); ++it)
        {
            const ESM4::LandTexture& candidate = *it;
            if (candidate.mId.mIndex == requestedFormId->mIndex)
            {
                static int resolvedIndexFallbackLogs = 0;
                if (resolvedIndexFallbackLogs < 40)
                {
                    ++resolvedIndexFallbackLogs;
                    Log(Debug::Warning) << "World viewer terrain: resolved LTEX " << ltexId.toString()
                                        << " by matching index to loaded " << candidate.mId.toString()
                                        << " editorId=\"" << candidate.mEditorId << "\"";
                }
                return &candidate;
            }
        }

        static int missingLtexSampleLogs = 0;
        if (missingLtexSampleLogs < 20)
        {
            ++missingLtexSampleLogs;
            std::ostringstream sample;
            unsigned int count = 0;
            for (auto it = ltexStore.begin(); it != ltexStore.end() && count < 12; ++it, ++count)
            {
                if (count != 0)
                    sample << ", ";
                sample << it->mId.toString() << ":" << it->mEditorId;
            }
            Log(Debug::Warning) << "World viewer terrain: LTEX lookup failed for " << ltexId.toString()
                                << " storeSize=" << ltexStore.getSize()
                                << " sample=[" << sample.str() << "]";
        }
        return nullptr;
    }

    const ESM4::TextureSet* TerrainStorage::getEsm4TextureSet(ESM::RefId txstId) const
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        return esmStore.get<ESM4::TextureSet>().search(txstId);
    }
}

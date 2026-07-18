#include "cell.hpp"

#include "esmstore.hpp"

#include "../mwbase/environment.hpp"

#include <components/esm3/loadcell.hpp>
#include <components/esm4/lightingcomposition.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadlgtm.hpp>
#include <components/esm4/loadwrld.hpp>
#include <components/misc/algorithm.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace MWWorld
{
    namespace
    {
        std::string getDescription(const ESM4::World& value)
        {
            if (!value.mEditorId.empty())
                return value.mEditorId;

            return ESM::RefId(value.mId).serializeText();
        }

        std::string getCellDescription(const ESM4::Cell& cell, const ESM4::World* world)
        {
            std::string result;

            if (!cell.mEditorId.empty())
                result = cell.mEditorId;
            else if (world != nullptr && cell.isExterior())
                result = getDescription(*world);
            else
                result = cell.mId.serializeText();

            if (cell.isExterior())
                result += " (" + std::to_string(cell.mX) + ", " + std::to_string(cell.mY) + ")";

            return result;
        }
    }

    Cell::Cell(const ESM4::Cell& cell)
        : ESM::CellVariant(cell)
        , mIsExterior(!(cell.mCellFlags & ESM4::CELL_Interior))
        , mIsQuasiExterior(cell.mCellFlags & ESM4::CELL_QuasiExt)
        , mHasWater(cell.mCellFlags & ESM4::CELL_HasWater)
        , mNoSleep(false) // No such notion in ESM4
        , mGridPos(cell.mX, cell.mY)
        , mDisplayname(cell.mFullName)
        , mNameID(cell.mEditorId)
        , mRegion(ESM::RefId()) // Unimplemented for now
        , mId(cell.mId)
        , mParent(cell.mParent)
        , mWaterHeight(cell.mWaterHeight)
        , mMood{
            .mAmbiantColor = cell.mLighting.ambient,
            .mDirectionalColor = cell.mLighting.directional,
            .mFogColor = cell.mLighting.fogColor,
            .mFogDensity = 1.f,
            .mHasFalloutFog = false,
            .mFogNear = cell.mLighting.fogNear,
            .mFogFar = cell.mLighting.fogFar,
            .mFogClipDistance = cell.mLighting.fogClipDist,
            .mFogPower = cell.mLighting.fogPower,
        }
    {
        const ESMStore* store = MWBase::Environment::get().getESMStore();
        const ESM4::World* world = store->get<ESM4::World>().search(mParent);
        ESM4::Lighting lighting = cell.mLighting;
        if (!cell.mLightingTemplate.isZeroOrUnset())
        {
            if (const ESM4::LightingTemplate* lightingTemplate
                = store->get<ESM4::LightingTemplate>().search(ESM::RefId(cell.mLightingTemplate)))
            {
                lighting = ESM4::composeLighting(
                    cell.mLighting, lightingTemplate->mLighting, cell.mLightingTemplateFlags);
            }
        }
        mMood = {
            .mAmbiantColor = lighting.ambient,
            .mDirectionalColor = lighting.directional,
            .mFogColor = lighting.fogColor,
            .mFogDensity = 1.f,
            .mHasFalloutFog = !cell.isExterior() && std::isfinite(lighting.fogNear)
                && std::isfinite(lighting.fogFar) && std::isfinite(lighting.fogPower) && lighting.fogFar > 0.f
                && lighting.fogFar >= lighting.fogNear && lighting.fogPower > 0.f,
            .mFogNear = lighting.fogNear,
            .mFogFar = lighting.fogFar,
            .mFogClipDistance = lighting.fogClipDist,
            .mFogPower = lighting.fogPower,
        };
        if (isExterior())
        {
            if (world == nullptr)
                throw std::runtime_error(
                    "Cell " + cell.mId.toDebugString() + " parent world " + mParent.toDebugString() + " is not found");
            mWaterHeight = world->mWaterLevel;
        }
        mDescription = getCellDescription(cell, world);
    }

    Cell::Cell(const ESM::Cell& cell)
        : ESM::CellVariant(cell)
        , mIsExterior(!(cell.mData.mFlags & ESM::Cell::Interior))
        , mIsQuasiExterior(cell.mData.mFlags & ESM::Cell::QuasiEx)
        , mHasWater(cell.mData.mFlags & ESM::Cell::HasWater)
        , mNoSleep(cell.mData.mFlags & ESM::Cell::NoSleep)
        , mGridPos(cell.getGridX(), cell.getGridY())
        , mDisplayname(cell.mName)
        , mNameID(cell.mName)
        , mRegion(cell.mRegion)
        , mId(cell.mId)
        , mParent(ESM::Cell::sDefaultWorldspaceId)
        , mWaterHeight(cell.mWater)
        , mDescription(cell.getDescription())
        , mMood{
            .mAmbiantColor = cell.mAmbi.mAmbient,
            .mDirectionalColor = cell.mAmbi.mSunlight,
            .mFogColor = cell.mAmbi.mFog,
            .mFogDensity = cell.mAmbi.mFogDensity,
            .mHasFalloutFog = false,
            .mFogNear = 0.f,
            .mFogFar = 0.f,
            .mFogClipDistance = 0.f,
            .mFogPower = 1.f,
        }
    {
        if (isExterior())
        {
            mWaterHeight = -1.f;
            mHasWater = true;
        }
        else
            mGridPos = {};
    }
}

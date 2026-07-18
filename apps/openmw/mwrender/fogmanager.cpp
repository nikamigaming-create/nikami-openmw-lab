#include "fogmanager.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include <components/debug/debuglog.hpp>
#include <components/esm/esmbridge.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/fallback/fallback.hpp>
#include <components/sceneutil/util.hpp>
#include <components/settings/values.hpp>

#include "apps/openmw/mwworld/cell.hpp"

namespace MWRender
{
    bool isUsableFalloutFog(bool hasAuthoredFog, float fogNear, float fogFar, float power)
    {
        return hasAuthoredFog && std::isfinite(fogNear) && std::isfinite(fogFar) && std::isfinite(power)
            && fogFar >= fogNear && power > 0.f;
    }

    float calculateFalloutFogFactor(float distance, float fogNear, float fogFar, float power)
    {
        // FalloutNV.esm intentionally authors equal ranges for the InvertedDaylight variants and HVDustStorm's
        // daytime slot. The retail saturated division is a step for every non-zero numerator; make the origin
        // deterministic instead of evaluating 0/0.
        if (fogFar == fogNear)
            return distance > fogNear ? 1.f : 0.f;

        const float normalizedDistance = std::clamp((distance - fogNear) / (fogFar - fogNear), 0.f, 1.f);
        return std::pow(normalizedDistance, power);
    }

    FogManager::FogManager()
        : mLandFogStart(0.f)
        , mLandFogEnd(std::numeric_limits<float>::max())
        , mUnderwaterFogStart(0.f)
        , mUnderwaterFogEnd(std::numeric_limits<float>::max())
        , mHasFalloutFog(false)
        , mFalloutFogStep(false)
        , mFalloutFogPower(1.f)
        , mFogColor(osg::Vec4f())
        , mUnderwaterColor(Fallback::Map::getColour("Water_UnderwaterColor"))
        , mUnderwaterWeight(Fallback::Map::getFloat("Water_UnderwaterColorWeight"))
        , mUnderwaterIndoorFog(Fallback::Map::getFloat("Water_UnderwaterIndoorFog"))
    {
    }

    void FogManager::configure(float viewDistance, const MWWorld::Cell& cell)
    {
        mHasFalloutFog = false;
        mFalloutFogStep = false;
        mFalloutFogPower = 1.f;

        osg::Vec4f color = SceneUtil::colourFromRGB(cell.getMood().mFogColor);

        const float fogDensity = cell.getMood().mFogDensity;
        if (cell.getMood().mHasFalloutFog)
        {
            configure(viewDistance, fogDensity, mUnderwaterIndoorFog, 1.f, 0.f, color, true,
                cell.getMood().mFogNear, cell.getMood().mFogFar, cell.getMood().mFogPower);
            return;
        }
        if (Settings::fog().mUseDistantFog)
        {
            float density = std::max(0.2f, fogDensity);
            mLandFogStart = Settings::fog().mDistantInteriorFogEnd * (1.0f - density)
                + Settings::fog().mDistantInteriorFogStart * density;
            mLandFogEnd = Settings::fog().mDistantInteriorFogEnd;
            mUnderwaterFogStart = Settings::fog().mDistantUnderwaterFogStart;
            mUnderwaterFogEnd = Settings::fog().mDistantUnderwaterFogEnd;
            mFogColor = color;
        }
        else
            configure(viewDistance, fogDensity, mUnderwaterIndoorFog, 1.0f, 0.0f, color, false, 0.f, 0.f, 1.f);
    }

    void FogManager::configure(float viewDistance, float fogDepth, float underwaterFog, float dlFactor, float dlOffset,
        const osg::Vec4f& color, bool hasFalloutFog, float falloutFogNear, float falloutFogFar, float falloutFogPower)
    {
        mHasFalloutFog = isUsableFalloutFog(hasFalloutFog, falloutFogNear, falloutFogFar, falloutFogPower);
        mFalloutFogStep = mHasFalloutFog && falloutFogFar == falloutFogNear;
        mFalloutFogPower = mHasFalloutFog ? falloutFogPower : 1.f;

        if (Settings::fog().mUseDistantFog)
        {
            mLandFogStart
                = dlFactor * (Settings::fog().mDistantLandFogStart - dlOffset * Settings::fog().mDistantLandFogEnd);
            mLandFogEnd = dlFactor * (1.0f - dlOffset) * Settings::fog().mDistantLandFogEnd;
            mUnderwaterFogStart = Settings::fog().mDistantUnderwaterFogStart;
            mUnderwaterFogEnd = Settings::fog().mDistantUnderwaterFogEnd;
        }
        else
        {
            if (fogDepth == 0.0)
            {
                mLandFogStart = 0.0f;
                mLandFogEnd = std::numeric_limits<float>::max();
            }
            else
            {
                mLandFogStart = viewDistance * (1 - fogDepth);
                mLandFogEnd = viewDistance;
            }
            mUnderwaterFogStart = std::min(viewDistance, 7168.f) * (1 - underwaterFog);
            mUnderwaterFogEnd = std::min(viewDistance, 7168.f);
        }

        if (mHasFalloutFog)
        {
            mLandFogStart = falloutFogNear;
            mLandFogEnd = falloutFogFar;

            static bool sLoggedAuthoredFalloutFogCurve = false;
            static bool sLoggedAuthoredFalloutFogStep = false;
            bool& logged = mFalloutFogStep ? sLoggedAuthoredFalloutFogStep : sLoggedAuthoredFalloutFogCurve;
            if (!logged)
            {
                const float range = falloutFogFar - falloutFogNear;
                std::ostringstream proof;
                proof << std::fixed << std::setprecision(6) << "FNV/ESM4 fog proof: mode="
                      << (mFalloutFogStep ? "authored-fnam-step" : "authored-fnam")
                      << " near=" << falloutFogNear << " far=" << falloutFogFar << " power=" << falloutFogPower
                      << " range=" << range << " denominator=" << range;
                Log(Debug::Info) << proof.str();
                logged = true;
            }
        }
        mFogColor = color;
    }

    float FogManager::getFogStart(bool isUnderwater) const
    {
        return isUnderwater ? mUnderwaterFogStart : mLandFogStart;
    }

    float FogManager::getFogEnd(bool isUnderwater) const
    {
        return isUnderwater ? mUnderwaterFogEnd : mLandFogEnd;
    }

    bool FogManager::hasFalloutFog(bool isUnderwater) const
    {
        return !isUnderwater && mHasFalloutFog;
    }

    float FogManager::getFalloutFogPower(bool isUnderwater) const
    {
        return hasFalloutFog(isUnderwater) ? mFalloutFogPower : 1.f;
    }

    bool FogManager::isFalloutFogStep(bool isUnderwater) const
    {
        return hasFalloutFog(isUnderwater) && mFalloutFogStep;
    }

    osg::Vec4f FogManager::getFogColor(bool isUnderwater) const
    {
        if (isUnderwater)
        {
            return mUnderwaterColor * mUnderwaterWeight + mFogColor * (1.f - mUnderwaterWeight);
        }

        return mFogColor;
    }
}

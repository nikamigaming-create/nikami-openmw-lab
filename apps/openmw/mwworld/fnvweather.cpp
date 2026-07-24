#include "fnvweather.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadclmt.hpp>
#include <components/esm4/loadwrld.hpp>

#include "store.hpp"

namespace
{
    constexpr float sClimateTimeScale = 1.f / 6.f;
    constexpr float sHighNoon = 12.f;
    constexpr std::uint32_t sCapturedSkyFlags = 0x20u;
    constexpr std::uint32_t sCapturedSkyMode = 3u;

    MWWorld::FalloutWeatherModelResolution failure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    bool isDeleted(std::uint32_t flags)
    {
        return (flags & ESM4::Rec_Deleted) != 0;
    }

    std::optional<std::string> validateClimate(const ESM4::Climate& climate)
    {
        if (isDeleted(climate.mFlags))
            return "linked FNV CLMT is deleted";
        if (!climate.mHasTiming)
            return "linked FNV CLMT has no TNAM timing";
        if (climate.mTiming.mVolatility != 0)
            return "volatile FNV climate selection is outside the captured static-weather slice";

        const float sunriseBegin = climate.mTiming.mSunriseBegin * sClimateTimeScale;
        const float sunriseEnd = climate.mTiming.mSunriseEnd * sClimateTimeScale;
        const float sunsetBegin = climate.mTiming.mSunsetBegin * sClimateTimeScale;
        const float sunsetEnd = climate.mTiming.mSunsetEnd * sClimateTimeScale;
        if (!(sunriseBegin >= 0.f && sunriseBegin < sunriseEnd && sunriseEnd <= sHighNoon
                && sHighNoon < sunsetBegin && sunsetBegin < sunsetEnd && sunsetEnd <= 24.f))
        {
            return "linked FNV CLMT has malformed or unsupported TNAM timing";
        }
        return std::nullopt;
    }

    std::optional<std::string> validateWeather(const ESM4::Weather& weather, std::string_view role)
    {
        const std::string prefix = std::string(role) + " FNV WTHR ";
        if (isDeleted(weather.mFlags))
            return prefix + "is deleted";
        if (!weather.mHasMaxCloudLayers || weather.mMaxCloudLayers == 0
            || weather.mMaxCloudLayers > ESM4::Weather::sCloudLayerCount)
        {
            return prefix + "has no valid LNAM cloud-layer count";
        }
        if (!weather.mHasCloudSpeeds)
            return prefix + "has no ONAM cloud speeds";
        if (!weather.mHasCloudColors || weather.mCloudColorSampleCount != ESM4::Weather::sTimeCount)
            return prefix + "does not have the required six-sample PNAM cloud colors";
        if (!weather.mHasColors || weather.mColorSampleCount != ESM4::Weather::sTimeCount)
            return prefix + "does not have the required six-sample NAM0 colors";
        if (!weather.mHasFogDistance)
            return prefix + "has no FNAM fog distances";
        if (!weather.mHasData)
            return prefix + "has no DATA weather parameters";
        if (!std::all_of(weather.mFogDistance.begin(), weather.mFogDistance.end(),
                [](float value) { return std::isfinite(value); }))
        {
            return prefix + "has a non-finite FNAM fog value";
        }
        return std::nullopt;
    }

    MWWorld::FalloutNativeWeatherSnapshot makeSnapshot(const ESM4::Weather& weather)
    {
        MWWorld::FalloutNativeWeatherSnapshot result;
        result.mWeather = weather.mId;
        result.mCloudTextures = weather.mCloudTextures;
        result.mCloudSpeeds = weather.mCloudSpeeds;
        result.mMaxCloudLayers = weather.mMaxCloudLayers;
        result.mFogDistance = weather.mFogDistance;
        result.mData = weather.mData;
        result.mLightningColor = MWWorld::convertFalloutWeatherColor(weather.mData.lightningColor);
        for (std::size_t row = 0; row < ESM4::Weather::sColorTypeCount; ++row)
        {
            for (std::size_t sample = 0; sample < ESM4::Weather::sTimeCount; ++sample)
            {
                result.mColorTable.mColors[row][sample]
                    = MWWorld::convertFalloutWeatherColor(weather.mColors[row][sample]);
            }
        }
        for (std::size_t layer = 0; layer < ESM4::Weather::sCloudLayerCount; ++layer)
        {
            for (std::size_t sample = 0; sample < ESM4::Weather::sTimeCount; ++sample)
            {
                result.mColorTable.mCloudColors[layer][sample]
                    = MWWorld::convertFalloutWeatherColor(weather.mCloudColors[layer][sample]);
            }
        }
        return result;
    }

    osg::Vec4f interpolate(const osg::Vec4f& first, const osg::Vec4f& second, float factor)
    {
        return first * (1.f - factor) + second * factor;
    }
}

namespace MWWorld
{
    osg::Vec4f convertFalloutWeatherColor(const ESM4::Weather::Color& value)
    {
        constexpr float byteToFloat = 1.f / 255.f;
        return { value.r * byteToFloat, value.g * byteToFloat, value.b * byteToFloat, 1.f };
    }

    FalloutWeatherTimeBlend getFalloutWeatherTimeBlend(float gameHour, float nightEnd, float dayStart,
        float dayEnd, float nightStart, float daytimeColorExtension)
    {
        nightEnd -= daytimeColorExtension;
        nightStart += daytimeColorExtension;

        if (gameHour <= nightEnd || gameHour >= nightStart)
            return { ESM4::Weather::Time_Night, ESM4::Weather::Time_Night, 1.f };

        if (gameHour > nightEnd && gameHour < dayStart)
        {
            const float midpoint = (nightEnd + dayStart) * 0.5f;
            const float halfDuration = (dayStart - nightEnd) * 0.5f;
            if (halfDuration <= 0.f)
                return {};
            if (gameHour < midpoint)
                return { ESM4::Weather::Time_Sunrise, ESM4::Weather::Time_Night,
                    std::clamp((gameHour - nightEnd) / halfDuration, 0.f, 1.f) };
            return { ESM4::Weather::Time_Sunrise, ESM4::Weather::Time_Day,
                std::clamp((dayStart - gameHour) / halfDuration, 0.f, 1.f) };
        }

        if (gameHour > dayStart && gameHour < sHighNoon)
        {
            const float duration = sHighNoon - dayStart;
            return { ESM4::Weather::Time_HighNoon, ESM4::Weather::Time_Day,
                duration > 0.f ? std::clamp((gameHour - dayStart) / duration, 0.f, 1.f) : 0.f };
        }

        if (gameHour > sHighNoon && gameHour < dayEnd)
        {
            const float duration = dayEnd - sHighNoon;
            return { ESM4::Weather::Time_Day, ESM4::Weather::Time_HighNoon,
                duration > 0.f ? std::clamp((gameHour - sHighNoon) / duration, 0.f, 1.f) : 1.f };
        }

        if (gameHour > dayEnd && gameHour < nightStart)
        {
            const float midpoint = (dayEnd + nightStart) * 0.5f;
            const float halfDuration = (nightStart - dayEnd) * 0.5f;
            if (halfDuration <= 0.f)
                return {};
            if (gameHour < midpoint)
                return { ESM4::Weather::Time_Sunset, ESM4::Weather::Time_Day,
                    std::clamp((gameHour - dayEnd) / halfDuration, 0.f, 1.f) };
            return { ESM4::Weather::Time_Sunset, ESM4::Weather::Time_Night,
                std::clamp((nightStart - gameHour) / halfDuration, 0.f, 1.f) };
        }

        return { ESM4::Weather::Time_Day, ESM4::Weather::Time_Day, 1.f };
    }

    FalloutWeatherModelResolution resolveFalloutWeatherModel(const Store<ESM4::World>& worlds,
        const Store<ESM4::Climate>& climates, const Store<ESM4::Weather>& weather,
        const FalloutExteriorPlayerPlacement& placement, const FalloutSaveLoadPlan::SceneState& scene)
    {
        if (placement.mWorldspaceRecord.isZeroOrUnset())
            return failure("FNV weather placement has no worldspace identity");
        const ESM4::World* world = worlds.search(ESM::RefId(placement.mWorldspaceRecord));
        if (world == nullptr || world->mId != placement.mWorldspaceRecord)
            return failure("FNV weather placement worldspace is not loaded exactly");
        if (isDeleted(world->mFlags))
            return failure("FNV weather placement worldspace is deleted");
        if ((world->mWorldFlags & ESM4::World::WLD_NoSky) != 0)
            return failure("FNV weather placement worldspace forbids sky rendering");
        if (world->mClimate.isZeroOrUnset())
            return failure("FNV weather placement worldspace has no CLMT link");

        const ESM4::Climate* climate = climates.search(ESM::RefId(world->mClimate));
        if (climate == nullptr || climate->mId != world->mClimate)
            return failure("FNV weather worldspace CLMT is not loaded exactly");
        if (const std::optional<std::string> error = validateClimate(*climate))
            return failure(*error);

        if (scene.mTransitionWeather.has_value())
            return failure("FNV transition weather is outside the captured static-weather slice");
        if (scene.mOverrideWeather.has_value())
            return failure("FNV override weather is outside the captured static-weather slice");
        if (!std::isfinite(scene.mGameHour) || !std::isfinite(scene.mLastUpdateHour)
            || !std::isfinite(scene.mWeatherPercent) || !std::isfinite(scene.mFogPower))
        {
            return failure("FNV saved sky scalars must all be finite");
        }
        if (scene.mLastUpdateHour < 0.f || scene.mLastUpdateHour >= 24.f)
            return failure("FNV saved last-update hour must be in [0, 24)");
        if (scene.mWeatherPercent != 1.f)
            return failure("FNV partial weather percentage is outside the captured static-weather slice");
        if (scene.mFlags != sCapturedSkyFlags || scene.mSkyMode != sCapturedSkyMode)
            return failure("FNV saved sky flags or mode are outside the captured Save330 slice");

        if (scene.mGameHour < 0.f || scene.mGameHour >= 24.f)
            return failure("FNV saved game hour must be in [0, 24)");
        constexpr float retailDaytimeColorExtension = 0.5f;
        const FalloutWeatherTimeBlend timeBlend = getFalloutWeatherTimeBlend(scene.mGameHour,
            climate->mTiming.mSunriseBegin * sClimateTimeScale, climate->mTiming.mSunriseEnd * sClimateTimeScale,
            climate->mTiming.mSunsetBegin * sClimateTimeScale, climate->mTiming.mSunsetEnd * sClimateTimeScale,
            retailDaytimeColorExtension);

        const auto resolveWeather = [&](ESM::FormId id) -> const ESM4::Weather* {
            if (id.isZeroOrUnset())
                return nullptr;
            const ESM4::Weather* result = weather.search(ESM::RefId(id));
            return result != nullptr && result->mId == id ? result : nullptr;
        };
        const ESM4::Weather* current = resolveWeather(scene.mCurrentWeather);
        if (current == nullptr)
            return failure("saved current FNV WTHR is not loaded exactly");
        const ESM4::Weather* defaultWeather = resolveWeather(scene.mDefaultWeather);
        if (defaultWeather == nullptr)
            return failure("saved default FNV WTHR is not loaded exactly");
        if (const std::optional<std::string> error = validateWeather(*current, "current"))
            return failure(*error);
        if (const std::optional<std::string> error = validateWeather(*defaultWeather, "default"))
            return failure(*error);

        FalloutWeatherModel model;
        model.mWorldspace = world->mId;
        model.mClimate = climate->mId;
        model.mCurrent = makeSnapshot(*current);
        model.mDefault = makeSnapshot(*defaultWeather);
        model.mCurrentWeatherInClimateList
            = std::any_of(climate->mWeatherTypes.begin(), climate->mWeatherTypes.end(), [&](const auto& entry) {
                  return entry.mWeather == current->mId;
              });
        model.mGameHour = scene.mGameHour;
        model.mTimeBlend = timeBlend;
        for (std::size_t row = 0; row < model.mCurrentColors.size(); ++row)
        {
            model.mCurrentColors[row]
                = interpolate(model.mCurrent.mColorTable.mColors[row][timeBlend.mSecondary],
                    model.mCurrent.mColorTable.mColors[row][timeBlend.mPrimary], timeBlend.mPrimaryStrength);
        }
        for (std::size_t layer = 0; layer < model.mCurrentCloudColors.size(); ++layer)
        {
            model.mCurrentCloudColors[layer]
                = interpolate(model.mCurrent.mColorTable.mCloudColors[layer][timeBlend.mSecondary],
                    model.mCurrent.mColorTable.mCloudColors[layer][timeBlend.mPrimary], timeBlend.mPrimaryStrength);
        }

        return { std::move(model), {} };
    }
}

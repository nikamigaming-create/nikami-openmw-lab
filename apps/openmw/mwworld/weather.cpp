#include "weather.hpp"

#include <cstdlib>

#include <components/debug/debuglog.hpp>
#include <components/esm/stringrefid.hpp>
#include <components/settings/values.hpp>

#include <components/misc/rng.hpp>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/weatherstate.hpp>
#include <components/esm4/loadwthr.hpp>
#include <components/esm4/imagespacecomposition.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclmt.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadimad.hpp>
#include <components/esm4/loadimgs.hpp>
#include <components/esm4/loadregn.hpp>
#include <components/esm4/loadwrld.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwsound/sound.hpp"

#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/postprocessor.hpp"
#include "../mwrender/sky.hpp"

#include "cellstore.hpp"
#include "esmstore.hpp"
#include "globalvariablename.hpp"
#include "player.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace MWWorld
{
    namespace
    {
        static const int invalidWeatherID = -1;

        bool usesFallout3Weather(ESM4Game game)
        {
            return game == ESM4Game::Fallout3 || game == ESM4Game::FalloutNewVegas;
        }

        bool isFalloutWorld(const ESM4::World& world)
        {
            std::string editorId = world.mEditorId;
            std::transform(editorId.begin(), editorId.end(), editorId.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            return editorId.find("wasteland") != std::string::npos
                || editorId.find("dcworld") != std::string::npos
                || editorId.find("commonwealth") != std::string::npos;
        }

        const ESM4::Climate* findFalloutClimate(const MWWorld::ESMStore& store)
        {
            const auto& climates = store.get<ESM4::Climate>();
            for (const ESM4::World& world : store.get<ESM4::World>())
            {
                if (!isFalloutWorld(world) || world.mClimate.isZeroOrUnset())
                    continue;
                if (const ESM4::Climate* climate = climates.search(world.mClimate))
                    return climate;
            }
            return nullptr;
        }

        const ESM4::Climate* findFalloutClimate(
            const MWWorld::ESMStore& store, const ESM4::Cell& cell)
        {
            const auto& climates = store.get<ESM4::Climate>();
            if (!cell.mClimate.isZeroOrUnset())
            {
                if (const ESM4::Climate* climate = climates.search(ESM::RefId(cell.mClimate)))
                    return climate;
            }

            ESM::RefId worldId = cell.mParent;
            for (unsigned int depth = 0; depth < 16 && !worldId.empty(); ++depth)
            {
                const ESM4::World* world = store.get<ESM4::World>().search(ESM::RefId(worldId));
                if (world == nullptr)
                    break;

                const bool inheritClimate = !world->mParent.isZeroOrUnset()
                    && (world->mParentUseFlags & ESM4::World::UseFlag_Climate) != 0;
                if (!inheritClimate && !world->mClimate.isZeroOrUnset())
                {
                    if (const ESM4::Climate* climate = climates.search(ESM::RefId(world->mClimate)))
                        return climate;
                }
                worldId = ESM::RefId(world->mParent);
            }

            return findFalloutClimate(store);
        }

        // linear interpolate between x and y based on factor.
        float lerp(float x, float y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }
        // linear interpolate between x and y based on factor.
        osg::Vec4f lerp(const osg::Vec4f& x, const osg::Vec4f& y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }

        osg::Vec3f calculateStormDirection(const std::string& particleEffect)
        {
            osg::Vec3f stormDirection = MWWorld::Weather::defaultDirection();
            if (particleEffect == Settings::models().mWeatherashcloud.get()
                || particleEffect == Settings::models().mWeatherblightcloud.get())
            {
                osg::Vec3f playerPos = MWMechanics::getPlayer().getRefData().getPosition().asVec3();
                playerPos.z() = 0;
                osg::Vec3f redMountainPos = osg::Vec3f(25000.f, 70000.f, 0.f);
                stormDirection = (playerPos - redMountainPos);
                stormDirection.normalize();
            }
            return stormDirection;
        }

        osg::Vec4f getOptionalWeatherColour(const std::string& weatherName, std::string_view colourGroup,
            std::string_view timeName, const osg::Vec4f& fallback)
        {
            const std::string key = "Weather_" + weatherName + "_" + std::string(colourGroup) + "_"
                + std::string(timeName) + "_Color";
            const auto& fallbackMap = Fallback::Map::getNonNumericFallbackMap();
            if (fallbackMap.find(key) == fallbackMap.end())
                return fallback;

            return Fallback::Map::getColour(key);
        }

        TimeOfDayInterpolator<osg::Vec4f> getOptionalWeatherColourInterpolator(const std::string& weatherName,
            std::string_view colourGroup, const TimeOfDayInterpolator<osg::Vec4f>& fallback)
        {
            return TimeOfDayInterpolator<osg::Vec4f>(
                getOptionalWeatherColour(weatherName, colourGroup, "Sunrise", fallback.getSunriseValue()),
                getOptionalWeatherColour(weatherName, colourGroup, "Day", fallback.getDayValue()),
                getOptionalWeatherColour(weatherName, colourGroup, "Sunset", fallback.getSunsetValue()),
                getOptionalWeatherColour(weatherName, colourGroup, "Night", fallback.getNightValue()));
        }

        osg::Vec4f falloutColor(const ESM4::Weather::Color& value)
        {
            constexpr float byteToFloat = 1.f / 255.f;
            return osg::Vec4f(value.r * byteToFloat, value.g * byteToFloat, value.b * byteToFloat, 1.f);
        }

        osg::Vec4f falloutCloudColor(const ESM4::Weather::Color& value)
        {
            constexpr float byteToFloat = 1.f / 255.f;
            return osg::Vec4f(
                value.r * byteToFloat, value.g * byteToFloat, value.b * byteToFloat, value.unused * byteToFloat);
        }

        FalloutWeatherColorSamples falloutColorSamples(
            const ESM4::Weather& weather, ESM4::Weather::ColorType type)
        {
            FalloutWeatherColorSamples result;
            for (std::size_t i = 0; i < result.size(); ++i)
                result[i] = falloutColor(weather.mColors[type][i]);
            return result;
        }

        struct FalloutWeatherTimeBlend
        {
            ESM4::Weather::Time mPrimary = ESM4::Weather::Time_Day;
            ESM4::Weather::Time mSecondary = ESM4::Weather::Time_Day;
            float mPrimaryStrength = 1.f;
        };

        FalloutWeatherTimeBlend getFalloutWeatherTimeBlend(
            float gameHour, const TimeOfDaySettings& timeSettings, float daytimeColorExtension)
        {
            // FalloutNV.exe 0x63F27E..0x63F510 plus the installed JIP selector patch. The executable uses strict
            // interiors for all four transition ranges; exact 08:00, 12:00 and 18:00 therefore hit its Day fallback.
            const float nightEnd = timeSettings.mNightEnd - daytimeColorExtension;
            const float dayStart = timeSettings.mDayStart;
            constexpr float highNoon = 12.f;
            const float dayEnd = timeSettings.mDayEnd;
            const float nightStart = timeSettings.mNightStart + daytimeColorExtension;

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

            if (gameHour > dayStart && gameHour < highNoon)
            {
                const float duration = highNoon - dayStart;
                return { ESM4::Weather::Time_HighNoon, ESM4::Weather::Time_Day,
                    duration > 0.f ? std::clamp((gameHour - dayStart) / duration, 0.f, 1.f) : 0.f };
            }

            if (gameHour > highNoon && gameHour < dayEnd)
            {
                const float duration = dayEnd - highNoon;
                return { ESM4::Weather::Time_Day, ESM4::Weather::Time_HighNoon,
                    duration > 0.f ? std::clamp((gameHour - highNoon) / duration, 0.f, 1.f) : 1.f };
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

        float getFalloutWeatherTimeStrength(
            const FalloutWeatherTimeBlend& blend, ESM4::Weather::Time time)
        {
            float result = 0.f;
            if (blend.mPrimary == time)
                result += blend.mPrimaryStrength;
            if (blend.mSecondary == time)
                result += 1.f - blend.mPrimaryStrength;
            return result;
        }

        float getFalloutDaytimeColorExtension(const MWWorld::ESMStore& store)
        {
            if (const ESM::GameSetting* setting
                = store.get<ESM::GameSetting>().search("fDaytimeColorExtension"))
                return setting->mValue.getFloat();
            // Retail FO3/FNV default. Keep malformed test content deterministic without changing non-Fallout paths.
            return 0.5f;
        }

        float getFalloutFogDayStrength(float gameHour, const TimeOfDaySettings& timeSettings)
        {
            if (gameHour <= timeSettings.mNightEnd || gameHour >= timeSettings.mNightStart)
                return 0.f;
            if (gameHour < timeSettings.mDayStart)
                return (gameHour - timeSettings.mNightEnd) / (timeSettings.mDayStart - timeSettings.mNightEnd);
            if (gameHour <= timeSettings.mDayEnd)
                return 1.f;
            return (timeSettings.mNightStart - gameHour) / (timeSettings.mNightStart - timeSettings.mDayEnd);
        }
    }

    osg::Vec4f sampleFalloutWeatherColor(
        const FalloutWeatherColorSamples& samples, float gameHour, const TimeOfDaySettings& timeSettings,
        float daytimeColorExtension)
    {
        const FalloutWeatherTimeBlend blend
            = getFalloutWeatherTimeBlend(gameHour, timeSettings, daytimeColorExtension);
        return lerp(samples[blend.mSecondary], samples[blend.mPrimary], blend.mPrimaryStrength);
    }

    osg::Vec3f falloutSunPosition(float orbit)
    {
        const float x = 800.f * orbit;
        return osg::Vec3f(x, -100.f, 800.f - std::abs(x));
    }

    MWRender::MoonState::Phase falloutMoonPhase(int gameDay, std::uint8_t encodedMoonInfo)
    {
        const unsigned int phaseLength = encodedMoonInfo & 0x3f;
        if (phaseLength == 0 || gameDay < 0)
            return MWRender::MoonState::Phase::Full;
        return static_cast<MWRender::MoonState::Phase>((static_cast<unsigned int>(gameDay) / phaseLength) % 8);
    }

    MWRender::MoonState falloutMoonState(
        float gameHour, MWRender::MoonState::Phase phase, bool visible)
    {
        float accumulator = std::fmod(90.f + 15.f * gameHour, 360.f);
        if (accumulator < 0.f)
            accumulator += 360.f;

        // FalloutNV.exe's two angle helpers use the authored 20..35 degree
        // horizon fade and a separate 0.5 degree early shadow/root fade.
        float phaseBlend = 0.f;
        if (accumulator >= 20.f && accumulator < 35.f)
            phaseBlend = (accumulator - 20.f) / 15.f;
        else if (accumulator >= 35.f && accumulator <= 145.f)
            phaseBlend = 1.f;
        else if (accumulator > 145.f && accumulator <= 160.f)
            phaseBlend = (160.f - accumulator) / 15.f;

        float rootAlpha = 0.f;
        if (accumulator >= 19.5f && accumulator < 20.f)
            rootAlpha = (accumulator - 19.5f) / 0.5f;
        else if (accumulator >= 20.f && accumulator <= 160.f)
            rootAlpha = 1.f;
        else if (accumulator > 160.f && accumulator <= 160.5f)
            rootAlpha = (160.5f - accumulator) / 0.5f;

        return { accumulator, 35.f, phase, phaseBlend, visible ? rootAlpha : 0.f };
    }

    template <typename T>
    T TimeOfDayInterpolator<T>::getValue(
        const float gameHour, const TimeOfDaySettings& timeSettings, const std::string& prefix) const
    {
        WeatherSetting setting = timeSettings.getSetting(prefix);
        float preSunriseTime = setting.mPreSunriseTime;
        float postSunriseTime = setting.mPostSunriseTime;
        float preSunsetTime = setting.mPreSunsetTime;
        float postSunsetTime = setting.mPostSunsetTime;

        // night
        if (gameHour < timeSettings.mNightEnd - preSunriseTime || gameHour > timeSettings.mNightStart + postSunsetTime)
            return mNightValue;
        // sunrise
        else if (gameHour >= timeSettings.mNightEnd - preSunriseTime
            && gameHour <= timeSettings.mDayStart + postSunriseTime)
        {
            float duration = timeSettings.mDayStart + postSunriseTime - timeSettings.mNightEnd + preSunriseTime;
            float middle = timeSettings.mNightEnd - preSunriseTime + duration / 2.f;

            if (gameHour <= middle)
            {
                // fade in
                float advance = middle - gameHour;
                float factor = 0.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunriseValue, mNightValue, factor);
            }
            else
            {
                // fade out
                float advance = gameHour - middle;
                float factor = 1.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunriseValue, mDayValue, factor);
            }
        }
        // day
        else if (gameHour > timeSettings.mDayStart + postSunriseTime && gameHour < timeSettings.mDayEnd - preSunsetTime)
            return mDayValue;
        // sunset
        else if (gameHour >= timeSettings.mDayEnd - preSunsetTime
            && gameHour <= timeSettings.mNightStart + postSunsetTime)
        {
            float duration = timeSettings.mNightStart + postSunsetTime - timeSettings.mDayEnd + preSunsetTime;
            float middle = timeSettings.mDayEnd - preSunsetTime + duration / 2.f;

            if (gameHour <= middle)
            {
                // fade in
                float advance = middle - gameHour;
                float factor = 0.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunsetValue, mDayValue, factor);
            }
            else
            {
                // fade out
                float advance = gameHour - middle;
                float factor = 1.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunsetValue, mNightValue, factor);
            }
        }
        // shut up compiler
        return T();
    }

    template class MWWorld::TimeOfDayInterpolator<float>;
    template class MWWorld::TimeOfDayInterpolator<osg::Vec4f>;

    osg::Vec3f Weather::defaultDirection()
    {
        static const osg::Vec3f direction = osg::Vec3f(0.f, 1.f, 0.f);
        return direction;
    }

    Weather::Weather(ESM::RefId id, int scriptId, const std::string& name, float stormWindSpeed, float rainSpeed,
        float dlFactor, float dlOffset, const std::string& particleEffect)
        : mId(id)
        , mScriptId(scriptId)
        , mName(name)
        , mCloudTexture(Fallback::Map::getString("Weather_" + name + "_Cloud_Texture"))
        , mSkyColor(Fallback::Map::getColour("Weather_" + name + "_Sky_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sky_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sky_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sky_Night_Color"))
        , mFogColor(Fallback::Map::getColour("Weather_" + name + "_Fog_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Fog_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Fog_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Fog_Night_Color"))
        , mSkyLowerColor(getOptionalWeatherColourInterpolator(name, "Sky_Lower", mSkyColor))
        , mSkyHorizonColor(getOptionalWeatherColourInterpolator(name, "Sky_Horizon", mFogColor))
        , mAmbientColor(Fallback::Map::getColour("Weather_" + name + "_Ambient_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Ambient_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Ambient_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Ambient_Night_Color"))
        , mSunColor(Fallback::Map::getColour("Weather_" + name + "_Sun_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sun_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sun_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sun_Night_Color"))
        , mLandFogDepth(Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth"),
              Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth"),
              Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth"),
              Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Night_Depth"))
        , mSunDiscSunsetColor(Fallback::Map::getColour("Weather_" + name + "_Sun_Disc_Sunset_Color"))
        , mWindSpeed(Fallback::Map::getFloat("Weather_" + name + "_Wind_Speed"))
        , mCloudSpeed(Fallback::Map::getFloat("Weather_" + name + "_Cloud_Speed"))
        , mGlareView(Fallback::Map::getFloat("Weather_" + name + "_Glare_View"))
        , mIsStorm(mWindSpeed > stormWindSpeed)
        , mRainSpeed(rainSpeed)
        , mRainEntranceSpeed(Fallback::Map::getFloat("Weather_" + name + "_Rain_Entrance_Speed"))
        , mRainMaxRaindrops(static_cast<int>(Fallback::Map::getFloat("Weather_" + name + "_Max_Raindrops")))
        , mRainDiameter(Fallback::Map::getFloat("Weather_" + name + "_Rain_Diameter"))
        , mRainThreshold(Fallback::Map::getFloat("Weather_" + name + "_Rain_Threshold"))
        , mRainMinHeight(Fallback::Map::getFloat("Weather_" + name + "_Rain_Height_Min"))
        , mRainMaxHeight(Fallback::Map::getFloat("Weather_" + name + "_Rain_Height_Max"))
        , mParticleEffect(particleEffect)
        , mRainEffect(Fallback::Map::getBool("Weather_" + name + "_Using_Precip") ? "meshes\\raindrop.nif" : "")
        , mStormDirection(Weather::defaultDirection())
        , mCloudsMaximumPercent(Fallback::Map::getFloat("Weather_" + name + "_Clouds_Maximum_Percent"))
        , mTransitionDelta(Fallback::Map::getFloat("Weather_" + name + "_Transition_Delta"))
        , mThunderFrequency(Fallback::Map::getFloat("Weather_" + name + "_Thunder_Frequency"))
        , mThunderThreshold(Fallback::Map::getFloat("Weather_" + name + "_Thunder_Threshold"))
        , mFlashDecrement(Fallback::Map::getFloat("Weather_" + name + "_Flash_Decrement"))
        , mFlashBrightness(0.0f)
    {
        mDL.FogFactor = dlFactor;
        mDL.FogOffset = dlOffset;
        mThunderSoundID[0]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_0"));
        mThunderSoundID[1]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_1"));
        mThunderSoundID[2]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_2"));
        mThunderSoundID[3]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_3"));

        if (!mRainEffect.empty()) // NOTE: in vanilla, the weathers with rain seem to be hardcoded; changing
                                  // Using_Precip has no effect
        {
            mRainLoopSoundID
                = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Rain_Loop_Sound_ID"));
            if (mRainLoopSoundID.empty()) // default to "rain" if not set
                mRainLoopSoundID = ESM::RefId::stringRefId("rain");
            else if (mRainLoopSoundID == "None")
                mRainLoopSoundID = ESM::RefId();
        }

        mAmbientLoopSoundID
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Ambient_Loop_Sound_ID"));
        if (mAmbientLoopSoundID == "None")
            mAmbientLoopSoundID = ESM::RefId();
    }

    float Weather::transitionDelta() const
    {
        // Transition Delta describes how quickly transitioning to the weather in question will take, in Hz. Note that
        // the measurement is in real time, not in-game time.
        return mTransitionDelta;
    }

    float Weather::cloudBlendFactor(const float transitionRatio) const
    {
        // Clouds Maximum Percent affects how quickly the sky transitions from one sky texture to the next.
        return transitionRatio / mCloudsMaximumPercent;
    }

    float Weather::calculateThunder(const float transitionRatio, const float elapsedSeconds, const bool isPaused)
    {
        // When paused, the flash brightness remains the same and no new strikes can occur.
        if (!isPaused)
        {
            // Morrowind doesn't appear to do any calculations unless the transition ratio is higher than the Thunder
            // Threshold.
            if (transitionRatio >= mThunderThreshold && mThunderFrequency > 0.0f)
            {
                flashDecrement(elapsedSeconds);
                auto& prng = MWBase::Environment::get().getWorld()->getPrng();
                if (Misc::Rng::rollProbability(prng) <= thunderChance(transitionRatio, elapsedSeconds))
                {
                    lightningAndThunder();
                }
            }
            else
            {
                mFlashBrightness = 0.0f;
            }
        }

        return mFlashBrightness;
    }

    inline void Weather::flashDecrement(const float elapsedSeconds)
    {
        // The Flash Decrement is measured in whole units per second. This means that if the flash brightness was
        // currently 1.0, then it should take approximately 0.25 seconds to decay to 0.0 (the minimum).
        float decrement = mFlashDecrement * elapsedSeconds;
        mFlashBrightness = decrement > mFlashBrightness ? 0.0f : mFlashBrightness - decrement;
    }

    inline float Weather::thunderChance(const float transitionRatio, const float elapsedSeconds) const
    {
        // This formula is reversed from the observation that with Thunder Frequency set to 1, there are roughly 10
        // strikes per minute. It doesn't appear to be tied to in game time as Timescale doesn't affect it. Various
        // values of Thunder Frequency seem to change the average number of strikes in a linear fashion.. During a
        // transition, it appears to scaled based on how far past it is past the Thunder Threshold.
        float scaleFactor = (transitionRatio - mThunderThreshold) / (1.0f - mThunderThreshold);
        return ((mThunderFrequency * 10.0f) / 60.0f) * elapsedSeconds * scaleFactor;
    }

    inline void Weather::lightningAndThunder(void)
    {
        // Morrowind seems to vary the intensity of the brightness based on which of the four sound IDs it selects.
        // They appear to go from 0 (brightest, closest) to 3 (faintest, farthest). The value of 0.25 per distance
        // was derived by setting the Flash Decrement to 0.1 and measuring how long each value took to decay to 0.
        // TODO: Determine the distribution of each distance to see if it's evenly weighted.
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        unsigned int distance = Misc::Rng::rollDice(4, prng);
        // Flash brightness appears additive, since if multiple strikes occur, it takes longer for it to decay to 0.
        mFlashBrightness += 1 - (distance * 0.25f);
        MWBase::Environment::get().getSoundManager()->playSound(mThunderSoundID[distance], 1.0, 1.0);
    }

    RegionWeather::RegionWeather(const ESM::Region& region)
        : mWeather(invalidWeatherID)
        , mChances(region.mData.mProbabilities.begin(), region.mData.mProbabilities.end())
    {
    }

    RegionWeather::RegionWeather(const ESM::RegionWeatherState& state)
        : mWeather(state.mWeather)
        , mChances(state.mChances)
    {
    }

    RegionWeather::operator ESM::RegionWeatherState() const
    {
        ESM::RegionWeatherState state = { mWeather, mChances };

        return state;
    }

    void RegionWeather::setChances(std::span<const uint8_t> chances)
    {
        mChances.assign(chances.begin(), chances.end());

        // Regional weather no longer supports the current type, select a new weather pattern.
        if ((static_cast<size_t>(mWeather) >= mChances.size()) || (mChances[mWeather] == 0))
        {
            chooseNewWeather();
        }
    }

    std::span<const uint8_t> RegionWeather::getChances() const
    {
        return mChances;
    }

    void RegionWeather::setWeather(int weatherID)
    {
        mWeather = weatherID;
    }

    int RegionWeather::getWeather()
    {
        // If the region weather was already set (by ChangeWeather, or by a previous call) then just return that value.
        // Note that the region weather will be expired periodically when the weather update timer expires.
        if (mWeather == invalidWeatherID)
        {
            chooseNewWeather();
        }

        return mWeather;
    }

    void RegionWeather::chooseNewWeather()
    {
        // All probabilities must add to 100 (responsibility of the user).
        // If chances A and B has values 30 and 70 then by generating 100 numbers 1..100, 30% will be lesser or equal 30
        // and 70% will be greater than 30 (in theory).
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        unsigned int chance = static_cast<unsigned int>(Misc::Rng::rollDice(100, prng) + 1); // 1..100
        unsigned int sum = 0;
        for (size_t i = 0; i < mChances.size(); ++i)
        {
            sum += mChances[i];
            if (chance <= sum)
            {
                mWeather = static_cast<int>(i);
                return;
            }
        }

        // if we hit this path then the chances don't add to 100, choose a default weather instead
        mWeather = 0;
    }

    MoonModel::MoonModel(float fadeInStart, float fadeInFinish, float fadeOutStart, float fadeOutFinish,
        float axisOffset, float speed, float dailyIncrement, float fadeStartAngle, float fadeEndAngle,
        float moonShadowEarlyFadeAngle)
    {
        mFadeInStart = fadeInStart;
        mFadeInFinish = fadeInFinish;
        mFadeOutStart = fadeOutStart;
        mFadeOutFinish = fadeOutFinish;
        mAxisOffset = axisOffset;
        mDailyIncrement = dailyIncrement;
        mFadeStartAngle = fadeStartAngle;
        mFadeEndAngle = fadeEndAngle;
        mMoonShadowEarlyFadeAngle = moonShadowEarlyFadeAngle;

        // Morrowind appears to have a minimum speed to avoid situations where the moon can't
        // complete a full rotation in a single 24-hour period. The reverse-engineered formula is
        // 180 degrees (full hemisphere) / 23 hours / 15 degrees (1 hour travel at speed 1.0).
        mSpeed = std::max(speed, (180.0f / 23.0f / 15.0f));

        // Morrowind appears to reduce mDailyIncrement with modulo 24.0f to avoid situations where
        // the moon would increment more than an entire rotation in a single day.
        mDailyIncrement = std::fmod(mDailyIncrement, 24.0f);
    }

    MoonModel::MoonModel(const std::string& name)
        : mFadeInStart(Fallback::Map::getFloat("Moons_" + name + "_Fade_In_Start"))
        , mFadeInFinish(Fallback::Map::getFloat("Moons_" + name + "_Fade_In_Finish"))
        , mFadeOutStart(Fallback::Map::getFloat("Moons_" + name + "_Fade_Out_Start"))
        , mFadeOutFinish(Fallback::Map::getFloat("Moons_" + name + "_Fade_Out_Finish"))
        , mAxisOffset(Fallback::Map::getFloat("Moons_" + name + "_Axis_Offset"))
        , mSpeed(Fallback::Map::getFloat("Moons_" + name + "_Speed"))
        , mDailyIncrement(Fallback::Map::getFloat("Moons_" + name + "_Daily_Increment"))
        , mFadeStartAngle(Fallback::Map::getFloat("Moons_" + name + "_Fade_Start_Angle"))
        , mFadeEndAngle(Fallback::Map::getFloat("Moons_" + name + "_Fade_End_Angle"))
        , mMoonShadowEarlyFadeAngle(Fallback::Map::getFloat("Moons_" + name + "_Moon_Shadow_Early_Fade_Angle"))
    {
        // Morrowind appears to have a minimum speed to avoid situations where the moon can't
        // complete a full rotation in a single 24-hour period. The reverse-engineered formula is
        // 180 degrees (full hemisphere) / 23 hours / 15 degrees (1 hour travel at speed 1.0).
        mSpeed = std::max(mSpeed, (180.0f / 23.0f / 15.0f));

        // Morrowind appears to reduce mDailyIncrement with modulo 24.0f to avoid situations where
        // the moon would increment more than an entire rotation in a single day.
        mDailyIncrement = std::fmod(mDailyIncrement, 24.0f);
    }

    MWRender::MoonState MoonModel::calculateState(const TimeStamp& gameTime) const
    {
        float rotationFromHorizon = angle(gameTime.getDay(), gameTime.getHour());
        MWRender::MoonState state = { rotationFromHorizon,
            mAxisOffset, // Reverse engineered from Morrowind's scene graph rotation matrices.
            phase(gameTime), shadowBlend(rotationFromHorizon),
            earlyMoonShadowAlpha(rotationFromHorizon) * hourlyAlpha(gameTime.getHour()) };

        return state;
    }

    inline float MoonModel::angle(int gameDay, float gameHour) const
    {
        // Morrowind's moons start travel on one side of the horizon (let's call it H-rise) and travel 180 degrees to
        // the opposite horizon (let's call it H-set). Upon reaching H-set, they reset to H-rise until the next moon
        // rise.

        // When calculating the angle of the moon, several cases have to be taken into account:
        // 1. Moon rises and then sets in one day.
        // 2. Moon sets and doesn't rise in one day (occurs when the moon rise hour is >= 24).
        // 3. Moon sets and then rises in one day.
        float moonRiseHourToday = moonRiseHour(gameDay);
        float moonRiseAngleToday = 0.0f;

        if (gameHour < moonRiseHourToday)
        {
            // Rise hour increases by mDailyIncrement each day, so yesterday's is easy to calculate
            float moonRiseHourYesterday = moonRiseHourToday - mDailyIncrement;
            if (moonRiseHourYesterday < 24.0f)
            {
                // Morrowind offsets the increment by -1 when the previous day's visible point crosses into the next
                // day. The offset lasts from this point until the next 24-day loop starts. To find this point we add
                // mDailyIncrement to the previous visible point and check the result.
                float moonShadowEarlyFadeAngle1 = mFadeEndAngle - mMoonShadowEarlyFadeAngle;
                float timeToVisible = moonShadowEarlyFadeAngle1 / rotation(1.0f);
                float cycleOffset = moonRiseHourYesterday + timeToVisible > 24.0f ? mDailyIncrement : 0.0f;

                float moonRiseAngleYesterday = rotation(24.0f - (moonRiseHourYesterday + cycleOffset));
                if (moonRiseAngleYesterday < 180.0f)
                {
                    // The moon rose but did not set yesterday, so accumulate yesterday's angle with how much we've
                    // travelled today.
                    moonRiseAngleToday = rotation(gameHour) + moonRiseAngleYesterday;
                }
            }
        }
        else
        {
            moonRiseAngleToday = rotation(gameHour - moonRiseHourToday);
        }

        if (moonRiseAngleToday >= 180.0f)
        {
            // The moon set today, reset the angle to the horizon.
            moonRiseAngleToday = 0.0f;
        }

        return moonRiseAngleToday;
    }

    inline float MoonModel::moonPhaseHour(int gameDay) const
    {
        // Morrowind delays moon phase changes until one of these is true:
        //   * The moon is invisible at midnight.
        //   * The moon reached moonShadowEarlyFadeAngle2 one daily increment ago (therefore invisible).
        if (!isVisible(gameDay, 0.0f))
            return 0.0f;
        else
        {
            // Calculate the angle at which the moon becomes transparent and the starting angle.
            float moonShadowEarlyFadeAngle2 = (180.0f - mFadeEndAngle) + mMoonShadowEarlyFadeAngle;
            float midnightAngle = angle(gameDay, 0.0f);

            // We can assume that moonShadowEarlyFadeAngle2 > midnightAngle, because the opposite
            // case would make the moon invisible at midnight, which is checked above.
            return ((moonShadowEarlyFadeAngle2 - midnightAngle) / rotation(1.0f)) + std::max(mDailyIncrement, 0.0f);
        }
    }

    inline float MoonModel::moonRiseHour(int gameDay) const
    {
        if (mDailyIncrement == 0.0f)
            return 0.0f;

        // This arises from the start date of 16 Last Seed, 427
        // TODO: Find an alternate formula that doesn't rely on this day being fixed.
        constexpr int startDay = 16;

        // This formula finds the number of missed increments necessary to make the rise hour a 24-day loop.
        // The offset increases on the first day of the loop and is multiplied by the number of completed loops.
        float incrementOffset = (24.0f - std::abs(24.0f / mDailyIncrement)) * std::floor((gameDay + startDay) / 24.0f);

        // This odd formula arises from the fact that on 16 Last Seed, 17 increments have occurred, meaning
        // that upon starting a new game, it must only calculate the moon phase as far back as 1 Last Seed.
        // Note that we don't modulo after adding the latest daily increment because other calculations need to
        // know if doing so would cause the moon rise to be postponed until the next day (which happens when
        // the moon rise hour is >= 24 in Morrowind).
        return mDailyIncrement + std::fmod((gameDay - 1 + startDay - incrementOffset) * mDailyIncrement, 24.0f);
    }

    inline float MoonModel::rotation(float hours) const
    {
        // 15 degrees per hour was reverse engineered from the rotation matrices of the Morrowind scene graph.
        // Note that this correlates to 360 / 24, which is a full rotation every 24 hours, so speed is a measure
        // of whole rotations that could be completed in a day.
        return 15.0f * mSpeed * hours;
    }

    MWRender::MoonState::Phase MoonModel::phase(const TimeStamp& gameTime) const
    {
        // Morrowind starts with a full moon on 16 Last Seed and then begins to wane 17 Last Seed, working on 3 day
        // phase cycle.

        // If the moon didn't rise yet today, use yesterday's moon phase.
        if (gameTime.getHour() < moonPhaseHour(gameTime.getDay()))
            return static_cast<MWRender::MoonState::Phase>((gameTime.getDay() / 3) % 8);
        else
            return static_cast<MWRender::MoonState::Phase>(((gameTime.getDay() + 1) / 3) % 8);
    }

    inline bool MoonModel::isVisible(int gameDay, float gameHour) const
    {
        // Moons are "visible" when their alpha value is non-zero.
        return hourlyAlpha(gameHour) > 0.f && earlyMoonShadowAlpha(angle(gameDay, gameHour)) > 0.f;
    }

    inline float MoonModel::shadowBlend(float angle) const
    {
        // The Fade End Angle and Fade Start Angle describe a region where the moon transitions from a solid disk
        // that is roughly the color of the sky, to a textured surface.
        // Depending on the current angle, the following values describe the ratio between the textured moon
        // and the solid disk:
        // 1. From Fade End Angle 1 to Fade Start Angle 1 (during moon rise): 0..1
        // 2. From Fade Start Angle 1 to Fade Start Angle 2 (between moon rise and moon set): 1 (textured)
        // 3. From Fade Start Angle 2 to Fade End Angle 2 (during moon set): 1..0
        // 4. From Fade End Angle 2 to Fade End Angle 1 (between moon set and moon rise): 0 (solid disk)
        float fadeAngle = mFadeStartAngle - mFadeEndAngle;
        float fadeEndAngle2 = 180.0f - mFadeEndAngle;
        float fadeStartAngle2 = 180.0f - mFadeStartAngle;
        if ((angle >= mFadeEndAngle) && (angle < mFadeStartAngle))
            return (angle - mFadeEndAngle) / fadeAngle;
        else if ((angle >= mFadeStartAngle) && (angle < fadeStartAngle2))
            return 1.0f;
        else if ((angle >= fadeStartAngle2) && (angle < fadeEndAngle2))
            return (fadeEndAngle2 - angle) / fadeAngle;
        else
            return 0.0f;
    }

    inline float MoonModel::hourlyAlpha(float gameHour) const
    {
        // Morrowind culls the moon one minute before mFadeOutFinish
        constexpr float oneMinute = 0.0167f;
        float adjustedFadeOutFinish = mFadeOutFinish - oneMinute;

        // The Fade Out Start / Finish and Fade In Start / Finish describe the hours at which the moon
        // appears and disappears.
        // Depending on the current hour, the following values describe how transparent the moon is.
        // 1. From Fade Out Start to Fade Out Finish: 1..0
        // 2. From Fade Out Finish to Fade In Start: 0 (transparent)
        // 3. From Fade In Start to Fade In Finish: 0..1
        // 4. From Fade In Finish to Fade Out Start: 1 (solid)
        if ((gameHour >= mFadeOutStart) && (gameHour < adjustedFadeOutFinish))
            return (adjustedFadeOutFinish - gameHour) / (adjustedFadeOutFinish - mFadeOutStart);
        else if ((gameHour >= adjustedFadeOutFinish) && (gameHour < mFadeInStart))
            return 0.0f;
        else if ((gameHour >= mFadeInStart) && (gameHour < mFadeInFinish))
            return (gameHour - mFadeInStart) / (mFadeInFinish - mFadeInStart);
        else
            return 1.0f;
    }

    inline float MoonModel::earlyMoonShadowAlpha(float angle) const
    {
        // The Moon Shadow Early Fade Angle describes an arc relative to Fade End Angle.
        // Depending on the current angle, the following values describe how transparent the moon is.
        // 1. From Moon Shadow Early Fade Angle 1 to Fade End Angle 1 (during moon rise): 0..1
        // 2. From Fade End Angle 1 to Fade End Angle 2 (between moon rise and moon set): 1 (solid)
        // 3. From Fade End Angle 2 to Moon Shadow Early Fade Angle 2 (during moon set): 1..0
        // 4. From Moon Shadow Early Fade Angle 2 to Moon Shadow Early Fade Angle 1: 0 (transparent)
        float moonShadowEarlyFadeAngle1 = mFadeEndAngle - mMoonShadowEarlyFadeAngle;
        float fadeEndAngle2 = 180.0f - mFadeEndAngle;
        float moonShadowEarlyFadeAngle2 = fadeEndAngle2 + mMoonShadowEarlyFadeAngle;
        if ((angle >= moonShadowEarlyFadeAngle1) && (angle < mFadeEndAngle))
            return (angle - moonShadowEarlyFadeAngle1) / mMoonShadowEarlyFadeAngle;
        else if ((angle >= mFadeEndAngle) && (angle < fadeEndAngle2))
            return 1.0f;
        else if ((angle >= fadeEndAngle2) && (angle < moonShadowEarlyFadeAngle2))
            return (moonShadowEarlyFadeAngle2 - angle) / mMoonShadowEarlyFadeAngle;
        else
            return 0.0f;
    }

    WeatherManager::WeatherManager(MWRender::RenderingManager& rendering, MWWorld::ESMStore& store)
        : mStore(store)
        , mRendering(rendering)
        , mSunriseTime(Fallback::Map::getFloat("Weather_Sunrise_Time"))
        , mSunsetTime(Fallback::Map::getFloat("Weather_Sunset_Time"))
        , mSunriseDuration(Fallback::Map::getFloat("Weather_Sunrise_Duration"))
        , mSunsetDuration(Fallback::Map::getFloat("Weather_Sunset_Duration"))
        , mSunPreSunsetTime(Fallback::Map::getFloat("Weather_Sun_Pre-Sunset_Time"))
        , mNightFade(0, 0, 0, 1)
        , mHoursBetweenWeatherChanges(Fallback::Map::getFloat("Weather_Hours_Between_Weather_Changes"))
        , mRainSpeed(Fallback::Map::getFloat("Weather_Precip_Gravity"))
        , mUnderwaterFog(Fallback::Map::getFloat("Water_UnderwaterSunriseFog"),
              Fallback::Map::getFloat("Water_UnderwaterDayFog"), Fallback::Map::getFloat("Water_UnderwaterSunsetFog"),
              Fallback::Map::getFloat("Water_UnderwaterNightFog"))
        , mWeatherSettings()
        , mFalloutClimate(usesFallout3Weather(store.getESM4Game()) ? findFalloutClimate(store) : nullptr)
        , mFalloutDaytimeColorExtension(
              usesFallout3Weather(store.getESM4Game()) ? getFalloutDaytimeColorExtension(store) : 0.5f)
        , mFalloutWeatherStart(0)
        , mFalloutWeatherInitialized(false)
        , mFalloutWeatherSource()
        , mMasser("Masser")
        , mSecunda("Secunda")
        , mWindSpeed(0.f)
        , mCurrentWindSpeed(0.f)
        , mNextWindSpeed(0.f)
        , mIsStorm(false)
        , mPrecipitation(false)
        , mStormDirection(Weather::defaultDirection())
        , mCurrentRegion()
        , mTimePassed(0)
        , mFastForward(false)
        , mWeatherUpdateTime(mHoursBetweenWeatherChanges)
        , mTransitionFactor(0)
        , mNightDayMode(Default)
        , mCurrentWeather(0)
        , mNextWeather(0)
        , mQueuedWeather(0)
        , mRegions()
        , mResult()
    {
        mTimeSettings.mNightStart = mSunsetTime + mSunsetDuration;
        mTimeSettings.mNightEnd = mSunriseTime;
        mTimeSettings.mDayStart = mSunriseTime + mSunriseDuration;
        mTimeSettings.mDayEnd = mSunsetTime;

        mTimeSettings.addSetting("Sky");
        mTimeSettings.addSetting("Ambient");
        mTimeSettings.addSetting("Fog");
        mTimeSettings.addSetting("Sun");

        // Morrowind handles stars settings differently for other ones
        mTimeSettings.mStarsPostSunsetStart = Fallback::Map::getFloat("Weather_Stars_Post-Sunset_Start");
        mTimeSettings.mStarsPreSunriseFinish = Fallback::Map::getFloat("Weather_Stars_Pre-Sunrise_Finish");
        mTimeSettings.mStarsFadingDuration = Fallback::Map::getFloat("Weather_Stars_Fading_Duration");

        WeatherSetting starSetting = { mTimeSettings.mStarsPreSunriseFinish,
            mTimeSettings.mStarsFadingDuration - mTimeSettings.mStarsPreSunriseFinish,
            mTimeSettings.mStarsPostSunsetStart,
            mTimeSettings.mStarsFadingDuration - mTimeSettings.mStarsPostSunsetStart };

        mTimeSettings.mSunriseTransitions["Stars"] = starSetting;

        mWeatherSettings.reserve(10);
        // These distant land fog factor and offset values are the defaults MGE XE provides. Should be
        // provided by settings somewhere?
        addWeather("Clear", 1.0f, 0.0f); // 0
        addWeather("Cloudy", 0.9f, 0.0f); // 1
        addWeather("Foggy", 0.2f, 30.0f); // 2
        addWeather("Overcast", 0.7f, 0.0f); // 3
        addWeather("Rain", 0.5f, 10.0f); // 4
        addWeather("Thunderstorm", 0.5f, 20.0f); // 5
        addWeather("Ashstorm", 0.2f, 50.0f, Settings::models().mWeatherashcloud.get()); // 6
        addWeather("Blight", 0.2f, 60.0f, Settings::models().mWeatherblightcloud.get()); // 7
        addWeather("Snow", 0.5f, 40.0f, Settings::models().mWeathersnow.get()); // 8
        addWeather("Blizzard", 0.16f, 70.0f, Settings::models().mWeatherblizzard.get()); // 9

        Store<ESM::Region>::iterator it = store.get<ESM::Region>().begin();
        for (; it != store.get<ESM::Region>().end(); ++it)
        {
            mRegions.insert(std::make_pair(it->mId, RegionWeather(*it)));
        }

        // Parsing ESM4 WTHR records is shared, but interpreting them is game-specific. In
        // particular, TES4/TES5/FO4/SF weather must never become resident Fallout state.
        if (usesFallout3Weather(store.getESM4Game()))
        {
            mFalloutWeatherStart = mWeatherSettings.size();
            importFalloutWeather();
        }

        forceWeather(0);
    }

    WeatherManager::~WeatherManager()
    {
        stopSounds();
    }

    const Weather* WeatherManager::getWeather(size_t index) const
    {
        if (index < mWeatherSettings.size())
            return &mWeatherSettings[index];

        return nullptr;
    }

    const Weather* WeatherManager::getWeather(const ESM::RefId& id) const
    {
        auto it = std::find_if(
            mWeatherSettings.begin(), mWeatherSettings.end(), [id](const auto& weather) { return weather.mId == id; });

        if (it != mWeatherSettings.end())
            return &*it;

        return nullptr;
    }

    void WeatherManager::applyFalloutClimate(const ESM4::Climate* climate)
    {
        if (climate == nullptr)
            return;
        const bool climateChanged = mFalloutClimate != climate;
        mFalloutClimate = climate;
        constexpr float climateTimeScale = 1.f / 6.f;
        mSunriseTime = climate->mTiming.mSunriseBegin * climateTimeScale;
        mSunriseDuration
            = (climate->mTiming.mSunriseEnd - climate->mTiming.mSunriseBegin) * climateTimeScale;
        mSunsetTime = climate->mTiming.mSunsetBegin * climateTimeScale;
        mSunsetDuration = (climate->mTiming.mSunsetEnd - climate->mTiming.mSunsetBegin) * climateTimeScale;
        mTimeSettings.mNightStart = mSunsetTime + mSunsetDuration;
        mTimeSettings.mNightEnd = mSunriseTime;
        mTimeSettings.mDayStart = mSunriseTime + mSunriseDuration;
        mTimeSettings.mDayEnd = mSunsetTime;
        if (climateChanged)
        {
            Log(Debug::Info) << "FNV/ESM4 climate " << climate->mEditorId << " sunrise=" << mSunriseTime << "-"
                             << (mSunriseTime + mSunriseDuration) << " sunset=" << mSunsetTime << "-"
                             << (mSunsetTime + mSunsetDuration) << " moonPhaseLength="
                             << static_cast<unsigned int>(climate->mTiming.getMoonPhaseLength())
                             << " masser=" << (climate->mTiming.hasMasser() ? 1 : 0)
                             << " secunda=" << (climate->mTiming.hasSecunda() ? 1 : 0)
                             << " sunTexture=" << climate->mSunTexture
                             << " sunGlareTexture=" << climate->mSunGlareTexture
                             << " daytimeColorExtension=" << mFalloutDaytimeColorExtension;
        }
    }

    bool WeatherManager::forceWeather(const ESM::RefId& weatherID)
    {
        applyFalloutClimate(mFalloutClimate);

        const Weather* weather = getWeather(weatherID);
        if (weather == nullptr)
            return false;
        forceWeather(weather->mScriptId);
        mFalloutWeatherInitialized = usesFallout3Weather(mStore.getESM4Game())
            && weather->mScriptId >= static_cast<int>(mFalloutWeatherStart);
        return true;
    }

    void WeatherManager::changeWeather(const ESM::RefId& regionID, const ESM::RefId& weatherID)
    {
        auto wIt = std::find_if(mWeatherSettings.begin(), mWeatherSettings.end(),
            [weatherID](const auto& weather) { return weather.mId == weatherID; });

        if (wIt != mWeatherSettings.end())
        {
            auto rIt = mRegions.find(regionID);
            if (rIt != mRegions.end())
            {
                rIt->second.setWeather(wIt->mScriptId);
                regionalWeatherChanged(rIt->first, rIt->second);
            }
        }
    }

    void WeatherManager::changeWeather(const ESM::RefId& regionID, const unsigned int weatherID)
    {
        // In Morrowind, this seems to have the following behavior, when applied to the current region:
        // - When there is no transition in progress, start transitioning to the new weather.
        // - If there is a transition in progress, queue up the transition and process it when the current one
        // completes.
        // - If there is a transition in progress, and a queued transition, overwrite the queued transition.
        // - If multiple calls to ChangeWeather are made while paused (console up), only the last call will be used,
        //   meaning that if there was no transition in progress, only the last ChangeWeather will be processed.
        // If the region isn't current, Morrowind will store the new weather for the region in question.

        if (weatherID < mWeatherSettings.size())
        {
            auto it = mRegions.find(regionID);
            if (it != mRegions.end())
            {
                it->second.setWeather(weatherID);
                regionalWeatherChanged(it->first, it->second);
            }
        }
    }

    void WeatherManager::modRegion(const ESM::RefId& regionID, std::span<const uint8_t> chances)
    {
        // Sets the region's probability for various weather patterns. Note that this appears to be saved permanently.
        // In Morrowind, this seems to have the following behavior when applied to the current region:
        // - If the region supports the current weather, no change in current weather occurs.
        // - If the region no longer supports the current weather, and there is no transition in progress, begin to
        //   transition to a new supported weather type.
        // - If the region no longer supports the current weather, and there is a transition in progress, queue a
        //   transition to a new supported weather type.

        auto it = mRegions.find(regionID);
        if (it != mRegions.end())
        {
            it->second.setChances(chances);
            regionalWeatherChanged(it->first, it->second);
        }
    }

    std::span<const uint8_t> WeatherManager::getRegionChances(const ESM::RefId& regionID) const
    {
        auto it = mRegions.find(regionID);
        if (it != mRegions.end())
            return it->second.getChances();
        return {};
    }

    void WeatherManager::playerTeleported(const ESM::RefId& playerRegion, bool isExterior)
    {
        // If the player teleports to an outdoors cell in a new region (for instance, by travelling), the weather needs
        // to be changed immediately, and any transitions for the previous region discarded.
        {
            auto it = mRegions.find(playerRegion);
            if (it != mRegions.end() && playerRegion != mCurrentRegion)
            {
                mCurrentRegion = playerRegion;
                forceWeather(it->second.getWeather());
            }
        }
    }

    float WeatherManager::calculateWindSpeed(int weatherId, float currentSpeed)
    {
        float targetSpeed = std::min(8.0f * mWeatherSettings[weatherId].mWindSpeed, 70.f);
        if (currentSpeed == 0.f)
            currentSpeed = targetSpeed;

        float multiplier = mWeatherSettings[weatherId].mRainEffect.empty() ? 1.f : 0.5f;
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        float updatedSpeed = (Misc::Rng::rollClosedProbability(prng) - 0.5f) * multiplier * targetSpeed + currentSpeed;

        if (updatedSpeed > 0.5f * targetSpeed && updatedSpeed < 2.f * targetSpeed)
            currentSpeed = updatedSpeed;

        return currentSpeed;
    }

    void WeatherManager::update(float duration, bool paused, const TimeStamp& time, bool isExterior)
    {
        MWWorld::ConstPtr player = MWMechanics::getPlayer();

        // FO3/FNV do not use OpenMW's synthetic Morrowind "Clear" slot. Exterior CELL
        // XCLR regions provide the primary authored weather list (REGN RDAT/RDWT);
        // CELL/WRLD climate WLST is the fallback. This is normal runtime state, not a
        // proof override, and follows region changes after startup.
        if (usesFallout3Weather(mStore.getESM4Game()) && isExterior && player.getCell() != nullptr
            && player.getCell()->getCell() != nullptr && player.getCell()->getCell()->isEsm4())
        {
            const ESM4::Cell& cell = player.getCell()->getCell()->getEsm4();
            const ESM4::Climate* climate = findFalloutClimate(mStore, cell);
            if (climate != nullptr && climate != mFalloutClimate)
                applyFalloutClimate(climate);

            const ESM4::Region* selectedRegion = nullptr;
            const ESM4::Region::RegionWeatherBlock* selectedRegionWeather = nullptr;
            for (const ESM::FormId& regionId : cell.mRegions)
            {
                const ESM4::Region* region = mStore.get<ESM4::Region>().search(ESM::RefId(regionId));
                if (region == nullptr)
                    continue;
                for (const ESM4::Region::RegionWeatherBlock& block : region->mWeather)
                {
                    if (block.mEntries.empty())
                        continue;
                    if (selectedRegionWeather == nullptr
                        || block.mData.priority > selectedRegionWeather->mData.priority)
                    {
                        selectedRegion = region;
                        selectedRegionWeather = &block;
                    }
                }
            }

            struct Candidate
            {
                ESM::RefId mWeather;
                unsigned int mChance = 0;
            };
            std::vector<Candidate> candidates;
            unsigned int totalChance = 0;
            MWBase::World* runtimeWorld = MWBase::Environment::get().getWorld();
            const auto effectiveChance = [&](std::int64_t authoredChance, const ESM::FormId& globalId) {
                double chance = static_cast<double>(authoredChance);
                // The index component is authoritative for null optional FormIDs.
                if (globalId.mIndex != 0)
                {
                    if (const ESM4::GlobalVariable* global
                        = mStore.get<ESM4::GlobalVariable>().search(ESM::RefId(globalId)))
                    {
                        chance = global->mValue;
                        if (runtimeWorld != nullptr && !global->mEditorId.empty())
                        {
                            const GlobalVariableName name(global->mEditorId);
                            if (runtimeWorld->getGlobalVariableType(name) != ' ')
                                chance = runtimeWorld->getGlobalFloat(name);
                        }
                    }
                }
                if (chance <= 0.0)
                    return 0u;
                return static_cast<unsigned int>(std::min(
                    chance, static_cast<double>(std::numeric_limits<unsigned int>::max())));
            };
            const auto addCandidate = [&](const ESM::FormId& weatherId, std::int64_t authoredChance,
                                          const ESM::FormId& globalId) {
                const ESM::RefId id(weatherId);
                if (getWeather(id) == nullptr)
                    return;
                const unsigned int chance = effectiveChance(authoredChance, globalId);
                if (chance == 0)
                    return;
                candidates.push_back({ id, chance });
                totalChance += chance;
            };

            ESM::RefId sourceId;
            std::string_view sourceKind;
            std::string_view sourceEditorId;
            unsigned int sourcePriority = 0;
            if (selectedRegion != nullptr && selectedRegionWeather != nullptr)
            {
                sourceId = ESM::RefId(selectedRegion->mId);
                sourceKind = "region";
                sourceEditorId = selectedRegion->mEditorId;
                sourcePriority = selectedRegionWeather->mData.priority;
                for (const ESM4::Region::RegionWeather& type : selectedRegionWeather->mEntries)
                    addCandidate(type.mWeather, type.mChance, type.mGlobal);
            }
            else if (climate != nullptr)
            {
                sourceId = ESM::RefId(climate->mId);
                sourceKind = "climate";
                sourceEditorId = climate->mEditorId;
                for (const ESM4::Climate::WeatherType& type : climate->mWeatherTypes)
                    addCandidate(type.mWeather, type.mChance, type.mGlobal);
            }

            // A malformed or conditionally empty list still falls back to its first
            // resolvable authored WTHR, never to an unrelated Morrowind preset.
            if (candidates.empty())
            {
                if (selectedRegionWeather != nullptr)
                {
                    for (const ESM4::Region::RegionWeather& type : selectedRegionWeather->mEntries)
                    {
                        const ESM::RefId weatherId(type.mWeather);
                        if (getWeather(weatherId) != nullptr)
                        {
                            candidates.push_back({ weatherId, 1u });
                            totalChance = 1u;
                            break;
                        }
                    }
                }
                else if (climate != nullptr)
                {
                    for (const ESM4::Climate::WeatherType& type : climate->mWeatherTypes)
                    {
                        const ESM::RefId weatherId(type.mWeather);
                        if (getWeather(weatherId) != nullptr)
                        {
                            candidates.push_back({ weatherId, 1u });
                            totalChance = 1u;
                            break;
                        }
                    }
                }
            }

            if (!candidates.empty() && !sourceId.empty())
            {
                // A loaded save or explicit proof force owns the initial WTHR. Record
                // its authored source without replacing it; later source changes are
                // normal region traversal and do select from the new list.
                if (mFalloutWeatherSource.empty() && mFalloutWeatherInitialized)
                    mFalloutWeatherSource = sourceId;
                else if (!mFalloutWeatherInitialized || sourceId != mFalloutWeatherSource)
                {
                    unsigned int roll = 1u;
                    if (totalChance > 1u && runtimeWorld != nullptr)
                        roll = static_cast<unsigned int>(Misc::Rng::rollDice(totalChance, runtimeWorld->getPrng()) + 1);
                    const Candidate* selected = &candidates.back();
                    unsigned int cumulative = 0;
                    for (const Candidate& candidate : candidates)
                    {
                        cumulative += candidate.mChance;
                        if (roll <= cumulative)
                        {
                            selected = &candidate;
                            break;
                        }
                    }

                    const bool selectedWeather = forceWeather(selected->mWeather);
                    if (selectedWeather)
                        mFalloutWeatherSource = sourceId;
                    Log(selectedWeather ? Debug::Info : Debug::Error)
                        << "FNV/ESM4: selected authored weather source=" << sourceKind
                        << " sourceId=" << sourceId << " editorId=" << sourceEditorId
                        << " priority=" << sourcePriority << " weather=" << selected->mWeather
                        << " chance=" << selected->mChance << " totalChance=" << totalChance
                        << " roll=" << roll << " runtimeSlot=" << mCurrentWeather
                        << " selected=" << selectedWeather;
                }
            }
        }

        if (!paused || mFastForward)
        {
            // Add new transitions when either the player's current external region changes.
            if (updateWeatherTime() || updateWeatherRegion(player.getCell()->getCell()->getRegion()))
            {
                auto it = mRegions.find(mCurrentRegion);
                if (it != mRegions.end())
                {
                    addWeatherTransition(it->second.getWeather());
                }
            }

            updateWeatherTransitions(duration);
        }

        bool isDay = time.getHour() >= mSunriseTime && time.getHour() <= mTimeSettings.mNightStart;
        if (isExterior && !isDay)
            mNightDayMode = ExteriorNight;
        else if (!isExterior && isDay && mWeatherSettings[mCurrentWeather].mGlareView >= 0.5f)
            mNightDayMode = InteriorDay;
        else
            mNightDayMode = Default;

        const auto applyFalloutImageSpace = [&]() {
            // This shader and its modifier-channel mapping are retail-derived from Fallout 3/New Vegas.
            // Other Creation Engine games retain their parsed records for their own adapters.
            if (!usesFallout3Weather(mStore.getESM4Game()))
            {
                mRendering.getPostProcessor()->clearFalloutImageSpace();
                return;
            }

            ESM::RefId imageSpaceId;
            if (const char* proofImageSpace = std::getenv("OPENMW_FNV_PROOF_IMAGE_SPACE_ID"))
            {
                const std::string_view value(proofImageSpace);
                if (value.starts_with("FormId:") || value.starts_with("formid:"))
                    imageSpaceId = ESM::RefId::deserializeText(value);
            }
            if (imageSpaceId.empty() && player.getCell() != nullptr && player.getCell()->getCell() != nullptr
                && player.getCell()->getCell()->isEsm4())
            {
                const ESM4::Cell& cell = player.getCell()->getCell()->getEsm4();
                const ESM4::World* parentWorld = cell.isExterior()
                    ? mStore.get<ESM4::World>().search(ESM::RefId(cell.mParent))
                    : nullptr;
                imageSpaceId = ESM::RefId(ESM4::resolveCellImageSpace(cell, parentWorld));
            }

            const ESM4::ImageSpace* base = mStore.get<ESM4::ImageSpace>().search(imageSpaceId);
            if (base == nullptr)
            {
                // Never retain a prior exterior grade when an interior has no valid XCIM.
                mRendering.getPostProcessor()->clearFalloutImageSpace();
                return;
            }

            std::vector<ESM4::ImageSpaceModifierContribution> modifiers;
            FalloutWeatherTimeBlend timeBlend;
            if (isExterior)
            {
                timeBlend = getFalloutWeatherTimeBlend(
                    time.getHour(), mTimeSettings, mFalloutDaytimeColorExtension);
                const auto addWeatherModifiers = [&](const Weather& falloutWeather, float weatherStrength) {
                    const auto addModifier = [&](ESM4::Weather::Time timeIndex, float timeStrength) {
                        const float strength = weatherStrength * timeStrength;
                        if (strength <= 0.f)
                            return;
                        const ESM::FormId id = falloutWeather.mFalloutImageSpaceModifiers[timeIndex];
                        if (const ESM4::ImageSpaceModifier* modifier
                            = mStore.get<ESM4::ImageSpaceModifier>().search(ESM::RefId(id)))
                            modifiers.push_back({ modifier, 0.f, strength });
                    };
                    addModifier(timeBlend.mPrimary, timeBlend.mPrimaryStrength);
                    addModifier(timeBlend.mSecondary, 1.f - timeBlend.mPrimaryStrength);
                };
                const float currentWeatherStrength = inTransition() ? mTransitionFactor : 1.f;
                addWeatherModifiers(mWeatherSettings[mCurrentWeather], currentWeatherStrength);
                if (inTransition())
                    addWeatherModifiers(mWeatherSettings[mNextWeather], 1.f - mTransitionFactor);
            }

            const ESM4::ComposedImageSpace composed = ESM4::composeImageSpace(*base, modifiers);
            const float sunlightDimmer = composed.mTraits[ESM4::ImageSpace::Trait_SunlightDimmer];
            if (isExterior)
            {
                mResult.mFalloutCloudRgbMultiplier
                    = composed.mTraits[ESM4::ImageSpace::Trait_LuminanceRampNoTexture];
                for (int component = 0; component < 3; ++component)
                    mResult.mSunColor[component] *= sunlightDimmer;
            }

            mRendering.getPostProcessor()->setFalloutImageSpace(
                osg::Vec4f(composed.mTraits[ESM4::ImageSpace::Trait_TargetLuminance],
                    composed.mTraits[ESM4::ImageSpace::Trait_BrightScale],
                    composed.mTraits[ESM4::ImageSpace::Trait_BrightClamp],
                    isExterior ? composed.mTraits[ESM4::ImageSpace::Trait_BloomAlphaExterior]
                               : composed.mTraits[ESM4::ImageSpace::Trait_BloomAlphaInterior]),
                osg::Vec4f(composed.mTraits[ESM4::ImageSpace::Trait_CinematicSaturation],
                    composed.mTraits[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance],
                    composed.mTraits[ESM4::ImageSpace::Trait_CinematicContrast],
                    composed.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness]),
                osg::Vec4f(composed.mTint[0], composed.mTint[1], composed.mTint[2], composed.mTint[3]),
                osg::Vec4f(composed.mFade[0], composed.mFade[1], composed.mFade[2], composed.mFade[3]));

            if (std::getenv("OPENMW_FNV_PROOF_IMAGE_SPACE_ID") != nullptr)
            {
                static int imageSpaceLogs = 0;
                if (imageSpaceLogs++ < 12)
                {
                    Log(Debug::Info) << "FNV/ESM4 proof: composed image space base=" << base->mId
                                     << " exterior=" << (isExterior ? 1 : 0)
                                     << " weather=" << mWeatherSettings[mCurrentWeather].mId
                                     << " modifiers=" << modifiers.size() << " timeSlots=("
                                     << static_cast<std::size_t>(timeBlend.mPrimary) << ":"
                                     << timeBlend.mPrimaryStrength << ","
                                     << static_cast<std::size_t>(timeBlend.mSecondary) << ":"
                                     << (1.f - timeBlend.mPrimaryStrength) << ")"
                                     << " skinDimmer="
                                     << composed.mTraits[ESM4::ImageSpace::Trait_SkinDimmer]
                                     << " cloudRgbMultiplier="
                                     << composed.mTraits[ESM4::ImageSpace::Trait_LuminanceRampNoTexture]
                                     << " sunlightDimmer=" << sunlightDimmer << " cinematic=("
                                     << composed.mTraits[ESM4::ImageSpace::Trait_CinematicSaturation] << ","
                                     << composed.mTraits[
                                            ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance]
                                     << "," << composed.mTraits[ESM4::ImageSpace::Trait_CinematicContrast] << ","
                                     << composed.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness] << ") tint=("
                                     << composed.mTint[0] << "," << composed.mTint[1] << "," << composed.mTint[2]
                                     << "," << composed.mTint[3] << ")";
                }
            }
        };

        if (!isExterior)
        {
            // Interior CELL XCIM is independent of exterior weather, so apply it before the weather-only return.
            applyFalloutImageSpace();
            mRendering.setSkyEnabled(false);
            stopSounds();
            mWindSpeed = 0.f;
            mCurrentWindSpeed = 0.f;
            mNextWindSpeed = 0.f;
            return;
        }

        calculateWeatherResult(time.getHour(), duration, paused);
        applyFalloutImageSpace();
        if (std::getenv("OPENMW_FNV_PROOF_WEATHER_ID") != nullptr)
        {
            static int proofWeatherLogs = 0;
            if (proofWeatherLogs < 12)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: weather render state hour=" << time.getHour()
                                 << " currentWeather=" << mCurrentWeather
                                 << " nextWeather=" << mNextWeather
                                 << " isExterior=" << isExterior
                                 << " isDay=" << isDay
                                 << " ambient=(" << mResult.mAmbientColor.r() << "," << mResult.mAmbientColor.g()
                                 << "," << mResult.mAmbientColor.b() << "," << mResult.mAmbientColor.a()
                                 << ") sun=(" << mResult.mSunColor.r() << "," << mResult.mSunColor.g() << ","
                                 << mResult.mSunColor.b() << "," << mResult.mSunColor.a()
                                 << ") sky=(" << mResult.mSkyColor.r() << "," << mResult.mSkyColor.g() << ","
                                 << mResult.mSkyColor.b() << "," << mResult.mSkyColor.a()
                                 << ") skyLower=(" << mResult.mSkyLowerColor.r() << ","
                                 << mResult.mSkyLowerColor.g() << "," << mResult.mSkyLowerColor.b() << ","
                                 << mResult.mSkyLowerColor.a()
                                 << ") skyHorizon=(" << mResult.mSkyHorizonColor.r() << ","
                                 << mResult.mSkyHorizonColor.g() << "," << mResult.mSkyHorizonColor.b() << ","
                                 << mResult.mSkyHorizonColor.a()
                                 << ") fog=(" << mResult.mFogColor.r() << "," << mResult.mFogColor.g() << ","
                                 << mResult.mFogColor.b() << "," << mResult.mFogColor.a() << ")";
                ++proofWeatherLogs;
            }
        }

        if (!paused)
        {
            mWindSpeed = mResult.mWindSpeed;
            mCurrentWindSpeed = mResult.mCurrentWindSpeed;
            mNextWindSpeed = mResult.mNextWindSpeed;
        }

        mIsStorm = mResult.mIsStorm;

        // For some reason Ash Storm is not considered as a precipitation weather in game
        mPrecipitation = !(mResult.mParticleEffect.empty() && mResult.mRainEffect.empty())
            && mResult.mParticleEffect != Settings::models().mWeatherashcloud.get();

        mStormDirection = calculateStormDirection(mResult.mParticleEffect);
        mRendering.getSkyManager()->setStormParticleDirection(mStormDirection);

        // disable sun during night
        if (time.getHour() >= mTimeSettings.mNightStart || time.getHour() <= mSunriseTime)
            mRendering.getSkyManager()->sunDisable();
        else
            mRendering.getSkyManager()->sunEnable();

        // Update the sun direction.  Run it east to west at a fixed angle from overhead.
        // The sun's speed at day and night may differ, since mSunriseTime and mNightStart
        // mark when the sun is level with the horizon.
        {
            // Shift times into a 24-hour window beginning at mSunriseTime...
            float adjustedHour = time.getHour();
            float adjustedNightStart = mTimeSettings.mNightStart;
            if (time.getHour() < mSunriseTime)
                adjustedHour += 24.f;
            if (mTimeSettings.mNightStart < mSunriseTime)
                adjustedNightStart += 24.f;

            const bool isNight = adjustedHour >= adjustedNightStart;
            const float dayDuration = adjustedNightStart - mSunriseTime;
            const float nightDuration = 24.f - dayDuration;

            float orbit;
            if (!isNight)
            {
                float t = (adjustedHour - mSunriseTime) / dayDuration;
                orbit = 1.f - 2.f * t;
            }
            else
            {
                float t = (adjustedHour - adjustedNightStart) / nightDuration;
                orbit = 2.f * t - 1.f;
            }

            if (mFalloutClimate != nullptr)
            {
                // Live FalloutNV.exe Sky::sun telemetry: X spans +800..-800 from
                // sunrise to sunset (and back overnight), Y is -100, and
                // Z = 800 - abs(X).
                const osg::Vec3f sunPosition = falloutSunPosition(orbit);
                if (std::getenv("OPENMW_FNV_PROOF_WEATHER_ID") != nullptr)
                {
                    static int proofFalloutSunOrbitLogs = 0;
                    if (proofFalloutSunOrbitLogs < 12)
                    {
                        Log(Debug::Info) << "FNV/ESM4 proof: retail sun orbit hour=" << time.getHour()
                                         << " sunrise=" << mSunriseTime
                                         << " nightStart=" << mTimeSettings.mNightStart
                                         << " orbit=" << orbit
                                         << " isNight=" << (isNight ? 1 : 0)
                                         << " position=(" << sunPosition.x() << "," << sunPosition.y() << ","
                                         << sunPosition.z() << ")";
                        ++proofFalloutSunOrbitLogs;
                    }
                }
                // FalloutNV.exe does not use the visible Sun root translation as its
                // directional-light vector. D3D9 shader captures across dawn, noon, and
                // afternoon show LightData = normalize(800 * orbit, 100, 100), while the
                // visible sky sun retains the authored piecewise path above.
                const osg::Vec3f sunlightPosition(sunPosition.x(), -sunPosition.y(), -sunPosition.y());
                mRendering.setSunPosition(sunPosition, sunlightPosition);
            }
            else
            {
                // Hardcoded constants from Morrowind.
                const osg::Vec3f sunDir(-400.f * orbit, 75.f, -100.f);
                mRendering.setSunDirection(sunDir);
            }
            mRendering.setNight(isNight);
        }

        float underwaterFog = mUnderwaterFog.getValue(time.getHour(), mTimeSettings, "Fog");

        float peakHour = mSunriseTime + (mTimeSettings.mNightStart - mSunriseTime) / 2;
        float glareFade = 1.f;
        if (time.getHour() < mSunriseTime || time.getHour() > mTimeSettings.mNightStart)
            glareFade = 0.f;
        else if (time.getHour() < peakHour)
            glareFade = 1.f - (peakHour - time.getHour()) / (peakHour - mSunriseTime);
        else
            glareFade = 1.f - (time.getHour() - peakHour) / (mTimeSettings.mNightStart - peakHour);

        mRendering.getSkyManager()->setGlareTimeOfDayFade(glareFade);

        if (mFalloutClimate != nullptr)
        {
            // The live retail object is size 85 with one Masser phase set; secundaMoon is null.
            // Its exact angular visibility window is resolved by falloutMoonState rather than
            // the coarser climate sunrise/sunset interval.
            const bool visible = mFalloutClimate->mTiming.hasMasser();
            const MWRender::MoonState::Phase phase
                = falloutMoonPhase(time.getDay(), mFalloutClimate->mTiming.mMoonInfo);
            MWRender::MoonState masser = falloutMoonState(time.getHour(), phase, visible);
            mRendering.getSkyManager()->setMasserState(masser);

            MWRender::MoonState secunda = masser;
            secunda.mMoonAlpha = mFalloutClimate->mTiming.hasSecunda() ? masser.mMoonAlpha : 0.f;
            mRendering.getSkyManager()->setSecundaState(secunda);
            if (std::getenv("OPENMW_FNV_PROOF_WEATHER_ID") != nullptr)
            {
                static int proofMoonLogs = 0;
                if (proofMoonLogs < 12)
                {
                    Log(Debug::Info) << "FNV/ESM4 proof: retail moon state hour=" << time.getHour()
                                     << " rotationFromHorizon=" << masser.mRotationFromHorizon
                                     << " rotationFromNorth=" << masser.mRotationFromNorth
                                     << " alpha=" << masser.mMoonAlpha
                                     << " phase=" << static_cast<unsigned int>(masser.mPhase)
                                     << " phaseLength="
                                     << static_cast<unsigned int>(mFalloutClimate->mTiming.getMoonPhaseLength())
                                     << " secundaAvailable=" << (mFalloutClimate->mTiming.hasSecunda() ? 1 : 0)
                                     << " size=85 speed=0.25";
                    ++proofMoonLogs;
                }
            }
        }
        else
        {
            mRendering.getSkyManager()->setMasserState(mMasser.calculateState(time));
            mRendering.getSkyManager()->setSecundaState(mSecunda.calculateState(time));
        }

        if (mResult.mHasFalloutFogRange)
            mRendering.configureFog(mResult.mFalloutFogNear, mResult.mFalloutFogFar, underwaterFog, mResult.mFogColor);
        else
            mRendering.configureFog(mResult.mFogDepth, underwaterFog, mResult.mDLFogFactor,
                mResult.mDLFogOffset / 100.0f, mResult.mFogColor);
        mRendering.setAmbientColour(mResult.mAmbientColor);
        mRendering.setSunColour(mResult.mSunColor, mResult.mSunColor, mResult.mGlareView * glareFade);

        mRendering.getSkyManager()->setWeather(mResult);

        // Play sounds
        if (mPlayingAmbientSoundID != mResult.mAmbientLoopSoundID)
        {
            if (mAmbientSound)
            {
                MWBase::Environment::get().getSoundManager()->stopSound(mAmbientSound);
                mAmbientSound = nullptr;
            }
            if (!mResult.mAmbientLoopSoundID.empty())
                mAmbientSound = MWBase::Environment::get().getSoundManager()->playSound(mResult.mAmbientLoopSoundID,
                    mResult.mAmbientSoundVolume, 1.0, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            mPlayingAmbientSoundID = mResult.mAmbientLoopSoundID;
        }
        else if (mAmbientSound)
            mAmbientSound->setVolume(mResult.mAmbientSoundVolume);

        if (mPlayingRainSoundID != mResult.mRainLoopSoundID)
        {
            if (mRainSound)
            {
                MWBase::Environment::get().getSoundManager()->stopSound(mRainSound);
                mRainSound = nullptr;
            }
            if (!mResult.mRainLoopSoundID.empty())
                mRainSound = MWBase::Environment::get().getSoundManager()->playSound(mResult.mRainLoopSoundID,
                    mResult.mAmbientSoundVolume, 1.0, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            mPlayingRainSoundID = mResult.mRainLoopSoundID;
        }
        else if (mRainSound)
            mRainSound->setVolume(mResult.mAmbientSoundVolume);
    }

    void WeatherManager::stopSounds()
    {
        MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
        if (mAmbientSound)
        {
            sndMgr->stopSound(mAmbientSound);
            mAmbientSound = nullptr;
        }
        mPlayingAmbientSoundID = ESM::RefId();

        if (mRainSound)
        {
            sndMgr->stopSound(mRainSound);
            mRainSound = nullptr;
        }
        mPlayingRainSoundID = ESM::RefId();

        for (ESM::RefId soundId : mWeatherSettings[mCurrentWeather].mThunderSoundID)
            if (!soundId.empty() && sndMgr->getSoundPlaying(MWWorld::ConstPtr(), soundId))
                sndMgr->stopSound3D(MWWorld::ConstPtr(), soundId);

        if (inTransition())
            for (ESM::RefId soundId : mWeatherSettings[mNextWeather].mThunderSoundID)
                if (!soundId.empty() && sndMgr->getSoundPlaying(MWWorld::ConstPtr(), soundId))
                    sndMgr->stopSound3D(MWWorld::ConstPtr(), soundId);
    }

    float WeatherManager::getWindSpeed() const
    {
        return mWindSpeed;
    }

    bool WeatherManager::isInStorm() const
    {
        return mIsStorm;
    }

    osg::Vec3f WeatherManager::getStormDirection() const
    {
        return mStormDirection;
    }

    void WeatherManager::advanceTime(double hours, bool incremental)
    {
        // In Morrowind, when the player sleeps/waits, serves jail time, travels, or trains, all weather transitions are
        // immediately applied, regardless of whatever transition time might have been remaining.
        mTimePassed += static_cast<float>(hours);
        mFastForward = !incremental ? true : mFastForward;
    }

    NightDayMode WeatherManager::getNightDayMode() const
    {
        return mNightDayMode;
    }

    bool WeatherManager::useTorches(float hour) const
    {
        bool isDark = hour < mSunriseTime || hour > mTimeSettings.mNightStart;

        return isDark && !mPrecipitation;
    }

    float WeatherManager::getSunPercentage(float hour) const
    {
        if (hour <= mTimeSettings.mNightEnd || hour >= mTimeSettings.mNightStart)
            return 0.f;
        else if (hour <= mTimeSettings.mDayStart)
            return (hour - mTimeSettings.mNightEnd) / mSunriseDuration;
        else if (hour > mTimeSettings.mDayEnd)
            return 1.f - ((hour - mTimeSettings.mDayEnd) / mSunsetDuration);
        return 1.f;
    }

    float WeatherManager::getSunVisibility() const
    {
        if (inTransition() && mTransitionFactor < mWeatherSettings[mNextWeather].mCloudsMaximumPercent)
        {
            float t = mTransitionFactor / mWeatherSettings[mNextWeather].mCloudsMaximumPercent;
            return (1.f - t) * mWeatherSettings[mCurrentWeather].mGlareView
                + t * mWeatherSettings[mNextWeather].mGlareView;
        }
        return mWeatherSettings[mCurrentWeather].mGlareView;
    }

    void WeatherManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        ESM::WeatherState state;
        state.mCurrentRegion = mCurrentRegion;
        state.mTimePassed = mTimePassed;
        state.mFastForward = mFastForward;
        state.mWeatherUpdateTime = mWeatherUpdateTime;
        state.mTransitionFactor = mTransitionFactor;
        state.mCurrentWeather = mCurrentWeather;
        state.mNextWeather = mNextWeather;
        state.mQueuedWeather = mQueuedWeather;

        auto it = mRegions.begin();
        for (; it != mRegions.end(); ++it)
        {
            state.mRegions.insert(std::make_pair(it->first, it->second));
        }

        writer.startRecord(ESM::REC_WTHR);
        state.save(writer);
        writer.endRecord(ESM::REC_WTHR);
    }

    bool WeatherManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (ESM::REC_WTHR == type)
        {
            ESM::WeatherState state;
            state.load(reader);

            std::swap(mCurrentRegion, state.mCurrentRegion);
            mTimePassed = state.mTimePassed;
            mFastForward = state.mFastForward;
            mWeatherUpdateTime = state.mWeatherUpdateTime;
            mTransitionFactor = state.mTransitionFactor;
            mCurrentWeather = state.mCurrentWeather;
            mNextWeather = state.mNextWeather;
            mQueuedWeather = state.mQueuedWeather;
            mFalloutWeatherInitialized = usesFallout3Weather(mStore.getESM4Game())
                && mCurrentWeather >= static_cast<int>(mFalloutWeatherStart)
                && mCurrentWeather < static_cast<int>(mWeatherSettings.size());
            mFalloutWeatherSource = ESM::RefId();

            mRegions.clear();
            importRegions();

            for (auto it = state.mRegions.begin(); it != state.mRegions.end(); ++it)
            {
                auto found = mRegions.find(it->first);
                if (found != mRegions.end())
                {
                    found->second = RegionWeather(it->second);
                }
            }

            return true;
        }

        return false;
    }

    void WeatherManager::clear()
    {
        stopSounds();

        mCurrentRegion = ESM::RefId();
        mTimePassed = 0.0f;
        mWeatherUpdateTime = 0.0f;
        mFalloutWeatherSource = ESM::RefId();
        forceWeather(0);
        mRegions.clear();
        importRegions();
    }

    inline void WeatherManager::addWeather(
        const std::string& name, float dlFactor, float dlOffset, const std::string& particleEffect)
    {
        static const float fStromWindSpeed = mStore.get<ESM::GameSetting>().find("fStromWindSpeed")->mValue.getFloat();
        ESM::StringRefId id(name);
        Weather weather(id, static_cast<int>(mWeatherSettings.size()), name, fStromWindSpeed, mRainSpeed, dlFactor,
            dlOffset, particleEffect);

        mWeatherSettings.push_back(std::move(weather));
    }

    void WeatherManager::importFalloutWeather()
    {
        std::size_t imported = 0;
        for (const ESM4::Weather& source : mStore.get<ESM4::Weather>())
        {
            const int scriptId = static_cast<int>(mWeatherSettings.size());
            // Construct from the known-complete Clear defaults, then replace
            // every FO3/FNV field currently consumed by the flat renderer.
            Weather weather(source.mId, scriptId, "Clear", 1000.f, mRainSpeed, 1.f, 0.f, "");
            weather.mName = source.mEditorId;
            for (auto it = source.mCloudTextures.rbegin(); it != source.mCloudTextures.rend(); ++it)
            {
                if (!it->empty())
                {
                    weather.mCloudTexture = *it;
                    break;
                }
            }
            const FalloutWeatherColorSamples sky
                = falloutColorSamples(source, ESM4::Weather::Color_SkyUpper);
            const FalloutWeatherColorSamples fog = falloutColorSamples(source, ESM4::Weather::Color_Fog);
            const FalloutWeatherColorSamples ambient
                = falloutColorSamples(source, ESM4::Weather::Color_Ambient);
            const FalloutWeatherColorSamples sunlight
                = falloutColorSamples(source, ESM4::Weather::Color_Sunlight);
            const FalloutWeatherColorSamples sunDisc
                = falloutColorSamples(source, ESM4::Weather::Color_Sun);
            const FalloutWeatherColorSamples stars
                = falloutColorSamples(source, ESM4::Weather::Color_Stars);
            const FalloutWeatherColorSamples skyLower
                = falloutColorSamples(source, ESM4::Weather::Color_SkyLower);
            const FalloutWeatherColorSamples horizon
                = falloutColorSamples(source, ESM4::Weather::Color_Horizon);
            weather.mSkyColor = TimeOfDayInterpolator<osg::Vec4f>(sky[0], sky[1], sky[2], sky[3]);
            weather.mFogColor = TimeOfDayInterpolator<osg::Vec4f>(fog[0], fog[1], fog[2], fog[3]);
            weather.mAmbientColor = TimeOfDayInterpolator<osg::Vec4f>(ambient[0], ambient[1], ambient[2], ambient[3]);
            weather.mSunColor
                = TimeOfDayInterpolator<osg::Vec4f>(sunlight[0], sunlight[1], sunlight[2], sunlight[3]);
            weather.mSkyLowerColor
                = TimeOfDayInterpolator<osg::Vec4f>(skyLower[0], skyLower[1], skyLower[2], skyLower[3]);
            weather.mSkyHorizonColor
                = TimeOfDayInterpolator<osg::Vec4f>(horizon[0], horizon[1], horizon[2], horizon[3]);
            const bool hasHighNoon = source.mImageSpaceModifiers[ESM4::Weather::Time_HighNoon].isSet()
                || ambient[ESM4::Weather::Time_HighNoon] != osg::Vec4f(0.f, 0.f, 0.f, 1.f);
            if (hasHighNoon)
            {
                weather.mFalloutSkyColors = sky;
                weather.mFalloutFogColors = fog;
                weather.mFalloutAmbientColors = ambient;
                weather.mFalloutSunlightColors = sunlight;
                weather.mFalloutSunDiscColors = sunDisc;
                weather.mFalloutStarColors = stars;
                weather.mFalloutSkyLowerColors = skyLower;
                weather.mFalloutHorizonColors = horizon;
            }
            weather.mWindSpeed = static_cast<float>(source.mData.windSpeed) / 255.f;
            weather.mCloudSpeed = static_cast<float>(source.mData.lowerCloudSpeed) / 255.f;
            weather.mGlareView = static_cast<float>(source.mData.sunGlare) / 255.f;
            weather.mFalloutImageSpaceModifiers = source.mImageSpaceModifiers;
            if (source.mHasFogDistance)
                weather.mFalloutFogDistance = source.mFogDistance;
            weather.mFalloutCloudTextures = source.mCloudTextures;
            std::array<float, 4> cloudSpeeds{};
            std::array<FalloutWeatherColorSamples, 4> cloudColors{};
            for (std::size_t layer = 0; layer < cloudSpeeds.size(); ++layer)
            {
                // FO3/FNV ONAM values use the legacy unsigned scale. TES5+
                // RNAM values are centred on 127 and cover roughly -0.1..0.1.
                cloudSpeeds[layer] = source.mUsesExtendedCloudSpeeds
                    ? (static_cast<float>(source.mCloudSpeeds[layer]) - 127.f) / 1270.f
                    : static_cast<float>(source.mCloudSpeeds[layer]) / 2550.f;
                for (std::size_t sample = 0; sample < cloudColors[layer].size(); ++sample)
                {
                    cloudColors[layer][sample] = falloutCloudColor(source.mCloudColors[layer][sample]);
                    if (source.mHasCloudAlphas)
                        cloudColors[layer][sample].a() = std::clamp(source.mCloudAlphas[layer][sample], 0.f, 1.f);
                }
            }
            weather.mFalloutCloudSpeeds = cloudSpeeds;
            weather.mFalloutCloudColors = cloudColors;
            mWeatherSettings.push_back(std::move(weather));
            ++imported;
        }
        if (imported != 0)
            Log(Debug::Info) << "FNV/ESM4: imported weather records count=" << imported;
    }

    inline void WeatherManager::importRegions()
    {
        for (const ESM::Region& region : mStore.get<ESM::Region>())
        {
            mRegions.insert(std::make_pair(region.mId, RegionWeather(region)));
        }
    }

    inline void WeatherManager::regionalWeatherChanged(const ESM::RefId& regionID, RegionWeather& region)
    {
        // If the region is current, then add a weather transition for it.
        MWWorld::ConstPtr player = MWMechanics::getPlayer();
        if (player.isInCell())
        {
            if (regionID == mCurrentRegion)
            {
                addWeatherTransition(region.getWeather());
            }
        }
    }

    inline bool WeatherManager::updateWeatherTime()
    {
        mWeatherUpdateTime -= mTimePassed;
        mTimePassed = 0.0f;
        if (mWeatherUpdateTime <= 0.0f)
        {
            // Expire all regional weather, so that any call to getWeather() will return a new weather ID.
            auto it = mRegions.begin();
            for (; it != mRegions.end(); ++it)
            {
                it->second.setWeather(invalidWeatherID);
            }

            mWeatherUpdateTime += mHoursBetweenWeatherChanges;

            return true;
        }

        return false;
    }

    inline bool WeatherManager::updateWeatherRegion(const ESM::RefId& playerRegion)
    {
        if (!playerRegion.empty() && playerRegion != mCurrentRegion)
        {
            mCurrentRegion = playerRegion;

            return true;
        }

        return false;
    }

    inline void WeatherManager::updateWeatherTransitions(const float elapsedRealSeconds)
    {
        // When a player chooses to train, wait, or serves jail time, any transitions will be fast forwarded to the last
        // weather type set, regardless of the remaining transition time.
        if (!mFastForward && inTransition())
        {
            const float delta = mWeatherSettings[mNextWeather].transitionDelta();
            mTransitionFactor -= elapsedRealSeconds * delta;
            if (mTransitionFactor <= 0.0f)
            {
                mCurrentWeather = mNextWeather;
                mNextWeather = mQueuedWeather;
                mQueuedWeather = invalidWeatherID;

                // We may have begun processing the queued transition, so we need to apply the remaining time towards
                // it.
                if (inTransition())
                {
                    const float newDelta = mWeatherSettings[mNextWeather].transitionDelta();
                    const float remainingSeconds = -(mTransitionFactor / delta);
                    mTransitionFactor = 1.0f - (remainingSeconds * newDelta);
                }
                else
                {
                    mTransitionFactor = 0.0f;
                }
            }
        }
        else
        {
            if (mQueuedWeather != invalidWeatherID)
            {
                mCurrentWeather = mQueuedWeather;
            }
            else if (mNextWeather != invalidWeatherID)
            {
                mCurrentWeather = mNextWeather;
            }

            mNextWeather = invalidWeatherID;
            mQueuedWeather = invalidWeatherID;
            mFastForward = false;
        }
    }

    inline void WeatherManager::forceWeather(const int weatherID)
    {
        mTransitionFactor = 0.0f;
        mCurrentWeather = weatherID;
        mNextWeather = invalidWeatherID;
        mQueuedWeather = invalidWeatherID;
        if (usesFallout3Weather(mStore.getESM4Game()))
            mFalloutWeatherInitialized = weatherID >= static_cast<int>(mFalloutWeatherStart)
                && weatherID < static_cast<int>(mWeatherSettings.size());
    }

    inline bool WeatherManager::inTransition() const
    {
        return mNextWeather != invalidWeatherID;
    }

    inline void WeatherManager::addWeatherTransition(const int weatherID)
    {
        // In order to work like ChangeWeather expects, this method begins transitioning to the new weather immediately
        // if no transition is in progress, otherwise it queues it to be transitioned.

        assert(weatherID >= 0 && static_cast<size_t>(weatherID) < mWeatherSettings.size());

        if (!inTransition() && (weatherID != mCurrentWeather))
        {
            mNextWeather = weatherID;
            mTransitionFactor = 1.0f;
        }
        else if (inTransition() && (weatherID != mNextWeather))
        {
            mQueuedWeather = weatherID;
        }
    }

    inline void WeatherManager::calculateWeatherResult(
        const float gameHour, const float elapsedSeconds, const bool isPaused)
    {
        float flash = 0.0f;
        if (!inTransition())
        {
            calculateResult(mCurrentWeather, gameHour);
            flash = mWeatherSettings[mCurrentWeather].calculateThunder(1.0f, elapsedSeconds, isPaused);
        }
        else
        {
            calculateTransitionResult(1 - mTransitionFactor, gameHour);
            float currentFlash
                = mWeatherSettings[mCurrentWeather].calculateThunder(mTransitionFactor, elapsedSeconds, isPaused);
            float nextFlash
                = mWeatherSettings[mNextWeather].calculateThunder(1 - mTransitionFactor, elapsedSeconds, isPaused);
            flash = currentFlash + nextFlash;
        }
        osg::Vec4f flashColor(flash, flash, flash, 0.0f);

        mResult.mFogColor += flashColor;
        mResult.mAmbientColor += flashColor;
        mResult.mSunColor += flashColor;
    }

    inline void WeatherManager::calculateResult(const int weatherID, const float gameHour)
    {
        const Weather& current = mWeatherSettings[weatherID];

        mResult.mCloudTexture = current.mCloudTexture;
        mResult.mCloudBlendFactor = 0;
        mResult.mNextWindSpeed = 0;
        mResult.mWindSpeed = mResult.mCurrentWindSpeed = calculateWindSpeed(weatherID, mWindSpeed);
        mResult.mBaseWindSpeed = mWeatherSettings[weatherID].mWindSpeed;

        mResult.mCloudSpeed = current.mCloudSpeed;
        mResult.mFalloutCloudRgbMultiplier = 1.f;
        mResult.mHasFalloutCloudLayers = current.mFalloutCloudTextures && current.mFalloutCloudSpeeds
            && current.mFalloutCloudColors;
        if (mResult.mHasFalloutCloudLayers)
        {
            mResult.mFalloutCloudTextures = *current.mFalloutCloudTextures;
            mResult.mFalloutCloudSpeeds = *current.mFalloutCloudSpeeds;
            for (std::size_t layer = 0; layer < mResult.mFalloutCloudColors.size(); ++layer)
            {
                const FalloutWeatherColorSamples& samples = (*current.mFalloutCloudColors)[layer];
                mResult.mFalloutCloudColors[layer] = sampleFalloutWeatherColor(
                    samples, gameHour, mTimeSettings, mFalloutDaytimeColorExtension);
            }
        }
        mResult.mGlareView = current.mGlareView;
        mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
        mResult.mRainLoopSoundID = current.mRainLoopSoundID;
        mResult.mAmbientSoundVolume = 1.f;
        mResult.mPrecipitationAlpha = 1.f;

        mResult.mIsStorm = current.mIsStorm;

        mResult.mRainSpeed = current.mRainSpeed;
        mResult.mRainEntranceSpeed = current.mRainEntranceSpeed;
        mResult.mRainDiameter = current.mRainDiameter;
        mResult.mRainMinHeight = current.mRainMinHeight;
        mResult.mRainMaxHeight = current.mRainMaxHeight;
        mResult.mRainMaxRaindrops = current.mRainMaxRaindrops;

        mResult.mParticleEffect = current.mParticleEffect;
        mResult.mRainEffect = current.mRainEffect;

        // Star visibility has its own climate fade window. A non-zero Night
        // contribution in the six-slot WTHR color blend does not mean that the
        // star dome is still visible after sunrise.
        mResult.mNight = (gameHour < mSunriseTime
            || gameHour > mTimeSettings.mNightStart + mTimeSettings.mStarsPostSunsetStart
                    - mTimeSettings.mStarsFadingDuration);
        mResult.mNightFade = mNightFade.getValue(gameHour, mTimeSettings, "Stars");
        mResult.mHasFalloutCelestialColors = current.mFalloutSunDiscColors && current.mFalloutStarColors;
        if (mResult.mHasFalloutCelestialColors)
        {
            mResult.mFalloutStarsColor = sampleFalloutWeatherColor(
                *current.mFalloutStarColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension);
            mResult.mFalloutStarsColor.a() = mResult.mNightFade;
        }

        mResult.mFogDepth = current.mLandFogDepth.getValue(gameHour, mTimeSettings, "Fog");
        mResult.mHasFalloutFogRange = current.mFalloutFogDistance.has_value();
        if (mResult.mHasFalloutFogRange)
        {
            const std::array<float, ESM4::Weather::sTimeCount>& fogDistance = *current.mFalloutFogDistance;
            const float dayStrength = std::clamp(getFalloutFogDayStrength(gameHour, mTimeSettings), 0.f, 1.f);
            mResult.mFalloutFogNear = lerp(fogDistance[2], fogDistance[0], dayStrength);
            mResult.mFalloutFogFar = lerp(fogDistance[3], fogDistance[1], dayStrength);
            mResult.mFalloutFogPower = lerp(fogDistance[5], fogDistance[4], dayStrength);

            if (std::getenv("OPENMW_FNV_PROOF_WEATHER_ID") != nullptr)
            {
                static int proofFogLogs = 0;
                if (proofFogLogs < 12)
                {
                    const float range = mResult.mFalloutFogFar - mResult.mFalloutFogNear;
                    Log(Debug::Info) << "FNV/ESM4 fog proof: mode=authored-fnam near="
                                     << mResult.mFalloutFogNear << " far=" << mResult.mFalloutFogFar
                                     << " power=" << mResult.mFalloutFogPower << " range=" << range
                                     << " denominator=" << range;
                    ++proofFogLogs;
                }
            }
        }
        mResult.mFogColor = current.mFalloutFogColors
            ? sampleFalloutWeatherColor(
                *current.mFalloutFogColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension)
            : current.mFogColor.getValue(gameHour, mTimeSettings, "Fog");
        mResult.mAmbientColor = current.mFalloutAmbientColors
            ? sampleFalloutWeatherColor(
                *current.mFalloutAmbientColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension)
            : current.mAmbientColor.getValue(gameHour, mTimeSettings, "Ambient");
        mResult.mSunColor = current.mFalloutSunlightColors
            ? sampleFalloutWeatherColor(
                *current.mFalloutSunlightColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension)
            : current.mSunColor.getValue(gameHour, mTimeSettings, "Sun");
        mResult.mSkyColor = current.mFalloutSkyColors
            ? sampleFalloutWeatherColor(
                *current.mFalloutSkyColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension)
            : current.mSkyColor.getValue(gameHour, mTimeSettings, "Sky");
        mResult.mSkyLowerColor = current.mFalloutSkyLowerColors
            ? sampleFalloutWeatherColor(
                *current.mFalloutSkyLowerColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension)
            : current.mSkyLowerColor.getValue(gameHour, mTimeSettings, "Sky");
        mResult.mSkyHorizonColor = current.mFalloutHorizonColors
            ? sampleFalloutWeatherColor(
                *current.mFalloutHorizonColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension)
            : current.mSkyHorizonColor.getValue(gameHour, mTimeSettings, "Sky");
        mResult.mDLFogFactor = current.mDL.FogFactor;
        mResult.mDLFogOffset = current.mDL.FogOffset;

        if (mResult.mHasFalloutCelestialColors)
        {
            mResult.mSunDiscColor = sampleFalloutWeatherColor(
                *current.mFalloutSunDiscColors, gameHour, mTimeSettings, mFalloutDaytimeColorExtension);
            float sunAlpha = 0.f;
            if (gameHour > mTimeSettings.mNightEnd && gameHour < mTimeSettings.mDayStart)
                sunAlpha = (gameHour - mTimeSettings.mNightEnd)
                    / (mTimeSettings.mDayStart - mTimeSettings.mNightEnd);
            else if (gameHour >= mTimeSettings.mDayStart && gameHour <= mTimeSettings.mDayEnd)
                sunAlpha = 1.f;
            else if (gameHour > mTimeSettings.mDayEnd && gameHour < mTimeSettings.mNightStart)
                sunAlpha = (mTimeSettings.mNightStart - gameHour)
                    / (mTimeSettings.mNightStart - mTimeSettings.mDayEnd);
            mResult.mSunDiscColor.a() = std::clamp(sunAlpha, 0.f, 1.f);
        }
        else
        {
            WeatherSetting setting = mTimeSettings.getSetting("Sun");
            float preSunsetTime = setting.mPreSunsetTime;
            if (gameHour >= mTimeSettings.mDayEnd - preSunsetTime)
            {
                float factor = 1.f;
                if (preSunsetTime > 0)
                    factor = (gameHour - (mTimeSettings.mDayEnd - preSunsetTime)) / preSunsetTime;
                factor = std::min(1.f, factor);
                mResult.mSunDiscColor = lerp(osg::Vec4f(1, 1, 1, 1), current.mSunDiscSunsetColor, factor);
                mResult.mSunDiscColor
                    = mResult.mSunDiscColor + osg::componentMultiply(mResult.mSunDiscColor, mResult.mAmbientColor);
                for (int i = 0; i < 3; ++i)
                    mResult.mSunDiscColor[i] = std::min(1.f, mResult.mSunDiscColor[i]);
            }
            else
                mResult.mSunDiscColor = osg::Vec4f(1, 1, 1, 1);

            if (gameHour >= mTimeSettings.mDayEnd)
            {
                float fade = std::min(
                    1.f, (gameHour - mTimeSettings.mDayEnd) / (mTimeSettings.mNightStart - mTimeSettings.mDayEnd));
                fade = fade * fade;
                mResult.mSunDiscColor.a() = 1.f - fade;
            }
            else if (gameHour >= mTimeSettings.mNightEnd
                && gameHour <= mTimeSettings.mNightEnd + mSunriseDuration / 2.f)
                mResult.mSunDiscColor.a() = gameHour - mTimeSettings.mNightEnd;
            else
                mResult.mSunDiscColor.a() = 1;
        }

        mResult.mStormDirection = calculateStormDirection(mResult.mParticleEffect);
    }

    inline void WeatherManager::calculateTransitionResult(const float factor, const float gameHour)
    {
        calculateResult(mCurrentWeather, gameHour);
        const MWRender::WeatherResult current = mResult;
        calculateResult(mNextWeather, gameHour);
        const MWRender::WeatherResult other = mResult;

        mResult.mStormDirection = current.mStormDirection;
        mResult.mNextStormDirection = other.mStormDirection;

        mResult.mCloudTexture = current.mCloudTexture;
        mResult.mNextCloudTexture = other.mCloudTexture;
        mResult.mCloudBlendFactor = mWeatherSettings[mNextWeather].cloudBlendFactor(factor);

        mResult.mFogColor = lerp(current.mFogColor, other.mFogColor, factor);
        mResult.mSunColor = lerp(current.mSunColor, other.mSunColor, factor);
        mResult.mSkyColor = lerp(current.mSkyColor, other.mSkyColor, factor);
        mResult.mSkyLowerColor = lerp(current.mSkyLowerColor, other.mSkyLowerColor, factor);
        mResult.mSkyHorizonColor = lerp(current.mSkyHorizonColor, other.mSkyHorizonColor, factor);

        mResult.mAmbientColor = lerp(current.mAmbientColor, other.mAmbientColor, factor);
        mResult.mSunDiscColor = lerp(current.mSunDiscColor, other.mSunDiscColor, factor);
        mResult.mHasFalloutCelestialColors
            = current.mHasFalloutCelestialColors && other.mHasFalloutCelestialColors;
        mResult.mFalloutStarsColor = lerp(current.mFalloutStarsColor, other.mFalloutStarsColor, factor);
        mResult.mFogDepth = lerp(current.mFogDepth, other.mFogDepth, factor);
        mResult.mHasFalloutFogRange = current.mHasFalloutFogRange && other.mHasFalloutFogRange;
        if (mResult.mHasFalloutFogRange)
        {
            mResult.mFalloutFogNear = lerp(current.mFalloutFogNear, other.mFalloutFogNear, factor);
            mResult.mFalloutFogFar = lerp(current.mFalloutFogFar, other.mFalloutFogFar, factor);
            mResult.mFalloutFogPower = lerp(current.mFalloutFogPower, other.mFalloutFogPower, factor);
        }
        mResult.mDLFogFactor = lerp(current.mDLFogFactor, other.mDLFogFactor, factor);
        mResult.mDLFogOffset = lerp(current.mDLFogOffset, other.mDLFogOffset, factor);

        mResult.mCurrentWindSpeed = calculateWindSpeed(mCurrentWeather, mCurrentWindSpeed);
        mResult.mNextWindSpeed = calculateWindSpeed(mNextWeather, mNextWindSpeed);
        mResult.mBaseWindSpeed = lerp(current.mBaseWindSpeed, other.mBaseWindSpeed, factor);

        mResult.mWindSpeed = lerp(mResult.mCurrentWindSpeed, mResult.mNextWindSpeed, factor);
        mResult.mCloudSpeed = lerp(current.mCloudSpeed, other.mCloudSpeed, factor);
        mResult.mHasFalloutCloudLayers = current.mHasFalloutCloudLayers && other.mHasFalloutCloudLayers;
        if (mResult.mHasFalloutCloudLayers)
        {
            for (std::size_t layer = 0; layer < mResult.mFalloutCloudTextures.size(); ++layer)
            {
                mResult.mFalloutCloudTextures[layer] = factor < 0.5f ? current.mFalloutCloudTextures[layer]
                                                                     : other.mFalloutCloudTextures[layer];
                mResult.mFalloutCloudSpeeds[layer]
                    = lerp(current.mFalloutCloudSpeeds[layer], other.mFalloutCloudSpeeds[layer], factor);
                mResult.mFalloutCloudColors[layer]
                    = lerp(current.mFalloutCloudColors[layer], other.mFalloutCloudColors[layer], factor);
            }
        }
        mResult.mGlareView = lerp(current.mGlareView, other.mGlareView, factor);
        mResult.mNightFade = lerp(current.mNightFade, other.mNightFade, factor);

        mResult.mNight = current.mNight;

        float threshold = mWeatherSettings[mNextWeather].mRainThreshold;
        if (threshold <= 0.f)
            threshold = 0.5f;

        if (factor < threshold)
        {
            mResult.mIsStorm = current.mIsStorm;
            mResult.mParticleEffect = current.mParticleEffect;
            mResult.mRainEffect = current.mRainEffect;
            mResult.mRainSpeed = current.mRainSpeed;
            mResult.mRainEntranceSpeed = current.mRainEntranceSpeed;
            mResult.mAmbientSoundVolume = 1.f - factor / threshold;
            mResult.mPrecipitationAlpha = mResult.mAmbientSoundVolume;
            mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
            mResult.mRainLoopSoundID = current.mRainLoopSoundID;
            mResult.mRainDiameter = current.mRainDiameter;
            mResult.mRainMinHeight = current.mRainMinHeight;
            mResult.mRainMaxHeight = current.mRainMaxHeight;
            mResult.mRainMaxRaindrops = current.mRainMaxRaindrops;
        }
        else
        {
            mResult.mIsStorm = other.mIsStorm;
            mResult.mParticleEffect = other.mParticleEffect;
            mResult.mRainEffect = other.mRainEffect;
            mResult.mRainSpeed = other.mRainSpeed;
            mResult.mRainEntranceSpeed = other.mRainEntranceSpeed;
            mResult.mAmbientSoundVolume = (factor - threshold) / (1 - threshold);
            mResult.mPrecipitationAlpha = mResult.mAmbientSoundVolume;
            mResult.mAmbientLoopSoundID = other.mAmbientLoopSoundID;
            mResult.mRainLoopSoundID = other.mRainLoopSoundID;

            mResult.mRainDiameter = other.mRainDiameter;
            mResult.mRainMinHeight = other.mRainMinHeight;
            mResult.mRainMaxHeight = other.mRainMaxHeight;
            mResult.mRainMaxRaindrops = other.mRainMaxRaindrops;
        }
    }
}

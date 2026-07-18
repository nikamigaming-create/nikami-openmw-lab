#include <gtest/gtest.h>

#include <cmath>

#include "apps/openmw/mwworld/timestamp.hpp"
#include "apps/openmw/mwworld/weather.hpp"

namespace MWWorld
{
    namespace
    {
        TEST(MWWorldWeatherTest, samplesRetailFNVHighNoonToDayAfternoonSegment)
        {
            FalloutWeatherColorSamples ambient{};
            ambient[1] = osg::Vec4f(87.f / 255.f, 105.f / 255.f, 138.f / 255.f, 1.f);
            ambient[4] = osg::Vec4f(99.f / 255.f, 120.f / 255.f, 154.f / 255.f, 1.f);
            TimeOfDaySettings settings{};
            settings.mNightEnd = 6.f;
            settings.mDayStart = 8.f;
            settings.mDayEnd = 18.f;
            settings.mNightStart = 20.f;

            const osg::Vec4f sampled = sampleFalloutWeatherColor(ambient, 14.4118919f, settings);

            EXPECT_NEAR(sampled.r(), 0.369318515f, 0.000001f);
            EXPECT_NEAR(sampled.g(), 0.4469423f, 0.000001f);
            EXPECT_NEAR(sampled.b(), 0.578699231f, 0.000001f);
        }

        TEST(MWWorldWeatherTest, samplesAllRetailFNVTimeSlotsAndStrictBoundaries)
        {
            FalloutWeatherColorSamples samples{};
            for (std::size_t slot = 0; slot < samples.size(); ++slot)
                samples[slot] = osg::Vec4f(static_cast<float>(slot), 0.f, 0.f, 1.f);
            TimeOfDaySettings settings{};
            settings.mNightEnd = 6.f;
            settings.mDayStart = 8.f;
            settings.mDayEnd = 18.f;
            settings.mNightStart = 20.f;

            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 2.f, settings).r(), 3.f); // Night
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 5.5f, settings).r(), 3.f); // inclusive Night edge
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 6.125f, settings).r(), 1.5f); // Night/Sunrise
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 6.75f, settings).r(), 0.f); // Sunrise apex
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 7.375f, settings).r(), 0.5f); // Sunrise/Day
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 8.f, settings).r(), 1.f); // strict Day boundary
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 10.f, settings).r(), 2.5f); // Day/HighNoon
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 12.f, settings).r(), 1.f); // strict Day boundary
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 15.f, settings).r(), 2.5f); // HighNoon/Day
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 18.f, settings).r(), 1.f); // strict Day boundary
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 18.625f, settings).r(), 1.5f); // Day/Sunset
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 19.25f, settings).r(), 2.f); // Sunset apex
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 19.875f, settings).r(), 2.5f); // Sunset/Night
            EXPECT_FLOAT_EQ(sampleFalloutWeatherColor(samples, 20.5f, settings).r(), 3.f); // inclusive Night edge
        }

        TEST(MWWorldWeatherTest, samplesRetailFalloutFogAcrossExtendedDayNightRanges)
        {
            const FalloutWeatherFogSamples samples{
                { 10.f, 120000.f, 0.25f },
                { -7500.f, 150000.f, 1.f },
            };
            TimeOfDaySettings settings{};
            settings.mNightEnd = 6.f;
            settings.mDayStart = 8.f;
            settings.mDayEnd = 18.f;
            settings.mNightStart = 20.f;

            const FalloutWeatherFog night = sampleFalloutWeatherFog(samples, 5.5f, settings);
            EXPECT_FLOAT_EQ(night.mNear, -7500.f);
            EXPECT_FLOAT_EQ(night.mFar, 150000.f);
            EXPECT_FLOAT_EQ(night.mPower, 1.f);

            const FalloutWeatherFog sunriseMidpoint = sampleFalloutWeatherFog(samples, 6.75f, settings);
            EXPECT_FLOAT_EQ(sunriseMidpoint.mNear, -3745.f);
            EXPECT_FLOAT_EQ(sunriseMidpoint.mFar, 135000.f);
            EXPECT_FLOAT_EQ(sunriseMidpoint.mPower, 0.625f);

            const FalloutWeatherFog day = sampleFalloutWeatherFog(samples, 8.f, settings);
            EXPECT_FLOAT_EQ(day.mNear, 10.f);
            EXPECT_FLOAT_EQ(day.mFar, 120000.f);
            EXPECT_FLOAT_EQ(day.mPower, 0.25f);
            EXPECT_EQ(sampleFalloutWeatherFog(samples, 18.f, settings).mFar, day.mFar);

            const FalloutWeatherFog sunsetMidpoint = sampleFalloutWeatherFog(samples, 19.25f, settings);
            EXPECT_FLOAT_EQ(sunsetMidpoint.mNear, sunriseMidpoint.mNear);
            EXPECT_FLOAT_EQ(sunsetMidpoint.mFar, sunriseMidpoint.mFar);
            EXPECT_FLOAT_EQ(sunsetMidpoint.mPower, sunriseMidpoint.mPower);

            const FalloutWeatherFog lateNight = sampleFalloutWeatherFog(samples, 20.5f, settings);
            EXPECT_FLOAT_EQ(lateNight.mNear, night.mNear);
            EXPECT_FLOAT_EQ(lateNight.mFar, night.mFar);
            EXPECT_FLOAT_EQ(lateNight.mPower, night.mPower);
        }

        TEST(MWWorldWeatherTest, reproducesCapturedGoodspringsFogParameters)
        {
            const FalloutWeatherFogSamples samples{
                { 10.f, 120000.f, 0.5f },
                { 0.f, 150000.f, 0.5f },
            };
            TimeOfDaySettings settings{};
            settings.mNightEnd = 6.f;
            settings.mDayStart = 8.f;
            settings.mDayEnd = 18.f;
            settings.mNightStart = 20.f;

            const FalloutWeatherFog morning = sampleFalloutWeatherFog(samples, 6.00486183f, settings);
            EXPECT_NEAR(morning.mFar, 143941.672f, 0.05f);
            EXPECT_NEAR(morning.mFar - morning.mNear, 143939.656f, 0.05f);
            EXPECT_FLOAT_EQ(morning.mPower, 0.5f);

            const FalloutWeatherFog noon = sampleFalloutWeatherFog(samples, 12.0011663f, settings);
            EXPECT_FLOAT_EQ(noon.mFar, 120000.f);
            EXPECT_FLOAT_EQ(noon.mFar - noon.mNear, 119990.f);
            EXPECT_FLOAT_EQ(noon.mPower, 0.5f);

            const FalloutWeatherFog evening = sampleFalloutWeatherFog(samples, 18.0045681f, settings);
            EXPECT_NEAR(evening.mFar, 120054.82f, 0.05f);
            EXPECT_NEAR(evening.mFar - evening.mNear, 120044.836f, 0.05f);
            EXPECT_FLOAT_EQ(evening.mPower, 0.5f);
        }

        TEST(MWWorldWeatherTest, interpolatesAllFalloutFogParametersAcrossWeatherTransition)
        {
            const FalloutWeatherFog current{ -250.f, 120000.f, 0.25f };
            const FalloutWeatherFog next{ 750.f, 200000.f, 1.f };

            const FalloutWeatherFog result = interpolateFalloutWeatherFog(current, next, 0.25f);

            EXPECT_FLOAT_EQ(result.mNear, 0.f);
            EXPECT_FLOAT_EQ(result.mFar, 140000.f);
            EXPECT_FLOAT_EQ(result.mPower, 0.4375f);
        }

        TEST(MWWorldWeatherTest, mapsRetailFalloutSunOrbit)
        {
            const osg::Vec3f highNoon = falloutSunPosition(0.f);
            EXPECT_FLOAT_EQ(highNoon.x(), 0.f);
            EXPECT_FLOAT_EQ(highNoon.y(), -100.f);
            EXPECT_FLOAT_EQ(highNoon.z(), 800.f);

            // xNVSE retail capture at GameHour 14.4118919.
            const osg::Vec3f captured = falloutSunPosition(-0.201698895f);
            EXPECT_NEAR(captured.x(), -161.359116f, 0.0001f);
            EXPECT_FLOAT_EQ(captured.y(), -100.f);
            EXPECT_NEAR(captured.z(), 638.640869f, 0.0001f);
        }

        TEST(MWWorldWeatherTest, mapsSingleRetailFalloutMoonToOpenMWQuadAxis)
        {
            const MWRender::MoonState day
                = falloutMoonState(14.4118919f, MWRender::MoonState::Phase::FirstQuarter, false);
            // The retail angle accumulator is normalized to [0, 360). 306.17838 degrees is
            // the same quad rotation as the legacy -53.82162 value, but preserves the domain
            // used by the retail 20..160 degree phase-shadow window below.
            EXPECT_NEAR(day.mRotationFromHorizon, 306.17838f, 0.0001f);
            EXPECT_FLOAT_EQ(day.mRotationFromNorth, 35.f);
            EXPECT_EQ(day.mPhase, MWRender::MoonState::Phase::FirstQuarter);
            EXPECT_FLOAT_EQ(day.mShadowBlend, 0.f);
            EXPECT_FLOAT_EQ(day.mMoonAlpha, 0.f);

            const MWRender::MoonState night
                = falloutMoonState(23.004034f, MWRender::MoonState::Phase::FirstQuarter, true);
            EXPECT_NEAR(night.mRotationFromHorizon, 75.06051f, 0.0001f);
            EXPECT_FLOAT_EQ(night.mShadowBlend, 1.f);
            EXPECT_FLOAT_EQ(night.mMoonAlpha, 1.f);
        }

        TEST(MWWorldWeatherTest, advancesRetailFalloutMoonPhaseFromClimateData)
        {
            // FNV/FO3 default CLMT TNAM ends in 0x83: Masser, no Secunda,
            // and three game-days per phase. xNVSE observed the runtime
            // phase global advancing 2,3,4,5,6,7,0 at these boundaries.
            constexpr std::uint8_t moonInfo = 0x83;
            EXPECT_EQ(falloutMoonPhase(0, moonInfo), MWRender::MoonState::Phase::Full);
            EXPECT_EQ(falloutMoonPhase(2, moonInfo), MWRender::MoonState::Phase::Full);
            EXPECT_EQ(falloutMoonPhase(3, moonInfo), MWRender::MoonState::Phase::WaningGibbous);
            EXPECT_EQ(falloutMoonPhase(6, moonInfo), MWRender::MoonState::Phase::ThirdQuarter);
            EXPECT_EQ(falloutMoonPhase(9, moonInfo), MWRender::MoonState::Phase::WaningCrescent);
            EXPECT_EQ(falloutMoonPhase(12, moonInfo), MWRender::MoonState::Phase::New);
            EXPECT_EQ(falloutMoonPhase(15, moonInfo), MWRender::MoonState::Phase::WaxingCrescent);
            EXPECT_EQ(falloutMoonPhase(18, moonInfo), MWRender::MoonState::Phase::FirstQuarter);
            EXPECT_EQ(falloutMoonPhase(21, moonInfo), MWRender::MoonState::Phase::WaxingGibbous);
            EXPECT_EQ(falloutMoonPhase(24, moonInfo), MWRender::MoonState::Phase::Full);
        }

        // MASSER PHASES

        TEST(MWWorldWeatherTest, masserPhasesFullToWaningGibbousAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 2 and 26, 11:57
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 2 + 11.0f + 56.0f / 60.0f);
            timeStampAfter += (24.0f * 2 + 11.0f + 58.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 26 + 11.0f + 56.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 26 + 11.0f + 58.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(0));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(1));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(0));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(1));
        }

        TEST(MWWorldWeatherTest, masserPhasesWaningGibbousToThirdQuarterAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 5 and 29, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 4 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 5 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 28 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 29 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(1));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(2));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(1));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(2));
        }

        TEST(MWWorldWeatherTest, masserPhasesThirdQuarterToWaningCrescentAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 8 and 32, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 7 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 8 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 31 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 32 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(2));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(3));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(2));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(3));
        }

        TEST(MWWorldWeatherTest, masserPhasesWaningCrescentToNewAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 11 and 35, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 10 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 11 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 34 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 35 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(3));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(4));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(3));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(4));
        }

        TEST(MWWorldWeatherTest, masserPhasesNewToWaxingCrescentAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 14 and 38, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 13 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 14 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 37 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 38 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(4));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(5));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(4));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(5));
        }

        TEST(MWWorldWeatherTest, masserPhasesWaxingCrescentToFirstQuarterAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 17 and 41, 2:57
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 17 + 2.0f + 56.0f / 60.0f);
            timeStampAfter += (24.0f * 17 + 2.0f + 58.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 41 + 2.0f + 56.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 41 + 2.0f + 58.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(5));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(6));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(5));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(6));
        }

        TEST(MWWorldWeatherTest, masserPhasesFirstQuarterToWaxingGibbousAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 20 and 44, 5:57
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 20 + 5.0f + 56.0f / 60.0f);
            timeStampAfter += (24.0f * 20 + 5.0f + 58.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 44 + 5.0f + 56.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 44 + 5.0f + 58.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(6));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(7));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(6));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(7));
        }

        TEST(MWWorldWeatherTest, masserPhasesWaxingGibbousToFullAtCorrectTimes)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 35.0f;

            // Days 23 and 47, 8:57
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 23 + 8.0f + 56.0f / 60.0f);
            timeStampAfter += (24.0f * 23 + 8.0f + 58.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 47 + 8.0f + 56.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 47 + 8.0f + 58.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(7));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(0));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(7));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(0));
        }

        // SECUNDA PHASES

        TEST(MWWorldWeatherTest, secundaPhasesFullToWaningGibbousAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 2 and 26, 14:19
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 2 + 14.0f + 18.0f / 60.0f);
            timeStampAfter += (24.0f * 2 + 14.0f + 20.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 26 + 14.0f + 18.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 26 + 14.0f + 20.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(0));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(1));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(0));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(1));
        }

        TEST(MWWorldWeatherTest, secundaPhasesWaningGibbousToThirdQuarterAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 5 and 29, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 4 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 5 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 28 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 29 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(1));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(2));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(1));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(2));
        }

        TEST(MWWorldWeatherTest, secundaPhasesThirdQuarterToWaningCrescentAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 8 and 32, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 7 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 8 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 31 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 32 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(2));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(3));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(2));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(3));
        }

        TEST(MWWorldWeatherTest, secundaPhasesWaningCrescentToNewAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 11 and 35, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 10 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 11 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 34 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 35 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(3));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(4));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(3));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(4));
        }

        TEST(MWWorldWeatherTest, secundaPhasesNewToWaxingCrescentAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 14 and 38, 0:00
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 13 + 23.0f + 59.0f / 60.0f);
            timeStampAfter += (24.0f * 14 + 0.0f + 1.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 37 + 23.0f + 59.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 38 + 0.0f + 1.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(4));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(5));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(4));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(5));
        }

        TEST(MWWorldWeatherTest, secundaPhasesWaxingCrescentToFirstQuarterAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 17 and 41, 3:31
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 17 + 3.0f + 30.0f / 60.0f);
            timeStampAfter += (24.0f * 17 + 3.0f + 32.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 41 + 3.0f + 30.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 41 + 3.0f + 32.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(5));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(6));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(5));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(6));
        }

        TEST(MWWorldWeatherTest, secundaPhasesFirstQuarterToWaxingGibbousAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 20 and 44, 7:07
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 20 + 7.0f + 6.0f / 60.0f);
            timeStampAfter += (24.0f * 20 + 7.0f + 8.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 44 + 7.0f + 6.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 44 + 7.0f + 8.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(6));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(7));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(6));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(7));
        }

        TEST(MWWorldWeatherTest, secundaPhasesWaxingGibbousToFullAtCorrectTimes)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 23 and 47, 10:43
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 23 + 10.0f + 42.0f / 60.0f);
            timeStampAfter += (24.0f * 23 + 10.0f + 44.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 47 + 10.0f + 42.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 47 + 10.0f + 44.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_EQ(beforeState.mPhase, static_cast<MWRender::MoonState::Phase>(7));
            EXPECT_EQ(afterState.mPhase, static_cast<MWRender::MoonState::Phase>(0));
            EXPECT_EQ(beforeStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(7));
            EXPECT_EQ(afterStatePostLoop.mPhase, static_cast<MWRender::MoonState::Phase>(0));
        }

        // OFFSETS

        TEST(MWWorldWeatherTest, secundaShouldApplyIncrementOffsetAfterFirstLoop)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 14.0f;
            float fadeInFinish = 15.0f;
            float fadeOutStart = 7.0f;
            float fadeOutFinish = 10.0f;
            float axisOffset = 50.0f;

            // Days 8 and 32, 3:16
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 8 + 3.0f + 15.0f / 60.0f);
            timeStampAfter += (24.0f * 8 + 3.0f + 17.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 32 + 3.0f + 15.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 32 + 3.0f + 17.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_LE(beforeState.mMoonAlpha, 0.0f);
            EXPECT_GT(afterState.mMoonAlpha, 0.0f);
            EXPECT_LE(beforeStatePostLoop.mMoonAlpha, 0.0f);
            EXPECT_GT(afterStatePostLoop.mMoonAlpha, 0.0f);
        }

        TEST(MWWorldWeatherTest, moonWithLowIncrementShouldApplyIncrementOffsetAfterCycle)
        {
            float dailyIncrement = 0.9f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 0.0f;
            float fadeInFinish = 0.0f;
            float fadeOutStart = 0.0f;
            float fadeOutFinish = 0.0f;
            float axisOffset = 35.0f;

            // Days 7 and 31, 1:44
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 7 + 1.0f + 43.0f / 60.0f);
            timeStampAfter += (24.0f * 7 + 1.0f + 45.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 31 + 1.0f + 43.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 31 + 1.0f + 45.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_LE(beforeState.mMoonAlpha, 0.0f);
            EXPECT_GT(afterState.mMoonAlpha, 0.0f);
            EXPECT_LE(beforeStatePostLoop.mMoonAlpha, 0.0f);
            EXPECT_GT(afterStatePostLoop.mMoonAlpha, 0.0f);
        }

        TEST(MWWorldWeatherTest, masserShouldApplyIncrementOffsetAfterCycle)
        {
            float dailyIncrement = 1.0f;
            float speed = 0.5f;
            float fadeEndAngle = 40.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 0.0f;
            float fadeInFinish = 0.0f;
            float fadeOutStart = 0.0f;
            float fadeOutFinish = 0.0f;
            float axisOffset = 35.0f;

            // Days 4 and 28, 1:02
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 4 + 1.0f + 1.0f / 60.0f);
            timeStampAfter += (24.0f * 4 + 1.0f + 3.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 28 + 1.0f + 1.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 28 + 1.0f + 3.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_LE(beforeState.mMoonAlpha, 0.0f);
            EXPECT_GT(afterState.mMoonAlpha, 0.0f);
            EXPECT_LE(beforeStatePostLoop.mMoonAlpha, 0.0f);
            EXPECT_GT(afterStatePostLoop.mMoonAlpha, 0.0f);
        }

        TEST(MWWorldWeatherTest, secundaShouldApplyIncrementOffsetAfterCycle)
        {
            float dailyIncrement = 1.2f;
            float speed = 0.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 0.0f;
            float fadeInFinish = 0.0f;
            float fadeOutStart = 0.0f;
            float fadeOutFinish = 0.0f;
            float axisOffset = 50.0f;

            // Days 3 and 27, 2:04
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 3 + 2.0f + 3.0f / 60.0f);
            timeStampAfter += (24.0f * 3 + 2.0f + 5.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 27 + 2.0f + 3.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 27 + 2.0f + 5.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_LE(beforeState.mMoonAlpha, 0.0f);
            EXPECT_GT(afterState.mMoonAlpha, 0.0f);
            EXPECT_LE(beforeStatePostLoop.mMoonAlpha, 0.0f);
            EXPECT_GT(afterStatePostLoop.mMoonAlpha, 0.0f);
        }

        TEST(MWWorldWeatherTest, moonWithIncreasedSpeedShouldApplyIncrementOffsetAfterCycle)
        {
            float dailyIncrement = 1.2f;
            float speed = 1.6f;
            float fadeEndAngle = 30.0f;
            float fadeStartAngle = 50.0f;
            float moonShadowEarlyFadeAngle = 0.5f;
            float fadeInStart = 0.0f;
            float fadeInFinish = 0.0f;
            float fadeOutStart = 0.0f;
            float fadeOutFinish = 0.0f;
            float axisOffset = 50.0f;

            // Days 4 and 28, 1:13
            TimeStamp timeStampBefore, timeStampAfter, timeStampBeforePostLoop, timeStampAfterPostLoop;
            timeStampBefore += (24.0f * 4 + 1.0f + 12.0f / 60.0f);
            timeStampAfter += (24.0f * 4 + 1.0f + 14.0f / 60.0f);
            timeStampBeforePostLoop += (24.0f * 28 + 1.0f + 12.0f / 60.0f);
            timeStampAfterPostLoop += (24.0f * 28 + 1.0f + 14.0f / 60.0f);

            MWWorld::MoonModel moon = MWWorld::MoonModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish,
                axisOffset, speed, dailyIncrement, fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle);

            MWRender::MoonState beforeState = moon.calculateState(timeStampBefore);
            MWRender::MoonState afterState = moon.calculateState(timeStampAfter);
            MWRender::MoonState beforeStatePostLoop = moon.calculateState(timeStampBeforePostLoop);
            MWRender::MoonState afterStatePostLoop = moon.calculateState(timeStampAfterPostLoop);

            EXPECT_LE(beforeState.mMoonAlpha, 0.0f);
            EXPECT_GT(afterState.mMoonAlpha, 0.0f);
            EXPECT_LE(beforeStatePostLoop.mMoonAlpha, 0.0f);
            EXPECT_GT(afterStatePostLoop.mMoonAlpha, 0.0f);
        }
    }
}

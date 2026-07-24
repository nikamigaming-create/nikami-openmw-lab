#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include <components/esm4/common.hpp>
#include <components/esm4/loadclmt.hpp>
#include <components/esm4/loadwrld.hpp>

#include "apps/openmw/mwworld/fnvweather.hpp"
#include "apps/openmw/mwworld/store.hpp"

namespace
{
    using testing::HasSubstr;

    constexpr ESM::FormId form(std::uint32_t index, std::int32_t contentFile = 0)
    {
        return ESM::FormId{ index, contentFile };
    }

    ESM4::Weather makeCompleteWeather(ESM::FormId id)
    {
        ESM4::Weather result;
        result.mId = id;
        result.mEditorId = id.mIndex == 0x1237d7 ? "NVWastelandGS" : "NVWastelandClear";
        result.mHasMaxCloudLayers = true;
        result.mMaxCloudLayers = ESM4::Weather::sCloudLayerCount;
        result.mHasCloudSpeeds = true;
        result.mCloudSpeeds = { 52, 0, 0, 65 };
        result.mCloudTextures = { "sky\\alpha.dds", "sky\\alpha.dds", "sky\\alpha.dds",
            "sky\\NVCloudlight.dds" };
        result.mHasCloudColors = true;
        result.mCloudColorSampleCount = ESM4::Weather::sTimeCount;
        result.mHasColors = true;
        result.mColorSampleCount = ESM4::Weather::sTimeCount;
        result.mHasFogDistance = true;
        result.mFogDistance = { 10.f, 120000.f, 0.f, 150000.f, 0.5f, 0.5f };
        result.mHasData = true;
        result.mData.windSpeed = 50;
        result.mData.lowerCloudSpeed = 45;
        result.mData.transitionDelta = 255;
        result.mData.sunGlare = 54;
        result.mData.classification = 1;

        for (std::size_t row = 0; row < ESM4::Weather::sColorTypeCount; ++row)
        {
            for (std::size_t sample = 0; sample < ESM4::Weather::sTimeCount; ++sample)
            {
                result.mColors[row][sample] = { static_cast<std::uint8_t>(10 + row),
                    static_cast<std::uint8_t>(20 + sample), static_cast<std::uint8_t>(30 + row + sample), 0 };
            }
        }
        for (std::size_t layer = 0; layer < ESM4::Weather::sCloudLayerCount; ++layer)
        {
            for (std::size_t sample = 0; sample < ESM4::Weather::sTimeCount; ++sample)
            {
                result.mCloudColors[layer][sample] = { static_cast<std::uint8_t>(40 + layer),
                    static_cast<std::uint8_t>(50 + sample), static_cast<std::uint8_t>(60 + layer + sample), 0 };
            }
        }
        return result;
    }

    struct WeatherFixture
    {
        ESM4::World mWorld{};
        ESM4::Climate mClimate{};
        ESM4::Weather mCurrent = makeCompleteWeather(form(0x1237d7));
        ESM4::Weather mDefault = makeCompleteWeather(form(0x0ffc88));
        MWWorld::FalloutExteriorPlayerPlacement mPlacement;
        MWWorld::FalloutSaveLoadPlan::SceneState mScene;

        WeatherFixture()
        {
            mWorld.mId = form(0x0da726);
            mWorld.mEditorId = "WastelandNV";
            mWorld.mClimate = form(0x08809b);

            mClimate.mId = mWorld.mClimate;
            mClimate.mEditorId = "NVDefaultClimate";
            mClimate.mHasTiming = true;
            mClimate.mTiming.mSunriseBegin = 36;
            mClimate.mTiming.mSunriseEnd = 48;
            mClimate.mTiming.mSunsetBegin = 108;
            mClimate.mTiming.mSunsetEnd = 120;
            mClimate.mTiming.mMoonInfo = 0x83;
            mClimate.mWeatherTypes.push_back({ mDefault.mId, 100, {} });

            mPlacement.mWorldspaceRecord = mWorld.mId;
            mPlacement.mCellRecord = form(0x0e1aa7);
            mPlacement.mCellX = -18;
            mPlacement.mCellY = -1;

            mScene.mCurrentWeather = mCurrent.mId;
            mScene.mDefaultWeather = mDefault.mId;
            mScene.mGameHour = 14.215002059936523f;
            mScene.mLastUpdateHour = 17.606250762939453f;
            mScene.mWeatherPercent = 1.f;
            mScene.mFogPower = 0.5f;
            mScene.mFlags = 0x20;
            mScene.mSkyMode = 3;
        }

        MWWorld::FalloutWeatherModelResolution resolve(bool includeWorld = true, bool includeClimate = true,
            bool includeCurrent = true, bool includeDefault = true) const
        {
            MWWorld::Store<ESM4::World> worlds;
            MWWorld::Store<ESM4::Climate> climates;
            MWWorld::Store<ESM4::Weather> weather;
            if (includeWorld)
                worlds.insertStatic(mWorld);
            if (includeClimate)
                climates.insertStatic(mClimate);
            if (includeCurrent)
                weather.insertStatic(mCurrent);
            if (includeDefault)
                weather.insertStatic(mDefault);
            return MWWorld::resolveFalloutWeatherModel(worlds, climates, weather, mPlacement, mScene);
        }
    };

    void expectColor(const osg::Vec4f& actual, float r, float g, float b, float tolerance = 1e-6f)
    {
        EXPECT_NEAR(actual.r(), r, tolerance);
        EXPECT_NEAR(actual.g(), g, tolerance);
        EXPECT_NEAR(actual.b(), b, tolerance);
        EXPECT_FLOAT_EQ(actual.a(), 1.f);
    }

    TEST(FNVWeatherModel, IgnoresEveryUnusedColorByteAndAcceptsForcedWeatherOutsideClimateList)
    {
        WeatherFixture fixture;
        fixture.mCurrent.mColors[ESM4::Weather::Color_SkyUpper][ESM4::Weather::Time_HighNoon].unused = 0x00;
        fixture.mCurrent.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon].unused = 0x7f;
        fixture.mCurrent.mCloudColors[3][ESM4::Weather::Time_HighNoon].unused = 0xff;
        fixture.mCurrent.mData.lightningColor.unused = 0x7f;
        fixture.mDefault.mColors[ESM4::Weather::Color_Fog][ESM4::Weather::Time_Day].unused = 0xff;

        const MWWorld::FalloutWeatherModelResolution resolution = fixture.resolve();
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutWeatherModel& model = *resolution.mModel;
        EXPECT_FALSE(model.mCurrentWeatherInClimateList);
        EXPECT_FLOAT_EQ(model.mCurrent.mColorTable.mColors[ESM4::Weather::Color_SkyUpper]
                             [ESM4::Weather::Time_HighNoon]
                                 .a(),
            1.f);
        EXPECT_FLOAT_EQ(model.mCurrent.mColorTable.mColors[ESM4::Weather::Color_Ambient]
                             [ESM4::Weather::Time_HighNoon]
                                 .a(),
            1.f);
        EXPECT_FLOAT_EQ(model.mCurrent.mColorTable.mCloudColors[3][ESM4::Weather::Time_HighNoon].a(), 1.f);
        EXPECT_FLOAT_EQ(model.mCurrent.mLightningColor.a(), 1.f);
        EXPECT_FLOAT_EQ(
            model.mDefault.mColorTable.mColors[ESM4::Weather::Color_Fog][ESM4::Weather::Time_Day].a(), 1.f);

        for (const auto& row : model.mCurrent.mColorTable.mColors)
            for (const osg::Vec4f& color : row)
                EXPECT_FLOAT_EQ(color.a(), 1.f);
        for (const auto& layer : model.mCurrent.mColorTable.mCloudColors)
            for (const osg::Vec4f& color : layer)
                EXPECT_FLOAT_EQ(color.a(), 1.f);
        for (const osg::Vec4f& color : model.mCurrentColors)
            EXPECT_FLOAT_EQ(color.a(), 1.f);
        for (const osg::Vec4f& color : model.mCurrentCloudColors)
            EXPECT_FLOAT_EQ(color.a(), 1.f);
    }

    TEST(FNVWeatherModel, RejectsEveryMissingRecordLink)
    {
        WeatherFixture fixture;
        EXPECT_THAT(fixture.resolve(false).mError, HasSubstr("worldspace is not loaded exactly"));
        EXPECT_THAT(fixture.resolve(true, false).mError, HasSubstr("CLMT is not loaded exactly"));
        EXPECT_THAT(fixture.resolve(true, true, false).mError, HasSubstr("current FNV WTHR"));
        EXPECT_THAT(fixture.resolve(true, true, true, false).mError, HasSubstr("default FNV WTHR"));

        fixture.mWorld.mClimate = {};
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("has no CLMT link"));
    }

    TEST(FNVWeatherModel, RejectsMissingRequiredFieldsAndMalformedTiming)
    {
        WeatherFixture fixture;
        fixture.mClimate.mHasTiming = false;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("no TNAM timing"));

        fixture = WeatherFixture{};
        fixture.mClimate.mTiming.mSunriseEnd = fixture.mClimate.mTiming.mSunriseBegin;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("malformed"));

        fixture = WeatherFixture{};
        fixture.mClimate.mTiming.mVolatility = 1;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("volatile"));

        fixture = WeatherFixture{};
        fixture.mCurrent.mHasColors = false;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("six-sample NAM0"));

        fixture = WeatherFixture{};
        fixture.mDefault.mHasCloudColors = false;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("six-sample PNAM"));

        fixture = WeatherFixture{};
        fixture.mCurrent.mHasFogDistance = false;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("no FNAM"));

        fixture = WeatherFixture{};
        fixture.mCurrent.mFogDistance[2] = std::numeric_limits<float>::quiet_NaN();
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("non-finite FNAM"));

        fixture = WeatherFixture{};
        fixture.mWorld.mFlags = ESM4::Rec_Deleted;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("worldspace is deleted"));

        fixture = WeatherFixture{};
        fixture.mWorld.mWorldFlags = ESM4::World::WLD_NoSky;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("forbids sky"));
    }

    TEST(FNVWeatherModel, RejectsUnsupportedTransitionOverrideAndSavedSkyState)
    {
        WeatherFixture fixture;
        fixture.mScene.mTransitionWeather = form(0x111111);
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("transition weather"));

        fixture = WeatherFixture{};
        fixture.mScene.mOverrideWeather = form(0x222222);
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("override weather"));

        fixture = WeatherFixture{};
        fixture.mScene.mWeatherPercent = 0.5f;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("partial weather percentage"));

        fixture = WeatherFixture{};
        fixture.mScene.mWeatherPercent = std::numeric_limits<float>::infinity();
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("must all be finite"));

        fixture = WeatherFixture{};
        fixture.mScene.mGameHour = std::numeric_limits<float>::quiet_NaN();
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("must all be finite"));

        fixture = WeatherFixture{};
        fixture.mScene.mLastUpdateHour = 24.f;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("last-update hour"));

        fixture = WeatherFixture{};
        fixture.mScene.mFlags = 0;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("flags or mode"));

        fixture = WeatherFixture{};
        fixture.mScene.mSkyMode = 0;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("flags or mode"));
    }

    TEST(FNVWeatherModel, UsesRetailColorSelectorAcrossTheEntireDay)
    {
        WeatherFixture fixture;
        auto& samples = fixture.mCurrent.mColors[ESM4::Weather::Color_Ambient];
        samples[ESM4::Weather::Time_Night] = { 1, 2, 3, 0xff };
        samples[ESM4::Weather::Time_Sunrise] = { 51, 52, 53, 0xff };
        samples[ESM4::Weather::Time_HighNoon] = { 10, 20, 30, 0xff };
        samples[ESM4::Weather::Time_Day] = { 110, 120, 130, 0x00 };
        samples[ESM4::Weather::Time_Sunset] = { 201, 202, 203, 0xff };

        fixture.mScene.mGameHour = 12.f;
        MWWorld::FalloutWeatherModelResolution atHighNoon = fixture.resolve();
        ASSERT_TRUE(atHighNoon) << atHighNoon.mError;
        EXPECT_EQ(atHighNoon.mModel->mTimeBlend.mPrimary, ESM4::Weather::Time_Day);
        expectColor(atHighNoon.mModel->mCurrentColors[ESM4::Weather::Color_Ambient], 110.f / 255.f,
            120.f / 255.f, 130.f / 255.f);

        fixture.mScene.mGameHour = 18.f;
        MWWorld::FalloutWeatherModelResolution atDay = fixture.resolve();
        ASSERT_TRUE(atDay) << atDay.mError;
        EXPECT_EQ(atDay.mModel->mTimeBlend.mPrimary, ESM4::Weather::Time_Day);
        expectColor(atDay.mModel->mCurrentColors[ESM4::Weather::Color_Ambient], 110.f / 255.f, 120.f / 255.f,
            130.f / 255.f);

        for (float hour : { 0.f, 2.f, 5.5f, 6.f, 7.f, 8.f, 10.f, 12.f, 15.f, 18.f, 19.f, 20.f, 23.99f })
        {
            fixture.mScene.mGameHour = hour;
            EXPECT_TRUE(fixture.resolve()) << "hour " << hour;
        }

        fixture.mScene.mGameHour = 2.f;
        const auto atNight = fixture.resolve();
        ASSERT_TRUE(atNight);
        EXPECT_EQ(atNight.mModel->mTimeBlend.mPrimary, ESM4::Weather::Time_Night);
        expectColor(atNight.mModel->mCurrentColors[ESM4::Weather::Color_Ambient], 1.f / 255.f, 2.f / 255.f,
            3.f / 255.f);

        fixture.mScene.mGameHour = 7.f;
        const auto atSunrise = fixture.resolve();
        ASSERT_TRUE(atSunrise);
        EXPECT_EQ(atSunrise.mModel->mTimeBlend.mPrimary, ESM4::Weather::Time_Sunrise);

        fixture.mScene.mGameHour = 19.f;
        const auto atSunset = fixture.resolve();
        ASSERT_TRUE(atSunset);
        EXPECT_EQ(atSunset.mModel->mTimeBlend.mPrimary, ESM4::Weather::Time_Sunset);

        fixture.mScene.mGameHour = 24.f;
        EXPECT_THAT(fixture.resolve().mError, HasSubstr("game hour must be"));
    }

    TEST(FNVWeatherModel, ReproducesTheSave330AfternoonColorAnchorsWithoutTes3Clear)
    {
        WeatherFixture fixture;
        auto setPair = [&](ESM4::Weather::ColorType type, ESM4::Weather::Color highNoon, ESM4::Weather::Color day) {
            fixture.mCurrent.mColors[type][ESM4::Weather::Time_HighNoon] = highNoon;
            fixture.mCurrent.mColors[type][ESM4::Weather::Time_Day] = day;
        };
        setPair(ESM4::Weather::Color_SkyUpper, { 72, 91, 159, 0xff }, { 50, 71, 135, 0x00 });
        setPair(ESM4::Weather::Color_Fog, { 150, 168, 190, 0x7f }, { 150, 168, 190, 0xff });
        setPair(ESM4::Weather::Color_Ambient, { 99, 120, 154, 0x00 }, { 87, 105, 138, 0x7f });
        setPair(ESM4::Weather::Color_Sunlight, { 255, 227, 170, 0xff }, { 255, 227, 170, 0x00 });
        setPair(ESM4::Weather::Color_Sun, { 255, 255, 255, 0x7f }, { 254, 241, 211, 0xff });

        const MWWorld::FalloutWeatherModelResolution resolution = fixture.resolve();
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutWeatherModel& model = *resolution.mModel;
        EXPECT_EQ(model.mWorldspace, form(0x0da726));
        EXPECT_EQ(model.mClimate, form(0x08809b));
        EXPECT_EQ(model.mCurrent.mWeather, form(0x1237d7));
        EXPECT_EQ(model.mDefault.mWeather, form(0x0ffc88));
        EXPECT_EQ(model.mCurrent.mFogDistance, fixture.mCurrent.mFogDistance);
        EXPECT_EQ(model.mCurrent.mData.windSpeed, 50);
        EXPECT_EQ(model.mCurrent.mData.sunGlare, 54);
        EXPECT_EQ(model.mTimeBlend.mPrimary, ESM4::Weather::Time_Day);
        EXPECT_EQ(model.mTimeBlend.mSecondary, ESM4::Weather::Time_HighNoon);
        EXPECT_NEAR(model.mTimeBlend.mPrimaryStrength, 0.3691670f, 1e-7f);
        expectColor(model.mCurrentColors[ESM4::Weather::Color_SkyUpper], 0.25050324f, 0.32790846f,
            0.58878428f);
        expectColor(model.mCurrentColors[ESM4::Weather::Color_Fog], 0.5882353f, 0.65882355f, 0.74509805f);
        expectColor(model.mCurrentColors[ESM4::Weather::Color_Ambient], 0.37086272f, 0.44887254f, 0.58075815f);
        expectColor(model.mCurrentColors[ESM4::Weather::Color_Sunlight], 1.f, 0.8901961f, 0.6666667f);
        expectColor(model.mCurrentColors[ESM4::Weather::Color_Sun], 0.99855226f, 0.9797320f, 0.9363006f);

        // Preflight retains the complete native record; it cannot flatten four FNV layers into a TES3 slot.
        EXPECT_EQ(model.mCurrent.mMaxCloudLayers, fixture.mCurrent.mMaxCloudLayers);
        EXPECT_EQ(model.mCurrent.mCloudTextures, fixture.mCurrent.mCloudTextures);
        EXPECT_EQ(model.mCurrent.mCloudSpeeds, fixture.mCurrent.mCloudSpeeds);
        EXPECT_EQ(model.mCurrent.mFogDistance, fixture.mCurrent.mFogDistance);
    }
}

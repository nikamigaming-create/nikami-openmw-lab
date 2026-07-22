#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm4/fonvsavegame.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadclmt.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadwrld.hpp>

#include "apps/openmw/mwworld/fnvsavepreflight.hpp"
#include "apps/openmw/mwworld/store.hpp"

namespace
{
    using testing::Contains;
    using testing::HasSubstr;

    constexpr ESM::FormId form(std::uint32_t index, std::int32_t contentFile = 1)
    {
        return ESM::FormId{ index, contentFile };
    }

    ESM4::Weather makeWeather(ESM::FormId id)
    {
        ESM4::Weather result;
        result.mId = id;
        result.mHasMaxCloudLayers = true;
        result.mMaxCloudLayers = ESM4::Weather::sCloudLayerCount;
        result.mHasCloudSpeeds = true;
        result.mCloudSpeeds = { 52, 0, 0, 65 };
        result.mHasCloudColors = true;
        result.mCloudColorSampleCount = ESM4::Weather::sTimeCount;
        result.mHasColors = true;
        result.mColorSampleCount = ESM4::Weather::sTimeCount;
        result.mHasFogDistance = true;
        result.mFogDistance = { 10.f, 120000.f, 0.f, 150000.f, 0.5f, 0.5f };
        result.mHasData = true;
        result.mData.windSpeed = 50;
        result.mData.sunGlare = 54;
        return result;
    }

    ESM4::FONVSaveGamePrefix makeSave()
    {
        ESM4::FONVSaveGamePrefix result;
        result.mFileSize = 4096;
        result.mHeader.mSaveNumber.mValue = 330;
        result.mHeader.mPlayerLevel.mValue = 12;
        result.mHeader.mPlayerLocation.mValue = "Goodsprings";
        ESM4::FONVSaveMaster master;
        master.mFileName.mValue = "FalloutNV.esm";
        result.mMasters.push_back(std::move(master));

        ESM4::FONVSaveChangedFormEnvelope playerChange;
        playerChange.mResolvedFormId = ESM4::sFONVPlayerReferenceFormId;
        playerChange.mChangeType = ESM4::sFONVActorReferenceChangeType;
        playerChange.mChangeFlags.mValue = 0xb0000022u;
        playerChange.mUnparsedPayload.mRange = { 1024, 5095 };
        result.mChangedForms.mEntries.push_back(std::move(playerChange));

        ESM4::FONVSavePlayerReferenceMovement movement;
        movement.mCellOrWorldspace.mResolvedFormId = 0x000da726u;
        movement.mPosition[0].mValue = -72392.84375f;
        movement.mPosition[1].mValue = -1240.19275f;
        movement.mPosition[2].mValue = 8137.58643f;
        movement.mRotationRadians[2].mValue = 2.93332028f;
        result.mPlayerReferenceMovement = std::move(movement);

        ESM4::FONVSavePlayerProcessInventoryData processInventory;
        processInventory.mProcessLevel.mValue = 0;
        result.mPlayerProcessInventoryData = std::move(processInventory);

        ESM4::FONVSavePlayerMobileObjectProcessState processState;
        processState.mMiddleHighProcess.mWeaponOut.mValue = 1;
        result.mPlayerMobileObjectProcessState = std::move(processState);

        ESM4::FONVSavePlayerCharacterScalarReferenceState camera;
        camera.mFirstPersonMode.mValue = 0;
        camera.mFirstPersonModelFov.mValue = 55.f;
        camera.mWorldFov.mValue = 75.f;
        result.mPlayerCharacterScalarReferenceState = std::move(camera);

        ESM4::FONVSaveSkyState sky;
        sky.mCurrentWeather.mResolvedFormId = 0x001237d7u;
        sky.mDefaultWeather.mResolvedFormId = 0x000ffc88u;
        sky.mGameHour.mValue = 14.215002059936523f;
        sky.mLastUpdateHour.mValue = 17.606250762939453f;
        sky.mWeatherPercent.mValue = 1.f;
        sky.mFogPower.mValue = 0.5f;
        sky.mFlags.mValue = 0x20u;
        sky.mSkyMode.mValue = 3u;
        result.mSky = std::move(sky);
        return result;
    }

    struct PreflightFixture
    {
        std::vector<std::string> mContentFiles{ "builtin.omwscripts", "FalloutNV.esm" };
        MWWorld::FalloutPlayerState mPlayer;
        ESM4::Npc mBaseNpc;
        ESM4::Class mClass;
        ESM4::Race mRace;
        ESM4::World mWorld;
        ESM4::Cell mCell;
        ESM4::Climate mClimate;
        ESM4::Weather mCurrent = makeWeather(form(0x1237d7));
        ESM4::Weather mDefault = makeWeather(form(0x0ffc88));

        PreflightFixture()
        {
            mPlayer.mBaseRecord = form(7);
            mPlayer.mReferenceRecord = form(0x14);
            mPlayer.mReferenceBaseRecord = mPlayer.mBaseRecord;
            mPlayer.mClass = form(0x57e6a);
            mPlayer.mRace = form(0x19);
            mPlayer.mEditorId = "Player";
            mBaseNpc.mId = mPlayer.mBaseRecord;
            mClass.mId = mPlayer.mClass;
            mRace.mId = mPlayer.mRace;

            mWorld.mId = form(0x0da726);
            mWorld.mClimate = form(0x08809b);
            mWorld.mWorldFlags = 0;
            mCell.mId = ESM::RefId(form(0x0e1aa7));
            mCell.mParent = ESM::RefId(mWorld.mId);
            mCell.mCellFlags = ESM4::CELL_HasWater;
            mCell.mX = -18;
            mCell.mY = -1;

            mClimate.mId = mWorld.mClimate;
            mClimate.mHasTiming = true;
            mClimate.mTiming.mSunriseBegin = 36;
            mClimate.mTiming.mSunriseEnd = 48;
            mClimate.mTiming.mSunsetBegin = 108;
            mClimate.mTiming.mSunsetEnd = 120;
            mClimate.mWeatherTypes.push_back({ mDefault.mId, 100, {} });
        }

        MWWorld::FalloutSavePreflightResolution resolve(bool includePlayer = true, bool includeNative = true,
            bool includeCell = true, bool includeCurrentWeather = true,
            const std::vector<std::string>* contentFiles = nullptr) const
        {
            MWWorld::Store<ESM4::World> worlds;
            worlds.insertStatic(mWorld);
            MWWorld::Store<ESM4::Cell> cells;
            if (includeCell)
                cells.insertStatic(mCell);
            MWWorld::Store<ESM4::Climate> climates;
            climates.insertStatic(mClimate);
            MWWorld::Store<ESM4::Weather> weather;
            if (includeCurrentWeather)
                weather.insertStatic(mCurrent);
            weather.insertStatic(mDefault);
            MWWorld::Store<ESM4::FormIdList> formLists;
            MWWorld::Store<ESM4::Ammunition> ammunition;

            MWWorld::FalloutNativePlayerRecordsResolution native;
            if (includeNative)
                native.mRecords = MWWorld::FalloutNativePlayerRecords{ &mBaseNpc, &mClass, &mRace };
            else
                native.mError = "validated native records were not published";

            return MWWorld::resolveFalloutSavePreflightContext(makeSave(), includePlayer ? &mPlayer : nullptr,
                native, formLists, ammunition, worlds, cells, climates, weather,
                contentFiles == nullptr ? mContentFiles : *contentFiles);
        }
    };

    TEST(FalloutSavePreflight, RecognizesOnlyCaseInsensitiveFosExtension)
    {
        EXPECT_TRUE(MWWorld::isFalloutNewVegasSavePath("Save330.fos"));
        EXPECT_TRUE(MWWorld::isFalloutNewVegasSavePath("Save330.FOS"));
        EXPECT_FALSE(MWWorld::isFalloutNewVegasSavePath("Save330.omwsave"));
        EXPECT_FALSE(MWWorld::isFalloutNewVegasSavePath("Save330.fos.tmp"));
    }

    TEST(FalloutSavePreflight, ResolvesExactContextThroughBlockerEvaluationWithoutMutation)
    {
        PreflightFixture fixture;
        const MWWorld::FalloutSavePreflightResolution resolution = fixture.resolve();
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutSavePreflightContext& context = *resolution.mContext;
        EXPECT_EQ(context.mSave.mHeader.mSaveNumber.mValue, 330u);
        EXPECT_EQ(context.mPlayer.mBaseRecord, form(7));
        EXPECT_EQ(context.mNativePlayer.mBaseNpc, form(7));
        EXPECT_EQ(context.mNativePlayer.mReference, form(0x14));
        EXPECT_EQ(context.mNativePlayer.mClass, form(0x57e6a));
        EXPECT_EQ(context.mNativePlayer.mRace, form(0x19));
        EXPECT_EQ(context.mPlacement.mWorldspaceRecord, form(0x0da726));
        EXPECT_EQ(context.mPlacement.mCellRecord, form(0x0e1aa7));
        EXPECT_EQ(context.mWeather.mCurrent.mWeather, form(0x1237d7));
        EXPECT_EQ(context.mWeather.mDefault.mWeather, form(0x0ffc88));
        EXPECT_NO_THROW(MWWorld::requireFalloutSaveVisualApplicationReady(context));
        MWWorld::Store<ESM4::FormIdList> formLists;
        MWWorld::Store<ESM4::Ammunition> ammunition;
        const MWWorld::FalloutSaveLoadPlanResolution expectedPlan = MWWorld::resolveFalloutSaveLoadPlan(
            makeSave(), &fixture.mPlayer, formLists, ammunition, fixture.mContentFiles);
        ASSERT_TRUE(expectedPlan) << expectedPlan.mError;
        EXPECT_EQ(context.mPlan.mUncoveredState, expectedPlan.mPlan->mUncoveredState);
        EXPECT_THAT(context.mPlan.mUncoveredState, Contains("global-variables"));
        EXPECT_THAT(context.mPlan.mUncoveredState, Contains("quest-stages-objectives-variables"));

        int mutationCount = 0;
        bool rejected = false;
        try
        {
            MWWorld::requireFalloutSavePreflightReady(context);
            ++mutationCount;
        }
        catch (const std::runtime_error& error)
        {
            rejected = true;
            EXPECT_THAT(error.what(), HasSubstr("global-variables"));
            EXPECT_THAT(error.what(), HasSubstr("quest-stages-objectives-variables"));
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(mutationCount, 0);
    }

    TEST(FalloutSavePreflight, RejectsMissingValidatedPlayerNativeViewAndContent)
    {
        PreflightFixture fixture;
        EXPECT_THAT(fixture.resolve(false).mError, HasSubstr("no fully validated native FNV Player state"));
        EXPECT_THAT(fixture.resolve(true, false).mError, HasSubstr("validated native records were not published"));

        const std::vector<std::string> missingContent{ "builtin.omwscripts" };
        EXPECT_THAT(fixture.resolve(true, true, true, true, &missingContent).mError,
            HasSubstr("current content has no FalloutNV.esm"));
    }

    TEST(FalloutSavePreflight, RejectsMissingExactPlacementAndWeatherBeforeBlockerGate)
    {
        PreflightFixture fixture;
        EXPECT_THAT(fixture.resolve(true, true, false).mError, HasSubstr("no authored exterior CELL"));
        EXPECT_THAT(fixture.resolve(true, true, true, false).mError, HasSubstr("current FNV WTHR"));
    }
}

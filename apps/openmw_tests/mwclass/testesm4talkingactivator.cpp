#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>

#include <components/esm3/readerscache.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadtact.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwclass/esm4talkingactivator.hpp"
#include "apps/openmw/mwworld/actionesm4radio.hpp"
#include "apps/openmw/mwworld/cellstore.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/manualref.hpp"
#include "apps/openmw/mwworld/nullaction.hpp"
#include "apps/openmw/mwworld/worldmodel.hpp"

namespace
{
    constexpr std::uint32_t sCellId = 0x01010000;
    constexpr std::uint32_t sTactBaseId = 0x01010001;
    constexpr std::uint32_t sTactRefId = 0x01010002;
    constexpr std::uint32_t sLoopSoundId = 0x01010003;
    constexpr std::uint32_t sTemplateSoundId = 0x01010004;
    constexpr std::uint32_t sPositionRefId = 0x01010005;
    constexpr std::uint32_t sActiBaseId = 0x01010006;
    constexpr std::uint32_t sActiRefId = 0x01010007;

    ESM4::Cell makeCell()
    {
        ESM4::Cell cell{};
        cell.mId = ESM::RefId(ESM::FormId::fromUint32(sCellId));
        cell.mEditorId = "FnvTalkingActivatorTestCell";
        cell.mFullName = "FNV Talking Activator Test Cell";
        cell.mCellFlags = ESM4::CELL_Interior;
        return cell;
    }

    ESM4::TalkingActivator makeTalkingActivator()
    {
        ESM4::TalkingActivator result{};
        result.mId = ESM::FormId::fromUint32(sTactBaseId);
        result.mFlags = ESM4::TACT_RadioStation;
        result.mEditorId = "RadioNewVegas";
        result.mFullName = "Radio New Vegas";
        result.mModel = "clutter/radio/radiotable01.nif";
        result.mLoopSound = ESM::FormId::fromUint32(sLoopSoundId);
        result.mRadioTemplate = ESM::FormId::fromUint32(sTemplateSoundId);
        return result;
    }

    ESM4::Reference makeReference(std::uint32_t id, std::uint32_t base)
    {
        ESM4::Reference result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mParent = ESM::RefId(ESM::FormId::fromUint32(sCellId));
        result.mBaseObj = ESM::FormId::fromUint32(base);
        result.mCount = 1;
        result.mScale = 1.f;
        return result;
    }

    class ESM4TalkingActivatorTest : public ::testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::ESMStore mStore;
        ESM::ReadersCache mReaders;
        MWWorld::WorldModel mWorldModel{ mStore, mReaders };

        void SetUp() override
        {
            mEnvironment.setESMStore(mStore);
            mEnvironment.setWorldModel(mWorldModel);
            MWClass::registerClasses();
        }

        MWWorld::CellStore& finishAndLoadCell()
        {
            mStore.setUp();
            MWWorld::CellStore* cell
                = mWorldModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCellId)), false);
            EXPECT_NE(cell, nullptr);
            cell->load();
            return *cell;
        }

        static MWWorld::Ptr findTalkingActivator(MWWorld::CellStore& cell, std::uint32_t id = sTactRefId)
        {
            MWWorld::Ptr result;
            cell.forEachType<ESM4::TalkingActivator>([&](const MWWorld::Ptr& ptr) {
                if (ptr.getCellRef().getRefNum() == ESM::FormId::fromUint32(id))
                {
                    result = ptr;
                    return false;
                }
                return true;
            });
            return result;
        }

        void insertReference(const ESM4::Reference& reference)
        {
            const_cast<MWWorld::Store<ESM4::Reference>&>(mStore.get<ESM4::Reference>())
                .insertStatic(reference);
        }
    };

    TEST_F(ESM4TalkingActivatorTest, MaterializesAuthoredCellReferenceWithClassActionAndXrdo)
    {
        ESM4::TalkingActivator station = makeTalkingActivator();
        ESM4::Reference placement = makeReference(sTactRefId, sTactBaseId);
        placement.mRadio.rangeRadius = 4096.f;
        placement.mRadio.broadcastRange = 2;
        placement.mRadio.staticPercentage = 0.25f;
        placement.mRadio.posReference = ESM::FormId::fromUint32(sPositionRefId);

        mStore.overrideRecord(makeCell());
        mStore.overrideRecord(station);
        insertReference(placement);
        MWWorld::CellStore& cell = finishAndLoadCell();

        MWWorld::Ptr ptr = findTalkingActivator(cell);
        ASSERT_FALSE(ptr.isEmpty());
        EXPECT_EQ(ptr.getType(), ESM::REC_TACT4);
        EXPECT_NE(dynamic_cast<const MWClass::ESM4TalkingActivator*>(&ptr.getClass()), nullptr);
        EXPECT_EQ(ptr.getClass().getName(ptr), "Radio New Vegas");
        EXPECT_EQ(ptr.getClass().getModel(ptr), "clutter/radio/radiotable01.nif");
        EXPECT_TRUE(ptr.getClass().isActivator());

        const ESM4::RadioStationData* radio = ptr.getCellRef().getEsm4RadioStationData();
        ASSERT_NE(radio, nullptr);
        EXPECT_FLOAT_EQ(radio->rangeRadius, 4096.f);
        EXPECT_EQ(radio->broadcastRange, 2u);
        EXPECT_FLOAT_EQ(radio->staticPercentage, 0.25f);
        EXPECT_EQ(radio->posReference, ESM::FormId::fromUint32(sPositionRefId));

        std::unique_ptr<MWWorld::Action> action = ptr.getClass().activate(ptr, {});
        EXPECT_NE(dynamic_cast<MWWorld::ActionEsm4Radio*>(action.get()), nullptr);

        MWWorld::ManualRef manual(mStore, ESM::RefId(ESM::FormId::fromUint32(sTactBaseId)));
        EXPECT_EQ(manual.getPtr().getType(), ESM::REC_TACT4);
        EXPECT_EQ(manual.getPtr().getClass().getName(manual.getPtr()), "Radio New Vegas");
    }

    TEST_F(ESM4TalkingActivatorTest, UsesSnamThenInamOnlyForAuthoredRadioStations)
    {
        ESM4::TalkingActivator station = makeTalkingActivator();
        EXPECT_EQ(MWClass::selectFnvTalkingActivatorSound(station),
            ESM::FormId::fromUint32(sLoopSoundId));

        station.mLoopSound = {};
        EXPECT_EQ(MWClass::selectFnvTalkingActivatorSound(station),
            ESM::FormId::fromUint32(sTemplateSoundId));

        station.mFlags = 0;
        EXPECT_TRUE(MWClass::selectFnvTalkingActivatorSound(station).isZeroOrUnset());

        ESM4::Reference placement = makeReference(sTactRefId, sTactBaseId);
        MWWorld::LiveCellRef<ESM4::TalkingActivator> liveRef(placement, &station);
        MWWorld::Ptr ptr(&liveRef);
        EXPECT_NE(dynamic_cast<MWWorld::NullAction*>(ptr.getClass().activate(ptr, {}).get()), nullptr);
    }

    TEST_F(ESM4TalkingActivatorTest, ReferenceWithoutXrdoHasDeterministicZeroRadioData)
    {
        ESM4::RadioStationData radio;
        EXPECT_FLOAT_EQ(radio.rangeRadius, 0.f);
        EXPECT_EQ(radio.broadcastRange, 0u);
        EXPECT_FLOAT_EQ(radio.staticPercentage, 0.f);
        EXPECT_TRUE(radio.posReference.isZeroOrUnset());

        mStore.overrideRecord(makeCell());
        mStore.overrideRecord(makeTalkingActivator());
        insertReference(makeReference(sTactRefId, sTactBaseId));
        MWWorld::Ptr ptr = findTalkingActivator(finishAndLoadCell());
        ASSERT_FALSE(ptr.isEmpty());
        const ESM4::RadioStationData* retained = ptr.getCellRef().getEsm4RadioStationData();
        ASSERT_NE(retained, nullptr);
        EXPECT_FLOAT_EQ(retained->rangeRadius, 0.f);
        EXPECT_EQ(retained->broadcastRange, 0u);
        EXPECT_FLOAT_EQ(retained->staticPercentage, 0.f);
        EXPECT_TRUE(retained->posReference.isZeroOrUnset());
    }

    TEST_F(ESM4TalkingActivatorTest, DropsMissingAndMalformedTactRefsWithoutRegressingActi)
    {
        constexpr std::uint32_t missingRefId = 0x01010008;
        constexpr std::uint32_t missingBaseId = 0x01010009;
        constexpr std::uint32_t malformedRefId = 0x0101000a;

        ESM4::TalkingActivator station = makeTalkingActivator();
        ESM4::Activator activator{};
        activator.mId = ESM::FormId::fromUint32(sActiBaseId);
        activator.mEditorId = "FnvActiRegression";
        activator.mFullName = "Working Activator";
        activator.mModel = "clutter/switch/switch01.nif";
        activator.mLoopingSound = ESM::FormId::fromUint32(sLoopSoundId);

        ESM4::Reference validTact = makeReference(sTactRefId, sTactBaseId);
        ESM4::Reference missing = makeReference(missingRefId, missingBaseId);
        ESM4::Reference malformed = makeReference(malformedRefId, sTactBaseId);
        malformed.mRadio.staticPercentage = std::numeric_limits<float>::quiet_NaN();
        ESM4::Reference validActi = makeReference(sActiRefId, sActiBaseId);

        mStore.overrideRecord(makeCell());
        mStore.overrideRecord(station);
        mStore.overrideRecord(activator);
        insertReference(validTact);
        insertReference(missing);
        insertReference(malformed);
        insertReference(validActi);
        MWWorld::CellStore& cell = finishAndLoadCell();

        EXPECT_FALSE(findTalkingActivator(cell, sTactRefId).isEmpty());
        EXPECT_TRUE(findTalkingActivator(cell, malformedRefId).isEmpty());

        std::size_t tactCount = 0;
        cell.forEachType<ESM4::TalkingActivator>([&](const MWWorld::Ptr&) {
            ++tactCount;
            return true;
        });
        EXPECT_EQ(tactCount, 1u);

        MWWorld::Ptr acti;
        cell.forEachType<ESM4::Activator>([&](const MWWorld::Ptr& ptr) {
            acti = ptr;
            return false;
        });
        ASSERT_FALSE(acti.isEmpty());
        EXPECT_EQ(acti.getCellRef().getRefNum(), ESM::FormId::fromUint32(sActiRefId));
        EXPECT_EQ(acti.getClass().getName(acti), "Working Activator");
        EXPECT_NE(dynamic_cast<MWWorld::ActionEsm4Radio*>(acti.getClass().activate(acti, {}).get()), nullptr);
    }
}

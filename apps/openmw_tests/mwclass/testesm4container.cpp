#include <gtest/gtest.h>

#include <components/esm/format.hpp>
#include <components/esm3/containerstate.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadstat.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwbase/luamanager.hpp"

#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwclass/esm4container.hpp"
#include "apps/openmw/mwclass/esm4creature.hpp"

#include "apps/openmw/mwworld/actionopen.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/failedaction.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"
#include "apps/openmw/mwworld/worldmodel.hpp"

#include <limits>
#include <sstream>

namespace
{
    constexpr std::uint32_t sSaloonContainerRef = 0x0110873e;
    constexpr std::uint32_t sSaloonContainerBase = 0x01103b17;
    constexpr std::uint32_t sSaloonBottleBase = 0x01103b1e;
    constexpr std::uint32_t sSaloonKeyBase = 0x01103b1f;
    constexpr std::uint32_t sKeyHolderBase = 0x01103b20;
    constexpr std::uint32_t sKeyHolderRef = 0x0110873f;
    constexpr std::uint32_t sCreatureBase = 0x01103b21;
    constexpr std::uint32_t sCreatureRef = 0x01108740;
    constexpr std::uint32_t sCreatureTemplateBase = 0x01103b22;
    constexpr std::uint32_t sMissingItemBase = 0x01103b23;
    constexpr std::uint32_t sLevelledItemBase = 0x01103b24;
    constexpr std::uint32_t sUnsupportedStaticBase = 0x01103b25;

    class TestLuaManager final : public MWBase::LuaManager
    {
    public:
        void newGameStarted() override {}
        void gameLoaded() override {}
        void gameEnded() override {}
        void noGame() override {}
        void objectAddedToScene(const MWWorld::Ptr&) override {}
        void objectRemovedFromScene(const MWWorld::Ptr&) override {}
        void objectTeleported(const MWWorld::Ptr&) override {}
        void itemConsumed(const MWWorld::Ptr&, const MWWorld::Ptr&) override {}
        void objectActivated(const MWWorld::Ptr&, const MWWorld::Ptr&) override {}
        void useItem(const MWWorld::Ptr&, const MWWorld::Ptr&, bool) override {}
        void animationTextKey(const MWWorld::Ptr&, const std::string&) override {}
        void playAnimation(const MWWorld::Ptr&, const std::string&, const MWRender::AnimPriority&, int, bool, float,
            std::string_view, std::string_view, float, std::uint32_t, bool) override
        {
        }
        void jailTimeServed(const MWWorld::Ptr&, int) override {}
        void skillLevelUp(const MWWorld::Ptr&, ESM::RefId, std::string_view) override {}
        void skillUse(const MWWorld::Ptr&, ESM::RefId, int, float) override {}
        void onHit(const MWWorld::Ptr&, const MWWorld::Ptr&, const MWWorld::Ptr&, const MWWorld::Ptr&, int, float,
            float, bool, const osg::Vec3f&, bool, MWMechanics::DamageSourceType) override
        {
        }
        void exteriorCreated(MWWorld::CellStore&) override {}
        void actorDied(const MWWorld::Ptr&) override {}
        void questUpdated(const ESM::RefId&, int) override {}
        void uiModeChanged(const MWWorld::Ptr&) override {}
        void savePermanentStorage(const std::filesystem::path&) override {}
        void vrRecentered(bool, bool) override {}
        void inputEvent(const InputEvent&) override {}
        ActorControls* getActorControls(const MWWorld::Ptr&) const override { return nullptr; }
        void clear() override {}
        void setupPlayer(const MWWorld::Ptr&) override {}
        void write(ESM::ESMWriter&, Loading::Listener&) override {}
        void saveLocalScripts(const MWWorld::Ptr&, ESM::LuaScripts&) override {}
        void applyDelayedActions() override {}
        void readRecord(ESM::ESMReader&, std::uint32_t) override {}
        void loadLocalScripts(const MWWorld::Ptr&, const ESM::LuaScripts&) override {}
        void setContentFileMapping(const std::map<int, int>&) override {}
        void reloadAllScripts() override {}
        void handleConsoleCommand(const std::string&, const std::string&, const MWWorld::Ptr&) override {}
        std::string formatResourceUsageStats() const override { return {}; }
    };

    class ESM4ContainerTest : public ::testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::ESMStore mStore;
        ESM::ReadersCache mReaders;
        MWWorld::WorldModel mWorldModel{ mStore, mReaders };
        TestLuaManager mLuaManager;

        void SetUp() override
        {
            mEnvironment.setESMStore(mStore);
            mEnvironment.setWorldModel(mWorldModel);
            mEnvironment.setLuaManager(mLuaManager);
            mStore.setUp();
            MWClass::registerClasses();

            ESM4::MiscItem bottle{};
            bottle.mId = ESM::FormId::fromUint32(sSaloonBottleBase);
            bottle.mEditorId = "GSProspectorSaloonBottle";
            bottle.mFullName = "Bottle";
            mStore.overrideRecord(bottle);

            ESM4::Key key{};
            key.mId = ESM::FormId::fromUint32(sSaloonKeyBase);
            key.mEditorId = "GSProspectorSaloonKey";
            key.mFullName = "Prospector Saloon Key";
            mStore.overrideRecord(key);
        }

        static ESM4::Container makeSaloonContainer()
        {
            ESM4::Container container{};
            container.mId = ESM::FormId::fromUint32(sSaloonContainerBase);
            container.mEditorId = "GSProspectorSaloonContainer";
            container.mFullName = "Prospector Saloon Container";
            container.mWeight = 100.f;
            container.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 6 });
            return container;
        }

        static ESM4::Reference makeSaloonReference()
        {
            ESM4::Reference reference{};
            reference.mId = ESM::FormId::fromUint32(sSaloonContainerRef);
            reference.mParent = ESM::RefId(ESM::FormId::fromUint32(0x01104c10));
            reference.mBaseObj = ESM::FormId::fromUint32(sSaloonContainerBase);
            reference.mDoor.destDoor = ESM::FormId::fromUint32(0x01123456);
            reference.mCount = 1;
            return reference;
        }

        static ESM4::Container makeKeyHolder()
        {
            ESM4::Container container{};
            container.mId = ESM::FormId::fromUint32(sKeyHolderBase);
            container.mEditorId = "TestKeyHolder";
            container.mFullName = "Test Key Holder";
            container.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 1 });
            return container;
        }

        static ESM4::Reference makeKeyHolderReference()
        {
            ESM4::Reference reference{};
            reference.mId = ESM::FormId::fromUint32(sKeyHolderRef);
            reference.mBaseObj = ESM::FormId::fromUint32(sKeyHolderBase);
            reference.mCount = 1;
            return reference;
        }

        static ESM4::Creature makeCreature(std::uint32_t id = sCreatureBase)
        {
            ESM4::Creature creature{};
            creature.mId = ESM::FormId::fromUint32(id);
            creature.mEditorId = "GoodspringsCreatureInventoryTest";
            creature.mFullName = "Goodsprings Creature";
            creature.mIsFONV = true;
            // Keep this inventory fixture independent from the full MWBase::World actor-stat service. A zeroed,
            // present FNV DATA payload does not trigger CreatureStats' intelligence-derived magicka recalculation.
            creature.mHasFNVData = true;
            creature.mFNVData.health = 100;
            return creature;
        }

        static ESM4::Reference makeCreatureReference(
            std::uint32_t base = sCreatureBase, std::uint32_t ref = sCreatureRef)
        {
            ESM4::Reference reference{};
            reference.mId = ESM::FormId::fromUint32(ref);
            reference.mBaseObj = ESM::FormId::fromUint32(base);
            reference.mCount = 1;
            return reference;
        }
    };

    TEST_F(ESM4ContainerTest, CreatureInitializesDirectFixedInventoryOnceAndMergesDuplicateStacks)
    {
        ESM4::Creature creature = makeCreature();
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 2 });
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 3 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        const ESM::RefId keyId(ESM::FormId::fromUint32(sSaloonKeyBase));

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(bottleId), 5);
        EXPECT_EQ(store.count(keyId), 1);

        std::size_t bottleStacks = 0;
        for (const MWWorld::ConstPtr item : store)
        {
            if (item.getCellRef().getRefId() == bottleId)
            {
                ++bottleStacks;
                EXPECT_EQ(item.getCellRef().getCount(), 5);
            }
        }
        EXPECT_EQ(bottleStacks, 1u);

        ASSERT_EQ(store.remove(bottleId, 2, false, false), 2);
        EXPECT_EQ(ptr.getClass().getContainerStore(ptr).count(bottleId), 3);
    }

    TEST_F(ESM4ContainerTest, CreatureUsesResolvedInventoryTemplateCategoryInsteadOfDelegatingBase)
    {
        ESM4::Creature inventoryTemplate = makeCreature(sCreatureTemplateBase);
        inventoryTemplate.mEditorId = "GoodspringsCreatureInventoryTemplate";
        inventoryTemplate.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 7 });
        mStore.overrideRecord(inventoryTemplate);

        ESM4::Creature creature = makeCreature();
        creature.mBaseTemplate = ESM::FormId::fromUint32(sCreatureTemplateBase);
        creature.mBaseConfig.fo3.templateFlags |= ESM4::Creature::Template_UseInventory;
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 9 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 7);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase))), 0);
    }

    TEST_F(ESM4ContainerTest, CreatureRejectsInvalidMissingUnsupportedAndOverflowingFixedItems)
    {
        ESM4::LevelledItem levelled{};
        levelled.mId = ESM::FormId::fromUint32(sLevelledItemBase);
        levelled.mEditorId = "GoodspringsCreatureRandomLoot";
        mStore.overrideRecord(levelled);

        ESM4::Static unsupported{};
        unsupported.mId = ESM::FormId::fromUint32(sUnsupportedStaticBase);
        unsupported.mEditorId = "GoodspringsCreatureUnsupportedLoot";
        mStore.overrideRecord(unsupported);

        ESM4::Creature creature = makeCreature();
        creature.mInventory.push_back(ESM4::InventoryItem{ 0, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 0 });
        creature.mInventory.push_back(
            ESM4::InventoryItem{ sSaloonKeyBase, std::numeric_limits<std::uint32_t>::max() });
        creature.mInventory.push_back(ESM4::InventoryItem{ sMissingItemBase, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ sLevelledItemBase, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ sUnsupportedStaticBase, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{
            sSaloonBottleBase, static_cast<std::uint32_t>(std::numeric_limits<int>::max()) });
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 1 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))),
            std::numeric_limits<int>::max());
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase))), 0);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sMissingItemBase))), 0);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sLevelledItemBase))), 0);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sUnsupportedStaticBase))), 0);

        std::size_t stacks = 0;
        for (const MWWorld::ConstPtr item : store)
        {
            ++stacks;
            EXPECT_EQ(item.getCellRef().getRefId(),
                ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase)));
        }
        EXPECT_EQ(stacks, 1u);
    }

    TEST_F(ESM4ContainerTest, ClonedCreatureInventoryGetsFreshRefsAndMutationIsolation)
    {
        ESM4::Creature creature = makeCreature();
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 6 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> sourceRef(reference, &creature);
        MWWorld::Ptr source(&sourceRef);
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));

        MWWorld::ContainerStore& sourceStore = source.getClass().getContainerStore(source);
        ASSERT_EQ(sourceStore.count(bottleId), 6);
        ASSERT_NE(sourceStore.begin(), sourceStore.end());
        const ESM::RefNum sourceItemRefNum = sourceStore.begin()->getCellRef().getRefNum();

        MWWorld::LiveCellRef<ESM4::Creature> clonedRef(sourceRef);
        MWWorld::Ptr cloned(&clonedRef);
        MWWorld::ContainerStore& clonedStore = cloned.getClass().getContainerStore(cloned);
        ASSERT_EQ(clonedStore.count(bottleId), 6);
        ASSERT_NE(clonedStore.begin(), clonedStore.end());
        EXPECT_NE(clonedStore.begin()->getCellRef().getRefNum(), sourceItemRefNum);

        ASSERT_EQ(clonedStore.remove(bottleId, 2, false, false), 2);
        EXPECT_EQ(clonedStore.count(bottleId), 4);
        EXPECT_EQ(sourceStore.count(bottleId), 6);
    }

    TEST_F(ESM4ContainerTest, InitializesAuthoredFixedContentsOnlyOnce)
    {
        ESM4::Container container = makeSaloonContainer();
        ESM4::Reference reference = makeSaloonReference();
        MWWorld::LiveCellRef<ESM4::Container> liveRef(reference, &container);
        MWWorld::Ptr ptr(&liveRef);
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));

        MWWorld::ContainerStore& first = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(first.count(bottleId), 6);
        ASSERT_EQ(first.remove(bottleId, 2, false, false), 2);
        EXPECT_EQ(first.count(bottleId), 4);

        MWWorld::ContainerStore& second = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(&second, &first);
        EXPECT_EQ(second.count(bottleId), 4);

        std::size_t stacks = 0;
        for (const MWWorld::ConstPtr item : second)
        {
            ++stacks;
            EXPECT_EQ(item.getCellRef().getRefId(), bottleId);
            EXPECT_EQ(item.getCellRef().getCount(), 4);
        }
        EXPECT_EQ(stacks, 1u);
    }

    TEST_F(ESM4ContainerTest, ActivationUsesOpenActionAndHonorsLockedState)
    {
        ESM4::Container container = makeSaloonContainer();
        ESM4::Reference reference = makeSaloonReference();
        MWWorld::LiveCellRef<ESM4::Container> unlockedRef(reference, &container);
        MWWorld::Ptr unlocked(&unlockedRef);

        std::unique_ptr<MWWorld::Action> open = unlocked.getClass().activate(unlocked, {});
        EXPECT_NE(dynamic_cast<MWWorld::ActionOpen*>(open.get()), nullptr);

        reference.mIsLocked = true;
        reference.mLockLevel = 50;
        MWWorld::LiveCellRef<ESM4::Container> lockedRef(reference, &container);
        MWWorld::Ptr locked(&lockedRef);
        std::unique_ptr<MWWorld::Action> failed = locked.getClass().activate(locked, {});
        EXPECT_NE(dynamic_cast<MWWorld::FailedAction*>(failed.get()), nullptr);
    }

    TEST_F(ESM4ContainerTest, AuthoredKeyUnlocksContainerThroughActorInventory)
    {
        ESM4::Container container = makeSaloonContainer();
        ESM4::Reference reference = makeSaloonReference();
        reference.mIsLocked = true;
        reference.mLockLevel = 50;
        reference.mKey = ESM::FormId::fromUint32(sSaloonKeyBase);
        MWWorld::LiveCellRef<ESM4::Container> lockedRef(reference, &container);
        MWWorld::Ptr locked(&lockedRef);

        ESM4::Container keyHolder = makeKeyHolder();
        ESM4::Reference keyHolderReference = makeKeyHolderReference();
        MWWorld::LiveCellRef<ESM4::Container> keyHolderRef(keyHolderReference, &keyHolder);
        MWWorld::Ptr actor(&keyHolderRef);

        std::unique_ptr<MWWorld::Action> opened = locked.getClass().activate(locked, actor);
        EXPECT_NE(dynamic_cast<MWWorld::ActionOpen*>(opened.get()), nullptr);
        EXPECT_FALSE(locked.getCellRef().isLocked());
        EXPECT_EQ(locked.getCellRef().getLockLevel(), -50);

        ESM::ContainerState unlockedState;
        lockedRef.save(unlockedState);

        auto stream = std::make_unique<std::stringstream>();
        {
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            writer.startRecord(ESM::fourCC("TST0"));
            unlockedState.save(writer);
            writer.endRecord(ESM::fourCC("TST0"));
        }

        ESM::ContainerState diskState;
        {
            ESM::ESMReader reader;
            reader.open(std::move(stream), "esm4-unlocked-container-save");
            ASSERT_TRUE(reader.hasMoreRecs());
            ASSERT_EQ(reader.getRecName().toInt(), ESM::fourCC("TST0"));
            reader.getRecHeader();
            diskState.mRef.loadId(reader, true);
            diskState.load(reader);
        }
        EXPECT_TRUE(diskState.mRef.mKey.empty());

        ESM4::Reference restoredReference = reference;
        MWWorld::LiveCellRef<ESM4::Container> restoredRef(restoredReference, &container);
        restoredRef.load(diskState);
        EXPECT_FALSE(restoredRef.mRef.isLocked());
        EXPECT_EQ(restoredRef.mRef.getKey(), ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase)));
    }

    TEST_F(ESM4ContainerTest, ContainerStateRoundTripRetainsContentsAndEsm4Reference)
    {
        ESM4::Container container = makeSaloonContainer();
        ESM4::Reference reference = makeSaloonReference();
        MWWorld::LiveCellRef<ESM4::Container> sourceRef(reference, &container);
        MWWorld::Ptr source(&sourceRef);
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));

        MWWorld::ContainerStore& sourceStore = source.getClass().getContainerStore(source);
        ASSERT_EQ(sourceStore.remove(bottleId, 2, false, false), 2);
        source.getCellRef().lock(75);

        ESM::ContainerState state;
        sourceRef.save(state);
        ASSERT_TRUE(state.mHasCustomState);
        ASSERT_EQ(state.mInventory.mItems.size(), 1u);
        EXPECT_EQ(state.mInventory.mItems.front().mRef.mRefID, bottleId);
        EXPECT_EQ(state.mInventory.mItems.front().mRef.mCount, 4);

        auto stream = std::make_unique<std::stringstream>();
        {
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            writer.startRecord(ESM::fourCC("TST0"));
            state.save(writer);
            writer.endRecord(ESM::fourCC("TST0"));
        }

        ESM::ContainerState diskState;
        {
            ESM::ESMReader reader;
            reader.open(std::move(stream), "esm4-container-save");
            ASSERT_TRUE(reader.hasMoreRecs());
            ASSERT_EQ(reader.getRecName().toInt(), ESM::fourCC("TST0"));
            reader.getRecHeader();
            diskState.mRef.loadId(reader, true);
            diskState.load(reader);
        }

        ESM4::Reference restoredReference = reference;
        MWWorld::LiveCellRef<ESM4::Container> restoredRef(restoredReference, &container);
        restoredRef.load(diskState);
        MWWorld::Ptr restored(&restoredRef);

        EXPECT_TRUE(restored.getCellRef().getTeleport());
        EXPECT_EQ(restored.getCellRef().getRefNum(), ESM::FormId::fromUint32(sSaloonContainerRef));
        EXPECT_EQ(restored.getCellRef().getRefId(), ESM::RefId(ESM::FormId::fromUint32(sSaloonContainerBase)));
        EXPECT_TRUE(restored.getCellRef().isLocked());
        EXPECT_EQ(restored.getCellRef().getLockLevel(), 75);

        MWWorld::ContainerStore& restoredStore = restored.getClass().getContainerStore(restored);
        EXPECT_EQ(restoredStore.count(bottleId), 4);
        EXPECT_EQ(&restored.getClass().getContainerStore(restored), &restoredStore);
        EXPECT_EQ(restoredStore.count(bottleId), 4);
    }

    TEST_F(ESM4ContainerTest, ESM3CellRefStateStillReplacesAllMutableFields)
    {
        ESM::CellRef original = ESM::makeBlankCellRef();
        original.mRefID = ESM::RefId::stringRefId("old");
        original.mCount = 1;
        MWWorld::CellRef runtime(original);

        ESM::CellRef replacement = ESM::makeBlankCellRef();
        replacement.mRefID = ESM::RefId::stringRefId("new");
        replacement.mCount = 7;
        replacement.mScale = 2.f;
        replacement.mIsLocked = true;
        replacement.mLockLevel = 42;
        runtime.loadState(replacement);

        EXPECT_EQ(runtime.getRefId(), replacement.mRefID);
        EXPECT_EQ(runtime.getCount(), 7);
        EXPECT_FLOAT_EQ(runtime.getScale(), 2.f);
        EXPECT_TRUE(runtime.isLocked());
        EXPECT_EQ(runtime.getLockLevel(), 42);
    }
}

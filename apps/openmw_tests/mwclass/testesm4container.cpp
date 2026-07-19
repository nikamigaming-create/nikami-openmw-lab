#include <gtest/gtest.h>

#include <components/esm/format.hpp>
#include <components/esm3/aisequence.hpp>
#include <components/esm3/cellstate.hpp>
#include <components/esm3/containerstate.hpp>
#include <components/esm3/creaturestate.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadstat.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwbase/luamanager.hpp"

#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwclass/esm4base.hpp"
#include "apps/openmw/mwclass/esm4container.hpp"
#include "apps/openmw/mwclass/esm4creature.hpp"

#include "apps/openmw/mwgui/tradeitemmodel.hpp"

#include "apps/openmw/mwmechanics/aiwander.hpp"
#include "apps/openmw/mwmechanics/creaturestats.hpp"

#include "apps/openmw/mwworld/actionopen.hpp"
#include "apps/openmw/mwworld/actiondoor.hpp"
#include "apps/openmw/mwworld/actionteleport.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/failedaction.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"
#include "apps/openmw/mwworld/worldmodel.hpp"

#include <components/loadinglistener/loadinglistener.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    TEST(ESM4NamedActivationTest, OverridesGenericNullActivationForPickupRecords)
    {
        using PickupClass = MWClass::ESM4Named<ESM4::MiscItem>;
        using PickupActivation = std::unique_ptr<MWWorld::Action> (PickupClass::*)(
            const MWWorld::Ptr&, const MWWorld::Ptr&) const;

        EXPECT_TRUE((std::is_same_v<decltype(&PickupClass::activate), PickupActivation>));
    }

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
    constexpr std::uint32_t sCreatureCell = 0x01104c10;
    constexpr std::uint32_t sNonFnvCreatureBase = 0x01103b26;
    constexpr std::uint32_t sNonFnvCreatureRef = 0x01108741;
    constexpr std::uint32_t sUseAllLevelledItemBase = 0x01103b27;
    constexpr std::uint32_t sNestedLevelledItemBase = 0x01103b28;
    constexpr std::uint32_t sStatsTemplateBase = 0x01103b29;
    constexpr std::uint32_t sNpcBase = 0x01103b2a;
    constexpr std::uint32_t sNpcRef = 0x01108742;
    constexpr std::uint32_t sNonFnvNpcBase = 0x01103b2b;
    constexpr std::uint32_t sNonFnvNpcRef = 0x01108743;
    constexpr std::uint32_t sNpcRace = 0x01103b2c;
    constexpr std::uint32_t sDoorBase = 0x01103b2d;
    constexpr std::uint32_t sDoorRef = 0x01108744;
    constexpr std::uint32_t sDoorDestRef = 0x01108745;
    constexpr std::uint32_t sDoorDestCell = 0x01104c11;
    constexpr std::uint32_t sCapsBase = 0x0100000f;
    constexpr std::uint32_t sBarterPocketBase = 0x01103b30;
    constexpr std::uint32_t sBarterPocketRef = 0x01108746;
    constexpr std::uint32_t sBarterChestBase = 0x01103b31;
    constexpr std::uint32_t sBarterChestRef = 0x01108747;
    constexpr std::uint32_t sBarterPlayerBase = 0x01103b32;
    constexpr std::uint32_t sBarterPlayerRef = 0x01108748;

    class BarterTestItemModel final : public MWGui::ItemModel
    {
    public:
        explicit BarterTestItemModel(std::vector<MWWorld::Ptr> sources)
            : mSources(std::move(sources))
        {
        }

        MWGui::ItemStack getItem(ModelIndex index) override { return mItems.at(static_cast<std::size_t>(index)); }
        std::size_t getItemCount() override { return mItems.size(); }

        ModelIndex getIndex(const MWGui::ItemStack& item) override
        {
            for (std::size_t index = 0; index < mItems.size(); ++index)
            {
                if (mItems[index].mBase == item.mBase)
                    return static_cast<ModelIndex>(index);
            }
            return -1;
        }

        void update() override
        {
            mItems.clear();
            for (const MWWorld::Ptr& source : mSources)
            {
                MWWorld::ContainerStore& store = source.getClass().getContainerStore(source);
                for (MWWorld::Ptr item : store)
                {
                    if (!item.getClass().showsInInventory(item))
                        continue;

                    const auto existing = std::find_if(mItems.begin(), mItems.end(), [&](const MWGui::ItemStack& stack) {
                        return stack.mBase.getCellRef().getRefId() == item.getCellRef().getRefId();
                    });
                    if (existing != mItems.end())
                    {
                        existing->mCount += item.getCellRef().getCount();
                        continue;
                    }

                    MWGui::ItemStack stack;
                    stack.mBase = item;
                    stack.mCreator = this;
                    stack.mCount = item.getCellRef().getCount();
                    mItems.push_back(std::move(stack));
                }
            }
        }

        MWWorld::Ptr addItem(
            const MWGui::ItemStack& item, std::size_t count, bool /*allowAutoEquip*/ = true) override
        {
            const ESM::RefId id = item.mBase.getCellRef().getRefId();
            const auto existing = std::find_if(mItems.begin(), mItems.end(), [&](const MWGui::ItemStack& stack) {
                return stack.mBase.getCellRef().getRefId() == id;
            });
            if (existing != mItems.end())
                existing->mCount += count;
            else
            {
                MWGui::ItemStack added = item;
                added.mCreator = this;
                added.mCount = count;
                mItems.push_back(std::move(added));
            }
            return item.mBase;
        }

        MWWorld::Ptr copyItem(
            const MWGui::ItemStack& item, std::size_t count, bool allowAutoEquip = true) override
        {
            return addItem(item, count, allowAutoEquip);
        }

        void removeItem(const MWGui::ItemStack& item, std::size_t count) override
        {
            const ESM::RefId id = item.mBase.getCellRef().getRefId();
            const auto existing = std::find_if(mItems.begin(), mItems.end(), [&](const MWGui::ItemStack& stack) {
                return stack.mBase.getCellRef().getRefId() == id;
            });
            if (existing == mItems.end() || existing->mCount < count)
                throw std::runtime_error("barter test source did not contain the requested item count");
            existing->mCount -= count;
            if (existing->mCount == 0)
                mItems.erase(existing);
        }

        std::size_t count(const ESM::RefId& id) const
        {
            const auto existing = std::find_if(mItems.begin(), mItems.end(), [&](const MWGui::ItemStack& stack) {
                return stack.mBase.getCellRef().getRefId() == id;
            });
            return existing == mItems.end() ? 0 : existing->mCount;
        }

        bool usesContainer(const MWWorld::Ptr& container) override
        {
            return std::find(mSources.begin(), mSources.end(), container) != mSources.end();
        }

    private:
        std::vector<MWWorld::Ptr> mSources;
        std::vector<MWGui::ItemStack> mItems;
    };

    TEST(FnvCreatureAiPolicyTest, FlyAndWalkSandboxRetainsAuthoredPerch)
    {
        constexpr std::uint32_t flyAndWalk
            = ESM4::Creature::FO3_CanFly | ESM4::Creature::FO3_CanWalk;

        EXPECT_TRUE(MWClass::fnvAmbientFlyerRetainsAuthoredPosition(flyAndWalk, 11));
        EXPECT_TRUE(MWClass::fnvAmbientFlyerRetainsAuthoredPosition(flyAndWalk, 12));
        EXPECT_FALSE(MWClass::fnvAmbientFlyerRetainsAuthoredPosition(ESM4::Creature::FO3_CanFly, 12));
        EXPECT_FALSE(MWClass::fnvAmbientFlyerRetainsAuthoredPosition(ESM4::Creature::FO3_CanWalk, 12));
        EXPECT_FALSE(MWClass::fnvAmbientFlyerRetainsAuthoredPosition(flyAndWalk, 5));
        EXPECT_FALSE(MWClass::fnvAmbientFlyerRetainsAuthoredPosition(flyAndWalk, 6));
    }

    TEST(FnvCreatureAiPolicyTest, ZeroSavedToleranceKeepsLegacyArrivalDistance)
    {
        EXPECT_EQ(MWMechanics::AiWander::sanitizeDestinationTolerance(64, 0),
            MWMechanics::AiWander::sDefaultDestinationTolerance);
        EXPECT_EQ(MWMechanics::AiWander::sanitizeDestinationTolerance(64, 8), 8u);
        EXPECT_LT(MWMechanics::AiWander::sanitizeDestinationTolerance(
                      64, std::numeric_limits<std::uint32_t>::max()),
            64u);
    }

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

        static ESM4::Door makeDoor()
        {
            ESM4::Door door{};
            door.mId = ESM::FormId::fromUint32(sDoorBase);
            door.mEditorId = "GoodspringsKeyedDoorTest";
            door.mFullName = "Goodsprings Keyed Door";
            return door;
        }

        static ESM4::Reference makeLockedDoorReference(bool teleport)
        {
            ESM4::Reference reference{};
            reference.mId = ESM::FormId::fromUint32(sDoorRef);
            reference.mParent = ESM::RefId(ESM::FormId::fromUint32(sCreatureCell));
            reference.mBaseObj = ESM::FormId::fromUint32(sDoorBase);
            reference.mCount = 1;
            reference.mIsLocked = true;
            reference.mLockLevel = 50;
            reference.mKey = ESM::FormId::fromUint32(sSaloonKeyBase);
            if (teleport)
                reference.mDoor.destDoor = ESM::FormId::fromUint32(sDoorDestRef);
            return reference;
        }

        static ESM4::Cell makeDoorDestinationCell()
        {
            ESM4::Cell cell{};
            cell.mId = ESM::RefId(ESM::FormId::fromUint32(sDoorDestCell));
            cell.mEditorId = "GoodspringsDoorDestinationCell";
            cell.mFullName = "Goodsprings Door Destination Cell";
            cell.mCellFlags = ESM4::CELL_Interior;
            return cell;
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

        static ESM4::Npc makeNpc(std::uint32_t id = sNpcBase)
        {
            ESM4::Npc npc{};
            npc.mId = ESM::FormId::fromUint32(id);
            npc.mEditorId = "GoodspringsNpcStateTest";
            npc.mFullName = "Goodsprings NPC";
            npc.mIsFONV = true;
            npc.mHasFNVData = true;
            npc.mFNVData.health = 100;
            npc.mBaseConfig.fo3.levelOrMult = 1;
            npc.mRace = ESM::FormId::fromUint32(sNpcRace);
            npc.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 6 });
            return npc;
        }

        static ESM4::Race makeNpcRace()
        {
            ESM4::Race race{};
            race.mId = ESM::FormId::fromUint32(sNpcRace);
            race.mEditorId = "GoodspringsNpcStateRace";
            race.mFullName = "Human";
            return race;
        }

        static ESM4::LVLO makeLevelledEntry(
            std::int16_t level, std::uint32_t item, std::int16_t count = 1)
        {
            ESM4::LVLO result{};
            result.level = level;
            result.item = item;
            result.count = count;
            return result;
        }

        static ESM4::LevelledItem makeLevelledItem(std::uint32_t id, std::uint8_t flags,
            std::initializer_list<ESM4::LVLO> entries, std::int8_t chanceNone = 0)
        {
            ESM4::LevelledItem result{};
            result.mId = ESM::FormId::fromUint32(id);
            result.mEditorId = "SyntheticDeterministicFnvLevelledItem";
            result.mHasChanceNone = true;
            result.mChanceNone = chanceNone;
            result.mHasLvlItemFlags = true;
            result.mLvlItemFlags = flags;
            result.mLvlObject.assign(entries);
            return result;
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

        static ESM4::Cell makeCreatureCell()
        {
            ESM4::Cell cell{};
            cell.mId = ESM::RefId(ESM::FormId::fromUint32(sCreatureCell));
            cell.mEditorId = "GoodspringsCreatureStateCell";
            cell.mFullName = "Goodsprings Creature State Cell";
            cell.mCellFlags = ESM4::CELL_Interior;
            return cell;
        }

        static ESM4::ActorCreature makePlacedCreature()
        {
            ESM4::ActorCreature actor{};
            actor.mId = ESM::FormId::fromUint32(sCreatureRef);
            actor.mParent = ESM::RefId(ESM::FormId::fromUint32(sCreatureCell));
            actor.mBaseObj = ESM::FormId::fromUint32(sCreatureBase);
            actor.mEditorId = "GoodspringsCreatureStateRef";
            actor.mCount = 1;
            actor.mScale = 1.f;
            return actor;
        }

        static ESM4::ActorCharacter makePlacedNpc(
            std::uint32_t base = sNpcBase, std::uint32_t ref = sNpcRef)
        {
            ESM4::ActorCharacter actor{};
            actor.mId = ESM::FormId::fromUint32(ref);
            actor.mParent = ESM::RefId(ESM::FormId::fromUint32(sCreatureCell));
            actor.mBaseObj = ESM::FormId::fromUint32(base);
            actor.mEditorId = "GoodspringsNpcStateRef";
            actor.mCount = 1;
            actor.mScale = 1.f;
            return actor;
        }

        static void populateCreatureWorldStore(MWWorld::ESMStore& store)
        {
            ESM4::MiscItem bottle{};
            bottle.mId = ESM::FormId::fromUint32(sSaloonBottleBase);
            bottle.mEditorId = "GSProspectorSaloonBottle";
            bottle.mFullName = "Bottle";
            store.overrideRecord(bottle);

            ESM4::Key key{};
            key.mId = ESM::FormId::fromUint32(sSaloonKeyBase);
            key.mEditorId = "GSProspectorSaloonKey";
            key.mFullName = "Prospector Saloon Key";
            store.overrideRecord(key);

            ESM4::LevelledItem levelled = makeLevelledItem(
                sLevelledItemBase, 0, { makeLevelledEntry(1, sSaloonBottleBase, 3) });
            store.overrideRecord(levelled);

            ESM4::Creature creature = makeCreature();
            creature.mBaseConfig.fo3.levelOrMult = 1;
            creature.mInventory.push_back(ESM4::InventoryItem{ sLevelledItemBase, 2 });
            store.overrideRecord(creature);
            store.overrideRecord(makeCreatureCell());
            const_cast<MWWorld::Store<ESM4::ActorCreature>&>(store.get<ESM4::ActorCreature>())
                .insertStatic(makePlacedCreature());
            store.setUp();
        }

        static void populateNpcWorldStore(MWWorld::ESMStore& store)
        {
            ESM4::MiscItem bottle{};
            bottle.mId = ESM::FormId::fromUint32(sSaloonBottleBase);
            bottle.mEditorId = "GSProspectorSaloonBottle";
            bottle.mFullName = "Bottle";
            store.overrideRecord(bottle);

            ESM4::Key key{};
            key.mId = ESM::FormId::fromUint32(sSaloonKeyBase);
            key.mEditorId = "GSProspectorSaloonKey";
            key.mFullName = "Prospector Saloon Key";
            store.overrideRecord(key);

            store.overrideRecord(makeNpcRace());
            store.overrideRecord(makeNpc());
            store.overrideRecord(makeCreatureCell());
            const_cast<MWWorld::Store<ESM4::ActorCharacter>&>(store.get<ESM4::ActorCharacter>())
                .insertStatic(makePlacedNpc());
            store.setUp();
        }

        static void populateDoorWorldStore(MWWorld::ESMStore& store)
        {
            ESM4::Key key{};
            key.mId = ESM::FormId::fromUint32(sSaloonKeyBase);
            key.mEditorId = "GSProspectorSaloonKey";
            key.mFullName = "Prospector Saloon Key";
            store.overrideRecord(key);

            store.overrideRecord(makeNpcRace());
            store.overrideRecord(makeNpc());
            store.overrideRecord(makeDoor());
            store.overrideRecord(makeCreatureCell());
            store.overrideRecord(makeDoorDestinationCell());

            ESM4::Reference source = makeLockedDoorReference(true);
            source.mOwner = ESM::FormId::fromUint32(sNpcBase);
            source.mDoor.destPos.pos[0] = 401.f;
            source.mDoor.destPos.pos[1] = 402.f;
            source.mDoor.destPos.pos[2] = 403.f;
            ESM4::Reference destination = makeLockedDoorReference(false);
            destination.mId = ESM::FormId::fromUint32(sDoorDestRef);
            destination.mParent = ESM::RefId(ESM::FormId::fromUint32(sDoorDestCell));
            destination.mIsLocked = false;
            destination.mLockLevel = 0;
            destination.mKey = {};

            auto& references = const_cast<MWWorld::Store<ESM4::Reference>&>(store.get<ESM4::Reference>());
            references.insertStatic(source);
            references.insertStatic(destination);
            store.setUp();
        }

        static MWWorld::Ptr findPlacedCreature(MWWorld::CellStore& cell)
        {
            MWWorld::Ptr result;
            cell.forEachType<ESM4::Creature>([&](const MWWorld::Ptr& ptr) {
                if (ptr.getCellRef().getRefNum() == ESM::FormId::fromUint32(sCreatureRef))
                {
                    result = ptr;
                    return false;
                }
                return true;
            });
            return result;
        }

        static MWWorld::Ptr findPlacedNpc(MWWorld::CellStore& cell)
        {
            MWWorld::Ptr result;
            cell.forEachType<ESM4::Npc>([&](const MWWorld::Ptr& ptr) {
                if (ptr.getCellRef().getRefNum() == ESM::FormId::fromUint32(sNpcRef))
                {
                    result = ptr;
                    return false;
                }
                return true;
            });
            return result;
        }

        static MWWorld::Ptr findPlacedDoor(MWWorld::CellStore& cell, std::uint32_t id = sDoorRef)
        {
            MWWorld::Ptr result;
            cell.forEachType<ESM4::Door>([&](const MWWorld::Ptr& ptr) {
                if (ptr.getCellRef().getRefNum() == ESM::FormId::fromUint32(id))
                {
                    result = ptr;
                    return false;
                }
                return true;
            });
            return result;
        }

        static std::unique_ptr<std::stringstream> writeWorldState(MWWorld::WorldModel& worldModel)
        {
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            Loading::Listener progress;
            worldModel.write(writer, progress);
            return stream;
        }

        template <class State>
        static std::unique_ptr<std::stringstream> writeStates(
            std::initializer_list<const State*> states, ESM::RecNameInts objectType)
        {
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            writer.startRecord(ESM::REC_CSTA);
            writer.writeCellId(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)));
            ESM::CellState cellState{};
            cellState.mId = ESM::RefId(ESM::FormId::fromUint32(sCreatureCell));
            cellState.mIsInterior = true;
            cellState.save(writer);
            for (const State* state : states)
            {
                writer.writeHNT("OBJE", objectType);
                state->save(writer);
            }
            writer.endRecord(ESM::REC_CSTA);
            return stream;
        }

        static std::unique_ptr<std::stringstream> writeActorStates(
            std::initializer_list<const ESM::CreatureState*> states, ESM::RecNameInts objectType)
        {
            return writeStates(states, objectType);
        }

        static std::unique_ptr<std::stringstream> writeDoorStates(
            std::initializer_list<const ESM::ObjectState*> states, ESM::RecNameInts objectType = ESM::REC_DOOR4)
        {
            return writeStates(states, objectType);
        }

        static std::unique_ptr<std::stringstream> writeCreatureStates(
            std::initializer_list<const ESM::CreatureState*> states)
        {
            return writeActorStates(states, ESM::REC_CREA4);
        }

        static std::unique_ptr<std::stringstream> writeNpcStates(
            std::initializer_list<const ESM::CreatureState*> states)
        {
            return writeActorStates(states, ESM::REC_NPC_4);
        }

        static void readWorldState(std::unique_ptr<std::stringstream> stream, MWWorld::WorldModel& worldModel,
            const std::map<int, int>* contentFileMapping = nullptr)
        {
            ESM::ESMReader reader;
            reader.open(std::move(stream), "fnv-creature-csta-save");
            reader.setContentFileMapping(contentFileMapping);
            while (reader.hasMoreRecs())
            {
                const std::uint32_t type = reader.getRecName().toInt();
                reader.getRecHeader();
                ASSERT_TRUE(worldModel.readRecord(reader, type));
            }
        }
    };

    TEST(FnvCreatureAiPolicyTest, SupportsPatrolAndPreservesAuthoredSmallRadiusSandbox)
    {
        EXPECT_TRUE(MWClass::fnvCreatureAiPackageProcedureSupported(13));
        EXPECT_TRUE(MWClass::fnvCreatureAiPackageProcedureSupported(12));
        EXPECT_EQ(MWClass::fnvCreatureWanderDistance(16), 16);
        EXPECT_EQ(MWClass::fnvCreatureWanderDestinationTolerance(16), 2u);
        EXPECT_EQ(MWClass::fnvCreatureWanderDistance(0), 256);
        EXPECT_EQ(MWClass::fnvCreatureWanderDestinationTolerance(256), 32u);
    }

    TEST_F(ESM4ContainerTest, ResolvesExactBoundedVictorPatrolWithinOneWorldspace)
    {
        constexpr std::uint32_t worldId = 0x01000da7;
        constexpr std::array<std::uint32_t, 3> cellIds{ 0x01104c20, 0x01104c21, 0x01104c22 };
        for (std::size_t index = 0; index < cellIds.size(); ++index)
        {
            ESM4::Cell cell{};
            cell.mId = ESM::RefId(ESM::FormId::fromUint32(cellIds[index]));
            cell.mParent = ESM::RefId(ESM::FormId::fromUint32(worldId));
            cell.mEditorId = "SyntheticVictorPatrolCell" + std::to_string(index);
            cell.mX = static_cast<std::int32_t>(index);
            mStore.overrideRecord(cell);
        }

        constexpr std::array<std::uint32_t, 11> markerIds{ 0x0116adc6, 0x0116adc7, 0x0116adc8,
            0x0116adcc, 0x0116adce, 0x0116adcf, 0x0116adcd, 0x0116adc9, 0x0116adca, 0x0116adcb,
            0x01154154 };
        constexpr std::array<std::array<float, 3>, 11> positions{ {
            { -71370.422f, 2131.071f, 8371.142f },
            { -66739.336f, 2942.334f, 8371.142f },
            { -66144.289f, 5613.587f, 8446.199f },
            { -62775.902f, 10359.241f, 9761.637f },
            { -62254.207f, 12785.296f, 10264.f },
            { -63754.926f, 9666.817f, 9417.408f },
            { -70165.922f, 7881.824f, 8496.f },
            { -70336.977f, 5223.771f, 8414.835f },
            { -71903.633f, 991.477f, 8414.835f },
            { -72299.93f, -3123.941f, 8142.937f },
            { -72320.f, -6000.f, 8320.f },
        } };

        auto makeMarker = [&](std::size_t index) {
            ESM4::Reference marker{};
            marker.mId = ESM::FormId::fromUint32(markerIds[index]);
            marker.mParent = ESM::RefId(ESM::FormId::fromUint32(cellIds[index / 4]));
            marker.mBaseObj = ESM::FormId::fromUint32(index == 4 || index == 10 ? 0x01000034 : 0x0100003b);
            marker.mPos.pos[0] = positions[index][0];
            marker.mPos.pos[1] = positions[index][1];
            marker.mPos.pos[2] = positions[index][2];
            if (index + 1 < markerIds.size())
                marker.mLinkedReference = ESM::FormId::fromUint32(markerIds[index + 1]);
            if (index == 4)
            {
                marker.mPos.rot[2] = 0.8f;
                marker.mPatrolIdleTime = 3.f;
                marker.mHasPatrolIdleTime = true;
                marker.mIsPatrolIdleScriptMarker = true;
            }
            else if (index == 10)
            {
                marker.mPatrolIdleTime = 1.f;
                marker.mHasPatrolIdleTime = true;
                marker.mIsPatrolIdleScriptMarker = true;
            }
            return marker;
        };

        for (std::size_t index = 0; index < markerIds.size(); ++index)
            mStore.overrideRecord(makeMarker(index));

        const std::optional<std::vector<MWClass::FnvCreaturePatrolPoint>> route
            = MWClass::collectFnvCreaturePatrolRoute(
                mStore, ESM::FormId::fromUint32(markerIds.front()), markerIds.size());
        ASSERT_TRUE(route);
        ASSERT_EQ(route->size(), markerIds.size());
        for (std::size_t index = 0; index < markerIds.size(); ++index)
        {
            EXPECT_EQ((*route)[index].mReference, ESM::FormId::fromUint32(markerIds[index]));
            EXPECT_EQ((*route)[index].mWorldspace, ESM::RefId(ESM::FormId::fromUint32(worldId)));
            EXPECT_FLOAT_EQ((*route)[index].mPosition.x(), positions[index][0]);
            EXPECT_FLOAT_EQ((*route)[index].mPosition.y(), positions[index][1]);
            EXPECT_FLOAT_EQ((*route)[index].mPosition.z(), positions[index][2]);
        }
        EXPECT_TRUE((*route)[4].mUsesAuthoredHeading);
        EXPECT_FLOAT_EQ((*route)[4].mYaw, 0.8f);
        EXPECT_FLOAT_EQ((*route)[4].mWaitSeconds, 3.f);
        EXPECT_TRUE((*route)[4].mIsPatrolIdleScriptMarker);
        EXPECT_TRUE((*route)[10].mUsesAuthoredHeading);
        EXPECT_FLOAT_EQ((*route)[10].mWaitSeconds, 1.f);
        EXPECT_TRUE((*route)[10].mIsPatrolIdleScriptMarker);

        EXPECT_FALSE(MWClass::collectFnvCreaturePatrolRoute(
            mStore, ESM::FormId::fromUint32(markerIds.front()), markerIds.size() - 1));

        ESM4::Reference cyclicTerminal = makeMarker(markerIds.size() - 1);
        cyclicTerminal.mLinkedReference = ESM::FormId::fromUint32(markerIds.front());
        mStore.overrideRecord(cyclicTerminal);
        EXPECT_FALSE(MWClass::collectFnvCreaturePatrolRoute(
            mStore, ESM::FormId::fromUint32(markerIds.front()), 32));

        constexpr std::uint32_t foreignWorldId = 0x01000da8;
        ESM4::Cell foreignCell{};
        foreignCell.mId = ESM::RefId(ESM::FormId::fromUint32(0x01104c23));
        foreignCell.mParent = ESM::RefId(ESM::FormId::fromUint32(foreignWorldId));
        mStore.overrideRecord(foreignCell);
        ESM4::Reference foreignTerminal = makeMarker(markerIds.size() - 1);
        foreignTerminal.mParent = foreignCell.mId;
        mStore.overrideRecord(foreignTerminal);
        EXPECT_FALSE(MWClass::collectFnvCreaturePatrolRoute(
            mStore, ESM::FormId::fromUint32(markerIds.front()), 32));
    }

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

    TEST_F(ESM4ContainerTest, CreatureUsesResolvedFixedStatsLevelForUniqueHighestLevelledEntry)
    {
        ESM4::LevelledItem levelled = makeLevelledItem(sLevelledItemBase, 0,
            { makeLevelledEntry(1, sSaloonBottleBase, 2), makeLevelledEntry(5, sSaloonKeyBase, 3),
                makeLevelledEntry(6, sSaloonBottleBase, 7) });
        mStore.overrideRecord(levelled);

        ESM4::Creature statsTemplate = makeCreature(sStatsTemplateBase);
        statsTemplate.mBaseConfig.fo3.levelOrMult = 5;
        mStore.overrideRecord(statsTemplate);

        ESM4::Creature creature = makeCreature();
        creature.mBaseConfig.fo3.levelOrMult = 1;
        creature.mBaseTemplate = ESM::FormId::fromUint32(sStatsTemplateBase);
        creature.mBaseConfig.fo3.templateFlags |= ESM4::Creature::Template_UseStats;
        creature.mInventory.push_back(ESM4::InventoryItem{ sLevelledItemBase, 2 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 0);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase))), 6);
    }

    TEST_F(ESM4ContainerTest, CreatureExpandsUseAllAndUniqueCalculateEachListsWithoutRng)
    {
        ESM4::LevelledItem nested = makeLevelledItem(sNestedLevelledItemBase, 0x02,
            { makeLevelledEntry(1, sSaloonBottleBase, 3) });
        ESM4::LevelledItem useAll = makeLevelledItem(sUseAllLevelledItemBase, 0x04,
            { makeLevelledEntry(1, sNestedLevelledItemBase, 4), makeLevelledEntry(1, sSaloonKeyBase, 2) });
        mStore.overrideRecord(nested);
        mStore.overrideRecord(useAll);

        ESM4::Creature creature = makeCreature();
        creature.mBaseConfig.fo3.levelOrMult = 1;
        creature.mInventory.push_back(ESM4::InventoryItem{ sUseAllLevelledItemBase, 2 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 24);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase))), 4);
    }

    TEST_F(ESM4ContainerTest, CreatureRejectsChanceGlobalCoedAmbiguityAndPcLevelMultWithoutPartialLoot)
    {
        constexpr std::uint32_t chanceListId = 0x01104020;
        constexpr std::uint32_t globalListId = 0x01104021;
        constexpr std::uint32_t coedListId = 0x01104022;
        constexpr std::uint32_t ambiguousListId = 0x01104023;
        constexpr std::uint32_t pcLevelListId = 0x01104024;
        constexpr std::uint32_t unknownFlagsListId = 0x01104025;
        constexpr std::uint32_t highestTieListId = 0x01104026;

        ESM4::LevelledItem chance
            = makeLevelledItem(chanceListId, 0, { makeLevelledEntry(1, sSaloonBottleBase) }, 25);
        ESM4::LevelledItem global
            = makeLevelledItem(globalListId, 0, { makeLevelledEntry(1, sSaloonBottleBase) });
        global.mChanceGlobal = ESM::FormId::fromUint32(0x01104010);
        ESM4::LevelledItem coed
            = makeLevelledItem(coedListId, 0, { makeLevelledEntry(1, sSaloonBottleBase) });
        coed.mLvlObjectExtra.resize(1);
        coed.mLvlObjectExtra[0] = ESM4::LevelledItemExtraData{};
        ESM4::LevelledItem ambiguous = makeLevelledItem(ambiguousListId, 0x01,
            { makeLevelledEntry(1, sSaloonBottleBase), makeLevelledEntry(2, sSaloonKeyBase) });
        ESM4::LevelledItem pcLevel
            = makeLevelledItem(pcLevelListId, 0, { makeLevelledEntry(1, sSaloonBottleBase) });
        ESM4::LevelledItem unknownFlags
            = makeLevelledItem(unknownFlagsListId, 0x80, { makeLevelledEntry(1, sSaloonBottleBase) });
        ESM4::LevelledItem highestTie = makeLevelledItem(highestTieListId, 0,
            { makeLevelledEntry(3, sSaloonBottleBase), makeLevelledEntry(3, sSaloonKeyBase) });
        mStore.overrideRecord(chance);
        mStore.overrideRecord(global);
        mStore.overrideRecord(coed);
        mStore.overrideRecord(ambiguous);
        mStore.overrideRecord(pcLevel);
        mStore.overrideRecord(unknownFlags);
        mStore.overrideRecord(highestTie);

        ESM4::Creature creature = makeCreature();
        creature.mBaseConfig.fo3.levelOrMult = 10;
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 2 });
        creature.mInventory.push_back(ESM4::InventoryItem{ chanceListId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ globalListId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ coedListId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ ambiguousListId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ unknownFlagsListId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ highestTieListId, 1 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);
        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 0);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase))), 2);

        ESM4::Creature pcLevelCreature = makeCreature();
        pcLevelCreature.mBaseConfig.fo3.flags |= ESM4::Creature::FO3_PCLevelMult;
        pcLevelCreature.mBaseConfig.fo3.levelOrMult = 100;
        pcLevelCreature.mInventory.push_back(ESM4::InventoryItem{ pcLevelListId, 1 });
        MWWorld::LiveCellRef<ESM4::Creature> pcLevelRef(reference, &pcLevelCreature);
        MWWorld::Ptr pcLevelPtr(&pcLevelRef);
        EXPECT_EQ(pcLevelPtr.getClass().getContainerStore(pcLevelPtr)
                      .count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))),
            0);
    }

    TEST_F(ESM4ContainerTest, CreatureRejectsUnsupportedCyclesAndDepthPastSixteenButAcceptsBoundary)
    {
        constexpr std::uint32_t unsupportedListId = 0x01104100;
        constexpr std::uint32_t cycleAId = 0x01104101;
        constexpr std::uint32_t cycleBId = 0x01104102;
        constexpr std::uint32_t acceptedChainBase = 0x01104200;
        constexpr std::uint32_t rejectedChainBase = 0x01104300;

        ESM4::Static unsupported{};
        unsupported.mId = ESM::FormId::fromUint32(sUnsupportedStaticBase);
        unsupported.mEditorId = "UnsupportedLevelledTerminal";
        mStore.overrideRecord(unsupported);

        mStore.overrideRecord(makeLevelledItem(unsupportedListId, 0x04,
            { makeLevelledEntry(1, sSaloonBottleBase), makeLevelledEntry(1, sUnsupportedStaticBase) }));
        mStore.overrideRecord(
            makeLevelledItem(cycleAId, 0, { makeLevelledEntry(1, cycleBId) }));
        mStore.overrideRecord(
            makeLevelledItem(cycleBId, 0, { makeLevelledEntry(1, cycleAId) }));

        const auto addChain = [&](std::uint32_t base, int listCount) {
            for (int i = listCount - 1; i >= 0; --i)
            {
                const std::uint32_t terminal
                    = i + 1 == listCount ? sSaloonBottleBase : base + static_cast<std::uint32_t>(i + 1);
                mStore.overrideRecord(makeLevelledItem(
                    base + static_cast<std::uint32_t>(i), 0, { makeLevelledEntry(1, terminal) }));
            }
        };
        addChain(acceptedChainBase, 16);
        addChain(rejectedChainBase, 17);

        ESM4::Creature creature = makeCreature();
        creature.mBaseConfig.fo3.levelOrMult = 1;
        creature.mInventory.push_back(ESM4::InventoryItem{ unsupportedListId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ cycleAId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ acceptedChainBase, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ rejectedChainBase, 1 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 1);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sUnsupportedStaticBase))), 0);
    }

    TEST_F(ESM4ContainerTest, CreatureRejectsEveryLevelledCountOverflowBeforeMutation)
    {
        constexpr std::int16_t maxEntryCount = std::numeric_limits<std::int16_t>::max();
        constexpr std::uint32_t innerId = 0x01104400;
        constexpr std::uint32_t bigChildId = 0x01104401;
        constexpr std::uint32_t recursiveOverflowId = 0x01104402;
        constexpr std::uint32_t aggregateOverflowId = 0x01104403;
        constexpr std::uint32_t outerOverflowId = 0x01104404;
        constexpr std::uint32_t mergeOverflowId = 0x01104405;

        mStore.overrideRecord(makeLevelledItem(
            innerId, 0, { makeLevelledEntry(1, sSaloonBottleBase, maxEntryCount) }));
        mStore.overrideRecord(
            makeLevelledItem(bigChildId, 0, { makeLevelledEntry(1, innerId, maxEntryCount) }));
        mStore.overrideRecord(makeLevelledItem(
            recursiveOverflowId, 0, { makeLevelledEntry(1, bigChildId, 3) }));
        mStore.overrideRecord(makeLevelledItem(aggregateOverflowId, 0x04,
            { makeLevelledEntry(1, bigChildId), makeLevelledEntry(1, bigChildId),
                makeLevelledEntry(1, bigChildId) }));
        mStore.overrideRecord(makeLevelledItem(
            outerOverflowId, 0, { makeLevelledEntry(1, sSaloonBottleBase, 2) }));
        mStore.overrideRecord(makeLevelledItem(mergeOverflowId, 0x04,
            { makeLevelledEntry(1, sSaloonBottleBase), makeLevelledEntry(1, sSaloonKeyBase) }));

        ESM4::Creature creature = makeCreature();
        creature.mBaseConfig.fo3.levelOrMult = 1;
        creature.mInventory.push_back(ESM4::InventoryItem{
            sSaloonBottleBase, static_cast<std::uint32_t>(std::numeric_limits<int>::max()) });
        creature.mInventory.push_back(ESM4::InventoryItem{ recursiveOverflowId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ aggregateOverflowId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{
            outerOverflowId, static_cast<std::uint32_t>(std::numeric_limits<int>::max()) });
        creature.mInventory.push_back(ESM4::InventoryItem{ mergeOverflowId, 1 });
        creature.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 2 });
        ESM4::Reference reference = makeCreatureReference();
        MWWorld::LiveCellRef<ESM4::Creature> liveRef(reference, &creature);
        MWWorld::Ptr ptr(&liveRef);

        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))),
            std::numeric_limits<int>::max());
        EXPECT_EQ(store.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase))), 2);
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

    TEST_F(ESM4ContainerTest, CreatureCellStateOmitsUnaccessedFnvAndRejectsNonFnvWriteAndRead)
    {
        {
            MWWorld::ESMStore store;
            populateCreatureWorldStore(store);
            ESM::ReadersCache readers;
            MWWorld::WorldModel worldModel(store, readers);
            mEnvironment.setESMStore(store);
            mEnvironment.setWorldModel(worldModel);

            MWWorld::CellStore* cell
                = worldModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
            ASSERT_NE(cell, nullptr);
            ASSERT_EQ(cell->getState(), MWWorld::CellStore::State_Unloaded);
            cell->load();
            cell->setWaterLevel(11.f); // Save the cell without ever constructing creature CustomData.
            ASSERT_EQ(cell->count(), 1u);

            auto stream = writeWorldState(worldModel);
            ESM::ESMReader reader;
            reader.open(std::move(stream), "unaccessed-fnv-creature-omission");
            ASSERT_TRUE(reader.hasMoreRecs());
            ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_CSTA);
            reader.getRecHeader();
            EXPECT_EQ(reader.getCellId(), ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)));
            ESM::CellState state{};
            state.load(reader);
            EXPECT_FALSE(reader.isNextSub("OBJE"));
        }

        MWWorld::ESMStore store;
        ESM4::Creature creature = makeCreature(sNonFnvCreatureBase);
        creature.mIsFONV = false;
        creature.mEditorId = "OblivionCreatureStateMustRemainOmitted";
        store.overrideRecord(creature);
        store.overrideRecord(makeCreatureCell());
        ESM4::ActorCreature actor = makePlacedCreature();
        actor.mId = ESM::FormId::fromUint32(sNonFnvCreatureRef);
        actor.mBaseObj = ESM::FormId::fromUint32(sNonFnvCreatureBase);
        const_cast<MWWorld::Store<ESM4::ActorCreature>&>(store.get<ESM4::ActorCreature>()).insertStatic(actor);
        store.setUp();

        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        MWWorld::CellStore* sourceCell
            = sourceModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
        ASSERT_NE(sourceCell, nullptr);
        ASSERT_EQ(sourceCell->getState(), MWWorld::CellStore::State_Unloaded);
        sourceCell->load();
        MWWorld::Ptr source;
        sourceCell->forEachType<ESM4::Creature>([&](const MWWorld::Ptr& ptr) {
            source = ptr;
            return false;
        });
        ASSERT_FALSE(source.isEmpty());
        source.getCellRef().setCount(9);

        auto omitted = writeWorldState(sourceModel);
        {
            ESM::ESMReader reader;
            reader.open(std::move(omitted), "non-fnv-creature-omission");
            ASSERT_TRUE(reader.hasMoreRecs());
            ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_CSTA);
            reader.getRecHeader();
            EXPECT_EQ(reader.getCellId(), ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)));
            ESM::CellState state{};
            state.load(reader);
            EXPECT_FALSE(reader.isNextSub("OBJE"));
        }

        ESM::CreatureState crafted;
        source.get<ESM4::Creature>()->save(crafted);
        ASSERT_FALSE(crafted.mHasCustomState);
        crafted.mRef.mCount = 17;

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell
            = restoredModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        readWorldState(writeCreatureStates({ &crafted }), restoredModel);
        MWWorld::Ptr restored;
        restoredCell->forEachType<ESM4::Creature>([&](const MWWorld::Ptr& ptr) {
            restored = ptr;
            return false;
        });
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getCellRef().getCount(false), 1);
        EXPECT_EQ(restored.getRefData().getCustomData(), nullptr);
    }

    TEST_F(ESM4ContainerTest, CreatureCstaRoundTripFromUnloadedCellsRetainsMutableInventoryHealthAndDeath)
    {
        MWWorld::ESMStore store;
        populateCreatureWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);

        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        const ESM::RefNum creatureRef = ESM::FormId::fromUint32(sCreatureRef);
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        ASSERT_EQ(sourceCell->getState(), MWWorld::CellStore::State_Unloaded);
        EXPECT_TRUE(sourceModel.getPtr(creatureRef).isEmpty());

        sourceCell->load();
        MWWorld::Ptr source = findPlacedCreature(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        sourceModel.registerPtr(source);
        MWWorld::ContainerStore& sourceStore = source.getClass().getContainerStore(source);
        ASSERT_EQ(sourceStore.remove(bottleId, 2, false, false), 2);

        MWMechanics::CreatureStats& stats = source.getClass().getCreatureStats(source);
        ESM::CreatureStats deadState;
        stats.writeState(deadState);
        deadState.mDynamic[0].mCurrent = 0.f;
        deadState.mDead = true;
        deadState.mDied = true;
        stats.readState(deadState);
        ASSERT_TRUE(stats.isDead());
        ASSERT_TRUE(stats.hasDied());

        auto stream = writeWorldState(sourceModel);
        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        EXPECT_TRUE(restoredModel.getPtr(creatureRef).isEmpty());

        readWorldState(std::move(stream), restoredModel);
        EXPECT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Loaded);
        MWWorld::Ptr restored = restoredModel.getPtr(creatureRef);
        ASSERT_FALSE(restored.isEmpty());
        MWWorld::ContainerStore& restoredStore = restored.getClass().getContainerStore(restored);
        EXPECT_EQ(restoredStore.count(bottleId), 4);
        const MWMechanics::CreatureStats& restoredStats = restored.getClass().getCreatureStats(restored);
        EXPECT_FLOAT_EQ(restoredStats.getHealth().getCurrent(), 0.f);
        EXPECT_TRUE(restoredStats.isDead());
        EXPECT_TRUE(restoredStats.hasDied());
        EXPECT_EQ(restored.getCellRef().getRefNum(), creatureRef);
    }

    TEST_F(ESM4ContainerTest, CreatureStateDropsItemsWhoseContentFileWasRemoved)
    {
        MWWorld::ESMStore store;
        populateCreatureWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedCreature(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        sourceModel.registerPtr(source);
        source.getClass().getContainerStore(source);

        ESM::CreatureState state;
        source.get<ESM4::Creature>()->save(state);
        ESM::ObjectState missing;
        missing.blank();
        missing.mEnabled = 1;
        missing.mRef = ESM::makeBlankCellRef();
        missing.mRef.mRefID = ESM::RefId(ESM::FormId{ 0x00cafe, 2 });
        missing.mRef.mRefNum = ESM::FormId{ 0x00babe, 2 };
        missing.mRef.mCount = 1;
        missing.mPosition = missing.mRef.mPos;
        state.mInventory.mItems.push_back(std::move(missing));

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        const std::map<int, int> contentFileMapping{ { 1, 1 } };
        readWorldState(writeCreatureStates({ &state }), restoredModel, &contentFileMapping);

        MWWorld::Ptr restored = restoredModel.getPtr(ESM::FormId::fromUint32(sCreatureRef));
        ASSERT_FALSE(restored.isEmpty());
        MWWorld::ContainerStore& restoredStore = restored.getClass().getContainerStore(restored);
        EXPECT_EQ(restoredStore.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 6);
        std::size_t stacks = 0;
        for (const MWWorld::ConstPtr item : restoredStore)
        {
            ++stacks;
            EXPECT_FALSE(item.getCellRef().getRefId().empty());
        }
        EXPECT_EQ(stacks, 1u);
        EXPECT_TRUE(restoredModel.getPtr(ESM::FormId{ 0x00babe, 2 }).isEmpty());
    }

    TEST_F(ESM4ContainerTest, MalformedCreatureStateCannotPartiallyMutateLiveReferenceOrCustomData)
    {
        MWWorld::ESMStore store;
        populateCreatureWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel worldModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(worldModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        MWWorld::CellStore* cell = worldModel.findCell(cellId, false);
        ASSERT_NE(cell, nullptr);
        cell->load();
        MWWorld::Ptr target = findPlacedCreature(*cell);
        ASSERT_FALSE(target.isEmpty());
        worldModel.registerPtr(target);
        MWWorld::ContainerStore& inventory = target.getClass().getContainerStore(target);
        ASSERT_EQ(inventory.count(bottleId), 6);
        MWMechanics::CreatureStats& stats = target.getClass().getCreatureStats(target);
        auto health = stats.getHealth();
        health.setCurrent(55.f);
        stats.setHealth(health);

        ESM::Position sentinel{};
        sentinel.pos[0] = 111.f;
        sentinel.pos[1] = 222.f;
        sentinel.pos[2] = 333.f;
        sentinel.rot[2] = 0.75f;
        target.getCellRef().setCount(7);
        target.getCellRef().setScale(1.25f);
        target.getCellRef().setPosition(sentinel);
        target.getRefData().setPosition(sentinel);
        target.getRefData().disable();
        const bool refWasChanged = target.getCellRef().hasChanged();
        const bool dataWasChanged = target.getRefData().hasChanged();
        const ESM::RefNum generatedBefore = worldModel.getLastGeneratedRefNum();

        ESM::CreatureState nanState;
        target.get<ESM4::Creature>()->save(nanState);
        nanState.mRef.mCount = 2;
        nanState.mRef.mScale = std::numeric_limits<float>::quiet_NaN();
        nanState.mEnabled = 1;
        nanState.mPosition.pos[0] = -900.f;
        ASSERT_FALSE(nanState.mInventory.mItems.empty());
        nanState.mInventory.mItems.front().mRef.mCount = 1;
        nanState.mCreatureStats.mDynamic[0].mCurrent = 4.f;
        readWorldState(writeCreatureStates({ &nanState }), worldModel);

        EXPECT_EQ(target.getCellRef().getCount(false), 7);
        EXPECT_FLOAT_EQ(target.getCellRef().getScale(), 1.25f);
        EXPECT_EQ(target.getCellRef().getPosition(), sentinel);
        EXPECT_EQ(target.getRefData().getPosition(), sentinel);
        EXPECT_FALSE(target.getRefData().isEnabled());
        EXPECT_EQ(target.getClass().getContainerStore(target).count(bottleId), 6);
        EXPECT_FLOAT_EQ(target.getClass().getCreatureStats(target).getHealth().getCurrent(), 55.f);
        EXPECT_EQ(target.getCellRef().hasChanged(), refWasChanged);
        EXPECT_EQ(target.getRefData().hasChanged(), dataWasChanged);
        EXPECT_EQ(worldModel.getLastGeneratedRefNum(), generatedBefore);

        ESM::CreatureState negativeCountState;
        target.get<ESM4::Creature>()->save(negativeCountState);
        negativeCountState.mRef.mCount = -3;
        negativeCountState.mEnabled = 1;
        negativeCountState.mPosition.pos[0] = -700.f;
        readWorldState(writeCreatureStates({ &negativeCountState }), worldModel);
        EXPECT_EQ(target.getCellRef().getCount(false), 7);
        EXPECT_EQ(target.getRefData().getPosition(), sentinel);
        EXPECT_FALSE(target.getRefData().isEnabled());
        EXPECT_EQ(target.getClass().getContainerStore(target).count(bottleId), 6);
        EXPECT_FLOAT_EQ(target.getClass().getCreatureStats(target).getHealth().getCurrent(), 55.f);
        EXPECT_EQ(worldModel.getLastGeneratedRefNum(), generatedBefore);

        std::string validationError;
        negativeCountState.mRef.mCount = 1;
        negativeCountState.mRef.mRefNum = {};
        EXPECT_FALSE(MWClass::ESM4Creature::validateState(
            *target.get<ESM4::Creature>()->mBase, negativeCountState, store, validationError));

        negativeCountState.mRef.mRefNum = ESM::FormId::fromUint32(sCreatureRef);
        ESM::AnimationState::ScriptedAnimation animation;
        animation.mTime = std::numeric_limits<float>::quiet_NaN();
        negativeCountState.mAnimationState.mScriptedAnims.push_back(animation);
        EXPECT_FALSE(MWClass::ESM4Creature::validateState(
            *target.get<ESM4::Creature>()->mBase, negativeCountState, store, validationError));
        negativeCountState.mAnimationState.mScriptedAnims.clear();

        ESM::LuaScript luaScript;
        ESM::LuaTimer timer{};
        timer.mType = ESM::LuaTimer::Type::SIMULATION_TIME;
        timer.mTime = std::numeric_limits<double>::quiet_NaN();
        luaScript.mTimers.push_back(timer);
        negativeCountState.mLuaScripts.mScripts.push_back(luaScript);
        EXPECT_FALSE(MWClass::ESM4Creature::validateState(
            *target.get<ESM4::Creature>()->mBase, negativeCountState, store, validationError));
        negativeCountState.mLuaScripts.mScripts.clear();

        negativeCountState.mCreatureStats.mAiSequence.mLastAiPackage = 99;
        EXPECT_FALSE(MWClass::ESM4Creature::validateState(
            *target.get<ESM4::Creature>()->mBase, negativeCountState, store, validationError));
        negativeCountState.mCreatureStats.mAiSequence.mLastAiPackage = -1;
        ESM::AiSequence::AiPackageContainer aiPackage;
        aiPackage.mType = ESM::AiSequence::Ai_Travel;
        auto travel = std::make_unique<ESM::AiSequence::AiTravel>();
        travel->mData.mX = std::numeric_limits<float>::quiet_NaN();
        travel->mData.mY = 0.f;
        travel->mData.mZ = 0.f;
        aiPackage.mPackage = std::move(travel);
        negativeCountState.mCreatureStats.mAiSequence.mPackages.push_back(std::move(aiPackage));
        EXPECT_FALSE(MWClass::ESM4Creature::validateState(
            *target.get<ESM4::Creature>()->mBase, negativeCountState, store, validationError));
    }

    TEST_F(ESM4ContainerTest, DuplicateCreatureObjectsUseFirstFullyValidatedStateWithoutAllocatingSecond)
    {
        MWWorld::ESMStore store;
        populateCreatureWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedCreature(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        sourceModel.registerPtr(source);
        MWWorld::ContainerStore& inventory = source.getClass().getContainerStore(source);
        ASSERT_EQ(inventory.remove(bottleId, 4, false, false), 4);
        MWMechanics::CreatureStats& stats = source.getClass().getCreatureStats(source);
        auto health = stats.getHealth();
        health.setCurrent(60.f);
        stats.setHealth(health);
        ESM::CreatureState first;
        source.get<ESM4::Creature>()->save(first);

        MWWorld::Ptr secondBottleState = inventory.search(bottleId);
        ASSERT_FALSE(secondBottleState.isEmpty());
        secondBottleState.getCellRef().setCount(5);
        health = stats.getHealth();
        health.setCurrent(20.f);
        stats.setHealth(health);
        ESM::CreatureState second;
        source.get<ESM4::Creature>()->save(second);

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        const std::size_t revisionBefore = restoredModel.getPtrRegistryRevision();
        readWorldState(writeCreatureStates({ &first, &second }), restoredModel);
        MWWorld::Ptr restored = restoredModel.getPtr(ESM::FormId::fromUint32(sCreatureRef));
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getClass().getContainerStore(restored).count(bottleId), 2);
        EXPECT_FLOAT_EQ(restored.getClass().getCreatureStats(restored).getHealth().getCurrent(), 60.f);
        // One outer creature and one retained inventory stack are registered; the duplicate performs no registration.
        EXPECT_EQ(restoredModel.getPtrRegistryRevision(), revisionBefore + 2);
    }

    TEST_F(ESM4ContainerTest, NpcCellStateOmitsUnaccessedFnvAndRejectsNonFnvWriteAndRead)
    {
        {
            MWWorld::ESMStore store;
            populateNpcWorldStore(store);
            ESM::ReadersCache readers;
            MWWorld::WorldModel worldModel(store, readers);
            mEnvironment.setESMStore(store);
            mEnvironment.setWorldModel(worldModel);

            MWWorld::CellStore* cell
                = worldModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
            ASSERT_NE(cell, nullptr);
            ASSERT_EQ(cell->getState(), MWWorld::CellStore::State_Unloaded);
            cell->load();
            cell->setWaterLevel(12.f); // Save the cell without constructing NPC CustomData.
            ASSERT_EQ(cell->count(), 1u);

            auto stream = writeWorldState(worldModel);
            ESM::ESMReader reader;
            reader.open(std::move(stream), "unaccessed-fnv-npc-omission");
            ASSERT_TRUE(reader.hasMoreRecs());
            ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_CSTA);
            reader.getRecHeader();
            EXPECT_EQ(reader.getCellId(), ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)));
            ESM::CellState state{};
            state.load(reader);
            EXPECT_FALSE(reader.isNextSub("OBJE"));
        }

        MWWorld::ESMStore store;
        ESM4::Npc npc = makeNpc(sNonFnvNpcBase);
        npc.mIsFONV = false;
        npc.mEditorId = "OblivionNpcStateMustRemainOmitted";
        store.overrideRecord(makeNpcRace());
        store.overrideRecord(npc);
        store.overrideRecord(makeCreatureCell());
        const_cast<MWWorld::Store<ESM4::ActorCharacter>&>(store.get<ESM4::ActorCharacter>())
            .insertStatic(makePlacedNpc(sNonFnvNpcBase, sNonFnvNpcRef));
        store.setUp();

        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        MWWorld::CellStore* sourceCell
            = sourceModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source;
        sourceCell->forEachType<ESM4::Npc>([&](const MWWorld::Ptr& ptr) {
            source = ptr;
            return false;
        });
        ASSERT_FALSE(source.isEmpty());
        source.getCellRef().setCount(9);

        auto omitted = writeWorldState(sourceModel);
        {
            ESM::ESMReader reader;
            reader.open(std::move(omitted), "non-fnv-npc-omission");
            ASSERT_TRUE(reader.hasMoreRecs());
            ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_CSTA);
            reader.getRecHeader();
            EXPECT_EQ(reader.getCellId(), ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)));
            ESM::CellState state{};
            state.load(reader);
            EXPECT_FALSE(reader.isNextSub("OBJE"));
        }

        ESM::CreatureState crafted;
        source.get<ESM4::Npc>()->save(crafted);
        ASSERT_FALSE(crafted.mHasCustomState);
        crafted.mRef.mCount = 17;

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell
            = restoredModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        readWorldState(writeNpcStates({ &crafted }), restoredModel);
        MWWorld::Ptr restored;
        restoredCell->forEachType<ESM4::Npc>([&](const MWWorld::Ptr& ptr) {
            restored = ptr;
            return false;
        });
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getCellRef().getCount(false), 1);
        EXPECT_EQ(restored.getRefData().getCustomData(), nullptr);
    }

    TEST_F(ESM4ContainerTest, NpcCstaRoundTripFromUnloadedCellsRetainsOuterInventoryHealthDeathAndEmptyAi)
    {
        MWWorld::ESMStore store;
        populateNpcWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);

        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        const ESM::RefNum npcRef = ESM::FormId::fromUint32(sNpcRef);
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        ASSERT_EQ(sourceCell->getState(), MWWorld::CellStore::State_Unloaded);
        EXPECT_TRUE(sourceModel.getPtr(npcRef).isEmpty());

        sourceCell->load();
        MWWorld::Ptr source = findPlacedNpc(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        sourceModel.registerPtr(source);
        MWWorld::ContainerStore& sourceStore = source.getClass().getContainerStore(source);
        ASSERT_EQ(sourceStore.remove(bottleId, 2, false, false), 2);

        MWMechanics::CreatureStats& stats = source.getClass().getCreatureStats(source);
        ASSERT_TRUE(stats.getAiSequence().isEmpty());
        ESM::CreatureStats deadState;
        stats.writeState(deadState);
        deadState.mDynamic[0].mCurrent = 0.f;
        deadState.mDead = true;
        deadState.mDied = true;
        stats.readState(deadState);

        ESM::Position savedPosition{};
        savedPosition.pos[0] = 101.f;
        savedPosition.pos[1] = 202.f;
        savedPosition.pos[2] = 303.f;
        savedPosition.rot[2] = 0.5f;
        source.getCellRef().setCount(2);
        source.getCellRef().setScale(1.25f);
        source.getCellRef().setPosition(savedPosition);
        source.getRefData().setPosition(savedPosition);

        auto stream = writeWorldState(sourceModel);
        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        EXPECT_TRUE(restoredModel.getPtr(npcRef).isEmpty());

        readWorldState(std::move(stream), restoredModel);
        EXPECT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Loaded);
        MWWorld::Ptr restored = restoredModel.getPtr(npcRef);
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getClass().getContainerStore(restored).count(bottleId), 4);
        const MWMechanics::CreatureStats& restoredStats = restored.getClass().getCreatureStats(restored);
        EXPECT_FLOAT_EQ(restoredStats.getHealth().getCurrent(), 0.f);
        EXPECT_TRUE(restoredStats.isDead());
        EXPECT_TRUE(restoredStats.hasDied());
        EXPECT_TRUE(restoredStats.getAiSequence().isEmpty());
        EXPECT_EQ(restored.getCellRef().getCount(false), 2);
        EXPECT_FLOAT_EQ(restored.getCellRef().getScale(), 1.25f);
        EXPECT_EQ(restored.getCellRef().getPosition(), savedPosition);
        EXPECT_EQ(restored.getRefData().getPosition(), savedPosition);
        EXPECT_TRUE(restored.getRefData().isEnabled());
        EXPECT_EQ(restored.getCellRef().getRefNum(), npcRef);
    }

    TEST_F(ESM4ContainerTest, NpcStateDropsItemsWhoseContentFileWasRemoved)
    {
        MWWorld::ESMStore store;
        populateNpcWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedNpc(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        sourceModel.registerPtr(source);
        source.getClass().getContainerStore(source);

        ESM::CreatureState state;
        source.get<ESM4::Npc>()->save(state);
        ESM::ObjectState missing;
        missing.blank();
        missing.mEnabled = 1;
        missing.mRef = ESM::makeBlankCellRef();
        missing.mRef.mRefID = ESM::RefId(ESM::FormId{ 0x00cafe, 2 });
        missing.mRef.mRefNum = ESM::FormId{ 0x00babe, 2 };
        missing.mRef.mCount = 1;
        missing.mPosition = missing.mRef.mPos;
        state.mInventory.mItems.push_back(std::move(missing));

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        const std::map<int, int> contentFileMapping{ { 1, 1 } };
        readWorldState(writeNpcStates({ &state }), restoredModel, &contentFileMapping);

        MWWorld::Ptr restored = restoredModel.getPtr(ESM::FormId::fromUint32(sNpcRef));
        ASSERT_FALSE(restored.isEmpty());
        MWWorld::ContainerStore& restoredStore = restored.getClass().getContainerStore(restored);
        EXPECT_EQ(restoredStore.count(ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase))), 6);
        std::size_t stacks = 0;
        for (const MWWorld::ConstPtr item : restoredStore)
        {
            ++stacks;
            EXPECT_FALSE(item.getCellRef().getRefId().empty());
        }
        EXPECT_EQ(stacks, 1u);
        EXPECT_TRUE(restoredModel.getPtr(ESM::FormId{ 0x00babe, 2 }).isEmpty());
    }

    TEST_F(ESM4ContainerTest, MalformedNpcStateAndWrongObjeTypeCannotPartiallyMutateOrAllocateRefs)
    {
        MWWorld::ESMStore store;
        populateNpcWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel worldModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(worldModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        MWWorld::CellStore* cell = worldModel.findCell(cellId, false);
        ASSERT_NE(cell, nullptr);
        cell->load();
        MWWorld::Ptr target = findPlacedNpc(*cell);
        ASSERT_FALSE(target.isEmpty());
        worldModel.registerPtr(target);
        MWWorld::ContainerStore& inventory = target.getClass().getContainerStore(target);
        ASSERT_EQ(inventory.count(bottleId), 6);
        MWMechanics::CreatureStats& stats = target.getClass().getCreatureStats(target);
        auto health = stats.getHealth();
        health.setCurrent(55.f);
        stats.setHealth(health);

        ESM::Position sentinel{};
        sentinel.pos[0] = 111.f;
        sentinel.pos[1] = 222.f;
        sentinel.pos[2] = 333.f;
        sentinel.rot[2] = 0.75f;
        target.getCellRef().setCount(7);
        target.getCellRef().setScale(1.25f);
        target.getCellRef().setPosition(sentinel);
        target.getRefData().setPosition(sentinel);
        target.getRefData().disable();
        const bool refWasChanged = target.getCellRef().hasChanged();
        const bool dataWasChanged = target.getRefData().hasChanged();
        const ESM::RefNum generatedBefore = worldModel.getLastGeneratedRefNum();
        const std::size_t revisionBefore = worldModel.getPtrRegistryRevision();

        ESM::CreatureState candidate;
        target.get<ESM4::Npc>()->save(candidate);
        candidate.mRef.mCount = 2;
        candidate.mRef.mScale = 1.5f;
        candidate.mEnabled = 1;
        candidate.mPosition.pos[0] = -900.f;
        ESM::ObjectState allocatableItem;
        allocatableItem.blank();
        allocatableItem.mEnabled = 1;
        allocatableItem.mRef = ESM::makeBlankCellRef();
        allocatableItem.mRef.mRefID = ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase));
        allocatableItem.mRef.mCount = 1;
        allocatableItem.mPosition = allocatableItem.mRef.mPos;
        candidate.mInventory.mItems.push_back(std::move(allocatableItem));

        // The OBJE tag disagrees with the resolved NPC_ base and must be rejected before typed state loading.
        readWorldState(writeActorStates({ &candidate }, ESM::REC_CREA4), worldModel);
        EXPECT_EQ(target.getCellRef().getCount(false), 7);
        EXPECT_EQ(target.getRefData().getPosition(), sentinel);
        EXPECT_EQ(worldModel.getLastGeneratedRefNum(), generatedBefore);
        EXPECT_EQ(worldModel.getPtrRegistryRevision(), revisionBefore);

        // A malformed tail field must reject the whole payload before the valid-looking item can allocate a RefNum.
        candidate.mCreatureStats.mDynamic[0].mCurrent = std::numeric_limits<float>::quiet_NaN();
        readWorldState(writeNpcStates({ &candidate }), worldModel);

        EXPECT_EQ(target.getCellRef().getCount(false), 7);
        EXPECT_FLOAT_EQ(target.getCellRef().getScale(), 1.25f);
        EXPECT_EQ(target.getCellRef().getPosition(), sentinel);
        EXPECT_EQ(target.getRefData().getPosition(), sentinel);
        EXPECT_FALSE(target.getRefData().isEnabled());
        EXPECT_EQ(target.getClass().getContainerStore(target).count(bottleId), 6);
        EXPECT_FLOAT_EQ(target.getClass().getCreatureStats(target).getHealth().getCurrent(), 55.f);
        EXPECT_EQ(target.getCellRef().hasChanged(), refWasChanged);
        EXPECT_EQ(target.getRefData().hasChanged(), dataWasChanged);
        EXPECT_EQ(worldModel.getLastGeneratedRefNum(), generatedBefore);
        EXPECT_EQ(worldModel.getPtrRegistryRevision(), revisionBefore);
    }

    TEST_F(ESM4ContainerTest, DuplicateNpcObjectsUseFirstFullyValidatedStateAfterInvalidWithoutSecondRegistration)
    {
        MWWorld::ESMStore store;
        populateNpcWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedNpc(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        sourceModel.registerPtr(source);
        MWWorld::ContainerStore& inventory = source.getClass().getContainerStore(source);

        ESM::CreatureState invalid;
        source.get<ESM4::Npc>()->save(invalid);
        invalid.mRef.mScale = std::numeric_limits<float>::quiet_NaN();

        ASSERT_EQ(inventory.remove(bottleId, 4, false, false), 4);
        MWMechanics::CreatureStats& stats = source.getClass().getCreatureStats(source);
        auto health = stats.getHealth();
        health.setCurrent(60.f);
        stats.setHealth(health);
        ESM::CreatureState firstValid;
        source.get<ESM4::Npc>()->save(firstValid);

        MWWorld::Ptr secondBottleState = inventory.search(bottleId);
        ASSERT_FALSE(secondBottleState.isEmpty());
        secondBottleState.getCellRef().setCount(5);
        health = stats.getHealth();
        health.setCurrent(20.f);
        stats.setHealth(health);
        ESM::CreatureState secondValid;
        source.get<ESM4::Npc>()->save(secondValid);

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        const std::size_t revisionBefore = restoredModel.getPtrRegistryRevision();
        readWorldState(writeNpcStates({ &invalid, &firstValid, &secondValid }), restoredModel);
        MWWorld::Ptr restored = restoredModel.getPtr(ESM::FormId::fromUint32(sNpcRef));
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getClass().getContainerStore(restored).count(bottleId), 2);
        EXPECT_FLOAT_EQ(restored.getClass().getCreatureStats(restored).getHealth().getCurrent(), 60.f);
        EXPECT_TRUE(restored.getClass().getCreatureStats(restored).getAiSequence().isEmpty());
        // The invalid state consumes nothing; one outer NPC plus one retained stack register for the first valid state.
        EXPECT_EQ(restoredModel.getPtrRegistryRevision(), revisionBefore + 2);
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

    TEST_F(ESM4ContainerTest, AuthoredKeyUnlocksEsm4TeleportAndOrdinaryDoors)
    {
        ESM4::Door door = makeDoor();
        ESM4::Container keyHolder = makeKeyHolder();
        ESM4::Reference keyHolderReference = makeKeyHolderReference();
        MWWorld::LiveCellRef<ESM4::Container> keyHolderRef(keyHolderReference, &keyHolder);
        MWWorld::Ptr actor(&keyHolderRef);

        ESM4::Reference teleportReference = makeLockedDoorReference(true);
        MWWorld::LiveCellRef<ESM4::Door> teleportRef(teleportReference, &door);
        MWWorld::Ptr teleportDoor(&teleportRef);
        std::unique_ptr<MWWorld::Action> teleport = teleportDoor.getClass().activate(teleportDoor, actor);
        EXPECT_NE(dynamic_cast<MWWorld::ActionTeleport*>(teleport.get()), nullptr);
        EXPECT_FALSE(teleportDoor.getCellRef().isLocked());
        EXPECT_EQ(teleportDoor.getCellRef().getLockLevel(), -50);

        ESM4::Reference ordinaryReference = makeLockedDoorReference(false);
        MWWorld::LiveCellRef<ESM4::Door> ordinaryRef(ordinaryReference, &door);
        MWWorld::Ptr ordinaryDoor(&ordinaryRef);
        std::unique_ptr<MWWorld::Action> open = ordinaryDoor.getClass().activate(ordinaryDoor, actor);
        EXPECT_NE(dynamic_cast<MWWorld::ActionDoor*>(open.get()), nullptr);
        EXPECT_FALSE(ordinaryDoor.getCellRef().isLocked());
        EXPECT_EQ(ordinaryDoor.getCellRef().getLockLevel(), -50);
    }

    TEST_F(ESM4ContainerTest, Esm4DoorRejectsMissingOrWrongAuthoredKey)
    {
        ESM4::Door door = makeDoor();

        ESM4::Reference missingActorReference = makeLockedDoorReference(true);
        MWWorld::LiveCellRef<ESM4::Door> missingActorRef(missingActorReference, &door);
        MWWorld::Ptr missingActorDoor(&missingActorRef);
        std::unique_ptr<MWWorld::Action> missingActor
            = missingActorDoor.getClass().activate(missingActorDoor, {});
        EXPECT_NE(dynamic_cast<MWWorld::FailedAction*>(missingActor.get()), nullptr);
        EXPECT_TRUE(missingActorDoor.getCellRef().isLocked());

        ESM4::Container keyHolder = makeKeyHolder();
        ESM4::Reference keyHolderReference = makeKeyHolderReference();
        MWWorld::LiveCellRef<ESM4::Container> keyHolderRef(keyHolderReference, &keyHolder);
        MWWorld::Ptr actor(&keyHolderRef);

        ESM4::Reference wrongKeyReference = makeLockedDoorReference(false);
        wrongKeyReference.mKey = ESM::FormId::fromUint32(sSaloonBottleBase);
        MWWorld::LiveCellRef<ESM4::Door> wrongKeyRef(wrongKeyReference, &door);
        MWWorld::Ptr wrongKeyDoor(&wrongKeyRef);
        std::unique_ptr<MWWorld::Action> wrongKey = wrongKeyDoor.getClass().activate(wrongKeyDoor, actor);
        EXPECT_NE(dynamic_cast<MWWorld::FailedAction*>(wrongKey.get()), nullptr);
        EXPECT_TRUE(wrongKeyDoor.getCellRef().isLocked());
    }

    TEST_F(ESM4ContainerTest, UnlockedEsm4TeleportAndOrdinaryDoorsUseNativeActions)
    {
        ESM4::Door door = makeDoor();

        ESM4::Reference teleportReference = makeLockedDoorReference(true);
        teleportReference.mIsLocked = false;
        teleportReference.mLockLevel = -50;
        MWWorld::LiveCellRef<ESM4::Door> teleportRef(teleportReference, &door);
        MWWorld::Ptr teleportDoor(&teleportRef);
        std::unique_ptr<MWWorld::Action> teleport = teleportDoor.getClass().activate(teleportDoor, {});
        EXPECT_NE(dynamic_cast<MWWorld::ActionTeleport*>(teleport.get()), nullptr);
        EXPECT_FALSE(teleportDoor.getCellRef().isLocked());
        EXPECT_EQ(teleportDoor.getCellRef().getLockLevel(), -50);

        ESM4::Reference ordinaryReference = makeLockedDoorReference(false);
        ordinaryReference.mIsLocked = false;
        ordinaryReference.mLockLevel = -50;
        MWWorld::LiveCellRef<ESM4::Door> ordinaryRef(ordinaryReference, &door);
        MWWorld::Ptr ordinaryDoor(&ordinaryRef);
        std::unique_ptr<MWWorld::Action> open = ordinaryDoor.getClass().activate(ordinaryDoor, {});
        EXPECT_NE(dynamic_cast<MWWorld::ActionDoor*>(open.get()), nullptr);
        EXPECT_FALSE(ordinaryDoor.getCellRef().isLocked());
        EXPECT_EQ(ordinaryDoor.getCellRef().getLockLevel(), -50);
    }

    TEST_F(ESM4ContainerTest, DoorCellStateOmitsUnchangedAuthoredReference)
    {
        MWWorld::ESMStore store;
        populateDoorWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel worldModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(worldModel);

        MWWorld::CellStore* cell
            = worldModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
        ASSERT_NE(cell, nullptr);
        cell->load();
        cell->setWaterLevel(12.f);
        ASSERT_EQ(cell->count(), 1u);

        ESM::ESMReader reader;
        reader.open(writeWorldState(worldModel), "unchanged-fnv-door-omission");
        ASSERT_TRUE(reader.hasMoreRecs());
        ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_CSTA);
        reader.getRecHeader();
        EXPECT_EQ(reader.getCellId(), ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)));
        ESM::CellState cellState{};
        cellState.load(reader);
        EXPECT_FALSE(reader.isNextSub("OBJE"));
    }

    TEST_F(ESM4ContainerTest, DoorStateRoundTripRetainsUnlockOuterAndImmutableTeleportData)
    {
        MWWorld::ESMStore store;
        populateDoorWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId sourceCellId(ESM::FormId::fromUint32(sCreatureCell));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(sourceCellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedDoor(*sourceCell);
        ASSERT_FALSE(source.isEmpty());

        ESM::Position savedPosition{};
        savedPosition.pos[0] = 101.f;
        savedPosition.pos[1] = 202.f;
        savedPosition.pos[2] = 303.f;
        savedPosition.rot[2] = 0.5f;
        source.getCellRef().unlock();
        source.getCellRef().setCount(2);
        source.getCellRef().setScale(1.25f);
        source.getCellRef().setPosition(savedPosition);
        source.getRefData().setPosition(savedPosition);
        source.getRefData().disable();

        auto stream = writeWorldState(sourceModel);
        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(sourceCellId, false);
        ASSERT_NE(restoredCell, nullptr);
        ASSERT_EQ(restoredCell->getState(), MWWorld::CellStore::State_Unloaded);
        readWorldState(std::move(stream), restoredModel);

        MWWorld::Ptr restored = restoredModel.getPtr(ESM::FormId::fromUint32(sDoorRef));
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_FALSE(restored.getCellRef().isLocked());
        // ESM3 save serialization omits FLTV for unlocked references, so reload canonicalizes its level to zero.
        EXPECT_EQ(restored.getCellRef().getLockLevel(), 0);
        EXPECT_EQ(restored.getCellRef().getKey(), ESM::RefId(ESM::FormId::fromUint32(sSaloonKeyBase)));
        EXPECT_EQ(restored.getCellRef().getOwner(), ESM::RefId(ESM::FormId::fromUint32(sNpcBase)));
        EXPECT_EQ(restored.getCellRef().getCount(false), 2);
        EXPECT_FLOAT_EQ(restored.getCellRef().getScale(), 1.25f);
        EXPECT_EQ(restored.getCellRef().getPosition(), savedPosition);
        EXPECT_EQ(restored.getRefData().getPosition(), savedPosition);
        EXPECT_FALSE(restored.getRefData().isEnabled());
        EXPECT_TRUE(restored.getCellRef().getTeleport());
        EXPECT_EQ(restored.getCellRef().getDestCell(), ESM::RefId(ESM::FormId::fromUint32(sDoorDestCell)));
        ESM::Position expectedDestination{};
        expectedDestination.pos[0] = 401.f;
        expectedDestination.pos[1] = 402.f;
        expectedDestination.pos[2] = 403.f;
        EXPECT_EQ(restored.getCellRef().getDoorDest(), expectedDestination);
    }

    TEST_F(ESM4ContainerTest, MalformedAndWrongObjeDoorStatesCannotMutateLiveReference)
    {
        MWWorld::ESMStore store;
        populateDoorWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel worldModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(worldModel);
        MWWorld::CellStore* cell
            = worldModel.findCell(ESM::RefId(ESM::FormId::fromUint32(sCreatureCell)), false);
        ASSERT_NE(cell, nullptr);
        cell->load();
        MWWorld::Ptr target = findPlacedDoor(*cell);
        ASSERT_FALSE(target.isEmpty());
        worldModel.registerPtr(target);
        const std::size_t revisionBefore = worldModel.getPtrRegistryRevision();
        const ESM::RefNum generatedBefore = worldModel.getLastGeneratedRefNum();

        ESM::ObjectState candidate;
        target.get<ESM4::Door>()->save(candidate);
        candidate.mRef.mCount = 7;
        candidate.mRef.mScale = 1.5f;
        candidate.mEnabled = 0;
        candidate.mPosition.pos[0] = -900.f;

        readWorldState(writeDoorStates({ &candidate }, ESM::REC_CONT4), worldModel);
        EXPECT_EQ(target.getCellRef().getCount(false), 1);
        EXPECT_FLOAT_EQ(target.getCellRef().getScale(), 1.f);
        EXPECT_TRUE(target.getRefData().isEnabled());

        candidate.mRef.mScale = std::numeric_limits<float>::quiet_NaN();
        readWorldState(writeDoorStates({ &candidate }), worldModel);
        EXPECT_EQ(target.getCellRef().getCount(false), 1);
        EXPECT_FLOAT_EQ(target.getCellRef().getScale(), 1.f);
        EXPECT_TRUE(target.getCellRef().isLocked());
        EXPECT_TRUE(target.getRefData().isEnabled());
        EXPECT_EQ(worldModel.getPtrRegistryRevision(), revisionBefore);
        EXPECT_EQ(worldModel.getLastGeneratedRefNum(), generatedBefore);
    }

    TEST_F(ESM4ContainerTest, DuplicateDoorObjectsUseFirstFullyValidatedStateAfterInvalid)
    {
        MWWorld::ESMStore store;
        populateDoorWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedDoor(*sourceCell);
        ASSERT_FALSE(source.isEmpty());

        ESM::ObjectState invalid;
        source.get<ESM4::Door>()->save(invalid);
        invalid.mRef.mScale = std::numeric_limits<float>::quiet_NaN();

        ESM::ObjectState firstValid;
        source.get<ESM4::Door>()->save(firstValid);
        firstValid.mRef.mCount = 2;
        firstValid.mRef.mScale = 1.25f;
        firstValid.mEnabled = 0;
        firstValid.mPosition.pos[0] = 111.f;

        ESM::ObjectState secondValid = firstValid;
        secondValid.mRef.mCount = 3;
        secondValid.mRef.mScale = 1.5f;
        secondValid.mEnabled = 1;
        secondValid.mPosition.pos[0] = 222.f;

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        const std::size_t revisionBefore = restoredModel.getPtrRegistryRevision();
        readWorldState(writeDoorStates({ &invalid, &firstValid, &secondValid }), restoredModel);

        MWWorld::Ptr restored = restoredModel.getPtr(ESM::FormId::fromUint32(sDoorRef));
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getCellRef().getCount(false), 2);
        EXPECT_FLOAT_EQ(restored.getCellRef().getScale(), 1.25f);
        EXPECT_FALSE(restored.getRefData().isEnabled());
        EXPECT_FLOAT_EQ(restored.getRefData().getPosition().pos[0], 111.f);
        EXPECT_EQ(restoredModel.getPtrRegistryRevision(), revisionBefore + 1);
    }

    TEST_F(ESM4ContainerTest, DoorStateFromRemovedContentCannotMutateAuthoredReference)
    {
        MWWorld::ESMStore store;
        populateDoorWorldStore(store);
        ESM::ReadersCache readers;
        MWWorld::WorldModel sourceModel(store, readers);
        mEnvironment.setESMStore(store);
        mEnvironment.setWorldModel(sourceModel);
        const ESM::RefId cellId(ESM::FormId::fromUint32(sCreatureCell));
        MWWorld::CellStore* sourceCell = sourceModel.findCell(cellId, false);
        ASSERT_NE(sourceCell, nullptr);
        sourceCell->load();
        MWWorld::Ptr source = findPlacedDoor(*sourceCell);
        ASSERT_FALSE(source.isEmpty());
        ESM::ObjectState removed;
        source.get<ESM4::Door>()->save(removed);
        removed.mRef.mCount = 9;
        removed.mEnabled = 0;

        MWWorld::WorldModel restoredModel(store, readers);
        mEnvironment.setWorldModel(restoredModel);
        const std::map<int, int> noLoadedContentFiles;
        readWorldState(writeDoorStates({ &removed }), restoredModel, &noLoadedContentFiles);
        MWWorld::CellStore* restoredCell = restoredModel.findCell(cellId, false);
        ASSERT_NE(restoredCell, nullptr);
        restoredCell->load();
        MWWorld::Ptr restored = findPlacedDoor(*restoredCell);
        ASSERT_FALSE(restored.isEmpty());
        EXPECT_EQ(restored.getCellRef().getCount(false), 1);
        EXPECT_TRUE(restored.getCellRef().isLocked());
        EXPECT_TRUE(restored.getRefData().isEnabled());
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

    TEST_F(ESM4ContainerTest, FlatFalloutBarterFiltersCapsAndAcceptsSupportedInventory)
    {
        ESM4::MiscItem caps{};
        caps.mId = ESM::FormId::fromUint32(sCapsBase);
        caps.mEditorId = "Caps001";
        caps.mFullName = "Bottle Cap";
        mStore.overrideRecord(caps);

        ESM4::Npc merchant = makeNpc();
        ESM4::ActorCharacter merchantReference = makePlacedNpc();
        MWWorld::LiveCellRef<ESM4::Npc> merchantRef(merchantReference, &merchant);
        MWWorld::Ptr merchantPtr(&merchantRef);
        ASSERT_TRUE(MWGui::isFlatFalloutMerchant(merchantPtr));

        const ESM::RefId capsId(ESM::FormId::fromUint32(sCapsBase));
        EXPECT_EQ(MWGui::findFlatFalloutCurrency(mStore), capsId);

        ESM4::Container inventory{};
        inventory.mId = ESM::FormId::fromUint32(sBarterPocketBase);
        inventory.mInventory.push_back(ESM4::InventoryItem{ sCapsBase, 40 });
        inventory.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 2 });
        ESM4::Reference inventoryReference{};
        inventoryReference.mId = ESM::FormId::fromUint32(sBarterPocketRef);
        inventoryReference.mBaseObj = inventory.mId;
        inventoryReference.mCount = 1;
        MWWorld::LiveCellRef<ESM4::Container> inventoryRef(inventoryReference, &inventory);
        MWWorld::Ptr inventoryPtr(&inventoryRef);

        auto sourceModel = std::make_unique<BarterTestItemModel>(std::vector<MWWorld::Ptr>{ inventoryPtr });
        MWGui::TradeItemModel model(std::move(sourceModel), merchantPtr, capsId);
        model.update();
        ASSERT_EQ(model.getItemCount(), 1u);
        EXPECT_EQ(model.getItem(0).mBase.getCellRef().getRefId(),
            ESM::RefId(ESM::FormId::fromUint32(sSaloonBottleBase)));
        EXPECT_TRUE(MWGui::isItemAcceptedForBarter(model.getItem(0).mBase, merchantPtr, 0));

        ESM4::Static marker{};
        marker.mId = ESM::FormId::fromUint32(0x01103b33);
        ESM4::Reference markerReference{};
        markerReference.mId = ESM::FormId::fromUint32(0x01108749);
        markerReference.mBaseObj = marker.mId;
        markerReference.mCount = 1;
        MWWorld::LiveCellRef<ESM4::Static> markerRef(markerReference, &marker);
        EXPECT_FALSE(MWGui::isItemAcceptedForBarter(MWWorld::Ptr(&markerRef), merchantPtr, 0));
    }

    TEST_F(ESM4ContainerTest, FlatFalloutBarterAcceptCancelAndCapsPreflightStayCoherent)
    {
        ESM4::MiscItem caps{};
        caps.mId = ESM::FormId::fromUint32(sCapsBase);
        caps.mEditorId = "Caps001";
        caps.mFullName = "Bottle Cap";
        mStore.overrideRecord(caps);

        ESM4::Npc merchant = makeNpc();
        ESM4::ActorCharacter merchantReference = makePlacedNpc();
        MWWorld::LiveCellRef<ESM4::Npc> merchantRef(merchantReference, &merchant);
        MWWorld::Ptr merchantPtr(&merchantRef);

        ESM4::Container pocket{};
        pocket.mId = ESM::FormId::fromUint32(sBarterPocketBase);
        pocket.mInventory.push_back(ESM4::InventoryItem{ sCapsBase, 10 });
        pocket.mInventory.push_back(ESM4::InventoryItem{ sSaloonBottleBase, 2 });
        ESM4::Reference pocketReference{};
        pocketReference.mId = ESM::FormId::fromUint32(sBarterPocketRef);
        pocketReference.mBaseObj = pocket.mId;
        pocketReference.mCount = 1;
        MWWorld::LiveCellRef<ESM4::Container> pocketRef(pocketReference, &pocket);
        MWWorld::Ptr pocketPtr(&pocketRef);

        ESM4::Container chest{};
        chest.mId = ESM::FormId::fromUint32(sBarterChestBase);
        chest.mInventory.push_back(ESM4::InventoryItem{ sCapsBase, 40 });
        ESM4::Reference chestReference{};
        chestReference.mId = ESM::FormId::fromUint32(sBarterChestRef);
        chestReference.mBaseObj = chest.mId;
        chestReference.mCount = 1;
        MWWorld::LiveCellRef<ESM4::Container> chestRef(chestReference, &chest);
        MWWorld::Ptr chestPtr(&chestRef);

        ESM4::Container player{};
        player.mId = ESM::FormId::fromUint32(sBarterPlayerBase);
        player.mInventory.push_back(ESM4::InventoryItem{ sCapsBase, 100 });
        player.mInventory.push_back(ESM4::InventoryItem{ sSaloonKeyBase, 1 });
        ESM4::Reference playerReference{};
        playerReference.mId = ESM::FormId::fromUint32(sBarterPlayerRef);
        playerReference.mBaseObj = player.mId;
        playerReference.mCount = 1;
        MWWorld::LiveCellRef<ESM4::Container> playerRef(playerReference, &player);
        MWWorld::Ptr playerPtr(&playerRef);

        const ESM::RefId capsId(ESM::FormId::fromUint32(sCapsBase));
        const ESM::RefId bottleId(ESM::FormId::fromUint32(sSaloonBottleBase));
        const ESM::RefId keyId(ESM::FormId::fromUint32(sSaloonKeyBase));
        const std::vector<MWWorld::Ptr> merchantSources{ pocketPtr, chestPtr };
        const std::vector<MWWorld::Ptr> playerSources{ playerPtr };

        auto merchantSource = std::make_unique<BarterTestItemModel>(merchantSources);
        BarterTestItemModel* merchantInventory = merchantSource.get();
        MWGui::TradeItemModel merchantModel(std::move(merchantSource), merchantPtr, capsId);
        auto playerSource = std::make_unique<BarterTestItemModel>(playerSources);
        BarterTestItemModel* playerInventory = playerSource.get();
        MWGui::TradeItemModel playerModel(std::move(playerSource), merchantPtr, capsId);
        const auto update = [&] {
            merchantModel.update();
            playerModel.update();
        };
        const auto find = [](MWGui::TradeItemModel& model, const ESM::RefId& id) {
            for (std::size_t index = 0; index < model.getItemCount(); ++index)
            {
                if (model.getItem(static_cast<int>(index)).mBase.getCellRef().getRefId() == id)
                    return static_cast<int>(index);
            }
            return -1;
        };

        update();
        const int merchantBottle = find(merchantModel, bottleId);
        const int playerKey = find(playerModel, keyId);
        ASSERT_GE(merchantBottle, 0);
        ASSERT_GE(playerKey, 0);
        EXPECT_EQ(find(merchantModel, capsId), -1);
        EXPECT_EQ(find(playerModel, capsId), -1);

        playerModel.borrowItemToUs(merchantBottle, &merchantModel, 1);
        merchantModel.borrowItemFromUs(merchantBottle, 1);
        merchantModel.borrowItemToUs(playerKey, &playerModel, 1);
        playerModel.borrowItemFromUs(playerKey, 1);
        playerModel.abort();
        merchantModel.abort();
        EXPECT_EQ(pocketPtr.getClass().getContainerStore(pocketPtr).count(bottleId), 2);
        EXPECT_EQ(playerPtr.getClass().getContainerStore(playerPtr).count(keyId), 1);

        update();
        playerModel.borrowItemToUs(find(merchantModel, bottleId), &merchantModel, 1);
        merchantModel.borrowItemFromUs(find(merchantModel, bottleId), 1);
        merchantModel.borrowItemToUs(find(playerModel, keyId), &playerModel, 1);
        playerModel.borrowItemFromUs(find(playerModel, keyId), 1);
        merchantModel.transferItems();
        playerModel.transferItems();
        EXPECT_EQ(merchantInventory->count(bottleId), 1);
        EXPECT_EQ(playerInventory->count(bottleId), 1);
        EXPECT_EQ(merchantInventory->count(keyId), 1);
        EXPECT_EQ(playerInventory->count(keyId), 0);

        EXPECT_EQ(MWGui::countBarterCurrency(merchantSources, capsId), 50);
        EXPECT_EQ(MWGui::countBarterCurrency(playerSources, capsId), 100);
        EXPECT_TRUE(MWGui::transferBarterCurrency(merchantSources, playerSources, capsId, 0));
        EXPECT_FALSE(MWGui::transferBarterCurrency(merchantSources, playerSources, capsId, 51));
        EXPECT_EQ(MWGui::countBarterCurrency(merchantSources, capsId), 50);
        EXPECT_EQ(MWGui::countBarterCurrency(playerSources, capsId), 100);
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

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrefr.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwworld/cellstore.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/fnvlockpickruntime.hpp"
#include "apps/openmw/mwworld/fnvplayerruntimestate.hpp"
#include "apps/openmw/mwworld/worldmodel.hpp"

namespace
{
    using Error = MWWorld::FnvLockpickPreparationError;
    using Placement = MWWorld::FnvLockpickConservativePlacement;

    constexpr std::uint32_t sCellId = 0x01010000;
    constexpr std::uint32_t sDoorBaseId = 0x01010001;
    constexpr std::uint32_t sPlayerRefId = 0x01010002;
    constexpr std::uint32_t sTeleportDestination = 0x01010003;

    // Independent audit oracle. Do not derive this from the production table:
    // the test must fail if any frozen placement, tier, or XTEL shape drifts.
    constexpr std::array<Placement, 84> sExpectedPlacements{ {
        { 0x000824af, 25, false },
        { 0x00082618, 0, false },
        { 0x0008261d, 0, false },
        { 0x0008266c, 25, false },
        { 0x0008266f, 25, false },
        { 0x00083919, 25, false },
        { 0x000d70f3, 0, false },
        { 0x000d74ee, 75, false },
        { 0x000d80a3, 50, false },
        { 0x000e3ea9, 0, false },
        { 0x000e50e0, 25, false },
        { 0x000e5147, 75, false },
        { 0x000e5224, 50, false },
        { 0x000e5f49, 75, false },
        { 0x000e5f4a, 50, false },
        { 0x000e6a7c, 50, false },
        { 0x000e6a82, 25, false },
        { 0x000e6c5e, 50, false },
        { 0x000e6c61, 50, false },
        { 0x000e7727, 75, false },
        { 0x000eb4ba, 75, false },
        { 0x000eb4bf, 75, false },
        { 0x000eb59a, 0, true },
        { 0x000eb81b, 75, false },
        { 0x000ed9bc, 50, false },
        { 0x000edccc, 25, false },
        { 0x000edcce, 25, false },
        { 0x000edcd0, 0, false },
        { 0x000edcda, 75, false },
        { 0x000edce3, 50, false },
        { 0x000ef2f3, 0, true },
        { 0x000ef3ae, 50, false },
        { 0x000ef3b0, 50, false },
        { 0x000f2894, 75, false },
        { 0x000f34b0, 25, false },
        { 0x000f3d90, 50, false },
        { 0x000f5e39, 0, true },
        { 0x00101c3d, 25, true },
        { 0x00105982, 100, false },
        { 0x0010fa8f, 50, false },
        { 0x0010fa90, 50, false },
        { 0x0010fa91, 50, false },
        { 0x0010fa92, 50, false },
        { 0x0010fa93, 50, false },
        { 0x0011d9c6, 25, false },
        { 0x0011f93a, 100, false },
        { 0x0012af15, 25, true },
        { 0x00131f9e, 0, false },
        { 0x00131fc4, 50, false },
        { 0x001330d9, 25, false },
        { 0x00133e3f, 75, true },
        { 0x00139516, 50, false },
        { 0x0013dbe9, 75, false },
        { 0x0013dc44, 75, true },
        { 0x001464f1, 50, true },
        { 0x0014666e, 50, true },
        { 0x0014e1c1, 0, false },
        { 0x00153392, 0, false },
        { 0x0015ca39, 25, false },
        { 0x0100309b, 50, false },
        { 0x01005ff0, 25, false },
        { 0x01006075, 25, false },
        { 0x0100669c, 0, true },
        { 0x01008c0c, 0, false },
        { 0x01008fc6, 0, false },
        { 0x01009001, 50, false },
        { 0x0100924c, 25, false },
        { 0x0100cf72, 25, false },
        { 0x0100e7fb, 25, false },
        { 0x0100ee54, 50, false },
        { 0x0200d26f, 50, false },
        { 0x0200d4a5, 75, false },
        { 0x03000c43, 50, false },
        { 0x03007df7, 50, false },
        { 0x0300992c, 25, false },
        { 0x0300a2d8, 25, false },
        { 0x0300a2da, 50, false },
        { 0x0300b334, 50, false },
        { 0x03012a14, 50, false },
        { 0x0301424e, 25, false },
        { 0x04003f81, 25, false },
        { 0x04008777, 25, false },
        { 0x04009d04, 100, true },
        { 0x0400decf, 50, false },
    } };

    constexpr std::array<std::uint32_t, 64> sSha256Constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    std::string sha256(std::string_view input)
    {
        std::vector<std::uint8_t> bytes(input.begin(), input.end());
        const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;
        bytes.push_back(0x80);
        while (bytes.size() % 64 != 56)
            bytes.push_back(0);
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(bitLength >> shift));

        std::array<std::uint32_t, 8> hash{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        for (std::size_t block = 0; block < bytes.size(); block += 64)
        {
            std::array<std::uint32_t, 64> words{};
            for (std::size_t i = 0; i < 16; ++i)
            {
                const std::size_t offset = block + i * 4;
                words[i] = static_cast<std::uint32_t>(bytes[offset]) << 24
                    | static_cast<std::uint32_t>(bytes[offset + 1]) << 16
                    | static_cast<std::uint32_t>(bytes[offset + 2]) << 8
                    | static_cast<std::uint32_t>(bytes[offset + 3]);
            }
            for (std::size_t i = 16; i < words.size(); ++i)
            {
                const std::uint32_t s0
                    = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
                const std::uint32_t s1
                    = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
                words[i] = words[i - 16] + s0 + words[i - 7] + s1;
            }

            std::array<std::uint32_t, 8> state = hash;
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                const std::uint32_t sum1
                    = std::rotr(state[4], 6) ^ std::rotr(state[4], 11) ^ std::rotr(state[4], 25);
                const std::uint32_t choice = (state[4] & state[5]) ^ (~state[4] & state[6]);
                const std::uint32_t temp1 = state[7] + sum1 + choice + sSha256Constants[i] + words[i];
                const std::uint32_t sum0
                    = std::rotr(state[0], 2) ^ std::rotr(state[0], 13) ^ std::rotr(state[0], 22);
                const std::uint32_t majority
                    = (state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]);
                const std::uint32_t temp2 = sum0 + majority;
                state = { temp1 + temp2, state[0], state[1], state[2], state[3] + temp1, state[4], state[5],
                    state[6] };
            }
            for (std::size_t i = 0; i < hash.size(); ++i)
                hash[i] += state[i];
        }

        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (std::uint32_t value : hash)
            result << std::setw(8) << value;
        return result.str();
    }

    ESM::RefId refId(std::uint32_t value)
    {
        return ESM::RefId(ESM::FormId::fromUint32(value));
    }

    ESM4::Cell makeCell()
    {
        ESM4::Cell result{};
        result.mId = refId(sCellId);
        result.mEditorId = "FnvLockpickRuntimeTestCell";
        result.mFullName = "FNV Lockpick Runtime Test Cell";
        result.mCellFlags = ESM4::CELL_Interior;
        return result;
    }

    ESM4::Door makeDoor()
    {
        ESM4::Door result{};
        result.mId = ESM::FormId::fromUint32(sDoorBaseId);
        result.mEditorId = "FnvLockpickRuntimeTestDoor";
        result.mFullName = "FNV Lockpick Runtime Test Door";
        return result;
    }

    ESM4::MiscItem makeBobbyPin()
    {
        ESM4::MiscItem result{};
        result.mId = ESM::FormId::fromUint32(MWWorld::FnvLockpickBobbyPinFormId);
        result.mEditorId = "Lockpick";
        result.mFullName = "Bobby Pin";
        return result;
    }

    ESM4::Npc makePlayer()
    {
        ESM4::Npc result{};
        result.mId = ESM::FormId::fromUint32(MWWorld::FnvLockpickPlayerBaseFormId);
        result.mEditorId = "Player";
        result.mFullName = "Player";
        result.mIsFONV = true;
        result.mHasFNVData = true;
        result.mFNVData.health = 100;
        result.mBaseConfig.fo3.levelOrMult = 1;
        return result;
    }

    ESM4::Reference makeDoorPlacement(std::uint32_t placement, std::uint8_t level, bool teleport)
    {
        ESM4::Reference result{};
        result.mId = ESM::FormId::fromUint32(placement);
        result.mParent = refId(sCellId);
        result.mBaseObj = ESM::FormId::fromUint32(sDoorBaseId);
        result.mCount = 1;
        result.mIsLocked = true;
        result.mLockLevel = static_cast<std::int8_t>(level);
        if (teleport)
            result.mDoor.destDoor = ESM::FormId::fromUint32(sTeleportDestination);
        return result;
    }

    ESM4::ActorCharacter makePlayerPlacement()
    {
        ESM4::ActorCharacter result{};
        result.mId = ESM::FormId::fromUint32(sPlayerRefId);
        result.mParent = refId(sCellId);
        result.mBaseObj = ESM::FormId::fromUint32(MWWorld::FnvLockpickPlayerBaseFormId);
        result.mEditorId = "PlayerRef";
        result.mCount = 1;
        return result;
    }

    MWWorld::FalloutPlayerState makePlayerState(std::uint8_t lockpick = 100, std::uint8_t offset = 0)
    {
        MWWorld::FalloutPlayerState result;
        result.mBaseRecord = ESM::FormId::fromUint32(MWWorld::FnvLockpickPlayerBaseFormId);
        result.mHealth = 100;
        result.mSpecial.fill(5);
        result.mSkillValues.fill(50);
        result.mSkillValues[MWWorld::FnvLockpickActorValue - MWWorld::FalloutPlayerRuntimeState::SkillActorValueBegin]
            = lockpick;
        result.mSkillOffsets.fill(0);
        result.mSkillOffsets[MWWorld::FnvLockpickActorValue - MWWorld::FalloutPlayerRuntimeState::SkillActorValueBegin]
            = offset;
        return result;
    }

    class FakeInventory final : public MWWorld::FnvLockpickInventory
    {
    public:
        MWWorld::Ptr mOwner;
        std::map<ESM::RefId, std::int64_t> mCounts;
        int mPrepareCalls = 0;
        bool mPrepareSucceeds = true;
        bool mStillBelongs = true;
        bool mCountValid = true;

        bool prepareForActor(const MWWorld::Ptr& actor) noexcept override
        {
            ++mPrepareCalls;
            return mPrepareSucceeds && !actor.isEmpty() && actor == mOwner;
        }

        bool stillBelongsTo(const MWWorld::Ptr& actor) const noexcept override
        {
            return mStillBelongs && !actor.isEmpty() && actor == mOwner;
        }

        std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept override
        {
            if (!mCountValid)
                return std::nullopt;
            const auto found = mCounts.find(item);
            return found == mCounts.end() ? 0 : found->second;
        }
    };

    class LockpickWorld
    {
        MWBase::Environment mEnvironment;
        ESM::ReadersCache mReaders;

    public:
        MWWorld::ESMStore mStore;
        MWWorld::WorldModel mWorldModel{ mStore, mReaders };
        MWWorld::FalloutPlayerRuntimeState mPlayerState;
        FakeInventory mInventory;
        MWWorld::CellStore* mCell = nullptr;
        MWWorld::Ptr mTarget;
        MWWorld::Ptr mPlayer;
        ESM4::Reference* mReference = nullptr;
        ESM4::Door* mDoor = nullptr;
        ESM4::Cell* mCellRecord = nullptr;
        ESM4::MiscItem* mBobbyPin = nullptr;
        ESM4::Npc* mPlayerBase = nullptr;

        explicit LockpickWorld(
            std::uint32_t placement = 0x000824af, std::uint8_t level = 25, bool teleport = false)
        {
            mEnvironment.setESMStore(mStore);
            mEnvironment.setWorldModel(mWorldModel);
            MWClass::registerClasses();

            mCellRecord = const_cast<ESM4::Cell*>(mStore.overrideRecord(makeCell()));
            mDoor = const_cast<ESM4::Door*>(mStore.overrideRecord(makeDoor()));
            mBobbyPin = const_cast<ESM4::MiscItem*>(mStore.overrideRecord(makeBobbyPin()));
            mPlayerBase = const_cast<ESM4::Npc*>(mStore.overrideRecord(makePlayer()));
            auto& references = const_cast<MWWorld::Store<ESM4::Reference>&>(mStore.get<ESM4::Reference>());
            references.insertStatic(makeDoorPlacement(placement, level, teleport));
            auto& actors
                = const_cast<MWWorld::Store<ESM4::ActorCharacter>&>(mStore.get<ESM4::ActorCharacter>());
            actors.insertStatic(makePlayerPlacement());

            mStore.setUp();
            mReference = const_cast<ESM4::Reference*>(
                mStore.get<ESM4::Reference>().search(ESM::FormId::fromUint32(placement)));
            mCell = mWorldModel.findCell(refId(sCellId), false);
            if (mCell != nullptr)
            {
                mCell->load();
                mCell->forEachType<ESM4::Door>([&](const MWWorld::Ptr& ptr) {
                    if (ptr.getCellRef().getRefNum() == ESM::FormId::fromUint32(placement))
                    {
                        mTarget = ptr;
                        return false;
                    }
                    return true;
                });
                mCell->forEachType<ESM4::Npc>([&](const MWWorld::Ptr& ptr) {
                    if (ptr.getCellRef().getRefNum() == ESM::FormId::fromUint32(sPlayerRefId))
                    {
                        mPlayer = ptr;
                        return false;
                    }
                    return true;
                });
            }
            mPlayerState.initialize(makePlayerState());
            mInventory.mOwner = mPlayer;
            mInventory.mCounts[refId(MWWorld::FnvLockpickBobbyPinFormId)] = 5;
        }

        MWWorld::FnvLockpickPreparationSource source()
        {
            return { MWWorld::ESM4Game::FalloutNewVegas, &mStore, mTarget, mPlayer, mPlayer, &mInventory,
                &mPlayerState };
        }
    };

    Error preparationError(const MWWorld::FnvLockpickPreparationSource& source)
    {
        Error result = Error::None;
        EXPECT_FALSE(MWWorld::prepareFnvLockpickSession(source, &result));
        return result;
    }
}

TEST(FnvLockpickRuntimeTest, FrozenCorpusMatchesEveryAuditedPlacementTierShapeAndIdHash)
{
    const std::span<const Placement> actual = MWWorld::getFnvLockpickConservativePlacements();
    ASSERT_EQ(actual.size(), sExpectedPlacements.size());
    EXPECT_TRUE(std::equal(actual.begin(), actual.end(), sExpectedPlacements.begin()));
    EXPECT_TRUE(std::is_sorted(actual.begin(), actual.end(),
        [](const Placement& left, const Placement& right) { return left.mPlacement < right.mPlacement; }));

    std::array<std::size_t, 5> tierCounts{};
    std::size_t teleportCount = 0;
    std::ostringstream serializedIds;
    serializedIds << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        const Placement& placement = actual[i];
        ASSERT_NE(MWWorld::findFnvLockpickConservativePlacement(
                      ESM::FormId::fromUint32(placement.mPlacement)),
            nullptr);
        if (i != 0)
            serializedIds << '\n';
        serializedIds << std::setw(8) << placement.mPlacement;
        teleportCount += placement.mTeleport;
        switch (placement.mLockLevel)
        {
            case 0:
                ++tierCounts[0];
                break;
            case 25:
                ++tierCounts[1];
                break;
            case 50:
                ++tierCounts[2];
                break;
            case 75:
                ++tierCounts[3];
                break;
            case 100:
                ++tierCounts[4];
                break;
            default:
                ADD_FAILURE() << "unsupported frozen tier " << static_cast<int>(placement.mLockLevel);
        }
    }

    EXPECT_EQ(tierCounts, (std::array<std::size_t, 5>{ 14, 24, 30, 13, 3 }));
    EXPECT_EQ(teleportCount, 11u);
    EXPECT_EQ(sha256(serializedIds.str()), "305b7717827054620b5e075a5ad3cf660493389839173702705c25426f32e028");
    EXPECT_EQ(sha256(serializedIds.str()), MWWorld::FnvLockpickConservativePlacementIdSha256);
    EXPECT_EQ(MWWorld::findFnvLockpickConservativePlacement(ESM::FormId::fromUint32(0x000824ae)), nullptr);
}

TEST(FnvLockpickRuntimeTest, SignedXlocLevelCanonicalizationPreservesRequiresKey)
{
    EXPECT_EQ(MWWorld::canonicalizeFnvLockLevel(-1), 255);
    EXPECT_EQ(MWWorld::canonicalizeFnvLockLevel(0), 0);
    EXPECT_EQ(MWWorld::canonicalizeFnvLockLevel(25), 25);
    EXPECT_EQ(MWWorld::canonicalizeFnvLockLevel(50), 50);
    EXPECT_EQ(MWWorld::canonicalizeFnvLockLevel(75), 75);
    EXPECT_EQ(MWWorld::canonicalizeFnvLockLevel(100), 100);
}

TEST(FnvLockpickRuntimeTest, PreparesOrdinarySessionWithoutWorldInventoryOrActorValueMutation)
{
    LockpickWorld world;
    ASSERT_NE(world.mReference, nullptr);
    ASSERT_FALSE(world.mTarget.isEmpty());
    ASSERT_FALSE(world.mPlayer.isEmpty());
    const ESM4::Reference authoredBefore = *world.mReference;
    const int liveLevelBefore = world.mTarget.getCellRef().getLockLevel();
    const auto countsBefore = world.mInventory.mCounts;
    const auto skillBefore = world.mPlayerState.getCurrentActorValue(MWWorld::FnvLockpickActorValue);

    Error error = Error::NoBobbyPins;
    const std::optional<MWWorld::PreparedFnvLockpickSession> session
        = MWWorld::prepareFnvLockpickSession(world.source(), &error);

    ASSERT_TRUE(session);
    EXPECT_EQ(error, Error::None);
    EXPECT_EQ(session->getPlacement(), ESM::FormId::fromUint32(0x000824af));
    EXPECT_EQ(session->getDoor(), ESM::FormId::fromUint32(sDoorBaseId));
    EXPECT_EQ(session->getCell(), ESM::FormId::fromUint32(sCellId));
    EXPECT_EQ(session->getLockLevel(), 25);
    EXPECT_FLOAT_EQ(session->getLockpickSkill(), 100.f);
    EXPECT_EQ(session->getBobbyPinCount(), 5);
    EXPECT_FALSE(session->isTeleport());
    EXPECT_EQ(world.mInventory.mPrepareCalls, 1);
    EXPECT_EQ(world.mInventory.mCounts, countsBefore);
    EXPECT_EQ(world.mReference->mIsLocked, authoredBefore.mIsLocked);
    EXPECT_EQ(world.mReference->mLockLevel, authoredBefore.mLockLevel);
    EXPECT_EQ(world.mTarget.getCellRef().getLockLevel(), liveLevelBefore);
    EXPECT_TRUE(world.mTarget.getCellRef().isLocked());
    const auto skillAfter = world.mPlayerState.getCurrentActorValue(MWWorld::FnvLockpickActorValue);
    ASSERT_TRUE(skillBefore);
    ASSERT_TRUE(skillAfter);
    EXPECT_FLOAT_EQ(skillAfter->mValue, skillBefore->mValue);
    EXPECT_EQ(skillAfter->mRawSkillOffset, skillBefore->mRawSkillOffset);
}

TEST(FnvLockpickRuntimeTest, PreparesOnlyTheFrozenTeleportShape)
{
    LockpickWorld world(0x000eb59a, 0, true);
    Error error = Error::NoBobbyPins;
    const std::optional<MWWorld::PreparedFnvLockpickSession> session
        = MWWorld::prepareFnvLockpickSession(world.source(), &error);

    ASSERT_TRUE(session);
    EXPECT_EQ(error, Error::None);
    EXPECT_EQ(session->getPlacement(), ESM::FormId::fromUint32(0x000eb59a));
    EXPECT_EQ(session->getLockLevel(), 0);
    EXPECT_TRUE(session->isTeleport());
    EXPECT_TRUE(world.mTarget.getCellRef().isLocked());
}

TEST(FnvLockpickRuntimeTest, RejectsWrongGameMissingInputsAndNonConservativeTarget)
{
    {
        LockpickWorld world;
        auto source = world.source();
        source.mGame = MWWorld::ESM4Game::Fallout3;
        EXPECT_EQ(preparationError(source), Error::NotFalloutNewVegas);
    }
    {
        LockpickWorld world;
        auto source = world.source();
        source.mStore = nullptr;
        EXPECT_EQ(preparationError(source), Error::MissingStore);
    }
    {
        LockpickWorld world;
        auto source = world.source();
        source.mTarget = {};
        EXPECT_EQ(preparationError(source), Error::MissingTarget);
    }
    {
        LockpickWorld world;
        auto source = world.source();
        source.mTarget = world.mPlayer;
        EXPECT_EQ(preparationError(source), Error::TargetIsNotDoor);
    }
    {
        LockpickWorld world(0x000824ae, 25, false);
        EXPECT_EQ(preparationError(world.source()), Error::PlacementNotConservative);
    }
}

TEST(FnvLockpickRuntimeTest, RejectsDeletedDisabledNoAccessAndIgnoredWinningReferences)
{
    {
        LockpickWorld world;
        world.mReference->mFlags |= ESM4::Rec_Deleted;
        EXPECT_EQ(preparationError(world.source()), Error::DeletedRecord);
    }
    {
        LockpickWorld world;
        world.mTarget.getRefData().setDeletedByContentFile(true);
        EXPECT_EQ(preparationError(world.source()), Error::TargetNotLive);
    }
    {
        LockpickWorld world;
        world.mTarget.getRefData().disable();
        EXPECT_EQ(preparationError(world.source()), Error::TargetDisabled);
    }
    {
        LockpickWorld world;
        world.mReference->mFlags |= ESM4::Rec_NoAccess;
        EXPECT_EQ(preparationError(world.source()), Error::NoAccess);
    }
    {
        LockpickWorld world;
        world.mReference->mFlags |= ESM4::Rec_Disabled;
        EXPECT_EQ(preparationError(world.source()), Error::InitiallyDisabled);
    }
    {
        LockpickWorld world;
        world.mReference->mFlags |= ESM4::Rec_Ignored;
        EXPECT_EQ(preparationError(world.source()), Error::IgnoredReference);
    }
}

TEST(FnvLockpickRuntimeTest, RejectsUnsupportedReferenceOwnershipAndDoorData)
{
    {
        LockpickWorld world;
        world.mReference->mGlobal = ESM::FormId::fromUint32(0x01020000);
        EXPECT_EQ(preparationError(world.source()), Error::UnsupportedReferenceData);
    }
    {
        LockpickWorld world;
        world.mReference->mEsp.flags = ESM4::EnableParent::Flag_Inversed;
        EXPECT_EQ(preparationError(world.source()), Error::UnsupportedReferenceData);
    }
    {
        LockpickWorld world;
        world.mReference->mOwner = ESM::FormId::fromUint32(0x01020000);
        EXPECT_EQ(preparationError(world.source()), Error::OwnedTarget);
    }
    {
        LockpickWorld world;
        world.mCellRecord->mOwner = ESM::FormId::fromUint32(0x01020000);
        EXPECT_EQ(preparationError(world.source()), Error::OwnedTarget);
    }
    {
        LockpickWorld world;
        world.mDoor->mScriptId = ESM::FormId::fromUint32(0x01020000);
        EXPECT_EQ(preparationError(world.source()), Error::ScriptedDoor);
    }
    {
        LockpickWorld world;
        world.mDoor->mDoorFlags = ESM4::Door::Flag_AutomaticDoor;
        EXPECT_EQ(preparationError(world.source()), Error::UnsupportedDoorData);
    }
}

TEST(FnvLockpickRuntimeTest, RejectsUnlockedKeyedRequiresKeyUnsupportedAndDriftedLockShapes)
{
    {
        LockpickWorld world;
        world.mTarget.getCellRef().setLocked(false);
        EXPECT_EQ(preparationError(world.source()), Error::TargetNotLocked);
    }
    {
        LockpickWorld world;
        world.mReference->mKey = ESM::FormId::fromUint32(0x01020000);
        EXPECT_EQ(preparationError(world.source()), Error::AuthoredKey);
    }
    {
        LockpickWorld world;
        world.mReference->mLockLevel = -1;
        world.mTarget.getCellRef().setLockLevel(-1);
        EXPECT_EQ(preparationError(world.source()), Error::RequiresKey);
    }
    {
        LockpickWorld world;
        world.mReference->mLockLevel = 42;
        world.mTarget.getCellRef().setLockLevel(42);
        EXPECT_EQ(preparationError(world.source()), Error::UnsupportedLockLevel);
    }
    {
        LockpickWorld world;
        world.mReference->mLockLevel = 50;
        world.mTarget.getCellRef().setLockLevel(50);
        EXPECT_EQ(preparationError(world.source()), Error::PlacementShapeMismatch);
    }
    {
        LockpickWorld world;
        world.mReference->mDoor.destDoor = ESM::FormId::fromUint32(sTeleportDestination);
        EXPECT_EQ(preparationError(world.source()), Error::PlacementShapeMismatch);
    }
}

TEST(FnvLockpickRuntimeTest, RejectsDetachedWinningRecordsAndParentCell)
{
    {
        LockpickWorld world;
        ESM4::Door detached = *world.mDoor;
        world.mTarget.get<ESM4::Door>()->mBase = &detached;
        EXPECT_EQ(preparationError(world.source()), Error::DoorNotInStore);
    }
    {
        LockpickWorld world;
        world.mReference->mParent = refId(0x010100ff);
        EXPECT_EQ(preparationError(world.source()), Error::ParentCellNotInStore);
    }
}

TEST(FnvLockpickRuntimeTest, RequiresTheExactLivePlayerPointerAndNativePlayerState)
{
    {
        LockpickWorld world;
        auto source = world.source();
        source.mActor = world.mTarget;
        EXPECT_EQ(preparationError(source), Error::ActorIsNotPlayer);
    }
    {
        LockpickWorld world;
        auto source = world.source();
        source.mPlayer = world.mTarget;
        source.mActor = world.mTarget;
        EXPECT_EQ(preparationError(source), Error::PlayerStateMismatch);
    }
    {
        LockpickWorld world;
        world.mPlayerBase->mEditorId = "NotPlayer";
        EXPECT_EQ(preparationError(world.source()), Error::PlayerStateMismatch);
    }
    {
        LockpickWorld world;
        MWWorld::FalloutPlayerRuntimeState uninitialized;
        auto source = world.source();
        source.mPlayerState = &uninitialized;
        EXPECT_EQ(preparationError(source), Error::PlayerStateUninitialized);
    }
    {
        LockpickWorld world;
        MWWorld::FalloutPlayerState wrong = makePlayerState();
        wrong.mBaseRecord = ESM::FormId::fromUint32(8);
        world.mPlayerState.initialize(wrong);
        EXPECT_EQ(preparationError(world.source()), Error::PlayerStateMismatch);
    }
}

TEST(FnvLockpickRuntimeTest, RequiresResolvedSufficientNativeActorValue36)
{
    {
        LockpickWorld world;
        world.mPlayerState.initialize(makePlayerState(24));
        EXPECT_EQ(preparationError(world.source()), Error::InsufficientSkill);
    }
    {
        LockpickWorld world;
        world.mPlayerState.initialize(makePlayerState(100, 1));
        EXPECT_EQ(preparationError(world.source()), Error::UnresolvedSkillOffset);
    }
}

TEST(FnvLockpickRuntimeTest, RequiresExactOfficialBobbyPinRecordAndReadOnlyPositiveInventory)
{
    {
        LockpickWorld world;
        auto source = world.source();
        source.mInventory = nullptr;
        EXPECT_EQ(preparationError(source), Error::MissingInventory);
    }
    {
        LockpickWorld world;
        world.mInventory.mPrepareSucceeds = false;
        EXPECT_EQ(preparationError(world.source()), Error::InventoryMismatch);
    }
    {
        LockpickWorld world;
        auto& pins = const_cast<MWWorld::Store<ESM4::MiscItem>&>(world.mStore.get<ESM4::MiscItem>());
        ASSERT_TRUE(pins.erase(refId(MWWorld::FnvLockpickBobbyPinFormId)));
        EXPECT_EQ(preparationError(world.source()), Error::BobbyPinNotInStore);
    }
    {
        LockpickWorld world;
        world.mBobbyPin->mEditorId = "AlmostLockpick";
        EXPECT_EQ(preparationError(world.source()), Error::InvalidBobbyPinRecord);
    }
    {
        LockpickWorld world;
        world.mInventory.mCountValid = false;
        EXPECT_EQ(preparationError(world.source()), Error::InvalidInventory);
    }
    {
        LockpickWorld world;
        world.mInventory.mCounts[refId(MWWorld::FnvLockpickBobbyPinFormId)] = -1;
        EXPECT_EQ(preparationError(world.source()), Error::InvalidInventory);
    }
    {
        LockpickWorld world;
        world.mInventory.mCounts[refId(MWWorld::FnvLockpickBobbyPinFormId)] = 0;
        EXPECT_EQ(preparationError(world.source()), Error::NoBobbyPins);
    }
}

TEST(FnvLockpickRuntimeTest, EveryPreparationErrorHasAStableNonUnknownName)
{
    for (int value = static_cast<int>(Error::None); value <= static_cast<int>(Error::NoBobbyPins); ++value)
        EXPECT_NE(MWWorld::getFnvLockpickPreparationErrorName(static_cast<Error>(value)), "unknown");
}

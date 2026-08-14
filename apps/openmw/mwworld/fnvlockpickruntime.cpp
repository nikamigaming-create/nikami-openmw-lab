#include "fnvlockpickruntime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <components/esm/defs.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrefr.hpp>

#include "cell.hpp"
#include "cellstore.hpp"
#include "esmstore.hpp"
#include "fnvplayerruntimestate.hpp"

namespace
{
    using Placement = MWWorld::FnvLockpickConservativePlacement;

    // Frozen winning-live Fallout New Vegas Ultimate Edition corpus. The SHA
    // in the public header covers the uppercase placement IDs joined by '\n'
    // with no trailing newline. XACT and reference-level script facts are not
    // retained by the loader, so membership is a required safety boundary.
    constexpr std::array<Placement, MWWorld::FnvLockpickConservativePlacementCount> sPlacements{ {
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

    constexpr bool placementsAreSorted()
    {
        for (std::size_t i = 1; i < sPlacements.size(); ++i)
        {
            if (sPlacements[i - 1].mPlacement >= sPlacements[i].mPlacement)
                return false;
        }
        return true;
    }

    static_assert(placementsAreSorted());

    std::optional<MWWorld::PreparedFnvLockpickSession> fail(
        MWWorld::FnvLockpickPreparationError value, MWWorld::FnvLockpickPreparationError* output)
    {
        if (output != nullptr)
            *output = value;
        return std::nullopt;
    }

    bool isSupportedTier(std::uint8_t level)
    {
        return level == 0 || level == 25 || level == 50 || level == 75 || level == 100;
    }
}

namespace MWWorld
{
    class FnvLockpickSessionBuilder
    {
    public:
        static PreparedFnvLockpickSession make(ESM::FormId placement, ESM::FormId door, ESM::FormId cell,
            std::uint8_t lockLevel, float lockpickSkill, int bobbyPinCount, bool teleport)
        {
            return PreparedFnvLockpickSession(
                placement, door, cell, lockLevel, lockpickSkill, bobbyPinCount, teleport);
        }
    };

    PreparedFnvLockpickSession::PreparedFnvLockpickSession(ESM::FormId placement, ESM::FormId door,
        ESM::FormId cell, std::uint8_t lockLevel, float lockpickSkill, int bobbyPinCount, bool teleport)
        : mPlacement(placement)
        , mDoor(door)
        , mCell(cell)
        , mLockLevel(lockLevel)
        , mLockpickSkill(lockpickSkill)
        , mBobbyPinCount(bobbyPinCount)
        , mTeleport(teleport)
    {
    }

    std::span<const FnvLockpickConservativePlacement> getFnvLockpickConservativePlacements()
    {
        return sPlacements;
    }

    const FnvLockpickConservativePlacement* findFnvLockpickConservativePlacement(ESM::FormId placement)
    {
        if (!placement.hasContentFile())
            return nullptr;
        const std::uint32_t id = placement.toUint32();
        const auto found = std::lower_bound(sPlacements.begin(), sPlacements.end(), id,
            [](const Placement& candidate, std::uint32_t value) { return candidate.mPlacement < value; });
        return found != sPlacements.end() && found->mPlacement == id ? &*found : nullptr;
    }

    std::uint8_t canonicalizeFnvLockLevel(int storedLevel)
    {
        return static_cast<std::uint8_t>(storedLevel);
    }

    std::optional<PreparedFnvLockpickSession> prepareFnvLockpickSession(
        const FnvLockpickPreparationSource& source, FnvLockpickPreparationError* error)
    {
        if (error != nullptr)
            *error = FnvLockpickPreparationError::None;
        if (source.mGame != ESM4Game::FalloutNewVegas)
            return fail(FnvLockpickPreparationError::NotFalloutNewVegas, error);
        if (source.mStore == nullptr)
            return fail(FnvLockpickPreparationError::MissingStore, error);
        if (source.mTarget.isEmpty())
            return fail(FnvLockpickPreparationError::MissingTarget, error);
        if (source.mTarget.getType() != ESM::REC_DOOR4)
            return fail(FnvLockpickPreparationError::TargetIsNotDoor, error);
        if (!source.mTarget.isInCell())
            return fail(FnvLockpickPreparationError::TargetNotInCell, error);

        const ESM::FormId placement = source.mTarget.getCellRef().getRefNum();
        if (!placement.hasContentFile())
            return fail(FnvLockpickPreparationError::InvalidPlacement, error);
        const FnvLockpickConservativePlacement* frozen = findFnvLockpickConservativePlacement(placement);
        if (frozen == nullptr)
            return fail(FnvLockpickPreparationError::PlacementNotConservative, error);

        const ESMStore& store = *source.mStore;
        const ESM4::Reference* reference = store.get<ESM4::Reference>().search(placement);
        if (reference == nullptr || reference->mId != placement)
            return fail(FnvLockpickPreparationError::ReferenceNotInStore, error);
        const auto* liveDoor = source.mTarget.get<ESM4::Door>();
        if (liveDoor == nullptr || liveDoor->mBase == nullptr)
            return fail(FnvLockpickPreparationError::TargetIsNotDoor, error);
        const ESM4::Door* door = store.get<ESM4::Door>().search(ESM::RefId(liveDoor->mBase->mId));
        if (door == nullptr || door != liveDoor->mBase || reference->mBaseObj != door->mId
            || source.mTarget.getCellRef().getRefId() != ESM::RefId(door->mId))
            return fail(FnvLockpickPreparationError::DoorNotInStore, error);

        const ESM::FormId* parentId = reference->mParent.getIf<ESM::FormId>();
        const ESM4::Cell* cell = parentId != nullptr ? store.get<ESM4::Cell>().search(reference->mParent) : nullptr;
        const MWWorld::Cell* liveCell = source.mTarget.getCell()->getCell();
        if (parentId == nullptr || cell == nullptr || liveCell == nullptr || !liveCell->isEsm4()
            || &liveCell->getEsm4() != cell)
            return fail(FnvLockpickPreparationError::ParentCellNotInStore, error);

        if ((reference->mFlags & ESM4::Rec_Deleted) != 0 || (door->mFlags & ESM4::Rec_Deleted) != 0
            || (cell->mFlags & ESM4::Rec_Deleted) != 0)
            return fail(FnvLockpickPreparationError::DeletedRecord, error);
        if (source.mTarget.getBase()->isDeleted()
            || !CellStore::isAccessible(source.mTarget.getRefData(), source.mTarget.getCellRef()))
            return fail(FnvLockpickPreparationError::TargetNotLive, error);
        if (!source.mTarget.getRefData().isEnabled())
            return fail(FnvLockpickPreparationError::TargetDisabled, error);
        if ((reference->mFlags & ESM4::Rec_NoAccess) != 0)
            return fail(FnvLockpickPreparationError::NoAccess, error);
        if ((reference->mFlags & ESM4::Rec_Disabled) != 0)
            return fail(FnvLockpickPreparationError::InitiallyDisabled, error);
        if ((reference->mFlags & (ESM4::Rec_Ignored | ESM4::Rec_IgnoreObj)) != 0)
            return fail(FnvLockpickPreparationError::IgnoredReference, error);
        if (!reference->mGlobal.isZeroOrUnset() || reference->mFactionRank != -1
            || !reference->mEsp.parent.isZeroOrUnset() || reference->mEsp.flags != 0 || reference->mDoor.flags != 0
            || !reference->mDoor.transitionInterior.isZeroOrUnset())
            return fail(FnvLockpickPreparationError::UnsupportedReferenceData, error);
        if (!reference->mOwner.isZeroOrUnset() || !source.mTarget.getCellRef().getOwner().empty()
            || !cell->mOwner.isZeroOrUnset())
            return fail(FnvLockpickPreparationError::OwnedTarget, error);
        if (!door->mScriptId.isZeroOrUnset())
            return fail(FnvLockpickPreparationError::ScriptedDoor, error);
        if (door->mDoorFlags != 0 || !door->mRandomTeleport.isZeroOrUnset())
            return fail(FnvLockpickPreparationError::UnsupportedDoorData, error);
        if (!reference->mIsLocked || !source.mTarget.getCellRef().isLocked())
            return fail(FnvLockpickPreparationError::TargetNotLocked, error);
        if (!reference->mKey.isZeroOrUnset() || !source.mTarget.getCellRef().getKey().empty())
            return fail(FnvLockpickPreparationError::AuthoredKey, error);

        const int currentStoredLevel = source.mTarget.getCellRef().getLockLevel();
        if (currentStoredLevel < std::numeric_limits<std::int8_t>::min()
            || currentStoredLevel > std::numeric_limits<std::int8_t>::max())
            return fail(FnvLockpickPreparationError::UnsupportedLockLevel, error);
        const std::uint8_t authoredLevel = canonicalizeFnvLockLevel(reference->mLockLevel);
        const std::uint8_t currentLevel = canonicalizeFnvLockLevel(currentStoredLevel);
        if (authoredLevel == 255 || currentLevel == 255)
            return fail(FnvLockpickPreparationError::RequiresKey, error);
        if (!isSupportedTier(authoredLevel) || !isSupportedTier(currentLevel))
            return fail(FnvLockpickPreparationError::UnsupportedLockLevel, error);
        const bool teleport = !reference->mDoor.destDoor.isZeroOrUnset();
        if (authoredLevel != frozen->mLockLevel || currentLevel != authoredLevel || teleport != frozen->mTeleport
            || source.mTarget.getCellRef().getTeleport() != teleport)
            return fail(FnvLockpickPreparationError::PlacementShapeMismatch, error);

        if (source.mActor.isEmpty() || source.mPlayer.isEmpty())
            return fail(FnvLockpickPreparationError::MissingActor, error);
        if (source.mActor != source.mPlayer)
            return fail(FnvLockpickPreparationError::ActorIsNotPlayer, error);
        const ESM::FormId playerBaseId = ESM::FormId::fromUint32(FnvLockpickPlayerBaseFormId);
        if (source.mPlayer.getType() != ESM::REC_NPC_4 || !source.mPlayer.isInCell()
            || source.mPlayer.getBase()->isDeleted()
            || !CellStore::isAccessible(source.mPlayer.getRefData(), source.mPlayer.getCellRef())
            || !source.mPlayer.getRefData().isEnabled()
            || source.mPlayer.getCellRef().getRefId() != ESM::RefId(playerBaseId))
            return fail(FnvLockpickPreparationError::PlayerStateMismatch, error);
        const LiveCellRef<ESM4::Npc>* livePlayer = source.mPlayer.get<ESM4::Npc>();
        const ESM4::Npc* playerBase = store.get<ESM4::Npc>().search(ESM::RefId(playerBaseId));
        if (livePlayer == nullptr || livePlayer->mBase == nullptr || playerBase == nullptr || livePlayer->mBase != playerBase
            || playerBase->mId != playerBaseId || playerBase->mEditorId != "Player"
            || (playerBase->mFlags & ESM4::Rec_Deleted) != 0)
            return fail(FnvLockpickPreparationError::PlayerStateMismatch, error);
        if (source.mInventory == nullptr)
            return fail(FnvLockpickPreparationError::MissingInventory, error);
        if (!source.mInventory->prepareForActor(source.mActor)
            || !source.mInventory->stillBelongsTo(source.mActor))
            return fail(FnvLockpickPreparationError::InventoryMismatch, error);
        if (source.mPlayerState == nullptr || !source.mPlayerState->isInitialized()
            || !source.mPlayerState->getBaseState())
            return fail(FnvLockpickPreparationError::PlayerStateUninitialized, error);
        if (source.mPlayerState->getBaseState()->mBaseRecord != playerBaseId)
            return fail(FnvLockpickPreparationError::PlayerStateMismatch, error);

        const std::optional<FalloutRuntimeActorValue> skill
            = source.mPlayerState->getCurrentActorValue(FnvLockpickActorValue);
        if (!skill || !std::isfinite(skill->mValue))
            return fail(FnvLockpickPreparationError::UnsupportedSkill, error);
        if (skill->mRawSkillOffset && *skill->mRawSkillOffset != 0)
            return fail(FnvLockpickPreparationError::UnresolvedSkillOffset, error);
        if (skill->mValue < static_cast<float>(authoredLevel))
            return fail(FnvLockpickPreparationError::InsufficientSkill, error);

        const ESM::RefId bobbyPinId(ESM::FormId::fromUint32(FnvLockpickBobbyPinFormId));
        const ESM4::MiscItem* bobbyPin = store.get<ESM4::MiscItem>().search(bobbyPinId);
        if (bobbyPin == nullptr)
            return fail(FnvLockpickPreparationError::BobbyPinNotInStore, error);
        if (bobbyPin->mId != ESM::FormId::fromUint32(FnvLockpickBobbyPinFormId)
            || bobbyPin->mEditorId != "Lockpick" || (bobbyPin->mFlags & ESM4::Rec_Deleted) != 0
            || !bobbyPin->mScriptId.isZeroOrUnset())
            return fail(FnvLockpickPreparationError::InvalidBobbyPinRecord, error);
        const std::optional<std::int64_t> count = source.mInventory->getCount(bobbyPinId);
        if (!count || *count < 0 || *count > std::numeric_limits<int>::max())
            return fail(FnvLockpickPreparationError::InvalidInventory, error);
        if (*count == 0)
            return fail(FnvLockpickPreparationError::NoBobbyPins, error);

        return FnvLockpickSessionBuilder::make(
            placement, door->mId, *parentId, authoredLevel, skill->mValue, static_cast<int>(*count), teleport);
    }

    std::string_view getFnvLockpickPreparationErrorName(FnvLockpickPreparationError error)
    {
        switch (error)
        {
            case FnvLockpickPreparationError::None:
                return "none";
            case FnvLockpickPreparationError::NotFalloutNewVegas:
                return "not-fallout-new-vegas";
            case FnvLockpickPreparationError::MissingStore:
                return "missing-store";
            case FnvLockpickPreparationError::MissingTarget:
                return "missing-target";
            case FnvLockpickPreparationError::TargetIsNotDoor:
                return "target-is-not-door";
            case FnvLockpickPreparationError::TargetNotInCell:
                return "target-not-in-cell";
            case FnvLockpickPreparationError::InvalidPlacement:
                return "invalid-placement";
            case FnvLockpickPreparationError::PlacementNotConservative:
                return "placement-not-conservative";
            case FnvLockpickPreparationError::ReferenceNotInStore:
                return "reference-not-in-store";
            case FnvLockpickPreparationError::DoorNotInStore:
                return "door-not-in-store";
            case FnvLockpickPreparationError::ParentCellNotInStore:
                return "parent-cell-not-in-store";
            case FnvLockpickPreparationError::DeletedRecord:
                return "deleted-record";
            case FnvLockpickPreparationError::TargetNotLive:
                return "target-not-live";
            case FnvLockpickPreparationError::TargetDisabled:
                return "target-disabled";
            case FnvLockpickPreparationError::NoAccess:
                return "no-access";
            case FnvLockpickPreparationError::InitiallyDisabled:
                return "initially-disabled";
            case FnvLockpickPreparationError::IgnoredReference:
                return "ignored-reference";
            case FnvLockpickPreparationError::UnsupportedReferenceData:
                return "unsupported-reference-data";
            case FnvLockpickPreparationError::OwnedTarget:
                return "owned-target";
            case FnvLockpickPreparationError::ScriptedDoor:
                return "scripted-door";
            case FnvLockpickPreparationError::UnsupportedDoorData:
                return "unsupported-door-data";
            case FnvLockpickPreparationError::TargetNotLocked:
                return "target-not-locked";
            case FnvLockpickPreparationError::AuthoredKey:
                return "authored-key";
            case FnvLockpickPreparationError::RequiresKey:
                return "requires-key";
            case FnvLockpickPreparationError::UnsupportedLockLevel:
                return "unsupported-lock-level";
            case FnvLockpickPreparationError::PlacementShapeMismatch:
                return "placement-shape-mismatch";
            case FnvLockpickPreparationError::MissingActor:
                return "missing-actor";
            case FnvLockpickPreparationError::ActorIsNotPlayer:
                return "actor-is-not-player";
            case FnvLockpickPreparationError::MissingInventory:
                return "missing-inventory";
            case FnvLockpickPreparationError::InventoryMismatch:
                return "inventory-mismatch";
            case FnvLockpickPreparationError::PlayerStateUninitialized:
                return "player-state-uninitialized";
            case FnvLockpickPreparationError::PlayerStateMismatch:
                return "player-state-mismatch";
            case FnvLockpickPreparationError::UnsupportedSkill:
                return "unsupported-skill";
            case FnvLockpickPreparationError::UnresolvedSkillOffset:
                return "unresolved-skill-offset";
            case FnvLockpickPreparationError::InsufficientSkill:
                return "insufficient-skill";
            case FnvLockpickPreparationError::BobbyPinNotInStore:
                return "bobby-pin-not-in-store";
            case FnvLockpickPreparationError::InvalidBobbyPinRecord:
                return "invalid-bobby-pin-record";
            case FnvLockpickPreparationError::InvalidInventory:
                return "invalid-inventory";
            case FnvLockpickPreparationError::NoBobbyPins:
                return "no-bobby-pins";
        }
        return "unknown";
    }
}

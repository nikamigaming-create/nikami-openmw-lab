#ifndef OPENMW_MWWORLD_FNVLOCKPICKRUNTIME_H
#define OPENMW_MWWORLD_FNVLOCKPICKRUNTIME_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <components/esm/formid.hpp>
#include <components/esm/refid.hpp>

#include "ptr.hpp"

namespace MWWorld
{
    class ESMStore;
    enum class ESM4Game;
    class FalloutPlayerRuntimeState;

    inline constexpr std::uint32_t FnvLockpickPlayerBaseFormId = 0x00000007;
    inline constexpr std::uint32_t FnvLockpickBobbyPinFormId = 0x0000000a;
    inline constexpr std::uint32_t FnvLockpickActorValue = 36;
    inline constexpr std::size_t FnvLockpickConservativePlacementCount = 84;
    inline constexpr std::string_view FnvLockpickConservativePlacementIdSha256
        = "305b7717827054620b5e075a5ad3cf660493389839173702705c25426f32e028";

    struct FnvLockpickConservativePlacement
    {
        std::uint32_t mPlacement = 0;
        std::uint8_t mLockLevel = 0;
        bool mTeleport = false;

        bool operator==(const FnvLockpickConservativePlacement&) const = default;
    };

    enum class FnvLockpickPreparationError
    {
        None,
        NotFalloutNewVegas,
        MissingStore,
        MissingTarget,
        TargetIsNotDoor,
        TargetNotInCell,
        InvalidPlacement,
        PlacementNotConservative,
        ReferenceNotInStore,
        DoorNotInStore,
        ParentCellNotInStore,
        DeletedRecord,
        TargetNotLive,
        TargetDisabled,
        NoAccess,
        InitiallyDisabled,
        IgnoredReference,
        UnsupportedReferenceData,
        OwnedTarget,
        ScriptedDoor,
        UnsupportedDoorData,
        TargetNotLocked,
        AuthoredKey,
        RequiresKey,
        UnsupportedLockLevel,
        PlacementShapeMismatch,
        MissingActor,
        ActorIsNotPlayer,
        MissingInventory,
        InventoryMismatch,
        PlayerStateUninitialized,
        PlayerStateMismatch,
        UnsupportedSkill,
        UnresolvedSkillOffset,
        InsufficientSkill,
        BobbyPinNotInStore,
        InvalidBobbyPinRecord,
        InvalidInventory,
        NoBobbyPins,
    };

    /// Read-only inventory boundary for lockpick preparation. Implementations
    /// may resolve a live store but must not consume, create, or reorder items.
    class FnvLockpickInventory
    {
    public:
        virtual ~FnvLockpickInventory() = default;

        virtual bool prepareForActor(const Ptr& actor) noexcept = 0;
        virtual bool stillBelongsTo(const Ptr& actor) const noexcept = 0;
        virtual std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept = 0;
    };

    struct FnvLockpickPreparationSource
    {
        ESM4Game mGame;
        const ESMStore* mStore = nullptr;
        Ptr mTarget;
        Ptr mActor;
        Ptr mPlayer;
        FnvLockpickInventory* mInventory = nullptr;
        const FalloutPlayerRuntimeState* mPlayerState = nullptr;
    };

    class FnvLockpickSessionBuilder;

    /// Immutable preparation snapshot. It deliberately contains no input
    /// model, success roll, item mutation, unlock operation, or UI callback.
    class PreparedFnvLockpickSession final
    {
        ESM::FormId mPlacement;
        ESM::FormId mDoor;
        ESM::FormId mCell;
        std::uint8_t mLockLevel = 0;
        float mLockpickSkill = 0.f;
        int mBobbyPinCount = 0;
        bool mTeleport = false;

        PreparedFnvLockpickSession(ESM::FormId placement, ESM::FormId door, ESM::FormId cell,
            std::uint8_t lockLevel, float lockpickSkill, int bobbyPinCount, bool teleport);

        friend class FnvLockpickSessionBuilder;

    public:
        ESM::FormId getPlacement() const { return mPlacement; }
        ESM::FormId getDoor() const { return mDoor; }
        ESM::FormId getCell() const { return mCell; }
        std::uint8_t getLockLevel() const { return mLockLevel; }
        float getLockpickSkill() const { return mLockpickSkill; }
        int getBobbyPinCount() const { return mBobbyPinCount; }
        bool isTeleport() const { return mTeleport; }
    };

    [[nodiscard]] std::span<const FnvLockpickConservativePlacement> getFnvLockpickConservativePlacements();
    [[nodiscard]] const FnvLockpickConservativePlacement* findFnvLockpickConservativePlacement(
        ESM::FormId placement);
    [[nodiscard]] std::uint8_t canonicalizeFnvLockLevel(int storedLevel);

    /// Validate the exact frozen official placement, winning authored records,
    /// live target, player actor value, and official bobby-pin inventory. This
    /// function performs no world or inventory mutation.
    [[nodiscard]] std::optional<PreparedFnvLockpickSession> prepareFnvLockpickSession(
        const FnvLockpickPreparationSource& source, FnvLockpickPreparationError* error = nullptr);

    std::string_view getFnvLockpickPreparationErrorName(FnvLockpickPreparationError error);
}

#endif

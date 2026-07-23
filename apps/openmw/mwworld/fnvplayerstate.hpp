#ifndef OPENMW_MWWORLD_FNVPLAYERSTATE_H
#define OPENMW_MWWORLD_FNVPLAYERSTATE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/esm/formid.hpp>
#include <components/esm4/actor.hpp>

#include "esm4questruntime.hpp"

namespace ESM
{
    struct NPC;
}

namespace ESM4
{
    struct Ammunition;
    struct Cell;
    struct Class;
    struct FONVSaveGamePrefix;
    struct FormIdList;
    struct Npc;
    struct Race;
    struct Faction;
    struct World;
}

namespace MWWorld
{
    template <class T>
    class Store;

    enum class FalloutSpecial : std::size_t
    {
        Strength,
        Perception,
        Endurance,
        Charisma,
        Intelligence,
        Agility,
        Luck,
        Count,
    };

    enum class FalloutSkill : std::size_t
    {
        Barter,
        BigGuns,
        EnergyWeapons,
        Explosives,
        Lockpick,
        Medicine,
        MeleeWeapons,
        Repair,
        Science,
        SmallGuns,
        Sneak,
        Speech,
        SurvivalOrThrowing,
        Unarmed,
        Count,
    };

    struct FalloutActorValueComponents
    {
        // The exact authored component. For skills this is the first DNAM byte array, not a derived total.
        double mValue = 0.0;

        // FNV stores a second raw DNAM byte for every skill. Its signedness/application is deliberately not
        // inferred here; consumers that understand the retail rule can use this exact byte separately.
        std::optional<std::uint8_t> mRawSkillOffset;
    };

    struct FalloutInventoryItem
    {
        ESM::FormId mRecord;
        std::int32_t mCount = 0;

        bool operator==(const FalloutInventoryItem&) const = default;
    };

    struct FalloutPlayerState
    {
        static constexpr std::size_t SpecialCount = static_cast<std::size_t>(FalloutSpecial::Count);
        static constexpr std::size_t SkillCount = static_cast<std::size_t>(FalloutSkill::Count);

        ESM::FormId mBaseRecord;
        ESM::FormId mReferenceRecord;
        ESM::FormId mReferenceBaseRecord;
        ESM::FormId mTraitsRecord;
        ESM::FormId mStatsRecord;
        ESM::FormId mFactionsRecord;
        ESM::FormId mAIDataRecord;
        ESM::FormId mModelRecord;
        ESM::FormId mBaseDataRecord;
        ESM::FormId mInventoryRecord;

        std::string mEditorId;
        std::string mFullName;
        std::string mModel;
        ESM::FormId mRace;
        ESM::FormId mClass;
        ESM::FormId mHair;
        ESM::FormId mEyes;
        ESM::FormId mVoiceType;

        std::uint32_t mRecordFlags = 0;
        std::vector<ESM4::ActorFaction> mFactions;
        std::vector<FalloutInventoryItem> mInventoryItems;
        ESM4::ACBS_FO3 mStatsConfig{};
        ESM4::AIDataFO3 mAIData{};
        std::int32_t mHealth = 0;
        std::array<std::uint8_t, SpecialCount> mSpecial{};
        std::array<std::uint8_t, SkillCount> mSkillValues{};
        std::array<std::uint8_t, SkillCount> mSkillOffsets{};

        std::uint8_t getSpecial(FalloutSpecial value) const;
        std::uint8_t getSkillValue(FalloutSkill value) const;
        std::uint8_t getSkillOffset(FalloutSkill value) const;

        // FNV actor-value indices supported by exact NPC_ payloads: AI data 0-4, SPECIAL 5-11, health 16,
        // and skills 32-45. Unsupported/derived actor values fail closed with std::nullopt.
        std::optional<FalloutActorValueComponents> getActorValueComponents(std::uint32_t actorValue) const;
    };

    struct FalloutPlayerStateResolution
    {
        std::optional<FalloutPlayerState> mState;
        std::string mError;

        explicit operator bool() const { return mState.has_value(); }
    };

    FalloutPlayerStateResolution resolveFalloutPlayerState(
        const Store<ESM4::Npc>& npcs, ESM::FormId normalizedPlayerFormId);

    FalloutPlayerStateResolution resolveFalloutPlayerIdentity(
        const Store<ESM4::Npc>& npcs, ESM::FormId normalizedPlayerFormId, ESM::FormId normalizedPlayerReferenceFormId);

    // FNV's Player NPC_ gives compatible ammunition through an authored FLST. Retail materializes the first AMMO
    // member as the initial inventory stack, then applies save deltas to concrete AMMO records. Resolve that carrier
    // before creating the temporary ESM3 Player inventory so a FLST is never mistaken for an item.
    FalloutPlayerStateResolution resolveFalloutPlayerInventoryFormLists(FalloutPlayerState state,
        const Store<ESM4::FormIdList>& formLists, const Store<ESM4::Ammunition>& ammunition);

    struct FalloutNativePlayerRecords
    {
        const ESM4::Npc* mBaseNpc = nullptr;
        const ESM4::Class* mClass = nullptr;
        const ESM4::Race* mRace = nullptr;
    };

    struct FalloutNativePlayerRecordsResolution
    {
        std::optional<FalloutNativePlayerRecords> mRecords;
        std::string mError;

        explicit operator bool() const { return mRecords.has_value(); }
    };

    FalloutNativePlayerRecordsResolution resolveFalloutNativePlayerRecords(const Store<ESM4::Npc>& npcs,
        const Store<ESM4::Class>& classes, const Store<ESM4::Race>& races, const FalloutPlayerState& playerState);

    // Only fields with an exact meaning in the currently decoded retail save header belong here. In particular, the
    // location string is a display label, not a cell identity or permission to move the player.
    struct FalloutSavePlayerHeaderState
    {
        struct ConditionedStack
        {
            ESM::FormId mRecord;
            std::int32_t mCount = 0;
            float mHealth = 0.f;
            std::uint64_t mSourceOffset = 0;
        };

        struct WornVisualItem
        {
            ESM::FormId mRecord;
            std::optional<float> mHealth;
            std::uint64_t mSourceOffset = 0;
        };

        struct HotkeyItem
        {
            std::uint8_t mIndex = 0;
            ESM::FormId mRecord;
            std::uint64_t mSourceOffset = 0;
        };

        struct AmmoSelection
        {
            ESM::FormId mWeapon;
            ESM::FormId mAmmo;
            std::int32_t mSavedCount = 0;
            std::uint64_t mSourceOffset = 0;
        };

        struct FactionChange
        {
            ESM::FormId mFaction;
            std::int8_t mRank = -1;
            std::uint64_t mSourceOffset = 0;

            bool operator==(const FactionChange&) const = default;
        };

        enum class ActorValueModifierKind : std::uint8_t
        {
            Permanent,
            Damage,
            Temporary,
        };

        struct ActorValueModifier
        {
            std::uint8_t mActorValue = 0;
            float mModifier = 0.f;
            ActorValueModifierKind mKind = ActorValueModifierKind::Permanent;
            std::uint64_t mSourceOffset = 0;

            bool operator==(const ActorValueModifier&) const = default;
        };

        struct PerkRank
        {
            ESM::FormId mPerk;
            // Exact PlayerCharacter list byte. The retail list is authoritative for possession; callers that need
            // rank arithmetic must keep its zero-based representation distinct from user-facing rank numbers.
            std::uint8_t mRankByte = 0;
            bool mAlternate = false;
            std::uint64_t mSourceOffset = 0;

            bool operator==(const PerkRank&) const = default;
        };

        ESM::FormId mBaseRecord;
        ESM::FormId mReferenceRecord;
        std::size_t mSaveFalloutNewVegasMasterIndex = 0;
        std::size_t mCurrentFalloutNewVegasMasterIndex = 0;
        std::uint32_t mReferenceChangeFlags = 0;
        std::uint64_t mReferencePayloadOffset = 0;
        std::uint64_t mReferencePayloadBytes = 0;
        std::uint32_t mSaveNumber = 0;
        std::string mName;
        std::string mKarmaTitle;
        std::uint32_t mLevel = 0;
        std::string mLocationLabel;
        std::string mPlayTimeLabel;
        std::int8_t mProcessLevel = -1;
        bool mWeaponDrawn = false;
        std::int16_t mCurrentWeaponAction = -1;
        std::uint64_t mCurrentWeaponActionSourceOffset = 0;
        std::vector<FalloutInventoryItem> mInventoryItems;
        std::vector<ConditionedStack> mConditionedStacks;
        std::vector<WornVisualItem> mWornVisualItems;
        std::vector<HotkeyItem> mHotkeyItems;
        std::vector<AmmoSelection> mAmmoSelections;
        std::vector<FactionChange> mFactionChanges;
        std::vector<ActorValueModifier> mActorValueModifiers;
        std::vector<PerkRank> mPerks;
    };

    struct FalloutSaveLoadPlan
    {
        struct GlobalValue
        {
            ESM::FormId mVariable;
            float mValue = 0.f;
            std::uint64_t mSourceOffset = 0;

            bool operator==(const GlobalValue&) const = default;
        };

        struct FactionRelation
        {
            ESM::FormId mFaction;
            std::int32_t mModifier = 0;
            std::uint32_t mGroupCombatReaction = 0;

            bool operator==(const FactionRelation&) const = default;
        };

        struct FactionState
        {
            ESM::FormId mFaction;
            std::vector<FactionRelation> mRelations;
        };

        struct WorldReferenceTransform
        {
            ESM::FormId mReference;
            std::uint8_t mChangeType = 0;
            ESM::FormId mCellOrWorldspace;
            std::array<float, 3> mPosition{};
            std::array<float, 3> mRotationRadians{};
            std::uint64_t mSourceOffset = 0;
        };

        FalloutSavePlayerHeaderState mPlayer;
        struct PlayerTransform
        {
            ESM::FormId mCellOrWorldspaceRecord;
            std::array<float, 3> mPosition{};
            std::array<float, 3> mRotationRadians{};
        } mTransform;
        struct CameraState
        {
            std::uint8_t mThirdPersonMode = 0;
            bool mFirstPerson = true;
            float mFirstPersonModelFov = 0.f;
            float mWorldFov = 0.f;
            std::uint64_t mModeOffset = 0;
            std::uint64_t mFirstPersonModelFovOffset = 0;
            std::uint64_t mWorldFovOffset = 0;
        } mCamera;
        struct SceneState
        {
            ESM::FormId mCurrentWeather;
            std::optional<ESM::FormId> mTransitionWeather;
            ESM::FormId mDefaultWeather;
            std::optional<ESM::FormId> mOverrideWeather;
            float mGameHour = 0.f;
            float mLastUpdateHour = 0.f;
            float mWeatherPercent = 0.f;
            float mFogPower = 0.f;
            std::uint32_t mFlags = 0;
            std::uint32_t mSkyMode = 0;
            std::uint64_t mPayloadOffset = 0;
            std::uint64_t mPayloadBytes = 0;
        } mScene;
        std::vector<GlobalValue> mGlobals;
        std::vector<FactionState> mFactions;
        std::vector<WorldReferenceTransform> mWorldReferenceTransforms;
        std::optional<ESM4SavedQuestProgress> mQuestProgress;
        std::vector<std::string> mUncoveredState;
    };

    struct FalloutExteriorPlayerPlacement
    {
        ESM::FormId mWorldspaceRecord;
        ESM::FormId mCellRecord;
        int mCellX = 0;
        int mCellY = 0;
    };

    struct FalloutExteriorPlayerPlacementResolution
    {
        std::optional<FalloutExteriorPlayerPlacement> mPlacement;
        std::string mError;

        explicit operator bool() const { return mPlacement.has_value(); }
    };

    bool targetsFalloutExteriorCell(const FalloutSaveLoadPlan::WorldReferenceTransform& transform,
        const FalloutExteriorPlayerPlacement& placement);

    struct FalloutSaveLoadPlanResolution
    {
        std::optional<FalloutSaveLoadPlan> mPlan;
        std::string mError;

        explicit operator bool() const { return mPlan.has_value(); }
    };

    FalloutSaveLoadPlanResolution resolveFalloutSaveLoadPlan(const ESM4::FONVSaveGamePrefix& save,
        const FalloutPlayerState* nativePlayerState, const Store<ESM4::FormIdList>& formLists,
        const Store<ESM4::Ammunition>& ammunition, std::span<const std::string> currentContentFiles);

    // Bethesda saves horizontal degrees at a 4:3 reference aspect; OpenMW consumes vertical fovy degrees.
    float convertFalloutReferenceFovToOpenMwVertical(float horizontalReferenceFov);

    FalloutExteriorPlayerPlacementResolution resolveFalloutExteriorPlayerPlacement(const Store<ESM4::World>& worlds,
        const Store<ESM4::Cell>& cells, const FalloutSaveLoadPlan::PlayerTransform& transform);

    // Apply exact name/level fields plus normalized positive inventory totals to the ESM3 compatibility carrier.
    // Per-instance condition and ExtraWorn remain separate runtime signals because ESM::InventoryList cannot carry
    // either property.
    void applyFalloutSavePlayerHeader(ESM::NPC& proxy, const FalloutSavePlayerHeaderState& state);

    // Apply the save's exact ExtraFactionChanges overlay to the authored Player faction list. Negative ranks are
    // the retail removal sentinel; non-negative ranks add or replace a membership.
    void applyFalloutSavePlayerFactionChanges(
        FalloutPlayerState& player, std::span<const FalloutSavePlayerHeaderState::FactionChange> changes);

    // Replace the authored relation list with the exact current list serialized by CHANGE_FACTION_REACTIONS.
    void applyFalloutSaveFactionState(ESM4::Faction& faction, const FalloutSaveLoadPlan::FactionState& state);

    // Seed only fields that have an explicit same-unit shared representation, including positive authored inventory
    // counts. The ESM3 proxy remains a compatibility carrier: its 0-100 attributes and 27 skills must retain their
    // existing compatibility values. FalloutPlayerState is authoritative and retains all FNV SPECIAL/skills.
    void seedFalloutPlayerProxy(ESM::NPC& proxy, const FalloutPlayerState& state);
}

#endif

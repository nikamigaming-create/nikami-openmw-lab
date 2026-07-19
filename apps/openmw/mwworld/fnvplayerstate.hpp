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

namespace ESM
{
    struct NPC;
}

namespace ESM4
{
    struct Cell;
    struct Class;
    struct FONVSaveGamePrefix;
    struct Npc;
    struct Race;
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
        struct WornVisualItem
        {
            ESM::FormId mRecord;
            std::uint64_t mSourceOffset = 0;
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
        std::vector<FalloutInventoryItem> mInventoryItems;
        std::vector<WornVisualItem> mWornVisualItems;
    };

    struct FalloutSaveLoadPlan
    {
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

    struct FalloutSaveLoadPlanResolution
    {
        std::optional<FalloutSaveLoadPlan> mPlan;
        std::string mError;

        explicit operator bool() const { return mPlan.has_value(); }
    };

    FalloutSaveLoadPlanResolution resolveFalloutSaveLoadPlan(const ESM4::FONVSaveGamePrefix& save,
        const FalloutPlayerState* nativePlayerState, std::span<const std::string> currentContentFiles);

    // Bethesda saves horizontal degrees at a 4:3 reference aspect; OpenMW consumes vertical fovy degrees.
    float convertFalloutReferenceFovToOpenMwVertical(float horizontalReferenceFov);

    FalloutExteriorPlayerPlacementResolution resolveFalloutExteriorPlayerPlacement(const Store<ESM4::World>& worlds,
        const Store<ESM4::Cell>& cells, const FalloutSaveLoadPlan::PlayerTransform& transform);

    // Apply exact name/level fields plus normalized positive inventory totals to the ESM3 compatibility carrier.
    // ExtraWorn remains a separate visual signal because per-instance condition and equipment semantics are not
    // represented by ESM::InventoryList.
    void applyFalloutSavePlayerHeader(ESM::NPC& proxy, const FalloutSavePlayerHeaderState& state);

    // Seed only fields that have an explicit same-unit shared representation, including positive authored inventory
    // counts. The ESM3 proxy remains a compatibility carrier: its 0-100 attributes and 27 skills must retain their
    // existing compatibility values. FalloutPlayerState is authoritative and retains all FNV SPECIAL/skills.
    void seedFalloutPlayerProxy(ESM::NPC& proxy, const FalloutPlayerState& state);
}

#endif

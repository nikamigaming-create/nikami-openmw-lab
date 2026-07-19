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
    struct ActorCharacter;
    struct Cell;
    struct FONVSaveGamePrefix;
    struct Npc;
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
        double mValue = 0.0;
        std::optional<std::uint8_t> mRawSkillOffset;
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
        ESM::FormId mAIDataRecord;
        ESM::FormId mModelRecord;
        ESM::FormId mBaseDataRecord;

        std::string mEditorId;
        std::string mFullName;
        std::string mModel;
        ESM::FormId mRace;
        ESM::FormId mClass;
        ESM::FormId mHair;
        ESM::FormId mEyes;
        ESM::FormId mVoiceType;

        std::uint32_t mRecordFlags = 0;
        ESM4::ACBS_FO3 mStatsConfig{};
        ESM4::AIDataFO3 mAIData{};
        std::int32_t mHealth = 0;
        std::array<std::uint8_t, SpecialCount> mSpecial{};
        std::array<std::uint8_t, SkillCount> mSkillValues{};
        std::array<std::uint8_t, SkillCount> mSkillOffsets{};

        std::uint8_t getSpecial(FalloutSpecial value) const;
        std::uint8_t getSkillValue(FalloutSkill value) const;
        std::uint8_t getSkillOffset(FalloutSkill value) const;
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

    FalloutPlayerStateResolution resolveFalloutPlayerIdentity(const Store<ESM4::Npc>& npcs,
        const Store<ESM4::ActorCharacter>& actorReferences, ESM::FormId normalizedPlayerFormId,
        ESM::FormId normalizedPlayerReferenceFormId);

    // Only fields with an exact meaning in the currently decoded retail save header belong here. In particular, the
    // location string is a display label, not a cell identity or permission to move the player.
    struct FalloutSavePlayerHeaderState
    {
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

    FalloutExteriorPlayerPlacementResolution resolveFalloutExteriorPlayerPlacement(
        const Store<ESM4::World>& worlds, const Store<ESM4::Cell>& cells,
        const FalloutSaveLoadPlan::PlayerTransform& transform);

    // Apply only a non-empty player name and the runtime level carried with exact semantics by the FNV save header.
    // Save330's header name is empty, so that case preserves the content-derived carrier name. This must not project
    // the display-only location string or invent health, actor values, inventory, faction, quest, or world state.
    void applyFalloutSavePlayerHeader(ESM::NPC& proxy, const FalloutSavePlayerHeaderState& state);

    // OpenMW's player/save machinery still needs an ESM3 carrier. Seed only fields
    // with the same units and meaning; exact SPECIAL and skills remain in the native state.
    void seedFalloutPlayerProxy(ESM::NPC& proxy, const FalloutPlayerState& state);
}

#endif

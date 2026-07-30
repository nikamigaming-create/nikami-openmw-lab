#ifndef OPENMW_ESM_CREATURESTATS_H
#define OPENMW_ESM_CREATURESTATS_H

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "statstate.hpp"

#include "components/esm/defs.hpp"

#include "activespells.hpp"
#include "aisequence.hpp"
#include "components/esm/attr.hpp"
#include "components/esm/refid.hpp"
#include "magiceffects.hpp"
#include "spellstate.hpp"
#include "timestamp.hpp"

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    // format 0, saved games only
    struct CreatureStats
    {
        struct CorprusStats
        {
            std::array<int32_t, Attribute::Length> mWorsenings;
            TimeStamp mNextWorsening;
        };

        std::array<StatState<float>, Attribute::Length> mAttributes;
        std::array<StatState<float>, 3> mDynamic;

        MagicEffects mMagicEffects;

        AiSequence::AiSequence mAiSequence;

        bool mHasAiSettings;
        std::array<StatState<int>, 4> mAiSettings;

        std::map<SummonKey, int> mSummonedCreatureMap;
        std::multimap<ESM::RefId, RefNum> mSummonedCreatures;
        std::vector<int> mSummonGraveyard;

        TimeStamp mTradeTime;
        int32_t mGoldPool;
        int32_t mActorId;

        enum Flags
        {
            Dead = 0x0001,
            DeathAnimationFinished = 0x0002,
            Died = 0x0004,
            Murdered = 0x0008,
            TalkedTo = 0x0010,
            Alarmed = 0x0020,
            Attacked = 0x0040,
            Knockdown = 0x0080,
            KnockdownOneFrame = 0x0100,
            KnockdownOverOneFrame = 0x0200,
            HitRecovery = 0x0400,
            Block = 0x0800,
            RecalcDynamicStats = 0x1000
        };
        bool mDead;
        bool mDeathAnimationFinished;
        bool mDied;
        bool mMurdered;
        bool mTalkedTo;
        bool mAlarmed;
        bool mAttacked;
        bool mKnockdown;
        bool mKnockdownOneFrame;
        bool mKnockdownOverOneFrame;
        bool mHitRecovery;
        bool mBlock;
        uint32_t mMovementFlags;
        float mFallHeight;
        ESM::RefId mLastHitObject;
        ESM::RefId mLastHitAttemptObject;
        bool mRecalcDynamicStats;
        int32_t mDrawState;
        int8_t mDeathAnimation;
        TimeStamp mTimeOfDeath;
        int32_t mLevel;
        bool mMissingACDT;

        // Optional FLMB data keeps accumulated native Fallout limb damage across saves.
        std::array<float, 7> mFalloutLimbDamage{};

        // Per-reference Fallout actor-value and faction changes made by quest scripts. Actor values use their
        // native numeric AV index. A faction rank of FalloutFactionRemoved records an explicit removal from the
        // authored base/template faction list; every other value must fit the signed rank byte used by ESM4.
        static constexpr std::int16_t FalloutFactionRemoved = -129;
        std::map<std::uint8_t, float> mFalloutActorValueOverrides;
        std::map<ESM::FormId, std::int16_t> mFalloutFactionOverrides;
        // Per-reference Fallout flags changed by actor script commands. Equipment overrides preserve the complete
        // equipped set after the first EquipItem/UnequipItem/RemoveAllItems mutation; the explicit presence bit
        // distinguishes "authored base equipment" from an intentionally empty equipped set.
        std::uint32_t mFalloutRuntimeFlags = 0;
        std::optional<ESM::FormId> mFalloutLookTarget;
        bool mFalloutLookRotateBody = false;
        bool mHasFalloutEquipmentOverride = false;
        std::vector<ESM::FormId> mFalloutEquippedItems;
        // AddSpell/RemoveSpell mutate the effective ESM4 SPEL list on one placed actor. The presence bit
        // distinguishes an explicitly empty runtime list from the immutable NPC_/CREA template list.
        bool mHasFalloutActorEffectOverride = false;
        std::vector<ESM::FormId> mFalloutActorEffects;
        // SetActorFullName is likewise per-reference. MESG.FULL is copied here so the save never retains a
        // pointer into a content store whose winning record may change after load-order remapping.
        std::optional<std::string> mFalloutFullName;

        std::map<ESM::RefId, CorprusStats> mCorprusSpells;
        SpellState mSpells;
        ActiveSpells mActiveSpells;

        /// Initialize to default state
        void blank();

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };
}

#endif

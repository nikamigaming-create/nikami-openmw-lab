#include "creaturestats.hpp"
#include "esmreader.hpp"
#include "esmwriter.hpp"

#include <components/esm3/loadmgef.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ESM
{

    void CreatureStats::load(ESMReader& esm)
    {
        const bool intFallback = esm.getFormatVersion() <= MaxIntFallbackFormatVersion;
        for (auto& attribute : mAttributes)
            attribute.load(esm, intFallback);

        for (auto& dynamic : mDynamic)
            dynamic.load(esm);

        mGoldPool = 0;
        esm.getHNOT(mGoldPool, "GOLD");

        mTradeTime.mDay = 0;
        mTradeTime.mHour = 0;
        if (esm.peekNextSub("TIME"))
            mTradeTime.load(esm);

        int32_t flags = 0;
        mDead = false;
        mDeathAnimationFinished = false;
        mDied = false;
        mMurdered = false;
        mTalkedTo = false;
        mAlarmed = false;
        mAttacked = false;
        mKnockdown = false;
        mKnockdownOneFrame = false;
        mKnockdownOverOneFrame = false;
        mHitRecovery = false;
        mBlock = false;
        mRecalcDynamicStats = false;
        if (esm.getFormatVersion() <= MaxUnoptimizedCharacterDataFormatVersion)
        {
            esm.getHNOT(mDead, "DEAD");
            esm.getHNOT(mDeathAnimationFinished, "DFNT");
            esm.getHNOT(mDied, "DIED");
            esm.getHNOT(mMurdered, "MURD");
            esm.getHNOT(mTalkedTo, "TALK");
            esm.getHNOT(mAlarmed, "ALRM");
            esm.getHNOT(mAttacked, "ATKD");
            esm.getHNOT(mKnockdown, "KNCK");
            esm.getHNOT(mKnockdownOneFrame, "KNC1");
            esm.getHNOT(mKnockdownOverOneFrame, "KNCO");
            esm.getHNOT(mHitRecovery, "HITR");
            esm.getHNOT(mBlock, "BLCK");
        }
        else
        {
            esm.getHNOT(flags, "AFLG");
            mDead = flags & Dead;
            mDeathAnimationFinished = flags & DeathAnimationFinished;
            mDied = flags & Died;
            mMurdered = flags & Murdered;
            mTalkedTo = flags & TalkedTo;
            mAlarmed = flags & Alarmed;
            mAttacked = flags & Attacked;
            mKnockdown = flags & Knockdown;
            mKnockdownOneFrame = flags & KnockdownOneFrame;
            mKnockdownOverOneFrame = flags & KnockdownOverOneFrame;
            mHitRecovery = flags & HitRecovery;
            mBlock = flags & Block;
            mRecalcDynamicStats = flags & RecalcDynamicStats;
        }

        mMovementFlags = 0;
        esm.getHNOT(mMovementFlags, "MOVE");

        mFallHeight = 0;
        esm.getHNOT(mFallHeight, "FALL");

        mLastHitObject = esm.getHNORefId("LHIT");

        mLastHitAttemptObject = esm.getHNORefId("LHAT");

        if (esm.getFormatVersion() <= MaxUnoptimizedCharacterDataFormatVersion)
            esm.getHNOT(mRecalcDynamicStats, "CALC");

        mDrawState = 0;
        esm.getHNOT(mDrawState, "DRAW");

        mLevel = 1;
        esm.getHNOT(mLevel, "LEVL");

        mActorId = -1;
        esm.getHNOT(mActorId, "ACID");

        mDeathAnimation = -1;
        esm.getHNOT(mDeathAnimation, "DANM");

        mTimeOfDeath.mDay = 0;
        mTimeOfDeath.mHour = 0;
        if (esm.peekNextSub("DTIM"))
            mTimeOfDeath.load(esm, "DTIM");

        mSpells.load(esm);
        mActiveSpells.load(esm);
        mAiSequence.load(esm);
        mMagicEffects.load(esm);

        if (esm.getFormatVersion() <= MaxClearModifiersFormatVersion)
        {
            while (esm.isNextSub("SUMM"))
            {
                int32_t magicEffect;
                esm.getHT(magicEffect);
                ESM::RefId source = esm.getHNORefId("SOUR");
                int32_t effectIndex = -1;
                esm.getHNOT(effectIndex, "EIND");
                int32_t actorId;
                esm.getHNT(actorId, "ACID");
                mSummonedCreatureMap[SummonKey(ESM::MagicEffect::indexToRefId(magicEffect), source, effectIndex)]
                    = actorId;
                mSummonedCreatures.emplace(ESM::MagicEffect::indexToRefId(magicEffect),
                    RefNum{ .mIndex = static_cast<uint32_t>(actorId), .mContentFile = -1 });
            }
        }
        else
        {
            while (esm.isNextSub("SUMM"))
            {
                RefId effectId;
                if (esm.getFormatVersion() <= MaxSerializeEffectRefIdFormatVersion)
                {
                    int32_t magicEffect;
                    esm.getHT(magicEffect);
                    effectId = ESM::MagicEffect::indexToRefId(magicEffect);
                }
                else
                    effectId = esm.getRefId();
                RefNum actor;
                if (esm.getFormatVersion() <= MaxActorIdSaveGameFormatVersion)
                    esm.getHNT(actor.mIndex, "ACID");
                else
                    actor = esm.getFormId(true, "ACID");
                mSummonedCreatures.emplace(effectId, actor);
            }
        }

        while (esm.isNextSub("GRAV"))
        {
            int32_t actorId;
            esm.getHT(actorId);
            mSummonGraveyard.push_back(actorId);
        }

        mHasAiSettings = false;
        esm.getHNOT(mHasAiSettings, "AISE");

        if (mHasAiSettings)
        {
            for (auto& setting : mAiSettings)
                setting.load(esm);
        }

        while (esm.isNextSub("CORP"))
        {
            ESM::RefId id = esm.getRefId();

            CorprusStats stats;
            esm.getHNT(stats.mWorsenings, "WORS");
            stats.mNextWorsening.load(esm);

            mCorprusSpells[id] = stats;
        }
        if (esm.getFormatVersion() <= MaxOldSkillsAndAttributesFormatVersion)
            mMissingACDT = mGoldPool == std::numeric_limits<int>::min();
        else
        {
            mMissingACDT = false;
            esm.getHNOT(mMissingACDT, "NOAC");
        }
        mFalloutLimbDamage.fill(0.f);
        esm.getHNOT(mFalloutLimbDamage, "FLMB");

        mFalloutActorValueOverrides.clear();
        std::uint32_t actorValueCount = 0;
        if (esm.peekNextSub("FAVC"))
        {
            esm.getHNT(actorValueCount, "FAVC");
            if (actorValueCount > 96)
                esm.fail("Unreasonable Fallout actor-value override count");
            for (std::uint32_t i = 0; i < actorValueCount; ++i)
            {
                std::uint8_t actorValue = 0;
                float value = 0.f;
                esm.getHNT(actorValue, "FAVI");
                esm.getHNT(value, "FAVV");
                if (!std::isfinite(value) || !mFalloutActorValueOverrides.emplace(actorValue, value).second)
                    esm.fail("Invalid or duplicate Fallout actor-value override");
            }
        }

        mFalloutFactionOverrides.clear();
        std::uint32_t factionCount = 0;
        if (esm.peekNextSub("FFCT"))
        {
            esm.getHNT(factionCount, "FFCT");
            constexpr std::uint32_t MaximumFactionCount = 1'000'000;
            if (factionCount > MaximumFactionCount)
                esm.fail("Unreasonable Fallout faction override count");
            for (std::uint32_t i = 0; i < factionCount; ++i)
            {
                ESM::FormId faction = esm.getFormId(true, "FFID");
                const bool contentAvailable = esm.applyContentFileMapping(faction);
                std::int16_t rank = 0;
                esm.getHNT(rank, "FFRK");
                if (rank != FalloutFactionRemoved
                    && (rank < std::numeric_limits<std::int8_t>::min()
                        || rank > std::numeric_limits<std::int8_t>::max()))
                    esm.fail("Invalid Fallout faction override rank");
                if (contentAvailable
                    && (faction.isZeroOrUnset() || !mFalloutFactionOverrides.emplace(faction, rank).second))
                    esm.fail("Invalid or duplicate Fallout faction override");
            }
        }

        mFalloutRuntimeFlags = 0;
        esm.getHNOT(mFalloutRuntimeFlags, "FRTF");
        constexpr std::uint32_t KnownFalloutRuntimeFlags = 0x0f;
        if ((mFalloutRuntimeFlags & ~KnownFalloutRuntimeFlags) != 0)
            esm.fail("Invalid Fallout actor runtime flags");

        mHasFalloutEquipmentOverride = false;
        mFalloutEquippedItems.clear();
        if (esm.peekNextSub("FEQC"))
        {
            mHasFalloutEquipmentOverride = true;
            std::uint32_t equippedCount = 0;
            esm.getHNT(equippedCount, "FEQC");
            constexpr std::uint32_t MaximumEquippedItems = 64;
            if (equippedCount > MaximumEquippedItems)
                esm.fail("Unreasonable Fallout equipped-item override count");
            for (std::uint32_t i = 0; i < equippedCount; ++i)
            {
                ESM::FormId item = esm.getFormId(true, "FEQI");
                const bool contentAvailable = esm.applyContentFileMapping(item);
                if (contentAvailable)
                {
                    if (item.isZeroOrUnset()
                        || std::find(mFalloutEquippedItems.begin(), mFalloutEquippedItems.end(), item)
                            != mFalloutEquippedItems.end())
                        esm.fail("Invalid or duplicate Fallout equipped-item override");
                    mFalloutEquippedItems.push_back(item);
                }
            }
        }
    }

    void CreatureStats::save(ESMWriter& esm) const
    {
        for (const auto& attribute : mAttributes)
            attribute.save(esm);

        for (const auto& dynamic : mDynamic)
            dynamic.save(esm);

        if (mGoldPool)
            esm.writeHNT("GOLD", mGoldPool);

        if (mTradeTime.mDay != 0 || mTradeTime.mHour != 0)
            esm.writeHNT("TIME", mTradeTime);

        int32_t flags = 0;
        if (mDead)
            flags |= Dead;
        if (mDeathAnimationFinished)
            flags |= DeathAnimationFinished;
        if (mDied)
            flags |= Died;
        if (mMurdered)
            flags |= Murdered;
        if (mTalkedTo)
            flags |= TalkedTo;
        if (mAlarmed)
            flags |= Alarmed;
        if (mAttacked)
            flags |= Attacked;
        if (mKnockdown)
            flags |= Knockdown;
        if (mKnockdownOneFrame)
            flags |= KnockdownOneFrame;
        if (mKnockdownOverOneFrame)
            flags |= KnockdownOverOneFrame;
        if (mHitRecovery)
            flags |= HitRecovery;
        if (mBlock)
            flags |= Block;
        if (mRecalcDynamicStats)
            flags |= RecalcDynamicStats;

        if (flags)
            esm.writeHNT("AFLG", flags);

        if (mMovementFlags)
            esm.writeHNT("MOVE", mMovementFlags);

        if (mFallHeight)
            esm.writeHNT("FALL", mFallHeight);

        if (!mLastHitObject.empty())
            esm.writeHNRefId("LHIT", mLastHitObject);

        if (!mLastHitAttemptObject.empty())
            esm.writeHNRefId("LHAT", mLastHitAttemptObject);

        if (mDrawState)
            esm.writeHNT("DRAW", mDrawState);

        if (mLevel != 1)
            esm.writeHNT("LEVL", mLevel);

        if (mDeathAnimation != -1)
            esm.writeHNT("DANM", mDeathAnimation);

        if (mTimeOfDeath.mHour != 0 || mTimeOfDeath.mDay != 0)
            esm.writeHNT("DTIM", mTimeOfDeath);

        mSpells.save(esm);
        mActiveSpells.save(esm);
        mAiSequence.save(esm);
        mMagicEffects.save(esm);

        for (const auto& [effectId, actor] : mSummonedCreatures)
        {
            esm.writeHNRefId("SUMM", effectId);
            esm.writeFormId(actor, true, "ACID");
        }

        esm.writeHNT("AISE", mHasAiSettings);
        if (mHasAiSettings)
        {
            for (const auto& setting : mAiSettings)
                setting.save(esm);
        }
        if (mMissingACDT)
            esm.writeHNT("NOAC", mMissingACDT);
        if (std::any_of(mFalloutLimbDamage.begin(), mFalloutLimbDamage.end(), [](float value) { return value != 0.f; }))
            esm.writeHNT("FLMB", mFalloutLimbDamage);
        if (!mFalloutActorValueOverrides.empty())
        {
            esm.writeHNT("FAVC", static_cast<std::uint32_t>(mFalloutActorValueOverrides.size()));
            for (const auto& [actorValue, value] : mFalloutActorValueOverrides)
            {
                esm.writeHNT("FAVI", actorValue);
                esm.writeHNT("FAVV", value);
            }
        }
        if (!mFalloutFactionOverrides.empty())
        {
            esm.writeHNT("FFCT", static_cast<std::uint32_t>(mFalloutFactionOverrides.size()));
            for (const auto& [faction, rank] : mFalloutFactionOverrides)
            {
                esm.writeFormId(faction, true, "FFID");
                esm.writeHNT("FFRK", rank);
            }
        }
        if (mFalloutRuntimeFlags != 0)
            esm.writeHNT("FRTF", mFalloutRuntimeFlags);
        if (mHasFalloutEquipmentOverride)
        {
            esm.writeHNT("FEQC", static_cast<std::uint32_t>(mFalloutEquippedItems.size()));
            for (const ESM::FormId item : mFalloutEquippedItems)
                esm.writeFormId(item, true, "FEQI");
        }
    }

    void CreatureStats::blank()
    {
        mTradeTime.mHour = 0;
        mTradeTime.mDay = 0;
        mGoldPool = 0;
        mActorId = -1;
        mHasAiSettings = false;
        mDead = false;
        mDeathAnimationFinished = false;
        mDied = false;
        mMurdered = false;
        mTalkedTo = false;
        mAlarmed = false;
        mAttacked = false;
        mKnockdown = false;
        mKnockdownOneFrame = false;
        mKnockdownOverOneFrame = false;
        mHitRecovery = false;
        mBlock = false;
        mMovementFlags = 0;
        mFallHeight = 0.f;
        mRecalcDynamicStats = false;
        mDrawState = 0;
        mDeathAnimation = -1;
        mLevel = 1;
        mCorprusSpells.clear();
        mMissingACDT = false;
        mFalloutLimbDamage.fill(0.f);
        mFalloutActorValueOverrides.clear();
        mFalloutFactionOverrides.clear();
        mFalloutRuntimeFlags = 0;
        mHasFalloutEquipmentOverride = false;
        mFalloutEquippedItems.clear();
    }

}

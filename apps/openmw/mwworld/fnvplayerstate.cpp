#include "fnvplayerstate.hpp"

#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loadnpc.hpp>

#include "store.hpp"

namespace
{
    struct CategoryResolution
    {
        const ESM4::Npc* mRecord = nullptr;
        std::string mError;
    };

    CategoryResolution resolveCategory(const MWWorld::Store<ESM4::Npc>& npcs, const ESM4::Npc& player,
        std::uint16_t templateFlag, std::string_view category)
    {
        std::unordered_set<ESM::FormId> visited;
        const ESM4::Npc* current = &player;
        while (current != nullptr)
        {
            if (!visited.insert(current->mId).second)
            {
                return { nullptr,
                    "template cycle while resolving " + std::string(category) + " at " + current->mId.toString() };
            }
            if (!current->mIsFONV)
            {
                return { nullptr,
                    "non-FNV template while resolving " + std::string(category) + " at " + current->mId.toString() };
            }
            if (!current->mHasFNVBaseConfig)
            {
                return { nullptr,
                    "missing exact 24-byte ACBS while resolving " + std::string(category) + " at "
                        + current->mId.toString() };
            }
            if ((current->mBaseConfig.fo3.templateFlags & templateFlag) == 0)
                return { current, {} };
            if (current->mBaseTemplate.isZeroOrUnset())
            {
                return { nullptr,
                    "missing TPLT while resolving delegated " + std::string(category) + " at "
                        + current->mId.toString() };
            }

            current = npcs.search(ESM::RefId(current->mBaseTemplate));
            if (current == nullptr)
            {
                return { nullptr,
                    "unresolved TPLT while resolving " + std::string(category) + " from " + player.mId.toString() };
            }
        }

        return { nullptr, "unresolved " + std::string(category) };
    }

    MWWorld::FalloutPlayerStateResolution failure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    template <class T, std::size_t Size>
    std::array<std::uint8_t, Size> bytesOf(const T& value)
    {
        static_assert(sizeof(T) == Size);
        std::array<std::uint8_t, Size> result{};
        std::memcpy(result.data(), &value, Size);
        return result;
    }
}

namespace MWWorld
{
    std::uint8_t FalloutPlayerState::getSpecial(FalloutSpecial value) const
    {
        return mSpecial.at(static_cast<std::size_t>(value));
    }

    std::uint8_t FalloutPlayerState::getSkillValue(FalloutSkill value) const
    {
        return mSkillValues.at(static_cast<std::size_t>(value));
    }

    std::uint8_t FalloutPlayerState::getSkillOffset(FalloutSkill value) const
    {
        return mSkillOffsets.at(static_cast<std::size_t>(value));
    }

    std::optional<FalloutActorValueComponents> FalloutPlayerState::getActorValueComponents(
        std::uint32_t actorValue) const
    {
        if (actorValue <= 4)
        {
            const std::array<std::uint8_t, 5> aiValues{ mAIData.aggression, mAIData.confidence, mAIData.energyLevel,
                mAIData.responsibility, mAIData.mood };
            return FalloutActorValueComponents{ static_cast<double>(aiValues[actorValue]), std::nullopt };
        }
        if (actorValue >= 5 && actorValue <= 11)
        {
            return FalloutActorValueComponents{ static_cast<double>(mSpecial[static_cast<std::size_t>(actorValue - 5)]),
                std::nullopt };
        }
        if (actorValue == 16)
            return FalloutActorValueComponents{ static_cast<double>(mHealth), std::nullopt };
        if (actorValue >= 32 && actorValue <= 45)
        {
            const std::size_t index = static_cast<std::size_t>(actorValue - 32);
            return FalloutActorValueComponents{ static_cast<double>(mSkillValues[index]), mSkillOffsets[index] };
        }
        return std::nullopt;
    }

    FalloutPlayerStateResolution resolveFalloutPlayerState(
        const Store<ESM4::Npc>& npcs, ESM::FormId normalizedPlayerFormId)
    {
        const ESM4::Npc* player = npcs.search(ESM::RefId(normalizedPlayerFormId));
        if (player == nullptr)
        {
            return failure("missing winning FalloutNV.esm Player NPC_ FormID 0x00000007 at normalized "
                + normalizedPlayerFormId.toString());
        }
        if (!player->mIsFONV)
            return failure("winning NPC_ FormID 0x00000007 is not an FNV record");
        if (player->mEditorId != "Player")
            return failure("winning NPC_ FormID 0x00000007 does not have EDID Player");

        const CategoryResolution traits = resolveCategory(npcs, *player, ESM4::Npc::Template_UseTraits, "traits");
        if (traits.mRecord == nullptr)
            return failure(traits.mError);
        const CategoryResolution stats = resolveCategory(npcs, *player, ESM4::Npc::Template_UseStats, "stats");
        if (stats.mRecord == nullptr)
            return failure(stats.mError);
        const CategoryResolution ai = resolveCategory(npcs, *player, ESM4::Npc::Template_UseAIData, "AI data");
        if (ai.mRecord == nullptr)
            return failure(ai.mError);
        const CategoryResolution model = resolveCategory(npcs, *player, ESM4::Npc::Template_UseModel, "model");
        if (model.mRecord == nullptr)
            return failure(model.mError);
        const CategoryResolution baseData
            = resolveCategory(npcs, *player, ESM4::Npc::Template_UseBaseData, "base data");
        if (baseData.mRecord == nullptr)
            return failure(baseData.mError);

        if (!stats.mRecord->mHasFNVData)
            return failure("resolved Player stats record lacks exact 11-byte DATA");
        if (!stats.mRecord->mHasFNVSkills)
            return failure("resolved Player stats record lacks exact 28-byte DNAM");
        if (!ai.mRecord->mHasFNVAIData)
            return failure("resolved Player AI record lacks exact 20-byte AIDT");
        if (traits.mRecord->mRace.isZeroOrUnset())
            return failure("resolved Player traits record lacks RNAM race identity");
        if (stats.mRecord->mClass.isZeroOrUnset())
            return failure("resolved Player stats record lacks CNAM class identity");
        if (model.mRecord->mModel.empty())
            return failure("resolved Player model record lacks MODL identity");
        if (stats.mRecord->mFNVData.health < 0
            || stats.mRecord->mFNVData.health > std::numeric_limits<std::uint16_t>::max())
            return failure("resolved Player health cannot be represented by the temporary ESM3 proxy");

        FalloutPlayerState result;
        result.mBaseRecord = player->mId;
        result.mTraitsRecord = traits.mRecord->mId;
        result.mStatsRecord = stats.mRecord->mId;
        result.mAIDataRecord = ai.mRecord->mId;
        result.mModelRecord = model.mRecord->mId;
        result.mBaseDataRecord = baseData.mRecord->mId;
        result.mEditorId = player->mEditorId;
        result.mFullName = baseData.mRecord->mFullName;
        result.mModel = model.mRecord->mModel;
        result.mRace = traits.mRecord->mRace;
        result.mClass = stats.mRecord->mClass;
        result.mHair = traits.mRecord->mHair;
        result.mEyes = traits.mRecord->mEyes;
        result.mVoiceType = traits.mRecord->mVoiceType;
        result.mRecordFlags = baseData.mRecord->mFlags;
        result.mStatsConfig = stats.mRecord->mBaseConfig.fo3;
        result.mAIData = ai.mRecord->mFNVAIData;
        result.mHealth = stats.mRecord->mFNVData.health;
        result.mSpecial = { stats.mRecord->mFNVData.strength, stats.mRecord->mFNVData.perception,
            stats.mRecord->mFNVData.endurance, stats.mRecord->mFNVData.charisma, stats.mRecord->mFNVData.intelligence,
            stats.mRecord->mFNVData.agility, stats.mRecord->mFNVData.luck };
        result.mSkillValues
            = bytesOf<ESM4::Npc::FNVSkillValues, FalloutPlayerState::SkillCount>(stats.mRecord->mFNVSkills.values);
        result.mSkillOffsets
            = bytesOf<ESM4::Npc::FNVSkillValues, FalloutPlayerState::SkillCount>(stats.mRecord->mFNVSkills.offsets);
        return { std::move(result), {} };
    }

    void seedFalloutPlayerProxy(ESM::NPC& proxy, const FalloutPlayerState& state)
    {
        if (!state.mFullName.empty())
            proxy.mName = state.mFullName;
        proxy.mModel = state.mModel;
        proxy.mNpdt.mLevel = state.mStatsConfig.levelOrMult;
        proxy.mNpdt.mHealth = static_cast<std::uint16_t>(state.mHealth);
        proxy.mNpdt.mFatigue = state.mStatsConfig.fatigue;
        if ((state.mStatsConfig.flags & ESM4::Npc::FO3_Female) != 0)
            proxy.mFlags |= ESM::NPC::Female;
        else
            proxy.mFlags &= ~ESM::NPC::Female;
    }
}

#include "fnvradioprogram.hpp"

#include "esm4questruntime.hpp"
#include "esmstore.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>

#include <components/esm/refid.hpp>
#include <components/esm4/dialogue.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/loadtact.hpp>
#include <components/esm4/script.hpp>
#include <components/misc/strings/algorithm.hpp>

namespace
{
    bool isExactStationBinding(const ESM4::TargetCondition& condition, ESM::FormId station)
    {
        return condition.condition == ESM4::CTF_EqualTo && condition.comparison == 1.f
            && condition.functionIndex == ESM4::FUN_GetIsID
            && ESM::FormId::fromUint32(condition.param1) == station && condition.param2 == 0
            && condition.runOn == 0 && condition.reference == 0;
    }

    bool mentionsStation(const ESM4::Quest& quest, ESM::FormId station)
    {
        return std::ranges::any_of(quest.mTargetConditions, [station](const ESM4::TargetCondition& condition) {
            return condition.functionIndex == ESM4::FUN_GetIsID
                && ESM::FormId::fromUint32(condition.param1) == station;
        });
    }

    bool hasQuest(const ESM4::Dialogue& dialogue, ESM::FormId quest)
    {
        return std::ranges::find(dialogue.mQuests, quest) != dialogue.mQuests.end()
            && std::ranges::find(dialogue.mQuestsRemoved, quest) == dialogue.mQuestsRemoved.end();
    }

    bool isEmptyScript(const ESM4::ScriptDefinition& script)
    {
        const ESM4::ScriptHeader& header = script.scriptHeader;
        return header.unused == 0 && header.refCount == 0 && header.compiledSize == 0
            && header.variableCount == 0 && header.type == 0 && (header.flag == 0 || header.flag == 1)
            && script.compiledData.empty() && script.scriptSource.empty() && script.localVarData.empty()
            && script.localRefVarIndex.empty() && script.references.empty() && script.globReference.isZeroOrUnset();
    }

    bool hasResultScript(const ESM4::DialogInfo& info)
    {
        return !isEmptyScript(info.mScript) || !isEmptyScript(info.mEndScript);
    }
}

namespace MWWorld
{
    std::optional<PreparedFnvRadioOneShot> prepareFnvRadioOneShot(
        const FnvRadioProgramContext& context, FnvRadioProgramPreparationError* error)
    {
        const auto fail = [error](FnvRadioProgramPreparationError value)
            -> std::optional<PreparedFnvRadioOneShot> {
            if (error != nullptr)
                *error = value;
            return std::nullopt;
        };

        if (error != nullptr)
            *error = FnvRadioProgramPreparationError::None;
        if (context.mGame != ESM4Game::FalloutNewVegas)
            return fail(FnvRadioProgramPreparationError::NotFalloutNewVegas);
        if (context.mStore == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingStore);
        if (context.mQuestRuntime == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingQuestRuntime);
        if (context.mStation == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingStation);

        const ESM4::TalkingActivator& station = *context.mStation;
        if (context.mStore->get<ESM4::TalkingActivator>().search(ESM::RefId(station.mId)) != &station)
            return fail(FnvRadioProgramPreparationError::DetachedStation);
        if ((station.mFlags & ESM4::TACT_RadioStation) == 0)
            return fail(FnvRadioProgramPreparationError::NotRadioStation);
        if (!station.mLoopSound.isZeroOrUnset() || !station.mRadioTemplate.isZeroOrUnset())
            return fail(FnvRadioProgramPreparationError::HasDirectBroadcast);
        if ((station.mFlags & ESM4::TACT_ContBroadcast) != 0)
            return fail(FnvRadioProgramPreparationError::ContinuousBroadcast);

        const ESM4::Quest* stationQuest = nullptr;
        for (const ESM4::Quest& quest : context.mStore->get<ESM4::Quest>())
        {
            if (!mentionsStation(quest, station.mId))
                continue;
            if (quest.mTargetConditions.size() != 1
                || !isExactStationBinding(quest.mTargetConditions.front(), station.mId))
                return fail(FnvRadioProgramPreparationError::UnsupportedQuestConditions);
            if (stationQuest != nullptr)
                return fail(FnvRadioProgramPreparationError::AmbiguousStationQuest);
            stationQuest = &quest;
        }
        if (stationQuest == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingStationQuest);

        const ESM4QuestState* questState = context.mQuestRuntime->search(stationQuest->mId);
        if (questState == nullptr || (questState->mFlags & ESM4QuestState::Flag_Running) == 0)
            return fail(FnvRadioProgramPreparationError::QuestNotRunning);

        const ESM4::Dialogue* helloTopic = nullptr;
        for (const ESM4::Dialogue& dialogue : context.mStore->get<ESM4::Dialogue>())
        {
            if (dialogue.mDialType != ESM4::DTYP_Radio
                || !Misc::StringUtils::ciEqual(dialogue.mEditorId, "RadioHello")
                || !hasQuest(dialogue, stationQuest->mId))
                continue;
            if (helloTopic != nullptr)
                return fail(FnvRadioProgramPreparationError::AmbiguousHelloTopic);
            helloTopic = &dialogue;
        }
        if (helloTopic == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingHelloTopic);

        const ESM4::DialogInfo* helloInfo = nullptr;
        for (const ESM4::DialogInfo& info : context.mStore->get<ESM4::DialogInfo>())
        {
            if (info.mQuest != stationQuest->mId || info.mTopic != helloTopic->mId)
                continue;
            if (helloInfo != nullptr)
                return fail(FnvRadioProgramPreparationError::AmbiguousHelloInfo);
            helloInfo = &info;
        }
        if (helloInfo == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingHelloInfo);

        constexpr std::uint16_t supportedInfoFlags = ESM4::INFO_RunImmediately;
        if (helloInfo->mDialType != ESM4::DTYP_Radio || helloInfo->mNextSpeaker != 0
            || (helloInfo->mInfoFlags & static_cast<std::uint16_t>(~supportedInfoFlags)) != 0
            || !helloInfo->mTargetConditions.empty()
            || !helloInfo->mSpeaker.isZeroOrUnset() || hasResultScript(*helloInfo))
            return fail(FnvRadioProgramPreparationError::UnsupportedInfo);
        if (helloInfo->mResponses.size() != 1 || helloInfo->mResponses.front().mData.sound == 0)
            return fail(FnvRadioProgramPreparationError::InvalidResponse);

        const ESM::FormId sound = ESM::FormId::fromUint32(helloInfo->mResponses.front().mData.sound);
        if (context.mStore->get<ESM4::Sound>().search(ESM::RefId(sound)) == nullptr
            && context.mStore->get<ESM4::SoundReference>().search(ESM::RefId(sound)) == nullptr)
            return fail(FnvRadioProgramPreparationError::MissingSound);

        return PreparedFnvRadioOneShot{ station.mId, stationQuest->mId, helloTopic->mId, helloInfo->mId, sound };
    }

    std::string_view getFnvRadioProgramPreparationErrorName(FnvRadioProgramPreparationError error)
    {
        switch (error)
        {
            case FnvRadioProgramPreparationError::None:
                return "none";
            case FnvRadioProgramPreparationError::NotFalloutNewVegas:
                return "not-fallout-new-vegas";
            case FnvRadioProgramPreparationError::MissingStore:
                return "missing-store";
            case FnvRadioProgramPreparationError::MissingQuestRuntime:
                return "missing-quest-runtime";
            case FnvRadioProgramPreparationError::MissingStation:
                return "missing-station";
            case FnvRadioProgramPreparationError::DetachedStation:
                return "detached-station";
            case FnvRadioProgramPreparationError::NotRadioStation:
                return "not-radio-station";
            case FnvRadioProgramPreparationError::HasDirectBroadcast:
                return "has-direct-broadcast";
            case FnvRadioProgramPreparationError::ContinuousBroadcast:
                return "continuous-broadcast";
            case FnvRadioProgramPreparationError::MissingStationQuest:
                return "missing-station-quest";
            case FnvRadioProgramPreparationError::AmbiguousStationQuest:
                return "ambiguous-station-quest";
            case FnvRadioProgramPreparationError::UnsupportedQuestConditions:
                return "unsupported-quest-conditions";
            case FnvRadioProgramPreparationError::QuestNotRunning:
                return "quest-not-running";
            case FnvRadioProgramPreparationError::MissingHelloTopic:
                return "missing-radio-hello-topic";
            case FnvRadioProgramPreparationError::AmbiguousHelloTopic:
                return "ambiguous-radio-hello-topic";
            case FnvRadioProgramPreparationError::MissingHelloInfo:
                return "missing-radio-hello-info";
            case FnvRadioProgramPreparationError::AmbiguousHelloInfo:
                return "ambiguous-radio-hello-info";
            case FnvRadioProgramPreparationError::UnsupportedInfo:
                return "unsupported-info";
            case FnvRadioProgramPreparationError::InvalidResponse:
                return "invalid-response";
            case FnvRadioProgramPreparationError::MissingSound:
                return "missing-sound";
        }
        return "unknown";
    }
}

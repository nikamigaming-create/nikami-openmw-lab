#ifndef GAME_MWWORLD_FNVRADIOPROGRAM_H
#define GAME_MWWORLD_FNVRADIOPROGRAM_H

#include <optional>
#include <string_view>

#include <components/esm/formid.hpp>

namespace ESM4
{
    struct TalkingActivator;
}

namespace MWWorld
{
    class ESM4QuestRuntime;
    class ESMStore;
    enum class ESM4Game;

    struct FnvRadioProgramContext
    {
        ESM4Game mGame;
        const ESMStore* mStore = nullptr;
        const ESM4QuestRuntime* mQuestRuntime = nullptr;
        const ESM4::TalkingActivator* mStation = nullptr;
    };

    enum class FnvRadioProgramPreparationError
    {
        None,
        NotFalloutNewVegas,
        MissingStore,
        MissingQuestRuntime,
        MissingStation,
        DetachedStation,
        NotRadioStation,
        HasDirectBroadcast,
        ContinuousBroadcast,
        MissingStationQuest,
        AmbiguousStationQuest,
        UnsupportedQuestConditions,
        QuestNotRunning,
        MissingHelloTopic,
        AmbiguousHelloTopic,
        MissingHelloInfo,
        AmbiguousHelloInfo,
        UnsupportedInfo,
        InvalidResponse,
        MissingSound,
    };

    struct PreparedFnvRadioOneShot
    {
        ESM::FormId mStation{};
        ESM::FormId mQuest{};
        ESM::FormId mTopic{};
        ESM::FormId mInfo{};
        ESM::FormId mSound{};
    };

    std::optional<PreparedFnvRadioOneShot> prepareFnvRadioOneShot(
        const FnvRadioProgramContext& context, FnvRadioProgramPreparationError* error = nullptr);

    std::string_view getFnvRadioProgramPreparationErrorName(FnvRadioProgramPreparationError error);
}

#endif

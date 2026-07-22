#ifndef GAME_MWWORLD_ACTIONESM4RADIO_H
#define GAME_MWWORLD_ACTIONESM4RADIO_H

#include <string>

#include "action.hpp"

namespace MWWorld
{
    enum class Esm4RadioPlaybackMode
    {
        Loop,
        OneShot,
    };

    struct Esm4RadioPlaybackSelection
    {
        ESM::FormId mSound{};
        Esm4RadioPlaybackMode mMode = Esm4RadioPlaybackMode::Loop;
    };

    enum class Esm4RadioPlaybackOperation
    {
        Stop,
        PlayLoop,
        PlayOneShot,
    };

    Esm4RadioPlaybackSelection selectEsm4RadioBroadcast(ESM::FormId ownerLoopSound,
        ESM::FormId ownerTemplateSound, ESM::FormId linkedLoopSound, ESM::FormId linkedTemplateSound,
        ESM::FormId preparedOneShot);

    Esm4RadioPlaybackOperation selectEsm4RadioPlaybackOperation(
        bool wasPlaying, Esm4RadioPlaybackMode mode);

    class ActionEsm4Radio final : public Action
    {
        ESM::RefId mBroadcastSound;
        ESM::RefId mStation;
        std::string mBroadcastVoice;
        Esm4RadioPlaybackMode mPlaybackMode;

        void executeImp(const Ptr& actor) override;

    public:
        ActionEsm4Radio(
            const Ptr& target, ESM::RefId activationSound, ESM::RefId broadcastSound, ESM::RefId station,
            std::string broadcastVoice = {}, Esm4RadioPlaybackMode playbackMode = Esm4RadioPlaybackMode::Loop);

        Esm4RadioPlaybackMode getPlaybackMode() const { return mPlaybackMode; }
    };
}

#endif

#include "actionesm4radio.hpp"

#include <components/debug/debuglog.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"

namespace MWWorld
{
    Esm4RadioPlaybackSelection selectEsm4RadioBroadcast(ESM::FormId ownerLoopSound,
        ESM::FormId ownerTemplateSound, ESM::FormId linkedLoopSound, ESM::FormId linkedTemplateSound,
        ESM::FormId preparedOneShot)
    {
        for (const ESM::FormId sound : { ownerLoopSound, ownerTemplateSound, linkedLoopSound, linkedTemplateSound })
            if (!sound.isZeroOrUnset())
                return { sound, Esm4RadioPlaybackMode::Loop };
        if (!preparedOneShot.isZeroOrUnset())
            return { preparedOneShot, Esm4RadioPlaybackMode::OneShot };
        return {};
    }

    Esm4RadioPlaybackOperation selectEsm4RadioPlaybackOperation(
        bool wasPlaying, Esm4RadioPlaybackMode mode)
    {
        if (wasPlaying)
            return Esm4RadioPlaybackOperation::Stop;
        return mode == Esm4RadioPlaybackMode::OneShot ? Esm4RadioPlaybackOperation::PlayOneShot
                                                      : Esm4RadioPlaybackOperation::PlayLoop;
    }

    ActionEsm4Radio::ActionEsm4Radio(
        const Ptr& target, ESM::RefId activationSound, ESM::RefId broadcastSound, ESM::RefId station,
        std::string broadcastVoice, Esm4RadioPlaybackMode playbackMode)
        : Action(false, target)
        , mBroadcastSound(std::move(broadcastSound))
        , mStation(std::move(station))
        , mBroadcastVoice(std::move(broadcastVoice))
        , mPlaybackMode(playbackMode)
    {
        setSound(activationSound);
    }

    void ActionEsm4Radio::executeImp(const Ptr& actor)
    {
        (void)actor;
        MWBase::SoundManager* soundManager = MWBase::Environment::get().getSoundManager();
        const Ptr& target = getTarget();
        if (soundManager == nullptr || target.isEmpty()
            || (mBroadcastSound.empty() && mBroadcastVoice.empty()))
        {
            Log(Debug::Warning) << "FNV/ESM4 radio: activation has no playable broadcast target="
                                << target.toString() << " station=" << mStation.toDebugString()
                                << " sound=" << mBroadcastSound.toDebugString()
                                << " voice=\"" << mBroadcastVoice << "\"";
            return;
        }

        if (mBroadcastSound.empty())
        {
            const bool wasPlaying = soundManager->sayActive(target);
            if (wasPlaying)
                soundManager->stopSay(target);
            else
                soundManager->say(target, VFS::Path::Normalized(mBroadcastVoice));
            const bool isPlaying = soundManager->sayActive(target);
            Log(Debug::Info) << "FNV/ESM4 radio: toggled target=" << target.toString()
                             << " station=" << mStation.toDebugString()
                             << " voice=\"" << mBroadcastVoice << "\""
                             << " wasPlaying=" << (wasPlaying ? 1 : 0)
                             << " isPlaying=" << (isPlaying ? 1 : 0);
            return;
        }

        const bool wasPlaying = soundManager->getSoundPlaying(target, mBroadcastSound);
        switch (selectEsm4RadioPlaybackOperation(wasPlaying, mPlaybackMode))
        {
            case Esm4RadioPlaybackOperation::Stop:
                soundManager->stopSound3D(target, mBroadcastSound);
                break;
            case Esm4RadioPlaybackOperation::PlayLoop:
                soundManager->playSound3D(
                    target, mBroadcastSound, 1.f, 1.f, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
                break;
            case Esm4RadioPlaybackOperation::PlayOneShot:
                soundManager->playSound3D(
                    target, mBroadcastSound, 1.f, 1.f, MWSound::Type::Sfx, MWSound::PlayMode::Normal);
                break;
        }

        const bool isPlaying = soundManager->getSoundPlaying(target, mBroadcastSound);
        Log(Debug::Info) << "FNV/ESM4 radio: toggled target=" << target.toString()
                         << " station=" << mStation.toDebugString()
                         << " sound=" << mBroadcastSound.toDebugString()
                         << " mode="
                         << (mPlaybackMode == Esm4RadioPlaybackMode::OneShot ? "one-shot" : "loop")
                         << " wasPlaying=" << (wasPlaying ? 1 : 0)
                         << " isPlaying=" << (isPlaying ? 1 : 0);
    }
}

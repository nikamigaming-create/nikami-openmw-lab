#include "soundbuffer.hpp"

<<<<<<< HEAD
=======
#include "falloutsoundpath.hpp"

>>>>>>> origin/main
#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadsoun.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/pathutil.hpp>

#include <algorithm>
#include <cmath>

namespace MWSound
{
    namespace
    {
<<<<<<< HEAD
        constexpr VFS::Path::NormalizedView soundDir("sound");
        constexpr VFS::Path::ExtensionView mp3("mp3");
=======
        constexpr unsigned int maxSoundReferenceDepth = 8;
>>>>>>> origin/main

        struct AudioParams
        {
            float mAudioDefaultMinDistance;
            float mAudioDefaultMaxDistance;
            float mAudioMinDistanceMult;
            float mAudioMaxDistanceMult;
        };

        AudioParams makeAudioParams(const MWWorld::Store<ESM::GameSetting>& settings)
        {
            AudioParams params;
            params.mAudioDefaultMinDistance = settings.find("fAudioDefaultMinDistance")->mValue.getFloat();
            params.mAudioDefaultMaxDistance = settings.find("fAudioDefaultMaxDistance")->mValue.getFloat();
            params.mAudioMinDistanceMult = settings.find("fAudioMinDistanceMult")->mValue.getFloat();
            params.mAudioMaxDistanceMult = settings.find("fAudioMaxDistanceMult")->mValue.getFloat();
            return params;
        }
<<<<<<< HEAD
=======

        std::string resolveESM4SoundReferencePath(
            const MWWorld::ESMStore& store, const ESM4::SoundReference& sound, unsigned int depth = 0)
        {
            if (!sound.mSoundFile.empty())
                return sound.mSoundFile;

            if (depth >= maxSoundReferenceDepth || sound.mSoundId.isZeroOrUnset())
                return {};

            if (const ESM4::SoundReference* linked = store.get<ESM4::SoundReference>().search(sound.mSoundId))
                return resolveESM4SoundReferencePath(store, *linked, depth + 1);

            if (const ESM4::Sound* linked = store.get<ESM4::Sound>().search(sound.mSoundId))
                return linked->mSoundFile;

            return {};
        }
>>>>>>> origin/main
    }

    SoundBufferPool::SoundBufferPool(SoundOutput& output)
        : mOutput(&output)
        , mBufferCacheMax(Settings::sound().mBufferCacheMax * 1024 * 1024)
        , mBufferCacheMin(
              std::min(static_cast<std::size_t>(Settings::sound().mBufferCacheMin) * 1024 * 1024, mBufferCacheMax))
    {
    }

    SoundBufferPool::~SoundBufferPool()
    {
        clear();
    }

    SoundBuffer* SoundBufferPool::lookup(const ESM::RefId& soundId) const
    {
        const auto it = mBufferNameMap.find(soundId);
        if (it != mBufferNameMap.end())
        {
            SoundBuffer* sfx = it->second;
            if (sfx->getHandle() != nullptr)
                return sfx;
        }
        return nullptr;
    }

<<<<<<< HEAD
    SoundBuffer* SoundBufferPool::lookup(VFS::Path::NormalizedView fileName) const
    {
        const auto it = mBufferFileNameMap.find(fileName);
=======
    SoundBuffer* SoundBufferPool::lookup(std::string_view fileName) const
    {
        const auto it = mBufferFileNameMap.find(std::string(fileName));
>>>>>>> origin/main
        if (it != mBufferFileNameMap.end())
        {
            SoundBuffer* sfx = it->second;
            if (sfx->getHandle() != nullptr)
                return sfx;
        }
        return nullptr;
    }

    SoundBuffer* SoundBufferPool::loadSfx(SoundBuffer* sfx)
    {
        if (sfx->getHandle() != nullptr)
            return sfx;

        auto [handle, size] = mOutput->loadSound(sfx->getResourceName());
        if (handle == nullptr)
            return {};

        sfx->mHandle = handle;

        mBufferCacheSize += size;
        if (mBufferCacheSize > mBufferCacheMax)
        {
            unloadUnused();
            if (!mUnusedBuffers.empty() && mBufferCacheSize > mBufferCacheMax)
                Log(Debug::Warning) << "No unused sound buffers to free, using " << mBufferCacheSize << " bytes!";
        }
        mUnusedBuffers.push_front(sfx);

        return sfx;
    }

    SoundBuffer* SoundBufferPool::load(const ESM::RefId& soundId)
    {
        if (mBufferNameMap.empty())
        {
            const MWWorld::ESMStore* esmstore = MWBase::Environment::get().getESMStore();
<<<<<<< HEAD
            for (const ESM::Sound& sound : esmstore->get<ESM::Sound>())
                insertSound(sound.mId, sound);
            for (const ESM4::Sound& sound : esmstore->get<ESM4::Sound>())
                insertSound(sound.mId, sound);
            for (const ESM4::SoundReference& sound : esmstore->get<ESM4::SoundReference>())
                insertSound(sound.mId, sound);
=======
            const bool falloutNewVegas = esmstore->getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas;
            if (!falloutNewVegas)
            {
                for (const ESM::Sound& sound : esmstore->get<ESM::Sound>())
                    insertSound(sound.mId, sound);
            }

            std::size_t soundEditorIdAliases = 0;
            for (const ESM4::Sound& sound : esmstore->get<ESM4::Sound>())
            {
                SoundBuffer* const buffer = insertSound(sound.mId, sound);
                if (buffer != nullptr && !sound.mEditorId.empty())
                    soundEditorIdAliases
                        += mBufferNameMap.emplace(ESM::RefId::stringRefId(sound.mEditorId), buffer).second;
            }

            std::size_t soundReferenceEditorIdAliases = 0;
            for (const ESM4::SoundReference& sound : esmstore->get<ESM4::SoundReference>())
            {
                SoundBuffer* const buffer = insertSound(sound.mId, sound);
                if (buffer != nullptr && !sound.mEditorId.empty())
                    soundReferenceEditorIdAliases
                        += mBufferNameMap.emplace(ESM::RefId::stringRefId(sound.mEditorId), buffer).second;
            }

            Log(Debug::Info) << "FNV/ESM4 sound: registered editor-id aliases sounds=" << soundEditorIdAliases
                             << " references=" << soundReferenceEditorIdAliases
                             << " legacyEsm3Fallback=" << (falloutNewVegas ? "disabled" : "enabled");
>>>>>>> origin/main
        }

        SoundBuffer* sfx;
        const auto it = mBufferNameMap.find(soundId);
        if (it != mBufferNameMap.end())
            sfx = it->second;
        else
        {
<<<<<<< HEAD
            const ESM::Sound* sound = MWBase::Environment::get().getESMStore()->get<ESM::Sound>().search(soundId);
            if (sound == nullptr)
                return {};
            sfx = insertSound(soundId, *sound);
=======
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            const bool falloutNewVegas = store->getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas;
            if (const ESM4::Sound* sound = store->get<ESM4::Sound>().search(soundId))
                sfx = insertSound(soundId, *sound);
            else if (const ESM4::SoundReference* sound = store->get<ESM4::SoundReference>().search(soundId))
                sfx = insertSound(soundId, *sound);
            else if (!falloutNewVegas)
            {
                if (const ESM::Sound* sound = store->get<ESM::Sound>().search(soundId))
                    sfx = insertSound(soundId, *sound);
                else
                    return {};
            }
            else
                return {};
            if (sfx == nullptr)
                return {};
>>>>>>> origin/main
        }

        return loadSfx(sfx);
    }

<<<<<<< HEAD
    SoundBuffer* SoundBufferPool::load(VFS::Path::NormalizedView fileName)
    {
        SoundBuffer* sfx;
        const auto it = mBufferFileNameMap.find(fileName);
        if (it != mBufferFileNameMap.end())
            sfx = it->second;
        else
            sfx = insertSound(fileName);
=======
    SoundBuffer* SoundBufferPool::load(std::string_view fileName)
    {
        SoundBuffer* sfx;
        const auto it = mBufferFileNameMap.find(std::string(fileName));
        if (it != mBufferFileNameMap.end())
            sfx = it->second;
        else
        {
            sfx = insertSound(fileName);
        }
>>>>>>> origin/main

        return loadSfx(sfx);
    }

    void SoundBufferPool::clear()
    {
        for (auto& sfx : mSoundBuffers)
        {
            if (sfx.mHandle)
                mOutput->unloadSound(sfx.mHandle);
            sfx.mHandle = nullptr;
        }

        mBufferFileNameMap.clear();
        mBufferNameMap.clear();
        mUnusedBuffers.clear();
    }

<<<<<<< HEAD
    SoundBuffer* SoundBufferPool::insertSound(VFS::Path::NormalizedView fileName)
=======
    SoundBuffer* SoundBufferPool::insertSound(std::string_view fileName)
>>>>>>> origin/main
    {
        static const AudioParams audioParams
            = makeAudioParams(MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>());

        float volume = 1.f;
        float min = std::max(audioParams.mAudioDefaultMinDistance * audioParams.mAudioMinDistanceMult, 1.f);
        float max = std::max(min, audioParams.mAudioDefaultMaxDistance * audioParams.mAudioMaxDistanceMult);

        min = std::max(min, 1.0f);
        max = std::max(min, max);

        SoundBuffer& sfx = mSoundBuffers.emplace_back(fileName, volume, min, max);

        mBufferFileNameMap.emplace(fileName, &sfx);
        return &sfx;
    }

    SoundBuffer* SoundBufferPool::insertSound(const ESM::RefId& soundId, const ESM::Sound& sound)
    {
        static const AudioParams audioParams
            = makeAudioParams(MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>());

        float volume = static_cast<float>(std::pow(10.0, (sound.mData.mVolume / 255.0 * 3348.0 - 3348.0) / 2000.0));
        float min = sound.mData.mMinRange;
        float max = sound.mData.mMaxRange;
        if (min == 0 && max == 0)
        {
            min = audioParams.mAudioDefaultMinDistance;
            max = audioParams.mAudioDefaultMaxDistance;
        }

        min *= audioParams.mAudioMinDistanceMult;
        max *= audioParams.mAudioMaxDistanceMult;
        min = std::max(min, 1.0f);
        max = std::max(min, max);

        SoundBuffer& sfx = mSoundBuffers.emplace_back(
<<<<<<< HEAD
            Misc::ResourceHelpers::correctSoundPath(VFS::Path::toNormalized(sound.mSound)), volume, min, max);
=======
            Misc::ResourceHelpers::correctSoundPath(VFS::Path::Normalized(sound.mSound)), volume, min, max);
>>>>>>> origin/main

        mBufferNameMap.emplace(soundId, &sfx);
        return &sfx;
    }

    SoundBuffer* SoundBufferPool::insertSound(const ESM::RefId& soundId, const ESM4::Sound& sound)
    {
<<<<<<< HEAD
        VFS::Path::Normalized path = Misc::ResourceHelpers::correctResourcePath({ { soundDir } },
            VFS::Path::toNormalized(sound.mSoundFile), *MWBase::Environment::get().getResourceSystem()->getVFS(), mp3);
        float volume = 1, min = 1, max = 255; // TODO: needs research
        SoundBuffer& sfx = mSoundBuffers.emplace_back(std::move(path), volume, min, max);
=======
        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const std::optional<VFS::Path::Normalized> path = resolveFalloutSoundPath(sound.mSoundFile, *vfs);
        if (!path)
            return nullptr;
        float volume = 1, min = 1, max = 255; // TODO: needs research
        SoundBuffer& sfx = mSoundBuffers.emplace_back(*path, volume, min, max);
>>>>>>> origin/main
        mBufferNameMap.emplace(soundId, &sfx);
        return &sfx;
    }

    SoundBuffer* SoundBufferPool::insertSound(const ESM::RefId& soundId, const ESM4::SoundReference& sound)
    {
<<<<<<< HEAD
        VFS::Path::Normalized path = Misc::ResourceHelpers::correctResourcePath({ { soundDir } },
            VFS::Path::toNormalized(sound.mSoundFile), *MWBase::Environment::get().getResourceSystem()->getVFS(), mp3);
        float volume = 1, min = 1, max = 255; // TODO: needs research
        // TODO: sound.mSoundId can link to another SoundReference, probably we will need to add additional lookups to
        // ESMStore.
        SoundBuffer& sfx = mSoundBuffers.emplace_back(std::move(path), volume, min, max);
=======
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const std::string soundFile = resolveESM4SoundReferencePath(*store, sound);
        if (soundFile.empty())
        {
            Log(Debug::Warning) << "Unable to resolve ESM4 sound reference " << soundId << " to an audio file";
            return nullptr;
        }

        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const std::optional<VFS::Path::Normalized> path = resolveFalloutSoundPath(soundFile, *vfs);
        if (!path)
            return nullptr;
        float volume = 1, min = 1, max = 255; // TODO: needs research
        SoundBuffer& sfx = mSoundBuffers.emplace_back(*path, volume, min, max);
>>>>>>> origin/main
        mBufferNameMap.emplace(soundId, &sfx);
        return &sfx;
    }

    void SoundBufferPool::unloadUnused()
    {
        while (!mUnusedBuffers.empty() && mBufferCacheSize > mBufferCacheMin)
        {
            SoundBuffer* const unused = mUnusedBuffers.back();

            mBufferCacheSize -= mOutput->unloadSound(unused->getHandle());
            unused->mHandle = nullptr;

            mUnusedBuffers.pop_back();
        }
    }
}

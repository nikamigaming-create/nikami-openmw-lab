#include "soundbuffer.hpp"

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadsoun.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include <algorithm>
#include <cmath>

namespace MWSound
{
    namespace
    {
        constexpr unsigned int maxSoundReferenceDepth = 8;
        constexpr VFS::Path::NormalizedView soundDir("sound");
        constexpr VFS::Path::ExtensionView mp3("mp3");
        constexpr VFS::Path::ExtensionView wav("wav");
        constexpr VFS::Path::ExtensionView ogg("ogg");

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

        bool isDirectoryPath(std::string_view path)
        {
            return !path.empty() && (path.back() == '/' || path.back() == '\\');
        }

        bool isAudioResource(VFS::Path::NormalizedView path)
        {
            const VFS::Path::ExtensionView extension = path.extension();
            return extension == wav || extension == mp3 || extension == ogg;
        }
    }

    std::vector<VFS::Path::Normalized> resolveESM4SoundResourcePaths(
        std::string_view authoredPath, const VFS::Manager& vfs)
    {
        if (authoredPath.empty())
            return {};

        const VFS::Path::Normalized normalized(authoredPath);
        VFS::Path::Normalized corrected
            = Misc::ResourceHelpers::correctResourcePath({ { soundDir } }, normalized, vfs, mp3);
        if (!isDirectoryPath(authoredPath))
            return { std::move(corrected) };

        std::string directoryValue(corrected.value());
        while (!directoryValue.empty() && directoryValue.back() == VFS::Path::separator)
            directoryValue.pop_back();
        const VFS::Path::Normalized directory(std::move(directoryValue));

        std::vector<VFS::Path::Normalized> result;
        for (const VFS::Path::Normalized& candidate
            : vfs.getRecursiveDirectoryIterator(VFS::Path::NormalizedView(directory)))
        {
            if (candidate.parent() == VFS::Path::NormalizedView(directory) && isAudioResource(candidate))
                result.push_back(candidate);
        }

        if (result.empty())
            result.push_back(std::move(corrected));
        return result;
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
            for (SoundBuffer* const sfx : it->second)
            {
                if (sfx->getHandle() != nullptr)
                    return sfx;
            }
        }
        return nullptr;
    }

    SoundBuffer* SoundBufferPool::lookup(VFS::Path::NormalizedView fileName) const
    {
        const auto it = mBufferFileNameMap.find(fileName);
        if (it != mBufferFileNameMap.end())
        {
            SoundBuffer* sfx = it->second;
            if (sfx->getHandle() != nullptr)
                return sfx;
        }
        return nullptr;
    }

    bool SoundBufferPool::matches(const ESM::RefId& soundId, const SoundBuffer& buffer) const
    {
        const auto it = mBufferNameMap.find(soundId);
        return it != mBufferNameMap.end() && std::find(it->second.begin(), it->second.end(), &buffer) != it->second.end();
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
            const bool falloutNewVegas = esmstore->getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas;
            if (!falloutNewVegas)
            {
                for (const ESM::Sound& sound : esmstore->get<ESM::Sound>())
                    insertSound(sound.mId, sound);
            }

            std::size_t soundEditorIdAliases = 0;
            std::size_t authoredDirectorySounds = 0;
            std::size_t authoredDirectorySoundVariants = 0;
            for (const ESM4::Sound& sound : esmstore->get<ESM4::Sound>())
            {
                SoundBuffer* const buffer = insertSound(sound.mId, sound);
                if (buffer != nullptr && !sound.mEditorId.empty())
                {
                    const auto byFormId = mBufferNameMap.find(sound.mId);
                    if (byFormId != mBufferNameMap.end())
                    {
                        if (byFormId->second.size() > 1)
                        {
                            ++authoredDirectorySounds;
                            authoredDirectorySoundVariants += byFormId->second.size();
                        }
                        soundEditorIdAliases
                            += mBufferNameMap.emplace(ESM::RefId::stringRefId(sound.mEditorId), byFormId->second).second;
                    }
                }
            }

            std::size_t soundReferenceEditorIdAliases = 0;
            std::size_t authoredDirectorySoundReferences = 0;
            std::size_t authoredDirectorySoundReferenceVariants = 0;
            for (const ESM4::SoundReference& sound : esmstore->get<ESM4::SoundReference>())
            {
                SoundBuffer* const buffer = insertSound(sound.mId, sound);
                if (buffer != nullptr && !sound.mEditorId.empty())
                {
                    const auto byFormId = mBufferNameMap.find(sound.mId);
                    if (byFormId != mBufferNameMap.end())
                    {
                        if (byFormId->second.size() > 1)
                        {
                            ++authoredDirectorySoundReferences;
                            authoredDirectorySoundReferenceVariants += byFormId->second.size();
                        }
                        soundReferenceEditorIdAliases
                            += mBufferNameMap.emplace(
                                   ESM::RefId::stringRefId(sound.mEditorId), byFormId->second)
                                   .second;
                    }
                }
            }

            Log(Debug::Info) << "FNV/ESM4 sound: registered editor-id aliases sounds=" << soundEditorIdAliases
                             << " references=" << soundReferenceEditorIdAliases
                             << " directorySounds=" << authoredDirectorySounds
                             << " directorySoundVariants=" << authoredDirectorySoundVariants
                             << " directoryReferences=" << authoredDirectorySoundReferences
                             << " directoryReferenceVariants=" << authoredDirectorySoundReferenceVariants
                             << " legacyEsm3Fallback=" << (falloutNewVegas ? "disabled" : "enabled");
        }

        SoundBuffer* sfx;
        const auto it = mBufferNameMap.find(soundId);
        if (it != mBufferNameMap.end())
        {
            const SoundBufferList& variants = it->second;
            if (variants.empty())
                return {};

            const std::size_t first = Misc::Rng::rollDice(variants.size());
            for (std::size_t offset = 0; offset < variants.size(); ++offset)
            {
                sfx = variants[(first + offset) % variants.size()];
                if (SoundBuffer* const loaded = loadSfx(sfx))
                    return loaded;
            }
            return {};
        }
        else
        {
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
        }

        return loadSfx(sfx);
    }

    SoundBuffer* SoundBufferPool::load(VFS::Path::NormalizedView fileName)
    {
        SoundBuffer* sfx;
        const auto it = mBufferFileNameMap.find(fileName);
        if (it != mBufferFileNameMap.end())
            sfx = it->second;
        else
            sfx = insertSound(fileName);

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

    SoundBuffer* SoundBufferPool::insertSound(VFS::Path::NormalizedView fileName)
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
            Misc::ResourceHelpers::correctSoundPath(VFS::Path::toNormalized(sound.mSound)), volume, min, max, soundId);

        mBufferNameMap.emplace(soundId, SoundBufferList{ &sfx });
        return &sfx;
    }

    SoundBuffer* SoundBufferPool::insertSound(const ESM::RefId& soundId, const ESM4::Sound& sound)
    {
        const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
        std::vector<VFS::Path::Normalized> paths = resolveESM4SoundResourcePaths(sound.mSoundFile, vfs);
        if (paths.empty())
            return nullptr;

        float volume = 1, min = 1, max = 255; // TODO: needs research
        SoundBufferList buffers;
        buffers.reserve(paths.size());
        for (VFS::Path::Normalized& path : paths)
            buffers.push_back(&mSoundBuffers.emplace_back(std::move(path), volume, min, max, soundId));

        SoundBuffer* const result = buffers.front();
        mBufferNameMap.emplace(soundId, std::move(buffers));
        return result;
    }

    SoundBuffer* SoundBufferPool::insertSound(const ESM::RefId& soundId, const ESM4::SoundReference& sound)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const std::string soundFile = resolveESM4SoundReferencePath(*store, sound);
        if (soundFile.empty())
        {
            Log(Debug::Warning) << "Unable to resolve ESM4 sound reference " << soundId << " to an audio file";
            return nullptr;
        }

        const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
        std::vector<VFS::Path::Normalized> paths = resolveESM4SoundResourcePaths(soundFile, vfs);
        if (paths.empty())
            return nullptr;

        float volume = 1, min = 1, max = 255; // TODO: needs research
        SoundBufferList buffers;
        buffers.reserve(paths.size());
        for (VFS::Path::Normalized& path : paths)
            buffers.push_back(&mSoundBuffers.emplace_back(std::move(path), volume, min, max, soundId));

        SoundBuffer* const result = buffers.front();
        mBufferNameMap.emplace(soundId, std::move(buffers));
        return result;
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

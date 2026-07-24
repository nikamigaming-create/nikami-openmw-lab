#include "regionsoundselector.hpp"

#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm4/loadregn.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/rng.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"

namespace MWSound
{
    RegionSoundSelector::RegionSoundSelector()
        : mMinTimeBetweenSounds(Fallback::Map::getFloat("Weather_Minimum_Time_Between_Environmental_Sounds"))
        , mMaxTimeBetweenSounds(Fallback::Map::getFloat("Weather_Maximum_Time_Between_Environmental_Sounds"))
    {
    }

    bool RegionSoundSelector::updateTimer(float duration)
    {
        mTimePassed += duration;

        if (mTimePassed < mTimeToNextEnvSound)
            return false;

        const float a = Misc::Rng::rollClosedProbability();
        mTimeToNextEnvSound = mMinTimeBetweenSounds + (mMaxTimeBetweenSounds - mMinTimeBetweenSounds) * a;
        mTimePassed = 0;
        return true;
    }

    ESM::RefId RegionSoundSelector::getNextRandom(float duration, const ESM::RefId& regionName)
    {
        if (!updateTimer(duration))
            return {};

        const ESM::Region* const region
            = MWBase::Environment::get().getESMStore()->get<ESM::Region>().search(regionName);

        if (region == nullptr)
            return {};

        for (const ESM::Region::SoundRef& sound : region->mSoundList)
        {
            if (Misc::Rng::roll0to99() < sound.mChance)
                return sound.mSound;
        }
        return {};
    }

    ESM::RefId RegionSoundSelector::getNextRandom(
        float duration, std::span<const ESM::FormId> regionIds, std::uint32_t weatherClassification)
    {
        if (!updateTimer(duration))
            return {};

        const MWWorld::ESMStore* const store = MWBase::Environment::get().getESMStore();
        const ESM4::Region* selectedRegion = nullptr;
        const ESM4::Region::RegionSoundBlock* selectedBlock = nullptr;
        for (const ESM::FormId& regionId : regionIds)
        {
            const ESM4::Region* const region = store->get<ESM4::Region>().search(ESM::RefId(regionId));
            if (region == nullptr)
                continue;
            for (const ESM4::Region::RegionSoundBlock& block : region->mSoundBlocks)
            {
                if (block.mEntries.empty())
                    continue;
                if (selectedBlock == nullptr || block.mData.priority > selectedBlock->mData.priority)
                {
                    selectedRegion = region;
                    selectedBlock = &block;
                }
            }
        }

        if (selectedBlock == nullptr)
            return {};

        if (weatherClassification == 0)
            weatherClassification = 1; // FNV clear/pleasant fallback for malformed WTHR records.
        for (const ESM4::Region::RegionSound& sound : selectedBlock->mEntries)
        {
            if (sound.mFlags != 0 && (sound.mFlags & weatherClassification) == 0)
                continue;
            if (sound.mChance == 0 || Misc::Rng::rollDice(1'000'000) >= std::min(sound.mChance, 1'000'000u))
                continue;

            Log(Debug::Info) << "FNV/ESM4 sound: selected region ambience region=" << selectedRegion->mId
                             << " editorId=" << selectedRegion->mEditorId << " sound=" << sound.mSound
                             << " weatherClassification=" << weatherClassification
                             << " chanceFixed4=" << sound.mChance;
            return ESM::RefId(sound.mSound);
        }
        return {};
    }
}

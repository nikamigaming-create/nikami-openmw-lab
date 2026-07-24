#ifndef GAME_SOUND_REGIONSOUNDSELECTOR_H
#define GAME_SOUND_REGIONSOUNDSELECTOR_H

#include <cstdint>
#include <span>

#include <components/esm/formid.hpp>
#include <components/esm/refid.hpp>

namespace MWSound
{
    class RegionSoundSelector
    {
    public:
        ESM::RefId getNextRandom(float duration, const ESM::RefId& regionName);
        ESM::RefId getNextRandom(
            float duration, std::span<const ESM::FormId> regionIds, std::uint32_t weatherClassification);

        RegionSoundSelector();

    private:
        float mTimeToNextEnvSound = 0.0f;
        float mTimePassed = 0.0;
        float mMinTimeBetweenSounds;
        float mMaxTimeBetweenSounds;

        bool updateTimer(float duration);
    };
}

#endif

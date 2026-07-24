#ifndef GAME_MWWORLD_FNVMOVEMENT_H
#define GAME_MWWORLD_FNVMOVEMENT_H

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace MWWorld
{
    inline constexpr float sFalloutMoveBaseSpeed = 77.f;
    inline constexpr float sFalloutMoveRunMultiplier = 4.f;

    [[nodiscard]] inline float getFalloutWalkSpeed(
        float speedMultiplier, float baseSpeed = sFalloutMoveBaseSpeed) noexcept
    {
        return std::max(baseSpeed, 1.f) * std::max(speedMultiplier, 0.01f);
    }

    [[nodiscard]] inline float getFalloutRunSpeed(float walkSpeed,
        float runMultiplier = sFalloutMoveRunMultiplier) noexcept
    {
        return walkSpeed * std::max(runMultiplier, 1.f);
    }

    [[nodiscard]] inline float parseFalloutPlayerSpeedScale(const char* value) noexcept
    {
        if (value == nullptr || *value == '\0')
            return 1.f;
        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value || *end != '\0' || !std::isfinite(parsed))
            return 1.f;
        return std::clamp(parsed, 0.1f, 10.f);
    }

    [[nodiscard]] inline float getFalloutPlayerSpeedScale() noexcept
    {
        static const float scale
            = parseFalloutPlayerSpeedScale(std::getenv("OPENMW_FNV_PLAYER_SPEED_MULTIPLIER"));
        return scale;
    }
}

#endif

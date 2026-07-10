#ifndef OPENMW_COMPONENTS_ESM4_LIP_H
#define OPENMW_COMPONENTS_ESM4_LIP_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace ESM4
{
    class LipAnimation
    {
    public:
        static constexpr std::size_t sTargetCount = 33;
        static constexpr float sFramesPerSecond = 30.f;

        static LipAnimation load(std::istream& stream);

        float getValue(std::string_view target, double seconds) const;
        float getValue(std::size_t target, double seconds) const;

        std::int32_t getStartFrame() const { return mStartFrame; }
        std::size_t getFrameCount() const { return mFrames.size(); }

        static std::string_view getTargetName(std::size_t target);

    private:
        std::int32_t mStartFrame = 0;
        std::vector<std::array<float, sTargetCount>> mFrames;
    };
}

#endif

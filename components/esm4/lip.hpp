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
    /// Fallout 3/New Vegas LIP animation curves.
    ///
    /// This is a data decoder only. Dialogue, sound, and actor-animation
    /// ownership stays with their existing engine systems.
    class LipAnimation
    {
    public:
        static constexpr std::size_t sTargetCount = 33;
        static constexpr float sFramesPerSecond = 30.f;

        // Resource-safety defaults are injectable for callers that own a
        // different loading policy; the file-format constants above are not.
        static constexpr std::size_t sDefaultMaxDecodedBytes = 512U * 1024U * 1024U;
        static constexpr std::size_t sDefaultMaxFrames = 30U * 60U * 60U;
        static constexpr float sDefaultMaxTargetValue = 16.f;

        struct DecodeLimits
        {
            std::size_t mMaxDecodedBytes = sDefaultMaxDecodedBytes;
            std::size_t mMaxFrames = sDefaultMaxFrames;
            float mMaxTargetValue = sDefaultMaxTargetValue;
        };

        static LipAnimation load(std::istream& stream);
        static LipAnimation load(std::istream& stream, DecodeLimits limits);

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

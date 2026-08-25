#include <components/esm4/lip.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    constexpr std::uint32_t kLipVersion = 1;
    constexpr std::uint32_t kCompressedFlag = 0x1;
    constexpr std::uint32_t kDecodedFlags = 0;
    constexpr std::uint8_t kUncompressedPayloadMarker = 2;
    constexpr std::uint32_t kFrameCount = 3;
    constexpr std::int32_t kStartFrame = -1;
    constexpr std::size_t kAahTarget = 0;
    constexpr std::size_t kWTarget = 15;
    constexpr std::size_t kBlinkLeftTarget = 16;
    constexpr std::size_t kHeadYawTarget = 32;
    constexpr std::size_t kStoredSizeOverheadBytes = 16;
    constexpr std::size_t kImplicitTailBytes = 4;
    constexpr std::uint8_t kZeroRunMarker = 0;

    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    std::string compressZeroRuns(std::string_view input)
    {
        std::string output;
        for (std::size_t offset = 0; offset < input.size();)
        {
            if (input[offset] != '\0')
            {
                output.push_back(input[offset++]);
                continue;
            }

            const std::size_t start = offset;
            while (offset < input.size() && input[offset] == '\0'
                && offset - start < std::numeric_limits<std::uint16_t>::max())
                ++offset;
            output.push_back(static_cast<char>(kZeroRunMarker));
            appendPod(output, static_cast<std::uint16_t>(offset - start));
        }
        return output;
    }

    std::string makeDecodedLip()
    {
        std::array<std::array<float, ESM4::LipAnimation::sTargetCount>, kFrameCount> frames{};
        frames[0][kAahTarget] = 0.f;
        frames[1][kAahTarget] = 0.5f;
        frames[2][kAahTarget] = 1.f;
        frames[0][kHeadYawTarget] = -0.25f;
        frames[1][kHeadYawTarget] = 0.25f;

        std::string decoded;
        appendPod(decoded, kFrameCount);
        appendPod(decoded, kStartFrame);
        appendPod(decoded, kDecodedFlags);
        for (const auto& frame : frames)
            for (float value : frame)
                appendPod(decoded, value);

        decoded.resize(decoded.size() - kImplicitTailBytes);
        return decoded;
    }

    std::string makeCompressedLip()
    {
        const std::string decoded = makeDecodedLip();
        std::string output;
        appendPod(output, kLipVersion);
        appendPod(output, static_cast<std::uint32_t>(decoded.size() + kStoredSizeOverheadBytes));
        appendPod(output, kCompressedFlag);
        output += compressZeroRuns(decoded);
        return output;
    }

    std::string makeUncompressedLip()
    {
        const std::string decoded = makeDecodedLip();
        std::string output;
        appendPod(output, kLipVersion);
        appendPod(output, static_cast<std::uint32_t>(decoded.size() + kStoredSizeOverheadBytes));
        appendPod(output, kDecodedFlags);
        output.push_back(static_cast<char>(kUncompressedPayloadMarker));
        output += decoded;
        return output;
    }

    TEST(Esm4LipTest, decodesFalloutCurvesAtTheAuthoredVoiceClock)
    {
        std::istringstream stream(makeCompressedLip());
        const ESM4::LipAnimation lip = ESM4::LipAnimation::load(stream);

        EXPECT_EQ(lip.getStartFrame(), kStartFrame);
        EXPECT_EQ(lip.getFrameCount(), kFrameCount);
        EXPECT_FLOAT_EQ(lip.getValue("Aah", 0.0), 0.5f);
        EXPECT_FLOAT_EQ(lip.getValue("aAH", 1.0 / 60.0), 0.75f);
        EXPECT_FLOAT_EQ(lip.getValue("HeadYaw", 0.0), 0.25f);
        EXPECT_FLOAT_EQ(lip.getValue("UnknownMorph", 0.0), 0.f);
        EXPECT_FLOAT_EQ(lip.getValue("Aah", 1.0), 0.f);
    }

    TEST(Esm4LipTest, exposesAuthoredTargetOrder)
    {
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(kAahTarget), "Aah");
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(kWTarget), "W");
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(kBlinkLeftTarget), "BlinkLeft");
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(kHeadYawTarget), "HeadYaw");
        EXPECT_TRUE(ESM4::LipAnimation::getTargetName(ESM4::LipAnimation::sTargetCount).empty());
    }

    TEST(Esm4LipTest, decodesUncompressedFalloutContainer)
    {
        std::istringstream stream(makeUncompressedLip());
        const ESM4::LipAnimation lip = ESM4::LipAnimation::load(stream);

        EXPECT_EQ(lip.getFrameCount(), kFrameCount);
        EXPECT_FLOAT_EQ(lip.getValue(kAahTarget, 0.0), 0.5f);
        EXPECT_FLOAT_EQ(lip.getValue(kHeadYawTarget, 0.0), 0.25f);
    }

    TEST(Esm4LipTest, rejectsTruncatedRunsAndInjectedOversizePolicies)
    {
        std::string truncated;
        appendPod(truncated, kLipVersion);
        appendPod(truncated, static_cast<std::uint32_t>(kStoredSizeOverheadBytes));
        appendPod(truncated, kCompressedFlag);
        truncated.push_back(static_cast<char>(kZeroRunMarker));
        std::istringstream truncatedStream(truncated);
        EXPECT_THROW(ESM4::LipAnimation::load(truncatedStream), std::runtime_error);

        ESM4::LipAnimation::DecodeLimits limits;
        limits.mMaxDecodedBytes = kStoredSizeOverheadBytes;
        std::istringstream limitedStream(makeCompressedLip());
        EXPECT_THROW(ESM4::LipAnimation::load(limitedStream, limits), std::runtime_error);
    }
}

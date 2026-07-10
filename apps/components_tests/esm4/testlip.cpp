#include <components/esm4/lip.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
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
            while (offset < input.size() && input[offset] == '\0' && offset - start < 0xffff)
                ++offset;
            output.push_back('\0');
            appendPod(output, static_cast<std::uint16_t>(offset - start));
        }
        return output;
    }

    std::string makeRetailLip()
    {
        constexpr std::uint32_t frameCount = 3;
        std::array<std::array<float, ESM4::LipAnimation::sTargetCount>, frameCount> frames{};
        frames[0][0] = 0.f;
        frames[1][0] = 0.5f;
        frames[2][0] = 1.f;
        frames[0][32] = -0.25f;
        frames[1][32] = 0.25f;

        std::string decoded;
        appendPod(decoded, frameCount);
        appendPod(decoded, std::int32_t{ -1 });
        appendPod(decoded, std::uint32_t{ 0 });
        for (const auto& frame : frames)
            for (float value : frame)
                appendPod(decoded, value);

        // Retail FO3/FNV zero-run streams omit four trailing zero bytes. The stored size includes sixteen bytes
        // outside this decoded payload, matching authored files in both games.
        decoded.resize(decoded.size() - 4);
        std::string output;
        appendPod(output, std::uint32_t{ 1 });
        appendPod(output, static_cast<std::uint32_t>(decoded.size() + 16));
        appendPod(output, std::uint32_t{ 1 });
        output += compressZeroRuns(decoded);
        return output;
    }

    TEST(Esm4LipTest, shouldDecodeRetailFalloutCurvesAtThirtyFramesPerSecond)
    {
        std::istringstream stream(makeRetailLip());
        const ESM4::LipAnimation lip = ESM4::LipAnimation::load(stream);

        EXPECT_EQ(lip.getStartFrame(), -1);
        EXPECT_EQ(lip.getFrameCount(), 3);
        EXPECT_FLOAT_EQ(lip.getValue("Aah", 0.0), 0.5f);
        EXPECT_FLOAT_EQ(lip.getValue("aAH", 1.0 / 60.0), 0.75f);
        EXPECT_FLOAT_EQ(lip.getValue("HeadYaw", 0.0), 0.25f);
        EXPECT_FLOAT_EQ(lip.getValue("UnknownMorph", 0.0), 0.f);
        EXPECT_FLOAT_EQ(lip.getValue("Aah", 1.0), 0.f);
    }

    TEST(Esm4LipTest, shouldExposeRetailTargetOrder)
    {
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(0), "Aah");
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(15), "W");
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(16), "BlinkLeft");
        EXPECT_EQ(ESM4::LipAnimation::getTargetName(32), "HeadYaw");
    }

    TEST(Esm4LipTest, shouldRejectTruncatedZeroRun)
    {
        std::string file;
        appendPod(file, std::uint32_t{ 1 });
        appendPod(file, std::uint32_t{ 32 });
        appendPod(file, std::uint32_t{ 1 });
        file.push_back('\0');
        std::istringstream stream(file);
        EXPECT_THROW(ESM4::LipAnimation::load(stream), std::runtime_error);
    }
}

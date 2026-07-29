#include "lip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <istream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ESM4
{
    namespace
    {
        constexpr std::uint32_t sCompressed = 0x1;
        constexpr std::uint32_t sBigEndian = 0x2;
        constexpr std::size_t sHeaderSize = 12;
        constexpr std::size_t sDecodedHeaderSize = 12;
        constexpr std::size_t sRetailImplicitTailSize = 4;
        constexpr std::uint32_t sMaxDecodedSize = 512 * 1024 * 1024;
        constexpr std::size_t sMaxFrames = 30 * 60 * 60;

        constexpr std::array<std::string_view, LipAnimation::sTargetCount> sTargetNames{
            "Aah", "BigAah", "BMP", "ChJSh", "DST", "Eee", "Eh", "FV", "I", "K", "N", "Oh",
            "OohQ", "R", "Th", "W", "BlinkLeft", "BlinkRight", "BrowDownLeft", "BrowDownRight",
            "BrowInLeft", "BrowInRight", "BrowUpLeft", "BrowUpRight", "LookDown", "LookLeft",
            "LookRight", "LookUp", "SquintLeft", "SquintRight", "HeadPitch", "HeadRoll", "HeadYaw"
        };

        std::uint16_t readUint16(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 2 > data.size())
                throw std::runtime_error("Truncated Fallout LIP uint16");
            return static_cast<std::uint16_t>(data[offset])
                | static_cast<std::uint16_t>(data[offset + 1] << 8);
        }

        std::uint32_t readUint32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 4 > data.size())
                throw std::runtime_error("Truncated Fallout LIP uint32");
            return static_cast<std::uint32_t>(data[offset])
                | static_cast<std::uint32_t>(data[offset + 1]) << 8
                | static_cast<std::uint32_t>(data[offset + 2]) << 16
                | static_cast<std::uint32_t>(data[offset + 3]) << 24;
        }

        std::vector<std::uint8_t> decodeZeroRuns(
            const std::vector<std::uint8_t>& file, std::size_t expectedSize)
        {
            std::vector<std::uint8_t> result;
            result.reserve(expectedSize);
            for (std::size_t offset = sHeaderSize; offset < file.size();)
            {
                const std::uint8_t value = file[offset++];
                if (value != 0)
                {
                    result.push_back(value);
                    continue;
                }

                const std::uint16_t count = readUint16(file, offset);
                offset += 2;
                if (count == 0 || result.size() + count > expectedSize)
                    throw std::runtime_error("Invalid Fallout LIP zero run");
                result.insert(result.end(), count, 0);
            }
            if (result.size() != expectedSize)
                throw std::runtime_error("Truncated Fallout LIP zero-run payload");
            return result;
        }

        bool equalAsciiCaseInsensitive(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
                return false;
            return std::equal(left.begin(), left.end(), right.begin(), [](char l, char r) {
                if (l >= 'A' && l <= 'Z')
                    l = static_cast<char>(l + ('a' - 'A'));
                if (r >= 'A' && r <= 'Z')
                    r = static_cast<char>(r + ('a' - 'A'));
                return l == r;
            });
        }
    }

    LipAnimation LipAnimation::load(std::istream& stream)
    {
        const std::vector<std::uint8_t> file{
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
        };
        if (file.size() < sHeaderSize)
            throw std::runtime_error("Truncated Fallout LIP header");

        const std::uint32_t version = readUint32(file, 0);
        const std::uint32_t storedSize = readUint32(file, 4);
        const std::uint32_t flags = readUint32(file, 8);
        if (version != 1)
            throw std::runtime_error("Unsupported Fallout LIP version");
        if ((flags & sBigEndian) != 0)
            throw std::runtime_error("Big-endian Fallout LIP is not supported");
        if (storedSize < 16 || storedSize > sMaxDecodedSize)
            throw std::runtime_error("Invalid Fallout LIP decoded size");

        std::vector<std::uint8_t> decoded;
        if ((flags & sCompressed) != 0)
            decoded = decodeZeroRuns(file, storedSize - 16);
        else
        {
            if (file.size() <= sHeaderSize || file[sHeaderSize] != 2)
                throw std::runtime_error("Invalid uncompressed Fallout LIP payload");
            decoded.assign(file.begin() + sHeaderSize + 1, file.end());
        }

        if (decoded.size() < sDecodedHeaderSize)
            throw std::runtime_error("Truncated Fallout LIP animation header");
        const std::uint32_t frameCount = readUint32(decoded, 0);
        const std::int32_t startFrame = static_cast<std::int32_t>(readUint32(decoded, 4));
        if (frameCount == 0 || frameCount > sMaxFrames)
            throw std::runtime_error("Invalid Fallout LIP frame count");

        const std::size_t requiredSize
            = sDecodedHeaderSize + static_cast<std::size_t>(frameCount) * sTargetCount * sizeof(float);
        if (decoded.size() + sRetailImplicitTailSize < requiredSize || decoded.size() > requiredSize)
            throw std::runtime_error("Unexpected Fallout LIP target payload size");
        decoded.resize(requiredSize, 0);

        LipAnimation result;
        result.mStartFrame = startFrame;
        result.mFrames.resize(frameCount);
        std::size_t offset = sDecodedHeaderSize;
        for (auto& frame : result.mFrames)
        {
            for (float& value : frame)
            {
                const std::uint32_t bits = readUint32(decoded, offset);
                std::memcpy(&value, &bits, sizeof(value));
                if (!std::isfinite(value) || std::abs(value) > 16.f)
                    throw std::runtime_error("Invalid Fallout LIP target value");
                offset += sizeof(float);
            }
        }
        return result;
    }

    float LipAnimation::getValue(std::string_view target, double seconds) const
    {
        for (std::size_t i = 0; i < sTargetNames.size(); ++i)
            if (equalAsciiCaseInsensitive(target, sTargetNames[i]))
                return getValue(i, seconds);
        return 0.f;
    }

    float LipAnimation::getValue(std::size_t target, double seconds) const
    {
        if (target >= sTargetCount || mFrames.empty() || !std::isfinite(seconds))
            return 0.f;

        const double position = seconds * sFramesPerSecond - static_cast<double>(mStartFrame);
        if (position < 0.0 || position > static_cast<double>(mFrames.size() - 1))
            return 0.f;
        const std::size_t lower = static_cast<std::size_t>(position);
        const std::size_t upper = std::min(lower + 1, mFrames.size() - 1);
        const float factor = static_cast<float>(position - static_cast<double>(lower));
        return mFrames[lower][target] + (mFrames[upper][target] - mFrames[lower][target]) * factor;
    }

    std::string_view LipAnimation::getTargetName(std::size_t target)
    {
        return target < sTargetNames.size() ? sTargetNames[target] : std::string_view{};
    }
}

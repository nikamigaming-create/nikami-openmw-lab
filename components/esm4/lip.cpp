#include "lip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <istream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ESM4
{
    namespace
    {
        constexpr std::uint32_t kLipVersion = 1;
        constexpr std::uint32_t kCompressedFlag = 0x1;
        constexpr std::uint32_t kBigEndianFlag = 0x2;
        constexpr std::uint32_t kSupportedFlags = kCompressedFlag | kBigEndianFlag;
        constexpr std::uint32_t kUncompressedPayloadMarker = 2;
        constexpr std::size_t kFileHeaderBytes = 3 * sizeof(std::uint32_t);
        constexpr std::size_t kDecodedHeaderBytes = 3 * sizeof(std::uint32_t);
        constexpr std::size_t kStoredSizeOverheadBytes = 16;
        constexpr std::size_t kImplicitTailBytes = 4;

        constexpr std::array<std::string_view, LipAnimation::sTargetCount> kTargetNames{
            "Aah", "BigAah", "BMP", "ChJSh", "DST", "Eee", "Eh", "FV", "I", "K", "N", "Oh",
            "OohQ", "R", "Th", "W", "BlinkLeft", "BlinkRight", "BrowDownLeft", "BrowDownRight",
            "BrowInLeft", "BrowInRight", "BrowUpLeft", "BrowUpRight", "LookDown", "LookLeft",
            "LookRight", "LookUp", "SquintLeft", "SquintRight", "HeadPitch", "HeadRoll", "HeadYaw"
        };

        std::uint16_t readUint16(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(std::uint16_t))
                throw std::runtime_error("Truncated Fallout LIP uint16");
            return static_cast<std::uint16_t>(data[offset])
                | static_cast<std::uint16_t>(data[offset + 1]) << 8;
        }

        std::uint32_t readUint32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(std::uint32_t))
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
            for (std::size_t offset = kFileHeaderBytes; offset < file.size();)
            {
                const std::uint8_t value = file[offset++];
                if (value != 0)
                {
                    if (result.size() == expectedSize)
                        throw std::runtime_error("Oversized Fallout LIP payload");
                    result.push_back(value);
                    continue;
                }

                const std::uint16_t count = readUint16(file, offset);
                offset += sizeof(std::uint16_t);
                if (count == 0 || count > expectedSize || result.size() > expectedSize - count)
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
        return load(stream, DecodeLimits{});
    }

    LipAnimation LipAnimation::load(std::istream& stream, DecodeLimits limits)
    {
        const std::vector<std::uint8_t> file{
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
        };
        if (file.size() < kFileHeaderBytes)
            throw std::runtime_error("Truncated Fallout LIP header");

        const std::uint32_t version = readUint32(file, 0);
        const std::uint32_t storedSize = readUint32(file, sizeof(std::uint32_t));
        const std::uint32_t flags = readUint32(file, 2 * sizeof(std::uint32_t));
        if (version != kLipVersion)
            throw std::runtime_error("Unsupported Fallout LIP version");
        if ((flags & kBigEndianFlag) != 0)
            throw std::runtime_error("Big-endian Fallout LIP is not supported");
        if ((flags & ~kSupportedFlags) != 0)
            throw std::runtime_error("Unsupported Fallout LIP flags");
        if (storedSize < kStoredSizeOverheadBytes || storedSize > limits.mMaxDecodedBytes)
            throw std::runtime_error("Invalid Fallout LIP decoded size");

        std::vector<std::uint8_t> decoded;
        if ((flags & kCompressedFlag) != 0)
            decoded = decodeZeroRuns(file, storedSize - kStoredSizeOverheadBytes);
        else
        {
            if (file.size() <= kFileHeaderBytes || file[kFileHeaderBytes] != kUncompressedPayloadMarker)
                throw std::runtime_error("Invalid uncompressed Fallout LIP payload");
            decoded.assign(file.begin() + kFileHeaderBytes + sizeof(std::uint8_t), file.end());
        }

        if (decoded.size() < kDecodedHeaderBytes)
            throw std::runtime_error("Truncated Fallout LIP animation header");
        const std::uint32_t frameCount = readUint32(decoded, 0);
        const std::int32_t startFrame = static_cast<std::int32_t>(readUint32(decoded, sizeof(std::uint32_t)));
        static_cast<void>(readUint32(decoded, 2 * sizeof(std::uint32_t)));
        if (frameCount == 0 || frameCount > limits.mMaxFrames)
            throw std::runtime_error("Invalid Fallout LIP frame count");

        constexpr std::size_t kFrameBytes = LipAnimation::sTargetCount * sizeof(float);
        if (frameCount > (limits.mMaxDecodedBytes - kDecodedHeaderBytes) / kFrameBytes)
            throw std::runtime_error("Fallout LIP frame data exceeds decode limit");
        const std::size_t requiredSize = kDecodedHeaderBytes + static_cast<std::size_t>(frameCount) * kFrameBytes;
        if (decoded.size() > limits.mMaxDecodedBytes - kImplicitTailBytes
            || decoded.size() + kImplicitTailBytes < requiredSize || decoded.size() > requiredSize)
            throw std::runtime_error("Unexpected Fallout LIP target payload size");
        decoded.resize(requiredSize, 0);

        LipAnimation result;
        result.mStartFrame = startFrame;
        result.mFrames.resize(frameCount);
        std::size_t offset = kDecodedHeaderBytes;
        for (auto& frame : result.mFrames)
        {
            for (float& value : frame)
            {
                const std::uint32_t bits = readUint32(decoded, offset);
                std::memcpy(&value, &bits, sizeof(value));
                if (!std::isfinite(value) || std::abs(value) > limits.mMaxTargetValue)
                    throw std::runtime_error("Invalid Fallout LIP target value");
                offset += sizeof(float);
            }
        }
        return result;
    }

    float LipAnimation::getValue(std::string_view target, double seconds) const
    {
        for (std::size_t i = 0; i < kTargetNames.size(); ++i)
            if (equalAsciiCaseInsensitive(target, kTargetNames[i]))
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
        return target < kTargetNames.size() ? kTargetNames[target] : std::string_view{};
    }
}

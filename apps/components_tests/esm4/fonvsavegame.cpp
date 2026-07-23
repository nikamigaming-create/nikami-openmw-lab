#include <components/esm4/fonvsavegame.hpp>
#include <components/esm/util.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::uint8_t sDelimiter = 0x7c;

    std::string sha256Hex(std::span<const std::uint8_t> input)
    {
        constexpr std::array<std::uint32_t, 64> constants = { 0x428a2f98, 0x71374491, 0xb5c0fbcf,
            0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1,
            0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351,
            0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb,
            0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814,
            0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        std::array<std::uint32_t, 8> hash = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

        std::vector<std::uint8_t> padded(input.begin(), input.end());
        const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8;
        padded.push_back(0x80);
        while (padded.size() % 64 != 56)
            padded.push_back(0);
        for (int shift = 56; shift >= 0; shift -= 8)
            padded.push_back(static_cast<std::uint8_t>(bitLength >> shift));

        for (std::size_t offset = 0; offset < padded.size(); offset += 64)
        {
            std::array<std::uint32_t, 64> words{};
            for (std::size_t i = 0; i < 16; ++i)
            {
                const std::size_t wordOffset = offset + i * 4;
                words[i] = (static_cast<std::uint32_t>(padded[wordOffset]) << 24)
                    | (static_cast<std::uint32_t>(padded[wordOffset + 1]) << 16)
                    | (static_cast<std::uint32_t>(padded[wordOffset + 2]) << 8)
                    | static_cast<std::uint32_t>(padded[wordOffset + 3]);
            }
            for (std::size_t i = 16; i < words.size(); ++i)
            {
                const std::uint32_t s0
                    = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
                const std::uint32_t s1
                    = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
                words[i] = words[i - 16] + s0 + words[i - 7] + s1;
            }

            std::uint32_t a = hash[0];
            std::uint32_t b = hash[1];
            std::uint32_t c = hash[2];
            std::uint32_t d = hash[3];
            std::uint32_t e = hash[4];
            std::uint32_t f = hash[5];
            std::uint32_t g = hash[6];
            std::uint32_t h = hash[7];
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                const std::uint32_t sum1
                    = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
                const std::uint32_t choose = (e & f) ^ (~e & g);
                const std::uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
                const std::uint32_t sum0
                    = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
                const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t temp2 = sum0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }
            hash[0] += a;
            hash[1] += b;
            hash[2] += c;
            hash[3] += d;
            hash[4] += e;
            hash[5] += f;
            hash[6] += g;
            hash[7] += h;
        }

        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (const std::uint32_t word : hash)
            result << std::setw(8) << word;
        return result.str();
    }

    std::vector<std::uint8_t> readFixtureBytes(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            throw std::runtime_error("could not open external fixture");
        const std::streampos end = stream.tellg();
        if (end < 0)
            throw std::runtime_error("could not size external fixture");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        stream.seekg(0);
        if (!bytes.empty())
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size())
            throw std::runtime_error("external fixture changed or was truncated while reading");
        return bytes;
    }

    void appendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
    {
        bytes.push_back(value);
    }

    void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    }

    void appendF32(std::vector<std::uint8_t>& bytes, float value)
    {
        appendU32(bytes, std::bit_cast<std::uint32_t>(value));
    }

    void appendF64(std::vector<std::uint8_t>& bytes, double value)
    {
        const std::uint64_t encoded = std::bit_cast<std::uint64_t>(value);
        appendU32(bytes, static_cast<std::uint32_t>(encoded));
        appendU32(bytes, static_cast<std::uint32_t>(encoded >> 32));
    }

    void overwriteU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
    {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    }

    void appendDelimiter(std::vector<std::uint8_t>& bytes)
    {
        bytes.push_back(sDelimiter);
    }

    void appendDelimitedReferenceId(std::vector<std::uint8_t>& bytes, std::uint32_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value));
        appendDelimiter(bytes);
    }

    void appendPackedCount(std::vector<std::uint8_t>& bytes, std::uint32_t value)
    {
        if (value <= 0x3f)
            appendU8(bytes, static_cast<std::uint8_t>(value << 2));
        else if (value <= 0x3fff)
            appendU16(bytes, static_cast<std::uint16_t>((value << 2) | 1));
        else if (value <= 0x3fffffff)
            appendU32(bytes, (value << 2) | 2);
        else
            throw std::logic_error("synthetic packed count does not fit U6to30");
        appendDelimiter(bytes);
    }

    std::vector<std::uint8_t> makeSkyPayload()
    {
        std::vector<std::uint8_t> result;
        for (const std::uint32_t weather : { 3u, 0u, 4u, 0u })
            appendDelimitedReferenceId(result, weather);
        for (const float value : { 14.25f, 14.f, 1.f })
        {
            appendF32(result, value);
            appendDelimiter(result);
        }
        appendU32(result, 0x20u);
        appendDelimiter(result);
        appendF32(result, 0.f);
        appendDelimiter(result);
        for (const float value : { 0.f, 0.f, 0.f })
        {
            appendF32(result, value);
            appendDelimiter(result);
        }
        appendU32(result, 0xa00f0800u);
        appendDelimiter(result);
        appendF32(result, 0.5f);
        appendDelimiter(result);
        appendU32(result, 3u);
        appendDelimiter(result);
        return result;
    }

    std::vector<std::uint8_t> makePlayerActorValueData()
    {
        std::vector<std::uint8_t> result;
        result.reserve(ESM4::sFONVPlayerActorValueDataBytes);
        for (std::size_t arrayIndex = 0; arrayIndex < 3; ++arrayIndex)
        {
            for (std::size_t valueIndex = 0; valueIndex < ESM4::sFONVPlayerActorValueCount; ++valueIndex)
            {
                float value = 0.f;
                if (arrayIndex == 0 && (valueIndex == 38 || valueIndex == 43))
                    value = 2.f;
                else if (arrayIndex == 1 && valueIndex == 24)
                    value = 10.f;
                appendF32(result, value);
                appendDelimiter(result);
            }
        }
        appendU32(result, 0);
        appendDelimiter(result);
        if (result.size() != ESM4::sFONVPlayerActorValueDataBytes)
            throw std::logic_error("synthetic player actor-value data has the wrong size");
        return result;
    }

    constexpr std::size_t sSyntheticPlayerProcessInventoryDataBytes = 97;

    std::vector<std::uint8_t> makePlayerProcessInventoryData(std::optional<float> referenceScale = std::nullopt,
        std::optional<std::uint32_t> actorExtraDataCount = std::nullopt,
        std::span<const std::uint8_t> actorExtraData = {})
    {
        std::vector<std::uint8_t> result;
        appendU8(result, 0);
        appendDelimiter(result);

        if (referenceScale.has_value())
        {
            appendF32(result, *referenceScale);
            appendDelimiter(result);
        }

        if (actorExtraDataCount.has_value())
        {
            appendPackedCount(result, *actorExtraDataCount);
            result.insert(result.end(), actorExtraData.begin(), actorExtraData.end());
        }
        else
        {
            appendPackedCount(result, 2);
            appendU8(result, ESM4::sFONVExtraFactionChangesType);
            appendDelimiter(result);
            appendPackedCount(result, 1);
            appendDelimitedReferenceId(result, 0x400111);
            appendU8(result, 1);
            appendDelimiter(result);
            appendU8(result, ESM4::sFONVExtraEncounterZoneType);
            appendDelimiter(result);
            appendDelimitedReferenceId(result, 0x400222);
        }

        appendPackedCount(result, 3);
        appendDelimitedReferenceId(result, 0x400333);
        appendU32(result, 5);
        appendDelimiter(result);
        appendPackedCount(result, 1);
        appendPackedCount(result, 3);
        appendU8(result, ESM4::sFONVExtraHealthType);
        appendDelimiter(result);
        appendF32(result, 75.f);
        appendDelimiter(result);
        appendU8(result, ESM4::sFONVExtraHotkeyType);
        appendDelimiter(result);
        appendU8(result, 6);
        appendDelimiter(result);
        appendU8(result, ESM4::sFONVExtraAmmoType);
        appendDelimiter(result);
        appendDelimitedReferenceId(result, 0x400444);
        appendU32(result, 9);
        appendDelimiter(result);

        appendDelimitedReferenceId(result, 0x400334);
        appendU32(result, 0);
        appendDelimiter(result);
        appendPackedCount(result, 1);
        appendPackedCount(result, 1);
        appendU8(result, ESM4::sFONVExtraWornType);
        appendDelimiter(result);

        appendDelimitedReferenceId(result, 0x400335);
        appendU32(result, 3);
        appendDelimiter(result);
        appendPackedCount(result, 1);
        appendPackedCount(result, 2);
        appendU8(result, ESM4::sFONVExtraCountType);
        appendDelimiter(result);
        appendU16(result, 2);
        appendDelimiter(result);
        appendU8(result, ESM4::sFONVExtraHealthType);
        appendDelimiter(result);
        appendF32(result, 25.f);
        appendDelimiter(result);

        if (!referenceScale.has_value() && !actorExtraDataCount.has_value()
            && result.size() != sSyntheticPlayerProcessInventoryDataBytes)
            throw std::logic_error("synthetic player process/inventory data has the wrong size");
        return result;
    }

    std::vector<std::uint8_t> makeSave213PlayerActorExtraData()
    {
        std::vector<std::uint8_t> result;

        appendU8(result, ESM4::sFONVExtraPackageStartLocationType);
        appendDelimiter(result);
        appendDelimitedReferenceId(result, 0x400111);
        appendF32(result, -16259.322f);
        appendF32(result, -5998.828f);
        appendF32(result, 6393.356f);
        appendDelimiter(result);
        appendU32(result, 0x12345678u);
        appendDelimiter(result);

        appendU8(result, ESM4::sFONVExtraFollowerArrayType);
        appendDelimiter(result);
        appendPackedCount(result, 2);
        appendDelimitedReferenceId(result, 0x400112);
        appendDelimitedReferenceId(result, 0x800113);

        appendU8(result, ESM4::sFONVExtraFactionChangesType);
        appendDelimiter(result);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x400114);
        appendU8(result, std::bit_cast<std::uint8_t>(std::int8_t{ -2 }));
        appendDelimiter(result);

        appendU8(result, ESM4::sFONVExtraActorCauseType);
        appendDelimiter(result);
        appendU32(result, 0xaabbccddu);
        appendDelimiter(result);

        appendU8(result, ESM4::sFONVExtraEncounterZoneType);
        appendDelimiter(result);
        appendDelimitedReferenceId(result, 0x400115);

        appendU8(result, ESM4::sFONVExtraSayToTopicInfoType);
        appendDelimiter(result);
        appendDelimitedReferenceId(result, 0x400116);
        appendDelimitedReferenceId(result, 0x800117);
        appendU8(result, 0x42);
        appendDelimiter(result);

        if (result.size() != 71)
            throw std::logic_error("synthetic Save 213 player actor extra-data has the wrong size");
        return result;
    }

    void appendProcessVector3(std::vector<std::uint8_t>& bytes, float x, float y, float z)
    {
        appendF32(bytes, x);
        appendF32(bytes, y);
        appendF32(bytes, z);
        appendDelimiter(bytes);
    }

    constexpr std::size_t sSyntheticPlayerMobileObjectProcessStateBytes = 688;

    std::vector<std::uint8_t> makePlayerMobileObjectProcessState()
    {
        std::vector<std::uint8_t> result;
        const auto appendDelimitedU8 = [&](std::uint8_t value) {
            appendU8(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU16 = [&](std::uint16_t value) {
            appendU16(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU32 = [&](std::uint32_t value) {
            appendU32(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedF32 = [&](float value) {
            appendF32(result, value);
            appendDelimiter(result);
        };
        const auto appendNullReferences = [&](std::size_t count) {
            for (std::size_t i = 0; i < count; ++i)
                appendDelimitedReferenceId(result, 0);
        };

        for (const std::uint8_t value : { 0xff, 0, 1, 1, 1, 0, 0, 1 })
            appendDelimitedU8(value);
        appendDelimitedU32(0x3f9a4658u);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendNullReferences(2);

        appendDelimitedU32(0);
        appendDelimitedU32(0xbf800000u);
        appendDelimitedU32(0);
        appendDelimitedReferenceId(result, 0);

        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedReferenceId(result, 0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU16(0);
        appendDelimitedF32(-1.f);
        appendDelimitedF32(0.f);
        appendNullReferences(5);
        appendPackedCount(result, 0);

        appendDelimitedU32(0xffffffffu);

        for (const std::uint8_t value : { 1, 1, 0 })
            appendDelimitedU8(value);
        appendDelimitedU32(0x3f800000u);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendProcessVector3(result, 0.f, 0.f, 0.f);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU8(0x7f);
        appendDelimitedU8(0);
        appendDelimitedU16(0);
        appendProcessVector3(result, 0.f, 0.f, 0.f);
        for (std::size_t i = 0; i < 4; ++i)
            appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU8(1);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU16(2);
        appendDelimitedU32(0xffffffffu);
        appendDelimitedU8(0);
        appendDelimitedU32(0xffffffffu);
        appendDelimitedU32(0xbf800000u);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0xffffffffu);
        appendDelimitedU8(0);
        appendNullReferences(4);
        appendDelimitedU32(0);
        appendPackedCount(result, 0);
        appendDelimitedReferenceId(result, 0);
        appendPackedCount(result, 0);
        appendPackedCount(result, 0);
        appendNullReferences(3);
        appendPackedCount(result, 0);

        for (const std::uint8_t value : { 0, 1, 0, 0 })
            appendDelimitedU8(value);
        appendDelimitedU16(0xffffu);
        for (std::size_t i = 0; i < 11; ++i)
            appendDelimitedU32(i == 4 ? 0x3f4353f8u : 0);
        appendDelimitedU16(13);
        appendDelimitedU16(0xffffu);
        appendDelimitedU16(0);
        appendDelimitedU8(0);
        appendProcessVector3(result, 11.7265625f, 39.951904296875f, 83.052734375f);
        for (const std::uint32_t value : { 0u, 0u, 0x3f800000u, 0x41200000u, 0u, 0u })
            appendDelimitedU32(value);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0x3f39999bu);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0xbf800000u);
        appendDelimitedU8(0);
        for (const std::uint32_t value : { 0x3f800000u, 0x4159475eu, 0x41200000u, 0x40400000u, 0u })
            appendDelimitedU32(value);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(2);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0x42f00000u);
        for (std::size_t i = 0; i < 4; ++i)
            appendDelimitedU8(0);
        appendNullReferences(7);
        for (std::size_t i = 0; i < 6; ++i)
        {
            appendDelimitedReferenceId(result, i == 0 ? 0x400014u : 0u);
            appendDelimitedU8(0);
        }
        appendPackedCount(result, 0);
        appendPackedCount(result, 0);
        appendPackedCount(result, 0);
        appendDelimitedU8(0);
        appendPackedCount(result, 0);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x400333u);
        appendDelimitedU8(1);
        appendDelimitedU32(0xfffffffeu);
        appendProcessVector3(result, 1.f, 2.f, 3.f);
        appendDelimitedF32(-4.f);
        appendDelimitedU8(5);
        appendDelimitedU8(6);
        appendDelimitedU8(7);
        appendDelimitedU32(8);
        appendDelimitedU8(9);
        appendPackedCount(result, 0);
        appendDelimitedU8(0);
        appendPackedCount(result, 3);
        result.insert(result.end(), { 0xaa, 0xbb, 0xcc });

        if (result.size() != sSyntheticPlayerMobileObjectProcessStateBytes)
            throw std::logic_error("synthetic player MobileObject process state has the wrong size");
        return result;
    }

    std::vector<std::uint8_t> makePlayerChangedCharacterState()
    {
        std::vector<std::uint8_t> result;
        const auto appendDelimitedU8 = [&](std::uint8_t value) {
            appendU8(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU16 = [&](std::uint16_t value) {
            appendU16(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU32 = [&](std::uint32_t value) {
            appendU32(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedF32 = [&](float value) {
            appendF32(result, value);
            appendDelimiter(result);
        };

        appendDelimitedF32(0.25f);
        for (const std::uint8_t value : { 0, 0, 1, 0 })
            appendDelimitedU8(value);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0xffu);
        for (const std::uint8_t value : { 0, 1, 0, 0, 0, 0 })
            appendDelimitedU8(value);
        for (const std::uint32_t value : { 0u, 0x3f800000u, 0u })
            appendDelimitedU32(value);
        for (const std::uint8_t value : { 0, 1, 0 })
            appendDelimitedU8(value);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU8(1);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedU32(0);
        appendDelimitedReferenceId(result, 0);
        appendDelimitedReferenceId(result, 0x400007u);
        appendDelimitedReferenceId(result, 1);

        appendDelimitedU16(0xffffu);
        appendDelimitedU16(0xffffu);
        appendDelimitedU8(0);
        appendDelimitedU32(257);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendProcessVector3(result, 0.f, 0.f, 0.f);
        appendProcessVector3(result, 0.f, 0.f, 0.f);
        appendDelimitedU32(0);
        for (std::size_t i = 0; i < 5; ++i)
            appendDelimitedU8(0);
        appendDelimitedU32(4);
        appendDelimitedU32(0xe3b9u);
        appendDelimitedU32(0);

        appendProcessVector3(result, 10.f, 20.f, 30.f);
        appendDelimitedReferenceId(result, 0);
        appendDelimitedReferenceId(result, 1);
        appendDelimitedReferenceId(result, 2);
        appendDelimitedU32(0xdeaddeadu);
        appendDelimitedU16(0xffffu);
        appendDelimitedU8(2);
        appendDelimitedU8(0);
        appendDelimitedReferenceId(result, 0);
        appendDelimitedU8(0x08);

        for (std::size_t i = 0; i < 5; ++i)
            appendProcessVector3(result, static_cast<float>(i), static_cast<float>(i + 1), static_cast<float>(i + 2));
        for (std::uint32_t i = 0; i < 20; ++i)
            appendDelimitedU32(i);
        appendDelimitedU32(10);
        appendDelimitedU32(20);
        for (std::uint32_t i = 0; i < 4; ++i)
            appendDelimitedU32(30 + i);
        for (const std::uint8_t value : { 1, 1, 0, 0, 0, 0, 0 })
            appendDelimitedU8(value);
        appendDelimitedU8(0);
        appendDelimitedU8(0);
        appendDelimitedU32(0);
        appendProcessVector3(result, -1.f, -2.f, -3.f);
        appendDelimitedU32(0);
        appendDelimitedReferenceId(result, 0x400014u);
        appendPackedCount(result, 0);

        appendProcessVector3(result, 308.f, 0.f, 0.f);
        appendDelimitedU32(576);
        appendDelimitedU32(32);
        appendDelimitedU32(0xbc031273u);
        appendDelimitedU8(0);
        appendDelimitedU8(0);

        if (result.size() != ESM4::sFONVPlayerChangedCharacterStateBytes)
            throw std::logic_error("synthetic player ChangedCharacter state has the wrong size");
        return result;
    }

    constexpr std::size_t sSyntheticPlayerCharacterAnimationBodyBytes = 145;
    constexpr std::size_t sSyntheticPlayerCharacterAnimationStateBytes
        = sSyntheticPlayerCharacterAnimationBodyBytes + 3;

    std::vector<std::uint8_t> makePlayerCharacterAnimationState()
    {
        std::vector<std::uint8_t> result;
        appendPackedCount(result, sSyntheticPlayerCharacterAnimationBodyBytes);
        for (std::size_t i = 0; i < sSyntheticPlayerCharacterAnimationBodyBytes; ++i)
            appendU8(result, static_cast<std::uint8_t>(i));
        if (result.size() != sSyntheticPlayerCharacterAnimationStateBytes)
            throw std::logic_error("synthetic player animation buffer has the wrong size");
        return result;
    }

    std::vector<std::uint8_t> makePlayerCharacterScalarReferenceState()
    {
        std::vector<std::uint8_t> result;
        const auto appendDelimitedU8 = [&](std::uint8_t value) {
            appendU8(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU32 = [&](std::uint32_t value) {
            appendU32(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedF32 = [&](float value) {
            appendF32(result, value);
            appendDelimiter(result);
        };

        for (const std::uint8_t value : { 1, 2, 3, 4 })
            appendDelimitedU8(value);
        for (const std::uint32_t value : { 0x654u, 0x660u, 0x664u, 0x668u })
            appendDelimitedU32(value);
        appendDelimitedU8(5);
        appendDelimitedU8(6);
        appendDelimitedU32(0x6d0u);
        appendDelimitedU32(0x6d4u);
        appendDelimitedU8(7);
        appendDelimitedU32(0x6dcu);
        for (const std::uint8_t value : { 8, 9, 10, 11 })
            appendDelimitedU8(value);
        appendDelimitedU32(0x6e4u);
        appendProcessVector3(result, 1.f, 2.f, 3.f);
        for (const std::uint32_t value : { 0x698u, 0x67cu, 0x738u })
            appendDelimitedU32(value);
        appendDelimitedU8(12);
        appendDelimitedU32(0x65cu);
        appendDelimitedF32(55.f);
        appendDelimitedF32(75.f);
        appendDelimitedU8(13);
        appendDelimitedU32(0x730u);
        appendDelimitedU32(0x790u);
        appendDelimitedU8(14);
        appendDelimitedU8(15);
        for (const std::uint32_t value : { 0x63cu, 0x640u, 0x644u, 0x200u })
            appendDelimitedU32(value);
        for (const std::uint8_t value : { 16, 17, 18 })
            appendDelimitedU8(value);
        appendDelimitedU32(0x794u);
        appendDelimitedF32(120.f);
        for (const std::uint32_t value : { 0xd6cu, 0xd70u, 0x228u, 0x22cu, 0x230u, 0x234u })
            appendDelimitedU32(value);
        for (const std::uint8_t value : { 19, 20, 13, 14, 16, 18 })
            appendDelimitedU8(value);
        appendDelimitedU32(0x1fcu);
        appendDelimitedU32(0x684u);
        for (std::uint32_t i = 0; i < 5; ++i)
            appendDelimitedU32(0x744u + i);
        appendDelimitedU8(21);
        appendDelimitedU32(0x878u);
        for (const std::uint32_t reference :
            { 1u, 0u, 2u, 0x400111u, 0x800222u, 0u, 1u, 0u, 2u, 0u, 0x400333u })
        {
            appendDelimitedReferenceId(result, reference);
        }

        if (result.size() != ESM4::sFONVPlayerCharacterScalarReferenceStateBytes)
            throw std::logic_error("synthetic PlayerCharacter scalar/reference state has the wrong size");
        return result;
    }

    std::vector<std::uint8_t> makePlayerCharacterListsState()
    {
        std::vector<std::uint8_t> result;
        const auto appendDelimitedU8 = [&](std::uint8_t value) {
            appendU8(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU16 = [&](std::uint16_t value) {
            appendU16(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU32 = [&](std::uint32_t value) {
            appendU32(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedF32 = [&](float value) {
            appendF32(result, value);
            appendDelimiter(result);
        };

        appendPackedCount(result, 2);
        appendDelimitedReferenceId(result, 0x400111u);
        appendDelimitedReferenceId(result, 0x400112u);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x400113u);

        appendPackedCount(result, 2);
        appendDelimitedReferenceId(result, 0x400114u);
        appendDelimitedU32(3u);
        appendPackedCount(result, 0);
        appendDelimitedReferenceId(result, 0x800115u);
        appendDelimitedU32(std::bit_cast<std::uint32_t>(std::int32_t{ -2 }));
        appendPackedCount(result, 0);

        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x400116u);
        appendDelimitedU8(0x44u);
        appendDelimitedU8(0x45u);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x400117u);
        appendDelimitedU8(2u);
        appendPackedCount(result, 1);
        appendDelimitedU32(0x60cu);
        appendDelimitedF32(12.5f);
        appendDelimitedReferenceId(result, 0x400118u);
        appendPackedCount(result, 1);
        appendDelimitedU32(0x610u);
        appendDelimitedU32(0x611u);
        appendDelimitedU16(0x612u);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x400119u);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x40011au);
        for (const std::uint32_t value : { 0x61cu, 0x620u, 0x624u, 0x628u, 0x62cu })
            appendDelimitedU32(value);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x40011bu);
        appendDelimitedU8(10u);
        appendDelimitedU8(1u);
        appendPackedCount(result, 1);
        appendDelimitedReferenceId(result, 0x40011cu);
        appendDelimitedU32(10u);

        if (result.size() != ESM4::sFONVPlayerCharacterListsStateBytes)
            throw std::logic_error("synthetic PlayerCharacter lists state has the wrong size");
        return result;
    }

    std::vector<std::uint8_t> makePlayerCharacterMagicTargetState()
    {
        std::vector<std::uint8_t> result;
        appendPackedCount(result, 4);
        constexpr std::array<std::uint32_t, 4> magicForms = { 0x400201u, 0x800202u, 1u, 2u };
        for (std::size_t entryIndex = 0; entryIndex < magicForms.size(); ++entryIndex)
        {
            appendDelimitedReferenceId(result, magicForms[entryIndex]);
            appendU8(result, static_cast<std::uint8_t>(entryIndex & 1u));
            appendDelimiter(result);
            appendPackedCount(result, entryIndex == 0 ? 33u : static_cast<std::uint32_t>(entryIndex));
            appendPackedCount(result, 48);
            for (std::uint32_t effectIndex = 0; effectIndex < 48; ++effectIndex)
                appendU8(result, static_cast<std::uint8_t>(entryIndex * 48 + effectIndex));
        }

        if (result.size() != ESM4::sFONVPlayerCharacterMagicTargetStateBytes)
            throw std::logic_error("synthetic PlayerCharacter magic-target state has the wrong size");
        return result;
    }

    std::vector<std::uint8_t> makePlayerCharacterFinalState()
    {
        std::vector<std::uint8_t> result;
        const auto appendDelimitedU8 = [&](std::uint8_t value) {
            appendU8(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedU32 = [&](std::uint32_t value) {
            appendU32(result, value);
            appendDelimiter(result);
        };
        const auto appendDelimitedF32 = [&](float value) {
            appendF32(result, value);
            appendDelimiter(result);
        };

        appendDelimitedU32(0xd64u);
        appendDelimitedF32(1.25f);
        appendDelimitedF32(-2.5f);
        appendPackedCount(result, 0);
        appendDelimitedU8(1u);
        appendDelimitedU8(0u);
        appendDelimitedU8(1u);
        for (const std::uint32_t formId :
            { 0u, 0x00000123u, 0xff000456u, 0x01020304u, 0u, 0u, 0u, 0u })
        {
            appendDelimitedU32(formId);
        }
        for (std::size_t i = 0; i < 4; ++i)
            appendPackedCount(result, 0);

        if (result.size() != ESM4::sFONVPlayerCharacterFinalStateBytes)
            throw std::logic_error("synthetic PlayerCharacter final state has the wrong size");
        return result;
    }

    void appendString(std::vector<std::uint8_t>& bytes, std::string_view value)
    {
        if (value.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::logic_error("synthetic string does not fit in the format");
        appendU16(bytes, static_cast<std::uint16_t>(value.size()));
        appendDelimiter(bytes);
        bytes.insert(bytes.end(), value.begin(), value.end());
        if (!value.empty())
            appendDelimiter(bytes);
    }

    void appendGlobalDataEntry(
        std::vector<std::uint8_t>& bytes, std::uint32_t type, std::span<const std::uint8_t> payload)
    {
        appendU32(bytes, type);
        appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }

    std::vector<std::uint8_t> makeGlobalVariablesPayload()
    {
        std::vector<std::uint8_t> result;
        appendPackedCount(result, 2);
        appendDelimitedReferenceId(result, 0x400035u);
        appendF32(result, 42.5f);
        appendDelimiter(result);
        appendDelimitedReferenceId(result, 0x400036u);
        appendF32(result, -7.25f);
        appendDelimiter(result);
        return result;
    }

    struct ChangedFormOffsets
    {
        std::size_t mReferenceId = 0;
        std::size_t mChangeFlags = 0;
        std::size_t mRawType = 0;
        std::size_t mDataLength = 0;
        std::size_t mPayload = 0;
    };

    ChangedFormOffsets appendChangedForm(std::vector<std::uint8_t>& bytes,
        const std::array<std::uint8_t, 3>& referenceId, std::uint32_t changeFlags, std::uint8_t changeType,
        std::uint8_t version, std::uint8_t lengthWidth, std::span<const std::uint8_t> payload)
    {
        ChangedFormOffsets result;
        result.mReferenceId = bytes.size();
        bytes.insert(bytes.end(), referenceId.begin(), referenceId.end());
        result.mChangeFlags = bytes.size();
        appendU32(bytes, changeFlags);
        result.mRawType = bytes.size();
        std::uint8_t lengthCode = 0;
        if (lengthWidth == 2)
            lengthCode = 1;
        else if (lengthWidth == 4)
            lengthCode = 2;
        else if (lengthWidth != 1)
            throw std::logic_error("invalid synthetic changed-form length width");
        appendU8(bytes, static_cast<std::uint8_t>((lengthCode << 6) | (changeType & 0x3f)));
        appendU8(bytes, version);
        result.mDataLength = bytes.size();
        if (lengthWidth == 1)
            appendU8(bytes, static_cast<std::uint8_t>(payload.size()));
        else if (lengthWidth == 2)
            appendU16(bytes, static_cast<std::uint16_t>(payload.size()));
        else
            appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
        result.mPayload = bytes.size();
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return result;
    }

    struct SaveBytes
    {
        std::vector<std::uint8_t> mBytes;
        std::size_t mHeaderBegin = 0;
        std::size_t mHeaderEnd = 0;
        std::size_t mScreenshotBegin = 0;
        std::size_t mScreenshotEnd = 0;
        std::size_t mMasterSizeOffset = 0;
        std::size_t mMasterTableBegin = 0;
        std::size_t mMasterCountDelimiter = 0;
        std::size_t mMasterTableEnd = 0;
        std::size_t mFileLocationBegin = 0;
        std::size_t mFileLocationEnd = 0;
        std::size_t mGlobalData1OffsetField = 0;
        std::size_t mChangedFormsOffsetField = 0;
        std::size_t mGlobalData2OffsetField = 0;
        std::size_t mRefIdArrayOffsetField = 0;
        std::size_t mUnknownTableOffsetField = 0;
        std::size_t mGlobalData1CountField = 0;
        std::size_t mGlobalData2CountField = 0;
        std::size_t mChangedFormsCountField = 0;
        std::size_t mGlobalData1Begin = 0;
        std::size_t mGlobalData1LengthField = 0;
        std::size_t mChangedFormsBegin = 0;
        std::array<std::size_t, 3> mChangedFormRawTypes{};
        std::array<std::size_t, 3> mChangedFormReferenceIds{};
        std::array<std::size_t, 3> mChangedFormChangeFlags{};
        std::array<std::size_t, 3> mChangedFormLengthFields{};
        std::array<std::size_t, 3> mChangedFormPayloads{};
        std::size_t mPlayerProcessInventoryBegin = 0;
        std::size_t mPlayerMobileObjectProcessStateBegin = 0;
        std::size_t mPlayerChangedCharacterStateBegin = 0;
        std::size_t mPlayerCharacterAnimationStateBegin = 0;
        std::size_t mPlayerCharacterScalarReferenceStateBegin = 0;
        std::size_t mPlayerCharacterListsStateBegin = 0;
        std::size_t mPlayerCharacterMagicTargetStateBegin = 0;
        std::size_t mPlayerCharacterFinalStateBegin = 0;
        std::size_t mGlobalData2Begin = 0;
        std::size_t mRefIdArrayBegin = 0;
        std::size_t mUnknownTableBegin = 0;
    };

    SaveBytes makeSave(bool hasLanguage = true, std::uint32_t width = 2, std::uint32_t height = 1,
        std::span<const std::string_view> masters = {}, bool includeScreenshot = true,
        std::string_view playerName = "Courier", bool includeSky = false,
        std::size_t playerActorValueBytes = ESM4::sFONVPlayerActorValueDataBytes,
        std::size_t playerProcessInventoryBytes = sSyntheticPlayerProcessInventoryDataBytes,
        std::size_t playerMobileObjectProcessStateBytes = sSyntheticPlayerMobileObjectProcessStateBytes,
        std::size_t playerChangedCharacterStateBytes = ESM4::sFONVPlayerChangedCharacterStateBytes,
        std::size_t playerCharacterAnimationStateBytes = sSyntheticPlayerCharacterAnimationStateBytes,
        std::size_t playerCharacterScalarReferenceStateBytes
            = ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
        std::size_t playerCharacterListsStateBytes = ESM4::sFONVPlayerCharacterListsStateBytes,
        std::size_t playerCharacterMagicTargetStateBytes
            = ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
        std::size_t playerCharacterFinalStateBytes = ESM4::sFONVPlayerCharacterFinalStateBytes,
        std::optional<float> playerReferenceScale = std::nullopt,
        std::optional<std::uint32_t> playerActorExtraDataCount = std::nullopt,
        std::span<const std::uint8_t> playerActorExtraData = {}, bool includeGlobalVariables = false)
    {
        std::vector<std::uint8_t> header;
        appendU32(header, 48);
        appendDelimiter(header);
        if (hasLanguage)
        {
            const std::string_view language = "ENGLISH";
            header.insert(header.end(), language.begin(), language.end());
            header.resize(header.size() + 64 - language.size(), 0);
            appendDelimiter(header);
        }
        appendU32(header, width);
        appendDelimiter(header);
        appendU32(header, height);
        appendDelimiter(header);
        appendU32(header, 330);
        appendDelimiter(header);
        appendString(header, playerName);
        appendString(header, "Desert Scavenger");
        appendU32(header, 9);
        appendDelimiter(header);
        appendString(header, "Goodsprings");
        appendString(header, "000.16.45");

        std::vector<std::uint8_t> masterTable;
        if (masters.size() > std::numeric_limits<std::uint8_t>::max())
            throw std::logic_error("synthetic master count does not fit in the format");
        appendU8(masterTable, static_cast<std::uint8_t>(masters.size()));
        appendDelimiter(masterTable);
        for (const std::string_view master : masters)
            appendString(masterTable, master);

        SaveBytes result;
        constexpr std::string_view magic = "FO3SAVEGAME";
        result.mBytes.insert(result.mBytes.end(), magic.begin(), magic.end());
        appendU32(result.mBytes, static_cast<std::uint32_t>(header.size()));
        result.mHeaderBegin = result.mBytes.size();
        result.mBytes.insert(result.mBytes.end(), header.begin(), header.end());
        result.mHeaderEnd = result.mBytes.size();
        result.mScreenshotBegin = result.mBytes.size();
        if (includeScreenshot)
        {
            const auto screenshotBytes = static_cast<std::uint64_t>(width) * height * 3;
            if (screenshotBytes > 1024)
                throw std::logic_error("synthetic screenshot is too large for this test helper");
            for (std::uint64_t i = 0; i < screenshotBytes; ++i)
                result.mBytes.push_back(static_cast<std::uint8_t>(i));
        }
        result.mScreenshotEnd = result.mBytes.size();
        appendU8(result.mBytes, 27);
        result.mMasterSizeOffset = result.mBytes.size();
        appendU32(result.mBytes, static_cast<std::uint32_t>(masterTable.size()));
        result.mMasterTableBegin = result.mBytes.size();
        result.mMasterCountDelimiter = result.mMasterTableBegin + 1;
        result.mBytes.insert(result.mBytes.end(), masterTable.begin(), masterTable.end());
        result.mMasterTableEnd = result.mBytes.size();

        std::vector<std::uint8_t> globalData1;
        if (includeGlobalVariables)
        {
            const std::vector<std::uint8_t> globals = makeGlobalVariablesPayload();
            appendGlobalDataEntry(globalData1, 3u, globals);
        }
        const std::vector<std::uint8_t> globalData1Payload
            = includeSky ? makeSkyPayload() : std::vector<std::uint8_t>{ 0xaa, 0xbb, 0xcc };
        appendGlobalDataEntry(globalData1, includeSky ? 8u : 0u, globalData1Payload);

        std::vector<std::uint8_t> changedForms;
        std::vector<std::uint8_t> changedPayload1 = { 0, 0, 2 };
        appendF32(changedPayload1, -72392.84375f);
        appendF32(changedPayload1, -1240.19275f);
        appendF32(changedPayload1, 8137.58643f);
        appendF32(changedPayload1, -0.06439045f);
        appendF32(changedPayload1, -0.0f);
        appendF32(changedPayload1, 2.93332028f);
        appendDelimiter(changedPayload1);
        const std::vector<std::uint8_t> playerActorValues = makePlayerActorValueData();
        if (playerActorValueBytes > playerActorValues.size())
            throw std::logic_error("synthetic player actor-value byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerActorValues.begin(),
            playerActorValues.begin() + static_cast<std::ptrdiff_t>(playerActorValueBytes));
        const std::vector<std::uint8_t> playerProcessInventory
            = makePlayerProcessInventoryData(playerReferenceScale, playerActorExtraDataCount, playerActorExtraData);
        const std::size_t actualPlayerProcessInventoryBytes
            = playerReferenceScale.has_value() || playerActorExtraDataCount.has_value()
            ? playerProcessInventory.size()
            : playerProcessInventoryBytes;
        if (actualPlayerProcessInventoryBytes > playerProcessInventory.size())
            throw std::logic_error("synthetic player process/inventory byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerProcessInventory.begin(),
            playerProcessInventory.begin() + static_cast<std::ptrdiff_t>(actualPlayerProcessInventoryBytes));
        const std::vector<std::uint8_t> playerMobileObjectProcessState = makePlayerMobileObjectProcessState();
        const std::size_t actualPlayerMobileObjectProcessStateBytes
            = actualPlayerProcessInventoryBytes == playerProcessInventory.size()
            ? playerMobileObjectProcessStateBytes
            : 0;
        if (actualPlayerMobileObjectProcessStateBytes > playerMobileObjectProcessState.size())
            throw std::logic_error("synthetic player MobileObject process-state byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerMobileObjectProcessState.begin(),
            playerMobileObjectProcessState.begin()
                + static_cast<std::ptrdiff_t>(actualPlayerMobileObjectProcessStateBytes));
        const std::vector<std::uint8_t> playerChangedCharacterState = makePlayerChangedCharacterState();
        const std::size_t actualPlayerChangedCharacterStateBytes
            = actualPlayerMobileObjectProcessStateBytes == sSyntheticPlayerMobileObjectProcessStateBytes
            ? playerChangedCharacterStateBytes
            : 0;
        if (actualPlayerChangedCharacterStateBytes > playerChangedCharacterState.size())
            throw std::logic_error("synthetic player ChangedCharacter byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerChangedCharacterState.begin(),
            playerChangedCharacterState.begin()
                + static_cast<std::ptrdiff_t>(actualPlayerChangedCharacterStateBytes));
        const std::vector<std::uint8_t> playerCharacterAnimationState = makePlayerCharacterAnimationState();
        const std::size_t actualPlayerCharacterAnimationStateBytes
            = actualPlayerChangedCharacterStateBytes == ESM4::sFONVPlayerChangedCharacterStateBytes
            ? playerCharacterAnimationStateBytes
            : 0;
        if (actualPlayerCharacterAnimationStateBytes > playerCharacterAnimationState.size())
            throw std::logic_error("synthetic player animation-buffer byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerCharacterAnimationState.begin(),
            playerCharacterAnimationState.begin()
                + static_cast<std::ptrdiff_t>(actualPlayerCharacterAnimationStateBytes));
        const std::vector<std::uint8_t> playerCharacterScalarReferenceState
            = makePlayerCharacterScalarReferenceState();
        const std::size_t actualPlayerCharacterScalarReferenceStateBytes
            = actualPlayerCharacterAnimationStateBytes == sSyntheticPlayerCharacterAnimationStateBytes
            ? playerCharacterScalarReferenceStateBytes
            : 0;
        if (actualPlayerCharacterScalarReferenceStateBytes > playerCharacterScalarReferenceState.size())
            throw std::logic_error("synthetic PlayerCharacter scalar/reference byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerCharacterScalarReferenceState.begin(),
            playerCharacterScalarReferenceState.begin()
                + static_cast<std::ptrdiff_t>(actualPlayerCharacterScalarReferenceStateBytes));
        const std::vector<std::uint8_t> playerCharacterListsState = makePlayerCharacterListsState();
        const std::size_t actualPlayerCharacterListsStateBytes
            = actualPlayerCharacterScalarReferenceStateBytes == ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
            ? playerCharacterListsStateBytes
            : 0;
        if (actualPlayerCharacterListsStateBytes > playerCharacterListsState.size())
            throw std::logic_error("synthetic PlayerCharacter lists-state byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerCharacterListsState.begin(),
            playerCharacterListsState.begin()
                + static_cast<std::ptrdiff_t>(actualPlayerCharacterListsStateBytes));
        const std::vector<std::uint8_t> playerCharacterMagicTargetState = makePlayerCharacterMagicTargetState();
        const std::size_t actualPlayerCharacterMagicTargetStateBytes
            = actualPlayerCharacterListsStateBytes == ESM4::sFONVPlayerCharacterListsStateBytes
            ? playerCharacterMagicTargetStateBytes
            : 0;
        if (actualPlayerCharacterMagicTargetStateBytes > playerCharacterMagicTargetState.size())
            throw std::logic_error("synthetic PlayerCharacter magic-target byte count is too large");
        changedPayload1.insert(changedPayload1.end(), playerCharacterMagicTargetState.begin(),
            playerCharacterMagicTargetState.begin()
                + static_cast<std::ptrdiff_t>(actualPlayerCharacterMagicTargetStateBytes));
        const std::vector<std::uint8_t> playerCharacterFinalState = makePlayerCharacterFinalState();
        const std::size_t actualPlayerCharacterFinalStateBytes
            = actualPlayerCharacterMagicTargetStateBytes == ESM4::sFONVPlayerCharacterMagicTargetStateBytes
            ? playerCharacterFinalStateBytes
            : 0;
        if (actualPlayerCharacterFinalStateBytes > playerCharacterFinalState.size() + 1)
            throw std::logic_error("synthetic PlayerCharacter final-state byte count is too large");
        const std::size_t serializedPlayerCharacterFinalStateBytes
            = std::min(actualPlayerCharacterFinalStateBytes, playerCharacterFinalState.size());
        changedPayload1.insert(changedPayload1.end(), playerCharacterFinalState.begin(),
            playerCharacterFinalState.begin()
                + static_cast<std::ptrdiff_t>(serializedPlayerCharacterFinalStateBytes));
        if (actualPlayerCharacterFinalStateBytes > playerCharacterFinalState.size())
            appendU8(changedPayload1, 0xaa);
        constexpr std::array<std::uint8_t, 3> changedPayload2 = { 0x20, 0x21, 0x22 };
        constexpr std::array<std::uint8_t, 4> changedPayload3 = { 0x30, 0x31, 0x32, 0x33 };
        const std::uint32_t playerChangeFlags
            = 0xb0000022u | (playerReferenceScale.has_value() ? 0x00000010u : 0u);
        const ChangedFormOffsets changed1 = appendChangedForm(
            changedForms, { 0, 0, 1 }, playerChangeFlags, 1, 27, 2, changedPayload1);
        const ChangedFormOffsets changed2 = appendChangedForm(
            changedForms, { 0x40, 0x12, 0x34 }, 0x90abcdef, 2, 26, 2, changedPayload2);
        const ChangedFormOffsets changed3 = appendChangedForm(
            changedForms, { 0x80, 0x56, 0x78 }, 0x01020304, 3, 25, 4, changedPayload3);

        std::vector<std::uint8_t> globalData2;
        appendGlobalDataEntry(globalData2, 1000, std::span<const std::uint8_t>{});
        std::vector<std::uint8_t> refIdAndVisitedWorldspace;
        appendU32(refIdAndVisitedWorldspace, includeSky ? 4u : 2u);
        appendU32(refIdAndVisitedWorldspace, ESM4::sFONVPlayerReferenceFormId);
        appendU32(refIdAndVisitedWorldspace, 0x000da726);
        if (includeSky)
        {
            appendU32(refIdAndVisitedWorldspace, 0x001237d7);
            appendU32(refIdAndVisitedWorldspace, 0x000ffc88);
        }
        appendU32(refIdAndVisitedWorldspace, 1);
        appendU32(refIdAndVisitedWorldspace, 0x000da726);
        std::vector<std::uint8_t> unknownTableAndTail;
        appendU32(unknownTableAndTail, 5);
        unknownTableAndTail.insert(unknownTableAndTail.end(), { 0, 0, 0, 0, sDelimiter });

        result.mFileLocationBegin = result.mBytes.size();
        result.mFileLocationEnd = result.mFileLocationBegin + 0x6e;
        result.mGlobalData1Begin = result.mFileLocationEnd;
        result.mChangedFormsBegin = result.mGlobalData1Begin + globalData1.size();
        result.mGlobalData2Begin = result.mChangedFormsBegin + changedForms.size();
        result.mRefIdArrayBegin = result.mGlobalData2Begin + globalData2.size();
        result.mUnknownTableBegin = result.mRefIdArrayBegin + refIdAndVisitedWorldspace.size();

        result.mRefIdArrayOffsetField = result.mBytes.size();
        appendU32(result.mBytes, static_cast<std::uint32_t>(result.mRefIdArrayBegin));
        result.mUnknownTableOffsetField = result.mBytes.size();
        appendU32(result.mBytes, static_cast<std::uint32_t>(result.mUnknownTableBegin));
        result.mGlobalData1OffsetField = result.mBytes.size();
        appendU32(result.mBytes, static_cast<std::uint32_t>(result.mGlobalData1Begin));
        result.mChangedFormsOffsetField = result.mBytes.size();
        appendU32(result.mBytes, static_cast<std::uint32_t>(result.mChangedFormsBegin));
        result.mGlobalData2OffsetField = result.mBytes.size();
        appendU32(result.mBytes, static_cast<std::uint32_t>(result.mGlobalData2Begin));
        result.mGlobalData1CountField = result.mBytes.size();
        appendU32(result.mBytes, includeGlobalVariables ? 2u : 1u);
        result.mGlobalData2CountField = result.mBytes.size();
        appendU32(result.mBytes, 1);
        result.mChangedFormsCountField = result.mBytes.size();
        appendU32(result.mBytes, 3);
        appendU32(result.mBytes, 0);
        result.mBytes.resize(result.mFileLocationEnd, 0);

        result.mGlobalData1LengthField = result.mGlobalData1Begin + 4;
        result.mChangedFormRawTypes = { result.mChangedFormsBegin + changed1.mRawType,
            result.mChangedFormsBegin + changed2.mRawType, result.mChangedFormsBegin + changed3.mRawType };
        result.mChangedFormReferenceIds = { result.mChangedFormsBegin + changed1.mReferenceId,
            result.mChangedFormsBegin + changed2.mReferenceId,
            result.mChangedFormsBegin + changed3.mReferenceId };
        result.mChangedFormChangeFlags = { result.mChangedFormsBegin + changed1.mChangeFlags,
            result.mChangedFormsBegin + changed2.mChangeFlags,
            result.mChangedFormsBegin + changed3.mChangeFlags };
        result.mChangedFormLengthFields = { result.mChangedFormsBegin + changed1.mDataLength,
            result.mChangedFormsBegin + changed2.mDataLength, result.mChangedFormsBegin + changed3.mDataLength };
        result.mChangedFormPayloads = { result.mChangedFormsBegin + changed1.mPayload,
            result.mChangedFormsBegin + changed2.mPayload, result.mChangedFormsBegin + changed3.mPayload };
        result.mPlayerProcessInventoryBegin
            = result.mChangedFormPayloads[0] + 28 + playerActorValueBytes;
        result.mPlayerMobileObjectProcessStateBegin
            = result.mPlayerProcessInventoryBegin + actualPlayerProcessInventoryBytes;
        result.mPlayerChangedCharacterStateBegin
            = result.mPlayerMobileObjectProcessStateBegin + actualPlayerMobileObjectProcessStateBytes;
        result.mPlayerCharacterAnimationStateBegin
            = result.mPlayerChangedCharacterStateBegin + actualPlayerChangedCharacterStateBytes;
        result.mPlayerCharacterScalarReferenceStateBegin
            = result.mPlayerCharacterAnimationStateBegin + actualPlayerCharacterAnimationStateBytes;
        result.mPlayerCharacterListsStateBegin
            = result.mPlayerCharacterScalarReferenceStateBegin + actualPlayerCharacterScalarReferenceStateBytes;
        result.mPlayerCharacterMagicTargetStateBegin
            = result.mPlayerCharacterListsStateBegin + actualPlayerCharacterListsStateBytes;
        result.mPlayerCharacterFinalStateBegin
            = result.mPlayerCharacterMagicTargetStateBegin + actualPlayerCharacterMagicTargetStateBytes;
        result.mBytes.insert(result.mBytes.end(), globalData1.begin(), globalData1.end());
        result.mBytes.insert(result.mBytes.end(), changedForms.begin(), changedForms.end());
        result.mBytes.insert(result.mBytes.end(), globalData2.begin(), globalData2.end());
        result.mBytes.insert(
            result.mBytes.end(), refIdAndVisitedWorldspace.begin(), refIdAndVisitedWorldspace.end());
        result.mBytes.insert(result.mBytes.end(), unknownTableAndTail.begin(), unknownTableAndTail.end());
        return result;
    }

    SaveBytes makeSaveWithPlayerProcessInventory(std::optional<float> referenceScale,
        std::optional<std::uint32_t> actorExtraDataCount = std::nullopt,
        std::span<const std::uint8_t> actorExtraData = {})
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        return makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes, ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes, ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
            ESM4::sFONVPlayerCharacterFinalStateBytes, referenceScale, actorExtraDataCount, actorExtraData);
    }

    SaveBytes makeSaveWithQuestChange()
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes result = makeSave(true, 2, 1, masters);
        std::vector<std::uint8_t> payload;
        appendU32(payload, 0x00000400u);
        appendDelimiter(payload);
        appendU8(payload, 0x21);
        appendDelimiter(payload);
        appendF32(payload, 5.f);
        appendDelimiter(payload);

        appendPackedCount(payload, 1);
        appendU8(payload, 20);
        appendDelimiter(payload);
        appendU8(payload, 1);
        appendDelimiter(payload);
        appendPackedCount(payload, 1);
        appendU8(payload, 2);
        appendDelimiter(payload);
        appendU8(payload, 1);
        appendDelimiter(payload);
        appendU16(payload, 0x1234);
        appendU16(payload, 0x5678);
        appendDelimiter(payload);

        appendPackedCount(payload, 2);
        appendU32(payload, 7);
        appendDelimiter(payload);
        appendF64(payload, 42.25);
        appendDelimiter(payload);
        appendU32(payload, 0x80000008u);
        appendDelimiter(payload);
        appendDelimitedReferenceId(payload, 0x400456);
        appendU8(payload, 1);
        appendDelimiter(payload);
        payload.insert(payload.end(), { 0, 1, 2, 3, 4, 5, 6, 7 });
        appendDelimiter(payload);
        appendU8(payload, 1);
        appendDelimiter(payload);

        appendPackedCount(payload, 1);
        appendU32(payload, 10);
        appendDelimiter(payload);
        appendU32(payload, 3);
        appendDelimiter(payload);

        std::vector<std::uint8_t> envelope;
        constexpr std::uint32_t flags = 0xe0000007u;
        appendChangedForm(envelope, { 0x40, 0x01, 0x23 }, flags, 9, 27, 2, payload);
        result.mBytes.insert(result.mBytes.begin() + static_cast<std::ptrdiff_t>(result.mGlobalData2Begin),
            envelope.begin(), envelope.end());
        overwriteU32(result.mBytes, result.mGlobalData2OffsetField,
            static_cast<std::uint32_t>(result.mGlobalData2Begin + envelope.size()));
        overwriteU32(result.mBytes, result.mRefIdArrayOffsetField,
            static_cast<std::uint32_t>(result.mRefIdArrayBegin + envelope.size()));
        overwriteU32(result.mBytes, result.mUnknownTableOffsetField,
            static_cast<std::uint32_t>(result.mUnknownTableBegin + envelope.size()));
        overwriteU32(result.mBytes, result.mChangedFormsCountField, 4);
        return result;
    }

    TEST(FONVSaveGame, ParsesTypedQuestChangeFormState)
    {
        const SaveBytes source = makeSaveWithQuestChange();
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        ASSERT_EQ(save.mQuestChanges.size(), 1u);
        const ESM4::FONVSaveQuestChange& quest = save.mQuestChanges.front();
        EXPECT_EQ(quest.mResolvedFormId, 0x00000123u);
        EXPECT_EQ(quest.mChangeFlags, 0xe0000007u);
        ASSERT_TRUE(quest.mFormFlags);
        EXPECT_EQ(quest.mFormFlags->mValue, 0x400u);
        ASSERT_TRUE(quest.mQuestFlags);
        EXPECT_EQ(quest.mQuestFlags->mValue, 0x21);
        ASSERT_TRUE(quest.mScriptDelay);
        EXPECT_FLOAT_EQ(quest.mScriptDelay->mValue, 5.f);
        ASSERT_EQ(quest.mStages.size(), 1u);
        EXPECT_EQ(quest.mStages[0].mStage.mValue, 20);
        EXPECT_EQ(quest.mStages[0].mStatus.mValue, 1);
        ASSERT_EQ(quest.mStages[0].mLogs.size(), 1u);
        EXPECT_EQ(quest.mStages[0].mLogs[0].mLogEntry.mValue, 2);
        ASSERT_TRUE(quest.mStages[0].mLogs[0].mLogDataUnknown);
        EXPECT_EQ(quest.mStages[0].mLogs[0].mLogDataUnknown->mValue, 0x1234);
        ASSERT_EQ(quest.mVariables.size(), 2u);
        ASSERT_TRUE(quest.mVariables[0].mNumericValue);
        EXPECT_DOUBLE_EQ(quest.mVariables[0].mNumericValue->mValue, 42.25);
        ASSERT_TRUE(quest.mVariables[1].mReferenceValue);
        EXPECT_EQ(quest.mVariables[1].mReferenceValue->mResolvedFormId, 0x00000456u);
        ASSERT_TRUE(quest.mEventState);
        EXPECT_EQ(quest.mEventState->mRaw.size(), 8u);
        ASSERT_TRUE(quest.mOnLoad);
        EXPECT_EQ(quest.mOnLoad->mValue, 1);
        ASSERT_EQ(quest.mObjectives.size(), 1u);
        EXPECT_EQ(quest.mObjectives[0].mObjective.mValue, 10u);
        EXPECT_EQ(quest.mObjectives[0].mData.mValue, 3u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 15u);
    }

    SaveBytes makeSaveWithFactionChange()
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes result = makeSave(true, 2, 1, masters);
        std::vector<std::uint8_t> payload;
        appendU32(payload, 0x00000400u);
        appendDelimiter(payload);
        appendPackedCount(payload, 1);
        appendDelimitedReferenceId(payload, 0x400456);
        appendU32(payload, std::bit_cast<std::uint32_t>(std::int32_t{ -25 }));
        appendDelimiter(payload);
        appendU32(payload, 1);
        appendDelimiter(payload);
        appendU32(payload, 0x00000003u);
        appendDelimiter(payload);
        appendU32(payload, 12);
        appendDelimiter(payload);
        appendU32(payload, 34);
        appendDelimiter(payload);

        std::vector<std::uint8_t> envelope;
        appendChangedForm(envelope, { 0x40, 0x01, 0x23 }, 0x80000007u, 34, 27, 2, payload);
        result.mBytes.insert(result.mBytes.begin() + static_cast<std::ptrdiff_t>(result.mGlobalData2Begin),
            envelope.begin(), envelope.end());
        overwriteU32(result.mBytes, result.mGlobalData2OffsetField,
            static_cast<std::uint32_t>(result.mGlobalData2Begin + envelope.size()));
        overwriteU32(result.mBytes, result.mRefIdArrayOffsetField,
            static_cast<std::uint32_t>(result.mRefIdArrayBegin + envelope.size()));
        overwriteU32(result.mBytes, result.mUnknownTableOffsetField,
            static_cast<std::uint32_t>(result.mUnknownTableBegin + envelope.size()));
        overwriteU32(result.mBytes, result.mChangedFormsCountField, 4);
        return result;
    }

    TEST(FONVSaveGame, ParsesTypedFactionChangeFormState)
    {
        const SaveBytes source = makeSaveWithFactionChange();
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        ASSERT_EQ(save.mFactionChanges.size(), 1u);
        const ESM4::FONVSaveFactionChange& faction = save.mFactionChanges.front();
        EXPECT_EQ(faction.mResolvedFormId, 0x00000123u);
        EXPECT_EQ(faction.mChangeFlags, 0x80000007u);
        ASSERT_TRUE(faction.mFormFlags);
        EXPECT_EQ(faction.mFormFlags->mValue, 0x400u);
        ASSERT_TRUE(faction.mReactionCount);
        EXPECT_EQ(faction.mReactionCount->mValue, 1u);
        ASSERT_EQ(faction.mReactions.size(), 1u);
        EXPECT_EQ(faction.mReactions[0].mFaction.mResolvedFormId, 0x00000456u);
        EXPECT_EQ(faction.mReactions[0].mModifier.mValue, -25);
        EXPECT_EQ(faction.mReactions[0].mReaction.mValue, 1u);
        ASSERT_TRUE(faction.mFactionFlags);
        EXPECT_EQ(faction.mFactionFlags->mValue, 3u);
        ASSERT_TRUE(faction.mCrimeCount44);
        EXPECT_EQ(faction.mCrimeCount44->mValue, 12u);
        ASSERT_TRUE(faction.mCrimeCount48);
        EXPECT_EQ(faction.mCrimeCount48->mValue, 34u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 15u);
    }

    SaveBytes makeSaveWithMovedWorldReference()
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes result = makeSave(true, 2, 1, masters);
        std::vector<std::uint8_t> payload = { 0x40, 0x04, 0x56 };
        for (const float value : { 10.f, 20.f, 30.f, 0.1f, 0.2f, 0.3f })
            appendF32(payload, value);
        appendDelimiter(payload);
        payload.push_back(0xaa);
        payload.push_back(0xbb);

        std::vector<std::uint8_t> envelope;
        appendChangedForm(envelope, { 0x40, 0x01, 0x23 }, 0x00000002u, 1, 27, 2, payload);
        result.mBytes.insert(result.mBytes.begin() + static_cast<std::ptrdiff_t>(result.mGlobalData2Begin),
            envelope.begin(), envelope.end());
        overwriteU32(result.mBytes, result.mGlobalData2OffsetField,
            static_cast<std::uint32_t>(result.mGlobalData2Begin + envelope.size()));
        overwriteU32(result.mBytes, result.mRefIdArrayOffsetField,
            static_cast<std::uint32_t>(result.mRefIdArrayBegin + envelope.size()));
        overwriteU32(result.mBytes, result.mUnknownTableOffsetField,
            static_cast<std::uint32_t>(result.mUnknownTableBegin + envelope.size()));
        overwriteU32(result.mBytes, result.mChangedFormsCountField, 4);
        return result;
    }

    TEST(FONVSaveGame, ParsesMovedWorldReferencePrefixAndKeepsActorStateOpaque)
    {
        const SaveBytes source = makeSaveWithMovedWorldReference();
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        ASSERT_EQ(save.mWorldReferenceMovements.size(), 1u);
        const ESM4::FONVSaveWorldReferenceMovement& change = save.mWorldReferenceMovements.front();
        EXPECT_EQ(change.mResolvedFormId, 0x00000123u);
        EXPECT_EQ(change.mChangeType, 1u);
        EXPECT_EQ(change.mMovement.mCellOrWorldspace.mResolvedFormId, 0x00000456u);
        EXPECT_FLOAT_EQ(change.mMovement.mPosition[0].mValue, 10.f);
        EXPECT_FLOAT_EQ(change.mMovement.mPosition[1].mValue, 20.f);
        EXPECT_FLOAT_EQ(change.mMovement.mPosition[2].mValue, 30.f);
        EXPECT_FLOAT_EQ(change.mMovement.mRotationRadians[0].mValue, 0.1f);
        EXPECT_FLOAT_EQ(change.mMovement.mRotationRadians[1].mValue, 0.2f);
        EXPECT_FLOAT_EQ(change.mMovement.mRotationRadians[2].mValue, 0.3f);
        EXPECT_EQ(change.mMovement.mRange.mSize, 28u);
        EXPECT_EQ(change.mMovement.mUnparsedRemainder.mRaw, (std::vector<std::uint8_t>{ 0xaa, 0xbb }));
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 17u);
    }

    TEST(FONVSaveGame, ParsesNewVegasPrefixAndPreservesRawProvenance)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm"), std::string_view("DeadMoney.esm") };
        const SaveBytes source = makeSave(true, 2, 1, masters);
        const std::filesystem::path provenance = "fixture/Save330.fos";

        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes, provenance);

        EXPECT_EQ(save.mSourcePath, provenance);
        EXPECT_EQ(save.mFileSize, source.mBytes.size());
        EXPECT_EQ(save.mMagicRange, (ESM4::FONVSaveRange{ 0, 11 }));
        EXPECT_EQ(save.mRawMagic, std::vector<std::uint8_t>(source.mBytes.begin(), source.mBytes.begin() + 11));
        EXPECT_EQ(
            save.mHeaderRange, (ESM4::FONVSaveRange{ source.mHeaderBegin, source.mHeaderEnd - source.mHeaderBegin }));
        EXPECT_EQ(save.mRawHeader,
            std::vector<std::uint8_t>(
                source.mBytes.begin() + source.mHeaderBegin, source.mBytes.begin() + source.mHeaderEnd));
        ASSERT_TRUE(save.mHeader.mLanguage.has_value());
        EXPECT_EQ(save.mHeader.mLanguage->mValue, "ENGLISH");
        EXPECT_EQ(save.mHeader.mLanguage->mRaw.size(), 64u);
        EXPECT_EQ(save.mHeader.mScreenshotWidth.mValue, 2u);
        EXPECT_EQ(save.mHeader.mScreenshotHeight.mValue, 1u);
        EXPECT_EQ(save.mHeader.mSaveNumber.mValue, 330u);
        EXPECT_EQ(save.mHeader.mPlayerName.mValue, "Courier");
        EXPECT_EQ(save.mHeader.mPlayerKarmaTitle.mValue, "Desert Scavenger");
        EXPECT_EQ(save.mHeader.mPlayerLevel.mValue, 9u);
        EXPECT_EQ(save.mHeader.mPlayerLocation.mValue, "Goodsprings");
        EXPECT_EQ(save.mHeader.mPlayTime.mValue, "000.16.45");

        EXPECT_EQ(save.mScreenshotRange,
            (ESM4::FONVSaveRange{ source.mScreenshotBegin, source.mScreenshotEnd - source.mScreenshotBegin }));
        EXPECT_EQ(save.mRawScreenshot, std::vector<std::uint8_t>({ 0, 1, 2, 3, 4, 5 }));
        EXPECT_EQ(save.mFormVersion.mValue, 27u);
        EXPECT_EQ(save.mMasterTableRange,
            (ESM4::FONVSaveRange{ source.mMasterTableBegin, source.mMasterTableEnd - source.mMasterTableBegin }));
        ASSERT_EQ(save.mMasters.size(), 2u);
        EXPECT_EQ(save.mMasters[0].mFileName.mValue, "FalloutNV.esm");
        EXPECT_EQ(save.mMasters[1].mFileName.mValue, "DeadMoney.esm");
        EXPECT_EQ(save.mMasters[0].mFileName.mRawValue,
            std::vector<std::uint8_t>({ 'F', 'a', 'l', 'l', 'o', 'u', 't', 'N', 'V', '.', 'e', 's', 'm' }));
        EXPECT_EQ(save.mHeaderAndMastersRange, (ESM4::FONVSaveRange{ 0, source.mMasterTableEnd }));
        EXPECT_EQ(save.mFileLocationTable.mRange,
            (ESM4::FONVSaveRange{ source.mFileLocationBegin, source.mFileLocationEnd - source.mFileLocationBegin }));
        EXPECT_EQ(save.mFileLocationTable.mRaw,
            std::vector<std::uint8_t>(source.mBytes.begin() + source.mFileLocationBegin,
                source.mBytes.begin() + source.mFileLocationEnd));
        EXPECT_EQ(save.mFileLocationTable.mGlobalDataTable1Offset.mValue, source.mGlobalData1Begin);
        EXPECT_EQ(save.mFileLocationTable.mChangedFormsOffset.mValue, source.mChangedFormsBegin);
        EXPECT_EQ(save.mFileLocationTable.mGlobalDataTable2Offset.mValue, source.mGlobalData2Begin);
        EXPECT_EQ(save.mFileLocationTable.mRefIdArrayCountOffset.mValue, source.mRefIdArrayBegin);
        EXPECT_EQ(save.mFileLocationTable.mUnknownTableOffset.mValue, source.mUnknownTableBegin);
        EXPECT_EQ(save.mFileLocationTable.mUnused.mRange,
            (ESM4::FONVSaveRange{ source.mFileLocationBegin + 36, 74 }));
        EXPECT_EQ(save.mFileLocationTable.mUnused.mRaw, std::vector<std::uint8_t>(74, 0));

        ASSERT_EQ(save.mGlobalDataTable1.mEntries.size(), 1u);
        EXPECT_EQ(save.mGlobalDataTable1.mRange,
            (ESM4::FONVSaveRange{ source.mGlobalData1Begin, source.mChangedFormsBegin - source.mGlobalData1Begin }));
        EXPECT_EQ(save.mGlobalDataTable1.mEntries[0].mType.mValue, 0u);
        EXPECT_EQ(save.mGlobalDataTable1.mEntries[0].mDataLength.mValue, 3u);
        EXPECT_EQ(
            save.mGlobalDataTable1.mEntries[0].mUnparsedPayload.mRaw, std::vector<std::uint8_t>({ 0xaa, 0xbb, 0xcc }));

        ASSERT_EQ(save.mChangedForms.mEntries.size(), 3u);
        EXPECT_EQ(save.mChangedForms.mRange,
            (ESM4::FONVSaveRange{ source.mChangedFormsBegin, source.mGlobalData2Begin - source.mChangedFormsBegin }));
        EXPECT_EQ(save.mChangedForms.mEntries[0].mReferenceId.mRaw, std::vector<std::uint8_t>({ 0, 0, 1 }));
        EXPECT_EQ(save.mChangedForms.mEntries[0].mEncodedReferenceId.mValue, 1u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mReferenceKind, ESM4::FONVSaveReferenceKind::FormIdArray);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mReferencePayload, 1u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mResolvedFormId, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mChangeFlags.mValue, 0xb0000022u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mChangeType, 1u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mLengthWidth, 2u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mVersion.mValue, 27u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mDataLength.mValue,
            28u + static_cast<std::uint32_t>(ESM4::sFONVPlayerActorValueDataBytes)
                + static_cast<std::uint32_t>(sSyntheticPlayerProcessInventoryDataBytes)
                + static_cast<std::uint32_t>(sSyntheticPlayerMobileObjectProcessStateBytes)
                + static_cast<std::uint32_t>(ESM4::sFONVPlayerChangedCharacterStateBytes)
                + static_cast<std::uint32_t>(sSyntheticPlayerCharacterAnimationStateBytes)
                + static_cast<std::uint32_t>(ESM4::sFONVPlayerCharacterScalarReferenceStateBytes)
                + static_cast<std::uint32_t>(ESM4::sFONVPlayerCharacterListsStateBytes)
                + static_cast<std::uint32_t>(ESM4::sFONVPlayerCharacterMagicTargetStateBytes)
                + static_cast<std::uint32_t>(ESM4::sFONVPlayerCharacterFinalStateBytes));
        EXPECT_EQ(save.mChangedForms.mEntries[1].mChangeType, 2u);
        EXPECT_EQ(save.mChangedForms.mEntries[1].mEncodedReferenceId.mValue, 0x401234u);
        EXPECT_EQ(save.mChangedForms.mEntries[1].mReferenceKind, ESM4::FONVSaveReferenceKind::DefaultForm);
        EXPECT_EQ(save.mChangedForms.mEntries[1].mResolvedFormId, 0x00001234u);
        EXPECT_EQ(save.mChangedForms.mEntries[1].mLengthWidth, 2u);
        EXPECT_EQ(save.mChangedForms.mEntries[1].mDataLength.mRange.mSize, 2u);
        EXPECT_EQ(save.mChangedForms.mEntries[2].mChangeType, 3u);
        EXPECT_EQ(save.mChangedForms.mEntries[2].mEncodedReferenceId.mValue, 0x805678u);
        EXPECT_EQ(save.mChangedForms.mEntries[2].mReferenceKind, ESM4::FONVSaveReferenceKind::CreatedForm);
        EXPECT_EQ(save.mChangedForms.mEntries[2].mResolvedFormId, 0xff005678u);
        EXPECT_EQ(save.mChangedForms.mEntries[2].mLengthWidth, 4u);
        EXPECT_EQ(save.mChangedForms.mEntries[2].mDataLength.mRange.mSize, 4u);

        ASSERT_EQ(save.mGlobalDataTable2.mEntries.size(), 1u);
        EXPECT_EQ(save.mGlobalDataTable2.mEntries[0].mType.mValue, 1000u);
        EXPECT_EQ(save.mGlobalDataTable2.mEntries[0].mDataLength.mValue, 0u);
        EXPECT_TRUE(save.mGlobalDataTable2.mEntries[0].mUnparsedPayload.mRaw.empty());
        ASSERT_EQ(save.mFormIdTable.mFormIds.size(), 2u);
        EXPECT_EQ(save.mFormIdTable.mCount.mValue, 2u);
        EXPECT_EQ(save.mFormIdTable.mFormIds[0].mValue, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(save.mFormIdTable.mFormIds[1].mValue, 0x000da726u);
        EXPECT_EQ(save.mFormIdTable.mRange, (ESM4::FONVSaveRange{ source.mRefIdArrayBegin, 12 }));
        ASSERT_EQ(save.mVisitedWorldspaces.mFormIds.size(), 1u);
        EXPECT_EQ(save.mVisitedWorldspaces.mCount.mValue, 1u);
        EXPECT_EQ(save.mVisitedWorldspaces.mFormIds[0].mValue, 0x000da726u);
        EXPECT_EQ(save.mVisitedWorldspaces.mRange, (ESM4::FONVSaveRange{ source.mRefIdArrayBegin + 12, 8 }));
        EXPECT_EQ(save.mUnknownTable.mCount.mValue, 5u);
        EXPECT_EQ(save.mUnknownTable.mUnparsedEntries.mRaw,
            std::vector<std::uint8_t>({ 0, 0, 0, 0, sDelimiter }));
        EXPECT_EQ(save.mUnknownTable.mRange, (ESM4::FONVSaveRange{ source.mUnknownTableBegin, 9 }));
        EXPECT_EQ(&save.requirePlayerReferenceChangeForm(), &save.mChangedForms.mEntries[0]);
        ASSERT_TRUE(save.mPlayerReferenceMovement.has_value());
        const auto& movement = *save.mPlayerReferenceMovement;
        EXPECT_EQ(movement.mRange, (ESM4::FONVSaveRange{ source.mChangedFormPayloads[0], 28 }));
        EXPECT_EQ(movement.mCellOrWorldspace.mEncoded.mValue, 2u);
        EXPECT_EQ(movement.mCellOrWorldspace.mKind, ESM4::FONVSaveReferenceKind::FormIdArray);
        EXPECT_EQ(movement.mCellOrWorldspace.mPayload, 2u);
        EXPECT_EQ(movement.mCellOrWorldspace.mResolvedFormId, 0x000da726u);
        EXPECT_FLOAT_EQ(movement.mPosition[0].mValue, -72392.84375f);
        EXPECT_FLOAT_EQ(movement.mPosition[1].mValue, -1240.19275f);
        EXPECT_FLOAT_EQ(movement.mPosition[2].mValue, 8137.58643f);
        EXPECT_FLOAT_EQ(movement.mRotationRadians[0].mValue, -0.06439045f);
        EXPECT_FLOAT_EQ(movement.mRotationRadians[1].mValue, 0.f);
        EXPECT_TRUE(std::signbit(movement.mRotationRadians[1].mValue));
        EXPECT_EQ(movement.mRotationRadians[1].mRaw, std::vector<std::uint8_t>({ 0, 0, 0, 0x80 }));
        EXPECT_FLOAT_EQ(movement.mRotationRadians[2].mValue, 2.93332028f);
        EXPECT_EQ(movement.mTerminator.mValue, sDelimiter);
        EXPECT_EQ(movement.mTerminator.mRange,
            (ESM4::FONVSaveRange{ source.mChangedFormPayloads[0] + 27, 1 }));
        EXPECT_EQ(movement.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mChangedFormPayloads[0] + 28,
                ESM4::sFONVPlayerActorValueDataBytes + sSyntheticPlayerProcessInventoryDataBytes
                    + sSyntheticPlayerMobileObjectProcessStateBytes
                    + ESM4::sFONVPlayerChangedCharacterStateBytes
                    + sSyntheticPlayerCharacterAnimationStateBytes
                    + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
                    + ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerActorValueData.has_value());
        const auto& actorValues = *save.mPlayerActorValueData;
        const std::size_t actorValuesBegin = source.mChangedFormPayloads[0] + 28;
        EXPECT_EQ(actorValues.mRange,
            (ESM4::FONVSaveRange{ actorValuesBegin, ESM4::sFONVPlayerActorValueDataBytes }));
        EXPECT_EQ(actorValues.mRaw,
            std::vector<std::uint8_t>(source.mBytes.begin() + static_cast<std::ptrdiff_t>(actorValuesBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(
                                            actorValuesBegin + ESM4::sFONVPlayerActorValueDataBytes)));
        std::vector<std::size_t> nonzero244;
        std::vector<std::size_t> nonzero378;
        std::vector<std::size_t> nonzero4B0;
        for (std::size_t i = 0; i < ESM4::sFONVPlayerActorValueCount; ++i)
        {
            if (actorValues.mActorValues244[i].mValue != 0.f)
                nonzero244.push_back(i);
            if (actorValues.mActorValues378[i].mValue != 0.f)
                nonzero378.push_back(i);
            if (actorValues.mActorValues4B0[i].mValue != 0.f)
                nonzero4B0.push_back(i);
        }
        EXPECT_EQ(nonzero244, (std::vector<std::size_t>{ 38, 43 }));
        EXPECT_EQ(nonzero378, (std::vector<std::size_t>{ 24 }));
        EXPECT_TRUE(nonzero4B0.empty());
        EXPECT_FLOAT_EQ(actorValues.mActorValues244[38].mValue, 2.f);
        EXPECT_FLOAT_EQ(actorValues.mActorValues378[24].mValue, 10.f);
        EXPECT_EQ(actorValues.mActorValues244[38].mRange, (ESM4::FONVSaveRange{ actorValuesBegin + 190, 4 }));
        EXPECT_EQ(actorValues.mActorValues244[38].mRaw, (std::vector<std::uint8_t>{ 0, 0, 0, 0x40 }));
        EXPECT_EQ(actorValues.mActorValues378[24].mRange, (ESM4::FONVSaveRange{ actorValuesBegin + 505, 4 }));
        EXPECT_EQ(actorValues.mActorValues378[24].mRaw, (std::vector<std::uint8_t>{ 0, 0, 0x20, 0x41 }));
        EXPECT_EQ(actorValues.mActorValues4B0[0].mRange, (ESM4::FONVSaveRange{ actorValuesBegin + 770, 4 }));
        EXPECT_EQ(actorValues.mUnk4AC.mValue, 0u);
        EXPECT_EQ(actorValues.mUnk4AC.mRange, (ESM4::FONVSaveRange{ actorValuesBegin + 1155, 4 }));
        EXPECT_EQ(actorValues.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ actorValuesBegin + ESM4::sFONVPlayerActorValueDataBytes,
                sSyntheticPlayerProcessInventoryDataBytes + sSyntheticPlayerMobileObjectProcessStateBytes
                    + ESM4::sFONVPlayerChangedCharacterStateBytes
                    + sSyntheticPlayerCharacterAnimationStateBytes
                    + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
                    + ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerProcessInventoryData.has_value());
        const auto& processInventory = *save.mPlayerProcessInventoryData;
        EXPECT_EQ(processInventory.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerProcessInventoryBegin,
                sSyntheticPlayerProcessInventoryDataBytes }));
        EXPECT_EQ(processInventory.mRaw,
            std::vector<std::uint8_t>(
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerProcessInventoryBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(
                                            source.mPlayerProcessInventoryBegin
                                            + sSyntheticPlayerProcessInventoryDataBytes)));
        EXPECT_EQ(processInventory.mProcessLevel.mValue, 0);
        EXPECT_FALSE(processInventory.mReferenceScale.has_value());
        ASSERT_EQ(processInventory.mActorExtraData.size(), 2u);
        EXPECT_EQ(processInventory.mActorExtraData[0].mType.mValue, ESM4::sFONVExtraFactionChangesType);
        ASSERT_EQ(processInventory.mActorExtraData[0].mFactionChanges.size(), 1u);
        EXPECT_EQ(processInventory.mActorExtraData[0].mFactionChanges[0].mFaction.mResolvedFormId, 0x00000111u);
        EXPECT_EQ(processInventory.mActorExtraData[0].mFactionChanges[0].mRank.mValue, 1);
        EXPECT_EQ(processInventory.mActorExtraData[1].mType.mValue, ESM4::sFONVExtraEncounterZoneType);
        ASSERT_TRUE(processInventory.mActorExtraData[1].mEncounterZone.has_value());
        EXPECT_EQ(processInventory.mActorExtraData[1].mEncounterZone->mResolvedFormId, 0x00000222u);
        EXPECT_EQ(processInventory.mInventoryEntryCount.mValue, 3u);
        ASSERT_EQ(processInventory.mInventoryEntries.size(), 3u);
        EXPECT_EQ(processInventory.mInventoryEntries[0].mType.mResolvedFormId, 0x00000333u);
        EXPECT_EQ(processInventory.mInventoryEntries[0].mDelta.mValue, 5);
        ASSERT_EQ(processInventory.mInventoryEntries[0].mExtendData.size(), 1u);
        ASSERT_EQ(processInventory.mInventoryEntries[0].mExtendData[0].mExtraData.size(), 3u);
        const auto& health = processInventory.mInventoryEntries[0].mExtendData[0].mExtraData[0];
        EXPECT_EQ(health.mType.mValue, ESM4::sFONVExtraHealthType);
        ASSERT_TRUE(health.mHealth.has_value());
        EXPECT_FLOAT_EQ(health.mHealth->mValue, 75.f);
        EXPECT_EQ(health.mHealth->mRaw, (std::vector<std::uint8_t>{ 0, 0, 0x96, 0x42 }));
        const auto& hotkey = processInventory.mInventoryEntries[0].mExtendData[0].mExtraData[1];
        EXPECT_EQ(hotkey.mType.mValue, ESM4::sFONVExtraHotkeyType);
        ASSERT_TRUE(hotkey.mHotkey.has_value());
        EXPECT_EQ(hotkey.mHotkey->mValue, 6u);
        const auto& ammo = processInventory.mInventoryEntries[0].mExtendData[0].mExtraData[2];
        EXPECT_EQ(ammo.mType.mValue, ESM4::sFONVExtraAmmoType);
        ASSERT_TRUE(ammo.mAmmo.has_value());
        EXPECT_EQ(ammo.mAmmo->mResolvedFormId, 0x00000444u);
        ASSERT_TRUE(ammo.mAmmoCount.has_value());
        EXPECT_EQ(ammo.mAmmoCount->mValue, 9);
        ASSERT_EQ(processInventory.mInventoryEntries[1].mExtendData.size(), 1u);
        ASSERT_EQ(processInventory.mInventoryEntries[1].mExtendData[0].mExtraData.size(), 1u);
        const auto& worn = processInventory.mInventoryEntries[1].mExtendData[0].mExtraData[0];
        EXPECT_EQ(worn.mType.mValue, ESM4::sFONVExtraWornType);
        EXPECT_FALSE(worn.mCount.has_value());
        EXPECT_FALSE(worn.mHealth.has_value());
        ASSERT_EQ(processInventory.mInventoryEntries[2].mExtendData.size(), 1u);
        ASSERT_EQ(processInventory.mInventoryEntries[2].mExtendData[0].mExtraData.size(), 2u);
        const auto& count = processInventory.mInventoryEntries[2].mExtendData[0].mExtraData[0];
        EXPECT_EQ(count.mType.mValue, ESM4::sFONVExtraCountType);
        ASSERT_TRUE(count.mCount.has_value());
        EXPECT_EQ(count.mCount->mValue, 2);
        EXPECT_EQ(processInventory.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerProcessInventoryBegin
                    + sSyntheticPlayerProcessInventoryDataBytes,
                sSyntheticPlayerMobileObjectProcessStateBytes
                    + ESM4::sFONVPlayerChangedCharacterStateBytes
                    + sSyntheticPlayerCharacterAnimationStateBytes
                    + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
                    + ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerMobileObjectProcessState.has_value());
        const auto& processState = *save.mPlayerMobileObjectProcessState;
        EXPECT_EQ(processState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin,
                sSyntheticPlayerMobileObjectProcessStateBytes }));
        EXPECT_EQ(processState.mRaw,
            std::vector<std::uint8_t>(
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerMobileObjectProcessStateBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(
                                            source.mPlayerMobileObjectProcessStateBegin
                                            + sSyntheticPlayerMobileObjectProcessStateBytes)));
        EXPECT_EQ(processState.mMobileObjectBase.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin, 38 }));
        EXPECT_EQ(processState.mMobileObjectBase.mBytes084_085_07C_07F_080_07D_07E_086[0].mValue, -1);
        EXPECT_EQ(processState.mBaseProcess.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin + 38, 19 }));
        EXPECT_FALSE(processState.mBaseProcess.mPackage.mResolvedFormId.has_value());
        EXPECT_EQ(processState.mLowProcess.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin + 57, 63 }));
        EXPECT_FLOAT_EQ(processState.mLowProcess.mUnk038[0].mValue, -1.f);
        EXPECT_EQ(processState.mMiddleLowProcess.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin + 120, 5 }));
        EXPECT_EQ(processState.mMiddleLowProcess.mUnk0B4.mValue, 0xffffffffu);
        EXPECT_EQ(processState.mMiddleHighProcess.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin + 125, 185 }));
        ASSERT_TRUE(processState.mMiddleHighProcess.mAnimation.has_value());
        EXPECT_EQ(processState.mMiddleHighProcess.mAnimation->mLength.mValue, 0u);
        EXPECT_TRUE(processState.mMiddleHighProcess.mAnimation->mData.mRaw.empty());
        EXPECT_EQ(processState.mHighProcess.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin + 310, 378 }));
        ASSERT_EQ(processState.mHighProcess.mList25C.size(), 1u);
        const auto& location = processState.mHighProcess.mList25C[0];
        EXPECT_EQ(location.mRange,
            (ESM4::FONVSaveRange{ processState.mHighProcess.mList25CCount.mRange.end() + 1, 42 }));
        EXPECT_EQ(location.mForm000.mResolvedFormId, 0x00000333u);
        EXPECT_EQ(location.mUnk004.mValue, 1u);
        EXPECT_EQ(location.mUnk008.mValue, 0xfffffffeu);
        EXPECT_FLOAT_EQ(location.mCoords.mComponents[0].mValue, 1.f);
        EXPECT_FLOAT_EQ(location.mCoords.mComponents[1].mValue, 2.f);
        EXPECT_FLOAT_EQ(location.mCoords.mComponents[2].mValue, 3.f);
        EXPECT_FLOAT_EQ(location.mTim018.mValue, -4.f);
        EXPECT_EQ(processState.mHighProcess.mSubBuffer.mLength.mValue, 3u);
        EXPECT_EQ(processState.mHighProcess.mSubBuffer.mData.mRaw,
            (std::vector<std::uint8_t>{ 0xaa, 0xbb, 0xcc }));
        EXPECT_EQ(processState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerMobileObjectProcessStateBegin
                    + sSyntheticPlayerMobileObjectProcessStateBytes,
                ESM4::sFONVPlayerChangedCharacterStateBytes
                    + sSyntheticPlayerCharacterAnimationStateBytes
                    + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
                    + ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerChangedCharacterState.has_value());
        const auto& characterState = *save.mPlayerChangedCharacterState;
        EXPECT_EQ(characterState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin,
                ESM4::sFONVPlayerChangedCharacterStateBytes }));
        EXPECT_EQ(characterState.mRaw,
            std::vector<std::uint8_t>(
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerChangedCharacterStateBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerChangedCharacterStateBegin
                    + ESM4::sFONVPlayerChangedCharacterStateBytes)));
        EXPECT_EQ(characterState.mActorFixed.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin, 113 }));
        EXPECT_FLOAT_EQ(characterState.mActorFixed.mUnk114.mValue, 0.25f);
        EXPECT_EQ(characterState.mActorFixed.mForm0C0_ActorBase_Form070[1].mResolvedFormId,
            ESM4::sFONVPlayerNpcFormId);
        EXPECT_EQ(characterState.mActorFixed.mForm0C0_ActorBase_Form070[2].mResolvedFormId,
            ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(characterState.mActorMover.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin + 113, 393 }));
        EXPECT_EQ(characterState.mActorMover.mContentFlags.mValue, 0x08u);
        EXPECT_EQ(characterState.mActorMover.mPathingLocation.mNavMesh_Cell_Worldspace[1].mResolvedFormId,
            ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(characterState.mActorMover.mPathingLocation.mNavMesh_Cell_Worldspace[2].mResolvedFormId,
            0x000da726u);
        EXPECT_EQ(characterState.mActorMover.mDetailedPathHandler.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin + 236, 242 }));
        EXPECT_EQ(characterState.mActorMover.mDetailedPathHandler.mList058Count.mValue, 0u);
        EXPECT_TRUE(characterState.mActorMover.mDetailedPathHandler.mList058.empty());
        EXPECT_EQ(characterState.mActorMover.mDetailedPathHandler.mForm0D8.mResolvedFormId,
            ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(characterState.mActorMover.mPlayerMover.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin + 478, 28 }));
        EXPECT_FLOAT_EQ(characterState.mActorMover.mPlayerMover.mCoords.mComponents[0].mValue, 308.f);
        EXPECT_EQ(characterState.mByt1C0.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin + 506, 1 }));
        EXPECT_EQ(characterState.mByt1C1.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin + 508, 1 }));
        EXPECT_EQ(characterState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerChangedCharacterStateBegin
                    + ESM4::sFONVPlayerChangedCharacterStateBytes,
                sSyntheticPlayerCharacterAnimationStateBytes
                    + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
                    + ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerCharacterAnimationState.has_value());
        const auto& animationState = *save.mPlayerCharacterAnimationState;
        EXPECT_EQ(animationState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterAnimationStateBegin,
                sSyntheticPlayerCharacterAnimationStateBytes }));
        EXPECT_EQ(animationState.mRaw,
            std::vector<std::uint8_t>(
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerCharacterAnimationStateBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerCharacterAnimationStateBegin
                    + sSyntheticPlayerCharacterAnimationStateBytes)));
        EXPECT_EQ(animationState.mAnimation.mRange, animationState.mRange);
        EXPECT_EQ(animationState.mAnimation.mLength.mValue, sSyntheticPlayerCharacterAnimationBodyBytes);
        EXPECT_EQ(animationState.mAnimation.mLength.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterAnimationStateBegin, 2 }));
        EXPECT_EQ(animationState.mAnimation.mLength.mRaw, (std::vector<std::uint8_t>{ 0x45, 0x02 }));
        EXPECT_EQ(animationState.mAnimation.mData.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterAnimationStateBegin + 3,
                sSyntheticPlayerCharacterAnimationBodyBytes }));
        ASSERT_EQ(animationState.mAnimation.mData.mRaw.size(), sSyntheticPlayerCharacterAnimationBodyBytes);
        for (std::size_t i = 0; i < animationState.mAnimation.mData.mRaw.size(); ++i)
            EXPECT_EQ(animationState.mAnimation.mData.mRaw[i], static_cast<std::uint8_t>(i));
        EXPECT_EQ(animationState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterAnimationStateBegin
                    + sSyntheticPlayerCharacterAnimationStateBytes,
                ESM4::sFONVPlayerCharacterScalarReferenceStateBytes
                    + ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerCharacterScalarReferenceState.has_value());
        const auto& scalarState = *save.mPlayerCharacterScalarReferenceState;
        EXPECT_EQ(scalarState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterScalarReferenceStateBegin,
                ESM4::sFONVPlayerCharacterScalarReferenceStateBytes }));
        EXPECT_EQ(scalarState.mRaw,
            std::vector<std::uint8_t>(
                source.mBytes.begin()
                    + static_cast<std::ptrdiff_t>(source.mPlayerCharacterScalarReferenceStateBegin),
                source.mBytes.begin()
                    + static_cast<std::ptrdiff_t>(source.mPlayerCharacterScalarReferenceStateBegin
                        + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes)));
        EXPECT_EQ(scalarState.mFirstPersonMode.mValue, 1u);
        EXPECT_EQ(scalarState.mFirstPersonMode.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterScalarReferenceStateBegin, 1 }));
        EXPECT_FLOAT_EQ(scalarState.mRefr6F4Pos.mComponents[0].mValue, 1.f);
        EXPECT_FLOAT_EQ(scalarState.mRefr6F4Pos.mComponents[1].mValue, 2.f);
        EXPECT_FLOAT_EQ(scalarState.mRefr6F4Pos.mComponents[2].mValue, 3.f);
        EXPECT_FLOAT_EQ(scalarState.mFirstPersonModelFov.mValue, 55.f);
        EXPECT_FLOAT_EQ(scalarState.mWorldFov.mValue, 75.f);
        EXPECT_FLOAT_EQ(scalarState.mFlt11E0B5C.mValue, 120.f);
        EXPECT_EQ(scalarState.mByt64F.mValue, 13u);
        EXPECT_EQ(scalarState.mByt650.mValue, 14u);
        EXPECT_EQ(scalarState.mByt7C7.mValue, 16u);
        EXPECT_EQ(scalarState.mByt5F8.mValue, 18u);
        EXPECT_EQ(scalarState.mVersion21.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterScalarReferenceStateBegin + 201, 35 }));
        EXPECT_EQ(scalarState.mVersion21.mUnk744[4].mValue, 0x748u);
        EXPECT_EQ(scalarState.mUnk878.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterScalarReferenceStateBegin + 236, 7 }));
        EXPECT_EQ(scalarState.mUnk878.mUnk004.mValue, 0x878u);
        EXPECT_EQ(scalarState.mQuest.mResolvedFormId, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_FALSE(scalarState.mClass.mResolvedFormId.has_value());
        EXPECT_EQ(scalarState.mRefr6F4ParentCell.mResolvedFormId, 0x000da726u);
        EXPECT_EQ(scalarState.mRegion.mResolvedFormId, 0x00000111u);
        EXPECT_EQ(scalarState.mRegionWeather.mResolvedFormId, 0xff000222u);
        EXPECT_EQ(scalarState.mForm224.mResolvedFormId, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(scalarState.mForm604.mResolvedFormId, 0x000da726u);
        EXPECT_EQ(scalarState.mFormD44.mResolvedFormId, 0x00000333u);
        EXPECT_EQ(scalarState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterScalarReferenceStateBegin
                    + ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
                ESM4::sFONVPlayerCharacterListsStateBytes
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerCharacterListsState.has_value());
        const auto& listsState = *save.mPlayerCharacterListsState;
        EXPECT_EQ(listsState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterListsStateBegin,
                ESM4::sFONVPlayerCharacterListsStateBytes }));
        EXPECT_EQ(listsState.mRaw,
            std::vector<std::uint8_t>(
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerCharacterListsStateBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerCharacterListsStateBegin
                    + ESM4::sFONVPlayerCharacterListsStateBytes)));
        EXPECT_EQ(listsState.mList6A8Count.mValue, 2u);
        ASSERT_EQ(listsState.mTopics.size(), 2u);
        EXPECT_EQ(listsState.mTopics[0].mResolvedFormId, 0x00000111u);
        EXPECT_EQ(listsState.mTopics[1].mResolvedFormId, 0x00000112u);
        EXPECT_EQ(listsState.mList5E4Count.mValue, 1u);
        ASSERT_EQ(listsState.mNotes.size(), 1u);
        EXPECT_EQ(listsState.mNotes[0].mResolvedFormId, 0x00000113u);
        EXPECT_EQ(listsState.mInventoryEntryCount.mValue, 2u);
        ASSERT_EQ(listsState.mInventoryEntries.size(), 2u);
        EXPECT_EQ(listsState.mInventoryEntries[0].mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterListsStateBegin + 18, 11 }));
        EXPECT_EQ(listsState.mInventoryEntries[0].mType.mResolvedFormId, 0x00000114u);
        EXPECT_EQ(listsState.mInventoryEntries[0].mDelta.mValue, 3);
        EXPECT_EQ(listsState.mInventoryEntries[1].mType.mResolvedFormId, 0xff000115u);
        EXPECT_EQ(listsState.mInventoryEntries[1].mDelta.mValue, -2);
        EXPECT_EQ(listsState.mListD48Count.mValue, 1u);
        ASSERT_EQ(listsState.mListD48.size(), 1u);
        EXPECT_EQ(listsState.mListD48[0].mForm000.mResolvedFormId, 0x00000116u);
        EXPECT_EQ(listsState.mListD48[0].mByt004.mValue, 0x44u);
        EXPECT_EQ(listsState.mListD48[0].mByt005.mValue, 0x45u);
        EXPECT_EQ(listsState.mPerkCount.mValue, 1u);
        ASSERT_EQ(listsState.mPerks.size(), 1u);
        EXPECT_EQ(listsState.mPerks[0].mPerk.mResolvedFormId, 0x00000117u);
        EXPECT_EQ(listsState.mPerks[0].mByt004.mValue, 2u);
        EXPECT_EQ(listsState.mList60CCount.mValue, 1u);
        ASSERT_EQ(listsState.mList60C.size(), 1u);
        EXPECT_EQ(listsState.mList60C[0].mUnk000.mValue, 0x60cu);
        EXPECT_FLOAT_EQ(listsState.mList60C[0].mFlt004.mValue, 12.5f);
        EXPECT_EQ(listsState.mList60C[0].mFlt004.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterListsStateBegin + 65, 4 }));
        EXPECT_EQ(listsState.mList60C[0].mFormId.mResolvedFormId, 0x00000118u);
        EXPECT_EQ(listsState.mList610Count.mValue, 1u);
        ASSERT_EQ(listsState.mList610.size(), 1u);
        EXPECT_EQ(listsState.mList610[0].mUnk000.mValue, 0x610u);
        EXPECT_EQ(listsState.mList610[0].mUnk004.mValue, 0x611u);
        EXPECT_EQ(listsState.mList610[0].mWrd008.mValue, 0x612u);
        ASSERT_EQ(listsState.mCards614.size(), 1u);
        ASSERT_EQ(listsState.mCards618.size(), 1u);
        EXPECT_EQ(listsState.mCards614[0].mResolvedFormId, 0x00000119u);
        EXPECT_EQ(listsState.mCards618[0].mResolvedFormId, 0x0000011au);
        EXPECT_EQ(listsState.mUnk61C.mValue, 0x61cu);
        EXPECT_EQ(listsState.mUnk62C.mValue, 0x62cu);
        EXPECT_EQ(listsState.mStageCount.mValue, 1u);
        ASSERT_EQ(listsState.mStages.size(), 1u);
        EXPECT_EQ(listsState.mStages[0].mQuest.mResolvedFormId, 0x0000011bu);
        EXPECT_EQ(listsState.mStages[0].mStage.mValue, 10u);
        EXPECT_EQ(listsState.mStages[0].mLogEntry.mValue, 1u);
        EXPECT_EQ(listsState.mObjectiveCount.mValue, 1u);
        ASSERT_EQ(listsState.mObjectives.size(), 1u);
        EXPECT_EQ(listsState.mObjectives[0].mQuest.mResolvedFormId, 0x0000011cu);
        EXPECT_EQ(listsState.mObjectives[0].mObjective.mValue, 10u);
        EXPECT_EQ(listsState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterListsStateBegin
                    + ESM4::sFONVPlayerCharacterListsStateBytes,
                ESM4::sFONVPlayerCharacterMagicTargetStateBytes
                    + ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerCharacterMagicTargetState.has_value());
        const auto& magicTargetState = *save.mPlayerCharacterMagicTargetState;
        EXPECT_EQ(magicTargetState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterMagicTargetStateBegin,
                ESM4::sFONVPlayerCharacterMagicTargetStateBytes }));
        EXPECT_EQ(magicTargetState.mRaw,
            std::vector<std::uint8_t>(source.mBytes.begin()
                    + static_cast<std::ptrdiff_t>(source.mPlayerCharacterMagicTargetStateBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerCharacterMagicTargetStateBegin
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes)));
        EXPECT_EQ(magicTargetState.mMagicItemCount.mValue, 4u);
        ASSERT_EQ(magicTargetState.mMagicItems.size(), 4u);
        constexpr std::array<std::uint32_t, 4> syntheticMagicFormIds
            = { 0x00000201u, 0xff000202u, ESM4::sFONVPlayerReferenceFormId, 0x000da726u };
        for (std::size_t entryIndex = 0; entryIndex < magicTargetState.mMagicItems.size(); ++entryIndex)
        {
            const auto& entry = magicTargetState.mMagicItems[entryIndex];
            const std::size_t entryBegin = source.mPlayerCharacterMagicTargetStateBegin + 2 + entryIndex * 58;
            EXPECT_EQ(entry.mRange, (ESM4::FONVSaveRange{ entryBegin, 58 }));
            EXPECT_EQ(entry.mMagicForm.mResolvedFormId, syntheticMagicFormIds[entryIndex]);
            EXPECT_EQ(entry.mArchType.mValue, entryIndex & 1u);
            EXPECT_EQ(entry.mUnk098.mValue, entryIndex == 0 ? 33u : entryIndex);
            EXPECT_EQ(entry.mEffectItemCount.mValue, 48u);
            ASSERT_EQ(entry.mEffectItems.size(), 48u);
            for (std::size_t effectIndex = 0; effectIndex < entry.mEffectItems.size(); ++effectIndex)
            {
                EXPECT_EQ(entry.mEffectItems[effectIndex].mValue,
                    static_cast<std::uint8_t>(entryIndex * 48 + effectIndex));
                EXPECT_EQ(entry.mEffectItems[effectIndex].mRange,
                    (ESM4::FONVSaveRange{ entryBegin + 10 + effectIndex, 1 }));
            }
        }
        EXPECT_EQ(magicTargetState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterMagicTargetStateBegin
                    + ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
                ESM4::sFONVPlayerCharacterFinalStateBytes }));
        ASSERT_TRUE(save.mPlayerCharacterFinalState.has_value());
        const auto& finalState = *save.mPlayerCharacterFinalState;
        EXPECT_EQ(finalState.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterFinalStateBegin,
                ESM4::sFONVPlayerCharacterFinalStateBytes }));
        EXPECT_EQ(finalState.mRaw,
            std::vector<std::uint8_t>(source.mBytes.begin()
                    + static_cast<std::ptrdiff_t>(source.mPlayerCharacterFinalStateBegin),
                source.mBytes.begin() + static_cast<std::ptrdiff_t>(source.mPlayerCharacterFinalStateBegin
                    + ESM4::sFONVPlayerCharacterFinalStateBytes)));
        EXPECT_EQ(finalState.mKeyForUnkD64.mValue, 0xd64u);
        EXPECT_EQ(finalState.mKeyForUnkD64.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterFinalStateBegin, 4 }));
        EXPECT_FLOAT_EQ(finalState.mUnknown11DFED4.mValue, 1.25f);
        EXPECT_FLOAT_EQ(finalState.mUnknown11DFED8.mValue, -2.5f);
        EXPECT_EQ(finalState.mPerksAD4Count.mValue, 0u);
        EXPECT_TRUE(finalState.mPerksAD4.empty());
        EXPECT_EQ(finalState.mHardcoreMode.mValue, 1u);
        EXPECT_EQ(finalState.mClearsHardcoreFlag.mValue, 0u);
        EXPECT_EQ(finalState.mByt66E.mValue, 1u);
        constexpr std::array<std::uint32_t, 8> syntheticE3CFormIds
            = { 0u, 0x00000123u, 0xff000456u, 0x01020304u, 0u, 0u, 0u, 0u };
        for (std::size_t i = 0; i < finalState.mUnknownE3C.size(); ++i)
        {
            EXPECT_EQ(finalState.mUnknownE3C[i].mEncoded.mValue, syntheticE3CFormIds[i]);
            EXPECT_EQ(finalState.mUnknownE3C[i].mEncoded.mRange,
                (ESM4::FONVSaveRange{ source.mPlayerCharacterFinalStateBegin + 23 + i * 5, 4 }));
            if (syntheticE3CFormIds[i] == 0)
                EXPECT_FALSE(finalState.mUnknownE3C[i].mResolvedFormId.has_value());
            else
                EXPECT_EQ(finalState.mUnknownE3C[i].mResolvedFormId, syntheticE3CFormIds[i]);
        }
        EXPECT_EQ(finalState.mEnabledCount.mValue, 0u);
        EXPECT_TRUE(finalState.mEnabled.empty());
        EXPECT_EQ(finalState.mDisabledCount.mValue, 0u);
        EXPECT_TRUE(finalState.mDisabled.empty());
        EXPECT_EQ(finalState.mUnknownCount.mValue, 0u);
        EXPECT_TRUE(finalState.mUnknown.empty());
        EXPECT_EQ(finalState.mFadeOutCount.mValue, 0u);
        EXPECT_TRUE(finalState.mFadeOut.empty());
        EXPECT_EQ(finalState.mUnparsedRemainder.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerCharacterFinalStateBegin
                    + ESM4::sFONVPlayerCharacterFinalStateBytes,
                0 }));
        EXPECT_TRUE(finalState.mUnparsedRemainder.mRaw.empty());
        EXPECT_EQ(save.findChangedForm(0x00001234u), &save.mChangedForms.mEntries[1]);
        EXPECT_EQ(save.findChangedForm(0x00001234u, 3), nullptr);
        EXPECT_EQ(save.mUnparsedSemanticPayloadRanges.size(), 4u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 15u);
        EXPECT_EQ(save.mStructurallyAccountedRange, (ESM4::FONVSaveRange{ 0, source.mBytes.size() }));
        EXPECT_EQ(save.mParsedPrefixRange, save.mStructurallyAccountedRange);
        EXPECT_EQ(save.mUnparsedBodyRange, (ESM4::FONVSaveRange{ source.mBytes.size(), 0 }));
    }

    TEST(FONVSaveGame, TestOnlySha256MatchesPublishedVectors)
    {
        constexpr std::array<std::uint8_t, 3> abc = { 'a', 'b', 'c' };
        EXPECT_EQ(sha256Hex({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT_EQ(sha256Hex(abc), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    TEST(FONVSaveGame, ParsesDelimitedSkyStateAndRejectsCorruptDelimiters)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters, true, "Courier", true);

        ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        ASSERT_TRUE(save.mSky.has_value());
        const ESM4::FONVSaveSkyState& sky = *save.mSky;
        EXPECT_EQ(sky.mRange, (ESM4::FONVSaveRange{ source.mGlobalData1Begin + 8, 71 }));
        EXPECT_EQ(sky.mRaw.size(), 71u);
        EXPECT_EQ(sky.mCurrentWeather.mResolvedFormId, 0x001237d7u);
        EXPECT_FALSE(sky.mTransitionWeather.mResolvedFormId.has_value());
        EXPECT_EQ(sky.mDefaultWeather.mResolvedFormId, 0x000ffc88u);
        EXPECT_FALSE(sky.mOverrideWeather.mResolvedFormId.has_value());
        EXPECT_FLOAT_EQ(sky.mGameHour.mValue, 14.25f);
        EXPECT_FLOAT_EQ(sky.mLastUpdateHour.mValue, 14.f);
        EXPECT_FLOAT_EQ(sky.mWeatherPercent.mValue, 1.f);
        EXPECT_EQ(sky.mFlags.mValue, 0x20u);
        EXPECT_FLOAT_EQ(sky.mFogPower.mValue, 0.5f);
        EXPECT_EQ(sky.mSkyMode.mValue, 3u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadRanges.size(), 3u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 12u);

        source.mBytes[source.mGlobalData1Begin + 8 + 3] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, ParsesFallout3StylePrefixWithoutLanguage)
    {
        constexpr std::array masters = { std::string_view("Fallout3.esm") };
        const SaveBytes source = makeSave(false, 1, 2, masters);

        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);

        EXPECT_FALSE(save.mHeader.mLanguage.has_value());
        EXPECT_EQ(save.mHeader.mScreenshotWidth.mValue, 1u);
        EXPECT_EQ(save.mHeader.mScreenshotHeight.mValue, 2u);
        ASSERT_EQ(save.mMasters.size(), 1u);
        EXPECT_EQ(save.mMasters[0].mFileName.mValue, "Fallout3.esm");
    }

    TEST(FONVSaveGame, ParsesRetailEmptyStringEncodingWithoutInventingASecondDelimiter)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        const SaveBytes source = makeSave(true, 2, 1, masters, true, "");

        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);

        EXPECT_EQ(save.mHeader.mPlayerName.mValue, "");
        EXPECT_EQ(save.mHeader.mPlayerName.mEncodedRange.mSize, 3u);
        EXPECT_EQ(save.mHeader.mPlayerName.mRawEncoded, std::vector<std::uint8_t>({ 0, 0, sDelimiter }));
        EXPECT_EQ(save.mHeader.mPlayerKarmaTitle.mValue, "Desert Scavenger");
    }

    TEST(FONVSaveGame, RejectsEveryTruncationInsideStructurallyAccountedFile)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        const SaveBytes source = makeSave(true, 2, 1, masters);

        for (std::size_t size = 0; size < source.mBytes.size(); ++size)
        {
            SCOPED_TRACE(size);
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(std::span(source.mBytes).first(size)), ESM4::FONVSaveError);
        }
    }

    TEST(FONVSaveGame, RejectsBadMagicAndDelimiters)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[0] = 'X';
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[19] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mMasterCountDelimiter] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsUnaccountedHeaderAndMasterTableBytes)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, 11, static_cast<std::uint32_t>(source.mHeaderEnd - source.mHeaderBegin + 1));
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mMasterSizeOffset,
            static_cast<std::uint32_t>(source.mMasterTableEnd - source.mMasterTableBegin + 1));
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsOutOfRangeOverlappingAndOutOfOrderSections)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mGlobalData1OffsetField,
            static_cast<std::uint32_t>(source.mGlobalData1Begin + 1));
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mChangedFormsOffsetField,
            static_cast<std::uint32_t>(source.mGlobalData1Begin - 1));
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mGlobalData2OffsetField,
            static_cast<std::uint32_t>(source.mChangedFormsBegin - 1));
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mUnknownTableOffsetField,
            static_cast<std::uint32_t>(source.mRefIdArrayBegin - 1));
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mRefIdArrayOffsetField, std::numeric_limits<std::uint32_t>::max());
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptEntryCountsLengthsAndWidthCodes)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mGlobalData1CountField, 2);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mChangedFormsCountField, 2);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mGlobalData2CountField, std::numeric_limits<std::uint32_t>::max());
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mGlobalData1LengthField, std::numeric_limits<std::uint32_t>::max());
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormRawTypes[0]] = 0xc1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormLengthFields[0]] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(
            source.mBytes, source.mChangedFormLengthFields[2], std::numeric_limits<std::uint32_t>::max());
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, ResolvesPackedReferenceNamespacesAndRejectsInvalidOrMissingArrayTargets)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormReferenceIds[0]] = 0xc0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormReferenceIds[0] + 2] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mRefIdArrayBegin + 4, 0);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RequiresExactlyOneCanonicalPlayerActorReference)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mRefIdArrayBegin + 4, 0x15);
        const ESM4::FONVSaveGamePrefix missing = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        EXPECT_THROW(missing.requirePlayerReferenceChangeForm(), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormReferenceIds[1]] = 0;
        source.mBytes[source.mChangedFormReferenceIds[1] + 1] = 0;
        source.mBytes[source.mChangedFormReferenceIds[1] + 2] = 1;
        source.mBytes[source.mChangedFormRawTypes[1]] = 0x41;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, ParsesCanonicalGlobalVariablesAndRejectsCorruption)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes, ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes, ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
            ESM4::sFONVPlayerCharacterFinalStateBytes, std::nullopt, std::nullopt, {}, true);

        ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        ASSERT_TRUE(save.mGlobalVariables.has_value());
        const ESM4::FONVSaveGlobalVariablesState& globals = *save.mGlobalVariables;
        EXPECT_EQ(globals.mCount.mValue, 2u);
        ASSERT_EQ(globals.mVariables.size(), 2u);
        EXPECT_EQ(globals.mVariables[0].mVariable.mResolvedFormId, 0x35u);
        EXPECT_FLOAT_EQ(globals.mVariables[0].mValue.mValue, 42.5f);
        EXPECT_EQ(globals.mVariables[1].mVariable.mResolvedFormId, 0x36u);
        EXPECT_FLOAT_EQ(globals.mVariables[1].mValue.mValue, -7.25f);
        EXPECT_EQ(globals.mRange.mSize, 20u);

        const std::size_t payload = source.mGlobalData1Begin + 8;
        source.mBytes[payload + 2] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes, ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes, ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
            ESM4::sFONVPlayerCharacterFinalStateBytes, std::nullopt, std::nullopt, {}, true);
        const std::uint32_t nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
        overwriteU32(source.mBytes, source.mGlobalData1Begin + 8 + 2 + 4, nan);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, ParsesFlaggedCanonicalPlayerReferenceScale)
    {
        const SaveBytes source = makeSaveWithPlayerProcessInventory(1.25f);
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);

        EXPECT_NE(save.requirePlayerReferenceChangeForm().mChangeFlags.mValue & 0x00000010u, 0u);
        ASSERT_TRUE(save.mPlayerProcessInventoryData.has_value());
        const auto& processInventory = *save.mPlayerProcessInventoryData;
        ASSERT_TRUE(processInventory.mReferenceScale.has_value());
        EXPECT_FLOAT_EQ(processInventory.mReferenceScale->mValue, 1.25f);
        EXPECT_EQ(processInventory.mReferenceScale->mRange,
            (ESM4::FONVSaveRange{ source.mPlayerProcessInventoryBegin + 2, sizeof(float) }));
        EXPECT_EQ(processInventory.mReferenceScale->mRaw,
            (std::vector<std::uint8_t>{ 0, 0, 0xa0, 0x3f }));
        EXPECT_EQ(processInventory.mActorExtraDataCount.mRange,
            (ESM4::FONVSaveRange{ source.mPlayerProcessInventoryBegin + 7, 1 }));
        ASSERT_EQ(processInventory.mActorExtraData.size(), 2u);
        EXPECT_EQ(processInventory.mInventoryEntries.size(), 3u);
        EXPECT_EQ(processInventory.mRange.mSize, sSyntheticPlayerProcessInventoryDataBytes + 5u);
    }

    TEST(FONVSaveGame, RejectsInvalidFlaggedCanonicalPlayerReferenceScale)
    {
        for (const std::uint32_t invalidBits :
            { 0u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u })
        {
            SaveBytes source = makeSaveWithPlayerProcessInventory(1.f);
            overwriteU32(source.mBytes, source.mPlayerProcessInventoryBegin + 2, invalidBits);
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError)
                << "accepted invalid player reference-scale bits 0x" << std::hex << invalidBits;
        }
    }

    TEST(FONVSaveGame, ParsesAllSave213CanonicalPlayerActorExtraDataLayouts)
    {
        const std::vector<std::uint8_t> actorExtraData = makeSave213PlayerActorExtraData();
        const SaveBytes source = makeSaveWithPlayerProcessInventory(1.f, 6u, actorExtraData);
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(source.mBytes);

        ASSERT_TRUE(save.mPlayerProcessInventoryData.has_value());
        const auto& processInventory = *save.mPlayerProcessInventoryData;
        ASSERT_TRUE(processInventory.mReferenceScale.has_value());
        EXPECT_FLOAT_EQ(processInventory.mReferenceScale->mValue, 1.f);
        EXPECT_EQ(processInventory.mActorExtraDataCount.mValue, 6u);
        ASSERT_EQ(processInventory.mActorExtraData.size(), 6u);
        EXPECT_EQ(processInventory.mRange.mSize, 157u);
        EXPECT_EQ(processInventory.mInventoryEntries.size(), 3u);

        const std::size_t actorExtrasBegin = source.mPlayerProcessInventoryBegin + 9;
        const auto& packageStart = processInventory.mActorExtraData[0];
        EXPECT_EQ(packageStart.mType.mValue, ESM4::sFONVExtraPackageStartLocationType);
        EXPECT_EQ(packageStart.mRange, (ESM4::FONVSaveRange{ actorExtrasBegin, 24 }));
        EXPECT_EQ(packageStart.mRaw,
            std::vector<std::uint8_t>(actorExtraData.begin(), actorExtraData.begin() + 24));
        ASSERT_TRUE(packageStart.mPackageStartCellOrWorldspace.has_value());
        EXPECT_EQ(packageStart.mPackageStartCellOrWorldspace->mResolvedFormId, 0x00000111u);
        ASSERT_TRUE(packageStart.mPackageStartPosition.has_value());
        EXPECT_FLOAT_EQ((*packageStart.mPackageStartPosition)[0].mValue, -16259.322f);
        EXPECT_FLOAT_EQ((*packageStart.mPackageStartPosition)[1].mValue, -5998.828f);
        EXPECT_FLOAT_EQ((*packageStart.mPackageStartPosition)[2].mValue, 6393.356f);
        EXPECT_EQ((*packageStart.mPackageStartPosition)[0].mRange,
            (ESM4::FONVSaveRange{ actorExtrasBegin + 6, sizeof(float) }));
        EXPECT_EQ((*packageStart.mPackageStartPosition)[1].mRange,
            (ESM4::FONVSaveRange{ actorExtrasBegin + 10, sizeof(float) }));
        EXPECT_EQ((*packageStart.mPackageStartPosition)[2].mRange,
            (ESM4::FONVSaveRange{ actorExtrasBegin + 14, sizeof(float) }));
        ASSERT_TRUE(packageStart.mPackageStartUnknown.has_value());
        EXPECT_EQ(packageStart.mPackageStartUnknown->mValue, 0x12345678u);

        const auto& followers = processInventory.mActorExtraData[1];
        EXPECT_EQ(followers.mType.mValue, ESM4::sFONVExtraFollowerArrayType);
        EXPECT_EQ(followers.mRange, (ESM4::FONVSaveRange{ actorExtrasBegin + 24, 12 }));
        ASSERT_TRUE(followers.mFollowerCount.has_value());
        EXPECT_EQ(followers.mFollowerCount->mValue, 2u);
        ASSERT_EQ(followers.mFollowers.size(), 2u);
        EXPECT_EQ(followers.mFollowers[0].mResolvedFormId, 0x00000112u);
        EXPECT_EQ(followers.mFollowers[1].mResolvedFormId, 0xff000113u);

        const auto& factions = processInventory.mActorExtraData[2];
        EXPECT_EQ(factions.mType.mValue, ESM4::sFONVExtraFactionChangesType);
        EXPECT_EQ(factions.mRange, (ESM4::FONVSaveRange{ actorExtrasBegin + 36, 10 }));
        ASSERT_TRUE(factions.mFactionChangeCount.has_value());
        EXPECT_EQ(factions.mFactionChangeCount->mValue, 1u);
        ASSERT_EQ(factions.mFactionChanges.size(), 1u);
        EXPECT_EQ(factions.mFactionChanges[0].mFaction.mResolvedFormId, 0x00000114u);
        EXPECT_EQ(factions.mFactionChanges[0].mRank.mValue, -2);

        const auto& actorCause = processInventory.mActorExtraData[3];
        EXPECT_EQ(actorCause.mType.mValue, ESM4::sFONVExtraActorCauseType);
        EXPECT_EQ(actorCause.mRange, (ESM4::FONVSaveRange{ actorExtrasBegin + 46, 7 }));
        ASSERT_TRUE(actorCause.mActorCause.has_value());
        EXPECT_EQ(actorCause.mActorCause->mValue, 0xaabbccddu);

        const auto& encounterZone = processInventory.mActorExtraData[4];
        EXPECT_EQ(encounterZone.mType.mValue, ESM4::sFONVExtraEncounterZoneType);
        EXPECT_EQ(encounterZone.mRange, (ESM4::FONVSaveRange{ actorExtrasBegin + 53, 6 }));
        ASSERT_TRUE(encounterZone.mEncounterZone.has_value());
        EXPECT_EQ(encounterZone.mEncounterZone->mResolvedFormId, 0x00000115u);

        const auto& sayTo = processInventory.mActorExtraData[5];
        EXPECT_EQ(sayTo.mType.mValue, ESM4::sFONVExtraSayToTopicInfoType);
        EXPECT_EQ(sayTo.mRange, (ESM4::FONVSaveRange{ actorExtrasBegin + 59, 12 }));
        ASSERT_TRUE(sayTo.mSayToTopic.has_value());
        EXPECT_EQ(sayTo.mSayToTopic->mResolvedFormId, 0x00000116u);
        ASSERT_TRUE(sayTo.mSayToTopicInfo.has_value());
        EXPECT_EQ(sayTo.mSayToTopicInfo->mResolvedFormId, 0xff000117u);
        ASSERT_TRUE(sayTo.mSayToUnknown.has_value());
        EXPECT_EQ(sayTo.mSayToUnknown->mValue, 0x42u);
    }

    TEST(FONVSaveGame, RejectsNonFiniteSave213PackageStartPosition)
    {
        const std::vector<std::uint8_t> actorExtraData = makeSave213PlayerActorExtraData();
        SaveBytes source = makeSaveWithPlayerProcessInventory(1.f, 6u, actorExtraData);
        const std::size_t actorExtrasBegin = source.mPlayerProcessInventoryBegin + 9;
        overwriteU32(source.mBytes, actorExtrasBegin + 10, 0x7fc00000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerMovementBeforeActorValues)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormPayloads[0] + 27] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mChangedFormPayloads[0] + 3, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormPayloads[0] + 2] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mChangedFormChangeFlags[0], 0xb0000028u);
        const ESM4::FONVSaveGamePrefix changedCell = ESM4::parseFONVSaveGamePrefix(source.mBytes);
        EXPECT_FALSE(changedCell.mPlayerReferenceMovement.has_value());
        EXPECT_FALSE(changedCell.mPlayerActorValueData.has_value());
        EXPECT_FALSE(changedCell.mPlayerProcessInventoryData.has_value());
        EXPECT_FALSE(changedCell.mPlayerMobileObjectProcessState.has_value());
        EXPECT_FALSE(changedCell.mPlayerChangedCharacterState.has_value());
        EXPECT_FALSE(changedCell.mPlayerCharacterAnimationState.has_value());
        EXPECT_FALSE(changedCell.mPlayerCharacterScalarReferenceState.has_value());
        EXPECT_FALSE(changedCell.mPlayerCharacterListsState.has_value());
        EXPECT_FALSE(changedCell.mPlayerCharacterMagicTargetState.has_value());
        EXPECT_FALSE(changedCell.mPlayerCharacterFinalState.has_value());
        EXPECT_EQ(changedCell.mUnparsedSemanticPayloadRanges.size(), 5u);
        EXPECT_EQ(changedCell.mUnparsedSemanticPayloadBytes,
            43u + static_cast<std::uint64_t>(ESM4::sFONVPlayerActorValueDataBytes)
                + static_cast<std::uint64_t>(sSyntheticPlayerProcessInventoryDataBytes)
                + static_cast<std::uint64_t>(sSyntheticPlayerMobileObjectProcessStateBytes)
                + static_cast<std::uint64_t>(ESM4::sFONVPlayerChangedCharacterStateBytes)
                + static_cast<std::uint64_t>(sSyntheticPlayerCharacterAnimationStateBytes)
                + static_cast<std::uint64_t>(ESM4::sFONVPlayerCharacterScalarReferenceStateBytes)
                + static_cast<std::uint64_t>(ESM4::sFONVPlayerCharacterListsStateBytes)
                + static_cast<std::uint64_t>(ESM4::sFONVPlayerCharacterMagicTargetStateBytes)
                + static_cast<std::uint64_t>(ESM4::sFONVPlayerCharacterFinalStateBytes));
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerActorValueData)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        const std::size_t actorValuesBegin = source.mChangedFormPayloads[0] + 28;
        source.mBytes[actorValuesBegin + 4] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mChangedFormPayloads[0] + 28 + 385, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormPayloads[0] + 28 + ESM4::sFONVPlayerActorValueDataBytes - 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes - 1, 0);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormRawTypes[0] + 1] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerProcessInventoryData)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + 2] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + 4] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + 8] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + 20] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + 35] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerProcessInventoryBegin + 37, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin + sSyntheticPlayerProcessInventoryDataBytes - 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, 0);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerMobileObjectProcessState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerProcessInventoryBegin] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 30] = 0xc0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 53] = 0x40;
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 54] = 0;
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 55] = 0x14;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerMobileObjectProcessStateBegin + 148, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 292] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 294] = 4;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerMobileObjectProcessStateBegin + 491, 12);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 631] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 633] = 4;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 635] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 637] = 0xc0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 637] = 0;
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 638] = 0;
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 639] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerMobileObjectProcessStateBegin + 648, 0x7fc00000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 681] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerMobileObjectProcessStateBegin + 683] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes, 0);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerChangedCharacterState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerChangedCharacterStateBegin, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerChangedCharacterStateBegin + 4] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerChangedCharacterStateBegin + 101] = 0xc0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mChangedFormChangeFlags[0], 0xb0000422u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerChangedCharacterStateBegin + 234] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerChangedCharacterStateBegin + 235] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerChangedCharacterStateBegin + 236, 0x7fc00000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerChangedCharacterStateBegin + 476] = 4;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerChangedCharacterStateBegin + 478, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerChangedCharacterStateBegin + 509] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, 0);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerCharacterAnimationState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterAnimationStateBegin] = 0x46;
        source.mBytes[source.mPlayerCharacterAnimationStateBegin + 1] = 0x02;
        source.mBytes[source.mPlayerCharacterAnimationStateBegin + 2] = 0;
        source.mBytes[source.mPlayerCharacterAnimationStateBegin + 3] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterAnimationStateBegin + 2] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterAnimationStateBegin] = 0x49;
        source.mBytes[source.mPlayerCharacterAnimationStateBegin + 1] = 0x02;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes - 1);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerCharacterScalarReferenceState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerCharacterScalarReferenceStateBegin + 62, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerCharacterScalarReferenceStateBegin + 97, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerCharacterScalarReferenceStateBegin + 102, 0x7fc00000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerCharacterScalarReferenceStateBegin + 154, 0xff800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 243] = 0xc0;
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 244] = 0;
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 245] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 243] = 0;
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 244] = 0;
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 245] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterScalarReferenceStateBegin + 246] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormRawTypes[0] + 1] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes,
            ESM4::sFONVPlayerCharacterScalarReferenceStateBytes - 1);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerCharacterListsState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 2] = 0xc0;
        source.mBytes[source.mPlayerCharacterListsStateBegin + 3] = 0;
        source.mBytes[source.mPlayerCharacterListsStateBegin + 4] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 2] = 0;
        source.mBytes[source.mPlayerCharacterListsStateBegin + 3] = 0;
        source.mBytes[source.mPlayerCharacterListsStateBegin + 4] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 5] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 27] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mPlayerCharacterListsStateBegin + 65, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 88] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterListsStateBegin + 141] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormRawTypes[0] + 1] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes,
            ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes - 1);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerCharacterMagicTargetState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 1] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 2] = 0xc0;
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 3] = 0;
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 4] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 2] = 0;
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 3] = 0;
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 4] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 5] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 7] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 8] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 9] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 10] = 0xbc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterMagicTargetStateBegin + 11] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormRawTypes[0] + 1] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes,
            ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes,
            ESM4::sFONVPlayerCharacterMagicTargetStateBytes - 1);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerCharacterFinalState)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        constexpr std::array<std::size_t, 19> delimiterOffsets
            = { 4, 9, 14, 16, 18, 20, 22, 27, 32, 37, 42, 47, 52, 57, 62, 64, 66, 68, 70 };
        for (const std::size_t delimiterOffset : delimiterOffsets)
        {
            SaveBytes source = makeSave(true, 2, 1, masters);
            source.mBytes[source.mPlayerCharacterFinalStateBegin + delimiterOffset] = 0;
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
        }

        for (const std::size_t floatOffset : { 5u, 10u })
        {
            SaveBytes source = makeSave(true, 2, 1, masters);
            overwriteU32(source.mBytes, source.mPlayerCharacterFinalStateBegin + floatOffset, 0x7f800000u);
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
        }

        constexpr std::array<std::size_t, 5> countOffsets = { 15, 63, 65, 67, 69 };
        for (const std::size_t countOffset : countOffsets)
        {
            SaveBytes source = makeSave(true, 2, 1, masters);
            source.mBytes[source.mPlayerCharacterFinalStateBegin + countOffset] = 3;
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

            source = makeSave(true, 2, 1, masters);
            source.mBytes[source.mPlayerCharacterFinalStateBegin + countOffset] = 0xfc;
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
        }

        SaveBytes source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 63] = 4;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 65] = 0xc0;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 66] = 0;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 67] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 63] = 4;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 65] = 0;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 66] = 0;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 67] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 63] = 4;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 65] = 0x40;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 66] = 0x01;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 67] = 0x23;
        source.mBytes[source.mPlayerCharacterFinalStateBegin + 68] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        source.mBytes[source.mChangedFormRawTypes[0] + 1] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes,
            ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes,
            ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
            ESM4::sFONVPlayerCharacterFinalStateBytes - 1);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters, true, "Courier", false,
            ESM4::sFONVPlayerActorValueDataBytes, sSyntheticPlayerProcessInventoryDataBytes,
            sSyntheticPlayerMobileObjectProcessStateBytes, ESM4::sFONVPlayerChangedCharacterStateBytes,
            sSyntheticPlayerCharacterAnimationStateBytes,
            ESM4::sFONVPlayerCharacterScalarReferenceStateBytes,
            ESM4::sFONVPlayerCharacterListsStateBytes,
            ESM4::sFONVPlayerCharacterMagicTargetStateBytes,
            ESM4::sFONVPlayerCharacterFinalStateBytes + 1);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsUnaccountedCountedTailBytes)
    {
        constexpr std::array masters = { std::string_view("FalloutNV.esm") };
        SaveBytes source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mRefIdArrayBegin, std::numeric_limits<std::uint32_t>::max());
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mRefIdArrayBegin + 12, std::numeric_limits<std::uint32_t>::max());
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        overwriteU32(source.mBytes, source.mUnknownTableBegin, 4);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsUnreasonableDimensionsAndCounts)
    {
        SaveBytes source = makeSave(true, 8193, 1, {}, false);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        source = makeSave(
            false, std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), {}, false);
        ESM4::FONVSaveLimits limits;
        limits.mMaxScreenshotDimension = std::numeric_limits<std::uint32_t>::max();
        limits.mMaxScreenshotBytes = std::numeric_limits<std::uint64_t>::max();
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes, {}, limits), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, {});
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes), ESM4::FONVSaveError);

        constexpr std::array masters = { std::string_view("FalloutNV.esm"), std::string_view("DeadMoney.esm"),
            std::string_view("HonestHearts.esm") };
        source = makeSave(true, 2, 1, masters);
        limits = {};
        limits.mMaxMasterCount = 2;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes, {}, limits), ESM4::FONVSaveError);

        source = makeSave(true, 2, 1, masters);
        limits = {};
        limits.mMaxMasterNameBytes = 8;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(source.mBytes, {}, limits), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, ParsesExternalSave330WhenFixtureIsProvided)
    {
        const char* fixture = std::getenv("OPENMW_FNV_SAVE330_FIXTURE");
        if (fixture == nullptr || *fixture == '\0')
            GTEST_SKIP() << "Set OPENMW_FNV_SAVE330_FIXTURE to the read-only retail Save 330 path";

        const std::filesystem::path sourcePath = std::filesystem::u8path(fixture);
        const std::vector<std::uint8_t> fixtureBytes = readFixtureBytes(sourcePath);
        ASSERT_EQ(sha256Hex(fixtureBytes), "07dbdd2d7c4abe3160628e5463a9603a40f4271042c1da1b89f1c4a4f7dbd81f")
            << "OPENMW_FNV_SAVE330_FIXTURE is not the pinned Save330 evidence file";
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(fixtureBytes, sourcePath);

        EXPECT_EQ(save.mSourcePath, sourcePath);
        EXPECT_EQ(save.mFileSize, 3395328u);
        EXPECT_EQ(save.mHeaderSize.mValue, 132u);
        EXPECT_EQ(save.mHeaderRange, (ESM4::FONVSaveRange{ 15, 132 }));
        ASSERT_TRUE(save.mHeader.mLanguage.has_value());
        EXPECT_EQ(save.mHeader.mLanguage->mValue, "ENGLISH");
        EXPECT_EQ(save.mHeader.mVersion.mValue, 48u);
        EXPECT_EQ(save.mHeader.mScreenshotWidth.mValue, 512u);
        EXPECT_EQ(save.mHeader.mScreenshotHeight.mValue, 320u);
        EXPECT_EQ(save.mHeader.mSaveNumber.mValue, 330u);
        EXPECT_EQ(save.mHeader.mPlayerName.mValue, "");
        EXPECT_EQ(save.mHeader.mPlayerKarmaTitle.mValue, "Drifter");
        EXPECT_EQ(save.mHeader.mPlayerLevel.mValue, 1u);
        EXPECT_EQ(save.mHeader.mPlayerLocation.mValue, "Goodsprings");
        EXPECT_EQ(save.mHeader.mPlayTime.mValue, "000.16.45");
        EXPECT_EQ(save.mScreenshotRange, (ESM4::FONVSaveRange{ 147, 491520 }));
        EXPECT_EQ(save.mFormVersion.mValue, 27u);
        EXPECT_EQ(save.mMasterTableSize.mValue, 199u);
        EXPECT_EQ(save.mMasterTableRange, (ESM4::FONVSaveRange{ 491672, 199 }));
        ASSERT_EQ(save.mMasters.size(), 10u);
        constexpr std::array expectedMasters = { std::string_view("FalloutNV.esm"), std::string_view("DeadMoney.esm"),
            std::string_view("HonestHearts.esm"), std::string_view("OldWorldBlues.esm"),
            std::string_view("LonesomeRoad.esm"), std::string_view("TribalPack.esm"),
            std::string_view("MercenaryPack.esm"), std::string_view("ClassicPack.esm"),
            std::string_view("CaravanPack.esm"), std::string_view("GunRunnersArsenal.esm") };
        for (std::size_t i = 0; i < expectedMasters.size(); ++i)
            EXPECT_EQ(save.mMasters[i].mFileName.mValue, expectedMasters[i]);
        EXPECT_EQ(save.mMasters.front().mFileName.mValueRange, (ESM4::FONVSaveRange{ 491677, 13 }));
        EXPECT_EQ(save.mHeaderAndMastersRange, (ESM4::FONVSaveRange{ 0, 491871 }));

        EXPECT_EQ(save.mFileLocationTable.mRange, (ESM4::FONVSaveRange{ 491871, 110 }));
        EXPECT_EQ(save.mFileLocationTable.mRefIdArrayCountOffset.mValue, 3308287u);
        EXPECT_EQ(save.mFileLocationTable.mUnknownTableOffset.mValue, 3395319u);
        EXPECT_EQ(save.mFileLocationTable.mGlobalDataTable1Offset.mValue, 491981u);
        EXPECT_EQ(save.mFileLocationTable.mChangedFormsOffset.mValue, 494730u);
        EXPECT_EQ(save.mFileLocationTable.mGlobalDataTable2Offset.mValue, 3308279u);
        EXPECT_EQ(save.mFileLocationTable.mGlobalDataTable1Count.mValue, 12u);
        EXPECT_EQ(save.mFileLocationTable.mGlobalDataTable2Count.mValue, 1u);
        EXPECT_EQ(save.mFileLocationTable.mChangedFormsCount.mValue, 7093u);
        EXPECT_EQ(save.mFileLocationTable.mUnknownCount.mValue, 0u);
        EXPECT_EQ(save.mFileLocationTable.mUnused.mRange, (ESM4::FONVSaveRange{ 491907, 74 }));
        EXPECT_EQ(save.mFileLocationTable.mUnused.mRaw, std::vector<std::uint8_t>(74, 0));

        EXPECT_EQ(save.mGlobalDataTable1.mRange, (ESM4::FONVSaveRange{ 491981, 2749 }));
        ASSERT_EQ(save.mGlobalDataTable1.mEntries.size(), 12u);
        constexpr std::array<std::uint32_t, 12> global1Types = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        constexpr std::array<std::uint32_t, 12> global1Lengths
            = { 220, 57, 121, 1803, 30, 36, 33, 2, 71, 7, 269, 4 };
        constexpr std::array<std::uint64_t, 12> global1Offsets
            = { 491981, 492209, 492274, 492403, 494214, 494252, 494296, 494337, 494347, 494426, 494441, 494718 };
        for (std::size_t i = 0; i < global1Types.size(); ++i)
        {
            EXPECT_EQ(save.mGlobalDataTable1.mEntries[i].mType.mValue, global1Types[i]);
            EXPECT_EQ(save.mGlobalDataTable1.mEntries[i].mDataLength.mValue, global1Lengths[i]);
            EXPECT_EQ(save.mGlobalDataTable1.mEntries[i].mEnvelopeRange.mOffset, global1Offsets[i]);
            EXPECT_EQ(save.mGlobalDataTable1.mEntries[i].mEnvelopeRange.mSize, global1Lengths[i] + 8u);
        }
        EXPECT_EQ(save.mGlobalDataTable1.mEntries.back().mEnvelopeRange.end(), 494730u);
        ASSERT_TRUE(save.mGlobalVariables.has_value());
        const ESM4::FONVSaveGlobalVariablesState& globals = *save.mGlobalVariables;
        EXPECT_EQ(globals.mCount.mValue, 200u);
        EXPECT_EQ(globals.mCount.mRange, (ESM4::FONVSaveRange{ 492411, 2 }));
        ASSERT_EQ(globals.mVariables.size(), 200u);
        EXPECT_EQ(globals.mRange, (ESM4::FONVSaveRange{ 492411, 1803 }));
        EXPECT_EQ(globals.mVariables.front().mVariable.mEncoded.mValue, 0x00014cu);
        EXPECT_EQ(globals.mVariables.front().mVariable.mResolvedFormId, 0x04003608u);
        EXPECT_FLOAT_EQ(globals.mVariables.front().mValue.mValue, 0.f);
        EXPECT_EQ(globals.mVariables[50].mVariable.mResolvedFormId, 0x00068e75u);
        EXPECT_FLOAT_EQ(globals.mVariables[50].mValue.mValue, 100.f);
        EXPECT_EQ(globals.mVariables.back().mVariable.mResolvedFormId, 0x00000035u);
        EXPECT_FLOAT_EQ(globals.mVariables.back().mValue.mValue, 2277.f);
        ASSERT_TRUE(save.mSky.has_value());
        const ESM4::FONVSaveSkyState& sky = *save.mSky;
        EXPECT_EQ(sky.mRange, (ESM4::FONVSaveRange{ 494355, 71 }));
        EXPECT_EQ(sha256Hex(sky.mRaw), "8492cf418cdf6f301a51d504b6a0b0df842282e77bae5607245d3fee04a71468");
        EXPECT_EQ(sky.mCurrentWeather.mEncoded.mRange, (ESM4::FONVSaveRange{ 494355, 3 }));
        EXPECT_EQ(sky.mCurrentWeather.mResolvedFormId, 0x001237d7u);
        EXPECT_FALSE(sky.mTransitionWeather.mResolvedFormId.has_value());
        EXPECT_EQ(sky.mDefaultWeather.mResolvedFormId, 0x000ffc88u);
        EXPECT_FALSE(sky.mOverrideWeather.mResolvedFormId.has_value());
        EXPECT_EQ(sky.mGameHour.mRange, (ESM4::FONVSaveRange{ 494371, 4 }));
        EXPECT_FLOAT_EQ(sky.mGameHour.mValue, 14.215002059936523f);
        EXPECT_FLOAT_EQ(sky.mLastUpdateHour.mValue, 17.606250762939453f);
        EXPECT_FLOAT_EQ(sky.mWeatherPercent.mValue, 1.f);
        EXPECT_EQ(sky.mFlags.mValue, 0x20u);
        EXPECT_FLOAT_EQ(sky.mFogPower.mValue, 0.5f);
        EXPECT_EQ(sky.mSkyMode.mValue, 3u);

        EXPECT_EQ(save.mChangedForms.mRange, (ESM4::FONVSaveRange{ 494730, 2813549 }));
        ASSERT_EQ(save.mChangedForms.mEntries.size(), 7093u);
        const auto& first = save.mChangedForms.mEntries.front();
        EXPECT_EQ(first.mEnvelopeRange, (ESM4::FONVSaveRange{ 494730, 308 }));
        EXPECT_EQ(first.mReferenceId.mRaw, std::vector<std::uint8_t>({ 0x00, 0x00, 0x00 }));
        EXPECT_EQ(first.mEncodedReferenceId.mValue, 0u);
        EXPECT_EQ(first.mReferenceKind, ESM4::FONVSaveReferenceKind::FormIdArray);
        EXPECT_FALSE(first.mResolvedFormId.has_value());
        EXPECT_EQ(first.mChangeFlags.mValue, 0x00040000u);
        EXPECT_EQ(first.mRawType.mValue, 0x41u);
        EXPECT_EQ(first.mChangeType, 1u);
        EXPECT_EQ(first.mVersion.mValue, 27u);
        EXPECT_EQ(first.mLengthWidth, 2u);
        EXPECT_EQ(first.mDataLength.mValue, 297u);
        EXPECT_EQ(first.mUnparsedPayload.mRange, (ESM4::FONVSaveRange{ 494741, 297 }));

        const auto& second = save.mChangedForms.mEntries[1];
        EXPECT_EQ(second.mEnvelopeRange, (ESM4::FONVSaveRange{ 495038, 536 }));
        EXPECT_EQ(second.mReferenceId.mRaw, std::vector<std::uint8_t>({ 0x00, 0x3a, 0x34 }));
        EXPECT_EQ(second.mDataLength.mValue, 525u);
        const auto& third = save.mChangedForms.mEntries[2];
        EXPECT_EQ(third.mEnvelopeRange, (ESM4::FONVSaveRange{ 495574, 536 }));
        EXPECT_EQ(third.mReferenceId.mRaw, std::vector<std::uint8_t>({ 0x00, 0x3a, 0x45 }));

        std::size_t oneByteLengths = 0;
        std::size_t twoByteLengths = 0;
        std::size_t fourByteLengths = 0;
        std::uint32_t maximumPayloadLength = 0;
        std::size_t maximumPayloadIndex = 0;
        std::uint64_t expectedChangedFormOffset = save.mChangedForms.mRange.mOffset;
        for (std::size_t i = 0; i < save.mChangedForms.mEntries.size(); ++i)
        {
            const auto& entry = save.mChangedForms.mEntries[i];
            EXPECT_EQ(entry.mEnvelopeRange.mOffset, expectedChangedFormOffset);
            EXPECT_EQ(entry.mHeaderRange.mOffset, entry.mEnvelopeRange.mOffset);
            EXPECT_EQ(entry.mUnparsedPayload.mRange.mOffset, entry.mHeaderRange.end());
            EXPECT_EQ(entry.mEnvelopeRange.end(), entry.mUnparsedPayload.mRange.end());
            EXPECT_EQ(entry.mUnparsedPayload.mRaw.size(), entry.mUnparsedPayload.mRange.mSize);
            expectedChangedFormOffset = entry.mEnvelopeRange.end();
            oneByteLengths += entry.mLengthWidth == 1;
            twoByteLengths += entry.mLengthWidth == 2;
            fourByteLengths += entry.mLengthWidth == 4;
            if (entry.mDataLength.mValue > maximumPayloadLength)
            {
                maximumPayloadLength = entry.mDataLength.mValue;
                maximumPayloadIndex = i;
            }
        }
        EXPECT_EQ(expectedChangedFormOffset, save.mChangedForms.mRange.end());
        EXPECT_EQ(oneByteLengths, 862u);
        EXPECT_EQ(twoByteLengths, 6231u);
        EXPECT_EQ(fourByteLengths, 0u);
        EXPECT_EQ(maximumPayloadLength, 5830u);
        EXPECT_EQ(maximumPayloadIndex, 2854u);

        const auto& beforeLast = save.mChangedForms.mEntries[7091];
        EXPECT_EQ(beforeLast.mEnvelopeRange, (ESM4::FONVSaveRange{ 3307761, 502 }));
        EXPECT_EQ(beforeLast.mReferenceId.mRaw, std::vector<std::uint8_t>({ 0x00, 0x45, 0xf9 }));
        EXPECT_EQ(beforeLast.mChangeFlags.mValue, 0x80000802u);
        EXPECT_EQ(beforeLast.mDataLength.mValue, 491u);
        const auto& last = save.mChangedForms.mEntries.back();
        EXPECT_EQ(last.mEnvelopeRange, (ESM4::FONVSaveRange{ 3308263, 16 }));
        EXPECT_EQ(last.mReferenceId.mRaw, std::vector<std::uint8_t>({ 0x00, 0x2e, 0x50 }));
        EXPECT_EQ(last.mChangeFlags.mValue, 0x40000000u);
        EXPECT_EQ(last.mRawType.mValue, 0x09u);
        EXPECT_EQ(last.mChangeType, 9u);
        EXPECT_EQ(last.mVersion.mValue, 27u);
        EXPECT_EQ(last.mLengthWidth, 1u);
        EXPECT_EQ(last.mDataLength.mValue, 6u);
        EXPECT_EQ(last.mUnparsedPayload.mRange, (ESM4::FONVSaveRange{ 3308273, 6 }));
        EXPECT_EQ(last.mEnvelopeRange.end(), 3308279u);

        EXPECT_EQ(save.mGlobalDataTable2.mRange, (ESM4::FONVSaveRange{ 3308279, 8 }));
        ASSERT_EQ(save.mGlobalDataTable2.mEntries.size(), 1u);
        EXPECT_EQ(save.mGlobalDataTable2.mEntries[0].mType.mValue, 1000u);
        EXPECT_EQ(save.mGlobalDataTable2.mEntries[0].mDataLength.mValue, 0u);

        EXPECT_EQ(save.mFormIdTable.mRange, (ESM4::FONVSaveRange{ 3308287, 87024 }));
        EXPECT_EQ(save.mFormIdTable.mCount.mValue, 21755u);
        ASSERT_EQ(save.mFormIdTable.mFormIds.size(), 21755u);
        EXPECT_EQ(save.mFormIdTable.mFormIds[0].mValue, 0x00031e12u);
        EXPECT_EQ(save.mFormIdTable.mFormIds[582].mValue, 0x000da726u);
        EXPECT_EQ(save.mFormIdTable.mFormIds[582].mRange, (ESM4::FONVSaveRange{ 3310619, 4 }));
        EXPECT_EQ(save.mFormIdTable.mFormIds[622].mValue, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(save.mFormIdTable.mFormIds[622].mRange, (ESM4::FONVSaveRange{ 3310779, 4 }));
        EXPECT_EQ(save.mVisitedWorldspaces.mRange, (ESM4::FONVSaveRange{ 3395311, 8 }));
        EXPECT_EQ(save.mVisitedWorldspaces.mCount.mValue, 1u);
        ASSERT_EQ(save.mVisitedWorldspaces.mFormIds.size(), 1u);
        EXPECT_EQ(save.mVisitedWorldspaces.mFormIds[0].mValue, 0x000da726u);
        EXPECT_EQ(save.mUnknownTable.mRange, (ESM4::FONVSaveRange{ 3395319, 9 }));
        EXPECT_EQ(save.mUnknownTable.mCount.mValue, 5u);
        EXPECT_EQ(save.mUnknownTable.mUnparsedEntries.mRange, (ESM4::FONVSaveRange{ 3395323, 5 }));
        EXPECT_EQ(save.mUnknownTable.mUnparsedEntries.mRaw,
            std::vector<std::uint8_t>({ 0, 0, 0, 0, sDelimiter }));

        const auto& player = save.requirePlayerReferenceChangeForm();
        EXPECT_EQ(&player, &save.mChangedForms.mEntries[6]);
        EXPECT_EQ(player.mEnvelopeRange, (ESM4::FONVSaveRange{ 497478, 5106 }));
        EXPECT_EQ(player.mEncodedReferenceId.mValue, 0x00026fu);
        EXPECT_EQ(player.mReferencePayload, 623u);
        EXPECT_EQ(player.mResolvedFormId, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(player.mChangeType, ESM4::sFONVActorReferenceChangeType);
        EXPECT_EQ(player.mChangeFlags.mValue, 0xb0000022u);
        EXPECT_EQ(player.mUnparsedPayload.mRange, (ESM4::FONVSaveRange{ 497489, 5095 }));
        ASSERT_TRUE(save.mPlayerReferenceMovement.has_value());
        const auto& movement = *save.mPlayerReferenceMovement;
        EXPECT_EQ(movement.mRange, (ESM4::FONVSaveRange{ 497489, 28 }));
        EXPECT_EQ(movement.mCellOrWorldspace.mEncoded.mValue, 0x000247u);
        EXPECT_EQ(movement.mCellOrWorldspace.mKind, ESM4::FONVSaveReferenceKind::FormIdArray);
        EXPECT_EQ(movement.mCellOrWorldspace.mPayload, 583u);
        EXPECT_EQ(movement.mCellOrWorldspace.mResolvedFormId, 0x000da726u);
        EXPECT_EQ(movement.mCellOrWorldspace.mEncoded.mRange, (ESM4::FONVSaveRange{ 497489, 3 }));
        EXPECT_FLOAT_EQ(movement.mPosition[0].mValue, -72392.84375f);
        EXPECT_FLOAT_EQ(movement.mPosition[1].mValue, -1240.19275f);
        EXPECT_FLOAT_EQ(movement.mPosition[2].mValue, 8137.58643f);
        EXPECT_EQ(movement.mPosition[0].mRange, (ESM4::FONVSaveRange{ 497492, 4 }));
        EXPECT_FLOAT_EQ(movement.mRotationRadians[0].mValue, -0.06439045f);
        EXPECT_FLOAT_EQ(movement.mRotationRadians[1].mValue, 0.f);
        EXPECT_FLOAT_EQ(movement.mRotationRadians[2].mValue, 2.93332028f);
        EXPECT_EQ(movement.mRotationRadians[0].mRange, (ESM4::FONVSaveRange{ 497504, 4 }));
        EXPECT_EQ(movement.mTerminator.mValue, sDelimiter);
        EXPECT_EQ(movement.mTerminator.mRange, (ESM4::FONVSaveRange{ 497516, 1 }));
        EXPECT_EQ(movement.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 497517, 5067 }));
        ASSERT_TRUE(save.mPlayerActorValueData.has_value());
        const auto& actorValues = *save.mPlayerActorValueData;
        EXPECT_EQ(actorValues.mRange, (ESM4::FONVSaveRange{ 497517, 1160 }));
        EXPECT_EQ(actorValues.mRaw.size(), 1160u);
        EXPECT_EQ(
            sha256Hex(actorValues.mRaw), "077d9f619cc7af0fd47d00a92de8548c44f425cb94ce075db174f56521f79767");
        EXPECT_EQ(actorValues.mActorValues244.front().mRange, (ESM4::FONVSaveRange{ 497517, 4 }));
        EXPECT_EQ(actorValues.mActorValues244.back().mRange, (ESM4::FONVSaveRange{ 497897, 4 }));
        EXPECT_EQ(actorValues.mActorValues378.front().mRange, (ESM4::FONVSaveRange{ 497902, 4 }));
        EXPECT_EQ(actorValues.mActorValues4B0.front().mRange, (ESM4::FONVSaveRange{ 498287, 4 }));
        const auto nonzeroIndices = [](const auto& values) {
            std::vector<std::size_t> result;
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                if (values[i].mValue != 0.f)
                    result.push_back(i);
            }
            return result;
        };
        EXPECT_EQ(nonzeroIndices(actorValues.mActorValues244), (std::vector<std::size_t>{ 38, 43 }));
        EXPECT_EQ(nonzeroIndices(actorValues.mActorValues378), (std::vector<std::size_t>{ 24 }));
        EXPECT_TRUE(nonzeroIndices(actorValues.mActorValues4B0).empty());
        EXPECT_FLOAT_EQ(actorValues.mActorValues244[38].mValue, 2.f);
        EXPECT_EQ(actorValues.mActorValues244[38].mRange, (ESM4::FONVSaveRange{ 497707, 4 }));
        EXPECT_EQ(actorValues.mActorValues244[38].mRaw, (std::vector<std::uint8_t>{ 0, 0, 0, 0x40 }));
        EXPECT_FLOAT_EQ(actorValues.mActorValues244[43].mValue, 2.f);
        EXPECT_EQ(actorValues.mActorValues244[43].mRange, (ESM4::FONVSaveRange{ 497732, 4 }));
        EXPECT_FLOAT_EQ(actorValues.mActorValues378[24].mValue, 10.f);
        EXPECT_EQ(actorValues.mActorValues378[24].mRange, (ESM4::FONVSaveRange{ 498022, 4 }));
        EXPECT_EQ(actorValues.mActorValues378[24].mRaw, (std::vector<std::uint8_t>{ 0, 0, 0x20, 0x41 }));
        EXPECT_EQ(actorValues.mUnk4AC.mValue, 0u);
        EXPECT_EQ(actorValues.mUnk4AC.mRange, (ESM4::FONVSaveRange{ 498672, 4 }));
        EXPECT_EQ(actorValues.mUnk4AC.mRaw, (std::vector<std::uint8_t>{ 0, 0, 0, 0 }));
        EXPECT_EQ(actorValues.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 498677, 3907 }));
        EXPECT_EQ(actorValues.mUnparsedRemainder.mRaw.size(), 3907u);
        EXPECT_EQ(movement.mUnparsedRemainder.mRange.mSize - actorValues.mRange.mSize,
            actorValues.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerProcessInventoryData.has_value());
        const auto& processInventory = *save.mPlayerProcessInventoryData;
        EXPECT_EQ(processInventory.mRange, (ESM4::FONVSaveRange{ 498677, 835 }));
        EXPECT_EQ(processInventory.mRaw.size(), 835u);
        EXPECT_EQ(sha256Hex(processInventory.mRaw),
            "cd6574a59d2c4f30717f1368d42f9628f88f05f92a705c19ee709469ea060155");
        EXPECT_EQ(processInventory.mProcessLevel.mValue, 0);
        EXPECT_EQ(processInventory.mProcessLevel.mRange, (ESM4::FONVSaveRange{ 498677, 1 }));
        EXPECT_EQ(processInventory.mActorExtraDataCount.mValue, 2u);
        EXPECT_EQ(processInventory.mActorExtraDataCount.mRange, (ESM4::FONVSaveRange{ 498679, 1 }));
        EXPECT_EQ(processInventory.mActorExtraDataCount.mRaw, (std::vector<std::uint8_t>{ 0x08 }));
        ASSERT_EQ(processInventory.mActorExtraData.size(), 2u);

        const auto& factionExtra = processInventory.mActorExtraData[0];
        EXPECT_EQ(factionExtra.mRange, (ESM4::FONVSaveRange{ 498681, 22 }));
        EXPECT_EQ(factionExtra.mType.mValue, ESM4::sFONVExtraFactionChangesType);
        ASSERT_TRUE(factionExtra.mFactionChangeCount.has_value());
        EXPECT_EQ(factionExtra.mFactionChangeCount->mValue, 3u);
        EXPECT_EQ(factionExtra.mFactionChangeCount->mRaw, (std::vector<std::uint8_t>{ 0x0c }));
        ASSERT_EQ(factionExtra.mFactionChanges.size(), 3u);
        constexpr std::array factionTokens = { 0x004adeu, 0x004adfu, 0x004ae0u };
        constexpr std::array factionFormIds = { 0x0200b42eu, 0x04003e41u, 0x03016154u };
        for (std::size_t i = 0; i < factionExtra.mFactionChanges.size(); ++i)
        {
            const auto& faction = factionExtra.mFactionChanges[i];
            EXPECT_EQ(faction.mRange, (ESM4::FONVSaveRange{ 498685 + i * 6, 6 }));
            EXPECT_EQ(faction.mFaction.mEncoded.mValue, factionTokens[i]);
            EXPECT_EQ(faction.mFaction.mResolvedFormId, factionFormIds[i]);
            EXPECT_EQ(faction.mRank.mValue, 1);
        }

        const auto& encounterZoneExtra = processInventory.mActorExtraData[1];
        EXPECT_EQ(encounterZoneExtra.mRange, (ESM4::FONVSaveRange{ 498703, 6 }));
        EXPECT_EQ(encounterZoneExtra.mType.mValue, ESM4::sFONVExtraEncounterZoneType);
        ASSERT_TRUE(encounterZoneExtra.mEncounterZone.has_value());
        EXPECT_EQ(encounterZoneExtra.mEncounterZone->mEncoded.mValue, 0x000606u);
        EXPECT_EQ(encounterZoneExtra.mEncounterZone->mEncoded.mRange, (ESM4::FONVSaveRange{ 498705, 3 }));
        EXPECT_EQ(encounterZoneExtra.mEncounterZone->mResolvedFormId, 0x0000001eu);

        EXPECT_EQ(processInventory.mInventoryEntryCount.mValue, 50u);
        EXPECT_EQ(processInventory.mInventoryEntryCount.mRange, (ESM4::FONVSaveRange{ 498709, 1 }));
        EXPECT_EQ(processInventory.mInventoryEntryCount.mRaw, (std::vector<std::uint8_t>{ 0xc8 }));
        ASSERT_EQ(processInventory.mInventoryEntries.size(), 50u);
        constexpr std::array<std::uint32_t, 50> expectedItemFormIds = { 0x000340fdu, 0x000425bau,
            0x0001cbdcu, 0x00004345u, 0x0000434fu, 0x00022102u, 0x0013b2b1u, 0x0005b6d0u,
            0x000250a7u, 0x000250a3u, 0x000250a8u, 0x000250a6u, 0x0013b2b2u, 0x0002210eu,
            0x0013b2b3u, 0x000e2c6fu, 0x00032c74u, 0x00050f8fu, 0x00015165u, 0x00004241u,
            0x000151a3u, 0x000cb05cu, 0x00015169u, 0x0000000fu, 0x0000000au, 0x0000421cu,
            0x00004323u, 0x00028ff9u, 0x00025b83u, 0x00015038u, 0x0000431eu, 0x0002042eu,
            0x0002935bu, 0x00034040u, 0x001735d1u, 0x001735d2u, 0x001735d4u, 0x001735e0u,
            0x001735e3u, 0x000e86f2u, 0x00140a68u, 0x000e6346u, 0x001735e1u, 0x001735e4u,
            0x0007ea26u, 0x000ccef2u, 0x001735e2u, 0x0014d2acu, 0x001735e5u, 0x001613d0u };
        constexpr std::array<std::int32_t, 50> expectedDeltas = { 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1,
            2, 1, 1, 1, 1, 3, 1, 2, 250, 4, 5, 21, 300, 21, 1, 1, 1, 0, 0, 14, 1, 50, 1, 1, 1, 1,
            1, 1, 40, 4, 1, 1, 1, 20, 3, 1, 10, 1, 5 };
        std::uint64_t expectedInventoryOffset = 498711;
        for (std::size_t i = 0; i < processInventory.mInventoryEntries.size(); ++i)
        {
            const auto& entry = processInventory.mInventoryEntries[i];
            EXPECT_EQ(entry.mRange.mOffset, expectedInventoryOffset);
            EXPECT_EQ(entry.mType.mResolvedFormId, expectedItemFormIds[i]);
            EXPECT_EQ(entry.mDelta.mValue, expectedDeltas[i]);
            EXPECT_EQ(entry.mRaw.size(), entry.mRange.mSize);
            expectedInventoryOffset = entry.mRange.end();
        }
        EXPECT_EQ(expectedInventoryOffset, 499512u);

        std::size_t wornCount = 0;
        std::size_t hotkeyCount = 0;
        std::size_t ammoSelectionCount = 0;
        for (const auto& entry : processInventory.mInventoryEntries)
        {
            for (const auto& extendData : entry.mExtendData)
            {
                for (const auto& extra : extendData.mExtraData)
                {
                    wornCount += extra.mType.mValue == ESM4::sFONVExtraWornType;
                    hotkeyCount += extra.mType.mValue == ESM4::sFONVExtraHotkeyType;
                    ammoSelectionCount += extra.mType.mValue == ESM4::sFONVExtraAmmoType;
                }
            }
        }
        EXPECT_EQ(wornCount, 3u);
        EXPECT_EQ(hotkeyCount, 0u);
        EXPECT_EQ(ammoSelectionCount, 0u);
        ASSERT_EQ(processInventory.mInventoryEntries[28].mExtendData.size(), 1u);
        ASSERT_EQ(processInventory.mInventoryEntries[28].mExtendData[0].mExtraData.size(), 1u);
        EXPECT_EQ(processInventory.mInventoryEntries[28].mExtendData[0].mExtraData[0].mType.mValue,
            ESM4::sFONVExtraWornType);
        ASSERT_EQ(processInventory.mInventoryEntries[29].mExtendData.size(), 1u);
        ASSERT_EQ(processInventory.mInventoryEntries[29].mExtendData[0].mExtraData.size(), 1u);
        EXPECT_EQ(processInventory.mInventoryEntries[29].mExtendData[0].mExtraData[0].mType.mValue,
            ESM4::sFONVExtraWornType);

        const auto& jumpsuit = processInventory.mInventoryEntries[30];
        EXPECT_EQ(jumpsuit.mExtendDataCount.mValue, 6u);
        ASSERT_EQ(jumpsuit.mExtendData.size(), 6u);
        constexpr std::array<std::int16_t, 5> expectedStackCounts = { 3, 2, 3, 3, 2 };
        constexpr std::array<std::uint32_t, 6> expectedHealthBits
            = { 0x41f00001u, 0x420c0000u, 0x42480000u, 0x42820000u, 0x42960000u, 0x42c7cccdu };
        for (std::size_t i = 0; i < expectedStackCounts.size(); ++i)
        {
            ASSERT_EQ(jumpsuit.mExtendData[i].mExtraData.size(), 2u);
            const auto& countExtra = jumpsuit.mExtendData[i].mExtraData[0];
            EXPECT_EQ(countExtra.mType.mValue, ESM4::sFONVExtraCountType);
            ASSERT_TRUE(countExtra.mCount.has_value());
            EXPECT_EQ(countExtra.mCount->mValue, expectedStackCounts[i]);
            const auto& healthExtra = jumpsuit.mExtendData[i].mExtraData[1];
            EXPECT_EQ(healthExtra.mType.mValue, ESM4::sFONVExtraHealthType);
            ASSERT_TRUE(healthExtra.mHealth.has_value());
            EXPECT_EQ(std::bit_cast<std::uint32_t>(healthExtra.mHealth->mValue), expectedHealthBits[i]);
        }
        ASSERT_EQ(jumpsuit.mExtendData[5].mExtraData.size(), 2u);
        const auto& finalHealth = jumpsuit.mExtendData[5].mExtraData[0];
        ASSERT_TRUE(finalHealth.mHealth.has_value());
        EXPECT_EQ(std::bit_cast<std::uint32_t>(finalHealth.mHealth->mValue), expectedHealthBits[5]);
        EXPECT_EQ(finalHealth.mHealth->mRaw, (std::vector<std::uint8_t>{ 0xcd, 0xcc, 0xc7, 0x42 }));
        EXPECT_EQ(jumpsuit.mExtendData[5].mExtraData[1].mType.mValue, ESM4::sFONVExtraWornType);

        EXPECT_EQ(processInventory.mInventoryEntries[19].mType.mResolvedFormId, 0x00004241u);
        EXPECT_EQ(processInventory.mInventoryEntries[19].mDelta.mValue, 250);
        EXPECT_EQ(processInventory.mInventoryEntries[32].mType.mResolvedFormId, 0x0002935bu);
        EXPECT_EQ(processInventory.mInventoryEntries[32].mDelta.mValue, 50);
        EXPECT_EQ(processInventory.mInventoryEntries[39].mType.mResolvedFormId, 0x000e86f2u);
        EXPECT_EQ(processInventory.mInventoryEntries[39].mDelta.mValue, 40);
        EXPECT_EQ(processInventory.mInventoryEntries[44].mType.mResolvedFormId, 0x0007ea26u);
        EXPECT_EQ(processInventory.mInventoryEntries[44].mDelta.mValue, 20);

        constexpr std::array<std::size_t, 9> unresolvedContentIndices = { 34, 35, 36, 37, 38, 42, 43, 46, 48 };
        constexpr std::array<std::uint32_t, 9> unresolvedContentFormIds = { 0x001735d1u, 0x001735d2u,
            0x001735d4u, 0x001735e0u, 0x001735e3u, 0x001735e1u, 0x001735e4u, 0x001735e2u,
            0x001735e5u };
        for (std::size_t i = 0; i < unresolvedContentIndices.size(); ++i)
            EXPECT_EQ(processInventory.mInventoryEntries[unresolvedContentIndices[i]].mType.mResolvedFormId,
                unresolvedContentFormIds[i]);

        EXPECT_EQ(processInventory.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 499512, 3072 }));
        EXPECT_EQ(processInventory.mUnparsedRemainder.mRaw.size(), 3072u);
        EXPECT_EQ(actorValues.mUnparsedRemainder.mRange.mSize - processInventory.mRange.mSize,
            processInventory.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerMobileObjectProcessState.has_value());
        const auto& processState = *save.mPlayerMobileObjectProcessState;
        EXPECT_EQ(processState.mRange, (ESM4::FONVSaveRange{ 499512, 1675 }));
        EXPECT_EQ(processState.mRaw.size(), 1675u);
        EXPECT_EQ(sha256Hex(processState.mRaw),
            "20e8deb09ebd4b1197ad5d77ce42dc8507c40ae70f9e4c14b43d6542c65c09f8");

        const auto& mobileBase = processState.mMobileObjectBase;
        EXPECT_EQ(mobileBase.mRange, (ESM4::FONVSaveRange{ 499512, 38 }));
        constexpr std::array<std::int8_t, 8> expectedMobileBytes = { -1, 0, 1, 1, 1, 0, 0, 1 };
        for (std::size_t i = 0; i < expectedMobileBytes.size(); ++i)
        {
            EXPECT_EQ(mobileBase.mBytes084_085_07C_07F_080_07D_07E_086[i].mValue,
                expectedMobileBytes[i]);
            EXPECT_EQ(mobileBase.mBytes084_085_07C_07F_080_07D_07E_086[i].mRange,
                (ESM4::FONVSaveRange{ 499512 + i * 2, 1 }));
        }
        EXPECT_EQ(mobileBase.mUnk074.mValue, 0x3f9a4658u);
        EXPECT_FALSE(mobileBase.mUnk06C.mResolvedFormId.has_value());
        EXPECT_FALSE(mobileBase.mUnk070.mResolvedFormId.has_value());

        const auto& baseProcess = processState.mBaseProcess;
        EXPECT_EQ(baseProcess.mRange, (ESM4::FONVSaveRange{ 499550, 19 }));
        EXPECT_EQ(baseProcess.mUnk020.mValue, 0xbf800000u);
        EXPECT_FALSE(baseProcess.mPackage.mResolvedFormId.has_value());

        const auto& lowProcess = processState.mLowProcess;
        EXPECT_EQ(lowProcess.mRange, (ESM4::FONVSaveRange{ 499569, 63 }));
        EXPECT_EQ(lowProcess.mWrd050.mValue, 21280u);
        EXPECT_FLOAT_EQ(lowProcess.mUnk038[0].mValue, -618.7711181640625f);
        EXPECT_FLOAT_EQ(lowProcess.mUnk038[1].mValue, 0.f);
        EXPECT_EQ(lowProcess.mList006CCount.mValue, 0u);
        EXPECT_TRUE(lowProcess.mList006C.empty());
        EXPECT_FALSE(lowProcess.mDamageModifierCount.has_value());
        EXPECT_EQ(processState.mMiddleLowProcess.mRange, (ESM4::FONVSaveRange{ 499632, 5 }));
        EXPECT_EQ(processState.mMiddleLowProcess.mUnk0B4.mValue, 0xffffffffu);
        EXPECT_FALSE(processState.mMiddleLowProcess.mTempModifierCount.has_value());

        const auto& middleHigh = processState.mMiddleHighProcess;
        EXPECT_EQ(middleHigh.mRange, (ESM4::FONVSaveRange{ 499637, 421 }));
        EXPECT_EQ(middleHigh.mUnk134.mValue, 1u);
        EXPECT_EQ(middleHigh.mWeaponOut.mValue, 1u);
        EXPECT_EQ(middleHigh.mUnk168.mValue, 0u);
        EXPECT_FLOAT_EQ(middleHigh.mCoords0E4.mComponents[0].mValue, 0.f);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(middleHigh.mCoords0E4.mComponents[2].mValue), 1u);
        EXPECT_EQ(middleHigh.mWrd22A.mValue, 2u);
        EXPECT_EQ(middleHigh.mList0C8Count.mValue, 0u);
        EXPECT_FALSE(middleHigh.mPackage.mResolvedFormId.has_value());
        ASSERT_TRUE(middleHigh.mAnimation.has_value());
        EXPECT_EQ(middleHigh.mAnimation->mRange, (ESM4::FONVSaveRange{ 499804, 238 }));
        EXPECT_EQ(middleHigh.mAnimation->mLength.mValue, 235u);
        EXPECT_EQ(middleHigh.mAnimation->mLength.mRange, (ESM4::FONVSaveRange{ 499804, 2 }));
        EXPECT_EQ(middleHigh.mAnimation->mLength.mRaw, (std::vector<std::uint8_t>{ 0xad, 0x03 }));
        EXPECT_EQ(middleHigh.mAnimation->mData.mRange, (ESM4::FONVSaveRange{ 499807, 235 }));
        EXPECT_EQ(sha256Hex(middleHigh.mAnimation->mData.mRaw),
            "62d12608cef8654ce644b435bf1189fc2166369d34563dcd8ff36c45ce6e9830");
        EXPECT_EQ(middleHigh.mMagicItemCount.mValue, 0u);
        EXPECT_EQ(middleHigh.mList230Count.mValue, 0u);

        const auto& high = processState.mHighProcess;
        EXPECT_EQ(high.mRange, (ESM4::FONVSaveRange{ 500058, 1129 }));
        EXPECT_EQ(high.mCurrentAction.mValue, -1);
        EXPECT_EQ(high.mCurrentAction.mRange, (ESM4::FONVSaveRange{ 500066, 2 }));
        EXPECT_FLOAT_EQ(high.mCoords.mComponents[0].mValue, 11.7265625f);
        EXPECT_FLOAT_EQ(high.mCoords.mComponents[1].mValue, 39.951904296875f);
        EXPECT_FLOAT_EQ(high.mCoords.mComponents[2].mValue, 83.052734375f);
        EXPECT_EQ(high.mUnk3D8Modulo12.mValue, 0u);
        EXPECT_EQ(high.mUnk30C_2A4_3F0_41C_37C_Idle_2AC[0].mEncoded.mValue, 0x002935u);
        EXPECT_EQ(high.mUnk30C_2A4_3F0_41C_37C_Idle_2AC[0].mResolvedFormId, 0x00104e85u);
        EXPECT_EQ(high.mUnk30C_2A4_3F0_41C_37C_Idle_2AC[3].mEncoded.mValue, 0x00026fu);
        EXPECT_EQ(high.mUnk30C_2A4_3F0_41C_37C_Idle_2AC[3].mResolvedFormId,
            ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(high.mUnknownEntries[0].mUnk3F8.mEncoded.mValue, 0x00026fu);
        EXPECT_EQ(high.mUnknownEntries[0].mUnk3F8.mResolvedFormId, ESM4::sFONVPlayerReferenceFormId);
        EXPECT_EQ(high.mList38CCount.mValue, 0u);
        EXPECT_EQ(high.mList394Count.mValue, 0u);
        EXPECT_EQ(high.mList264Count.mValue, 0u);
        EXPECT_EQ(high.mHasDialogueItems.mValue, 0u);
        EXPECT_EQ(high.mList44CCount.mValue, 0u);
        EXPECT_EQ(high.mList25CCount.mValue, 17u);
        EXPECT_EQ(high.mList25CCount.mRange, (ESM4::FONVSaveRange{ 500383, 1 }));
        EXPECT_EQ(high.mList25CCount.mRaw, (std::vector<std::uint8_t>{ 0x44 }));
        ASSERT_EQ(high.mList25C.size(), 17u);
        constexpr std::array<std::uint32_t, 17> expectedLocationTokens = { 0x002936u, 0x0029fdu,
            0x002935u, 0x002a48u, 0x0025f5u, 0x001798u, 0x00200bu, 0x001ffcu, 0x0029f9u,
            0x0029ffu, 0x002480u, 0x001557u, 0x002584u, 0x004af3u, 0x004af1u, 0x004af2u,
            0x001b39u };
        constexpr std::array<std::uint32_t, 17> expectedLocationFormIds = { 0x0010588eu, 0x00104f08u,
            0x00104e85u, 0x0010769du, 0x001073e8u, 0x0015ef63u, 0x00107077u, 0x0010706eu,
            0x00104f03u, 0x00104f0au, 0x00109a39u, 0x00106b16u, 0x00104c80u, 0x0014c448u,
            0x0014c44au, 0x0014c447u, 0x00157b37u };
        std::uint64_t expectedLocationOffset = 500385;
        for (std::size_t i = 0; i < high.mList25C.size(); ++i)
        {
            const auto& location = high.mList25C[i];
            EXPECT_EQ(location.mRange, (ESM4::FONVSaveRange{ expectedLocationOffset, 42 }));
            EXPECT_EQ(location.mRaw.size(), 42u);
            EXPECT_EQ(location.mForm000.mEncoded.mValue, expectedLocationTokens[i]);
            EXPECT_EQ(location.mForm000.mResolvedFormId, expectedLocationFormIds[i]);
            EXPECT_EQ(location.mUnk020.mValue, 0xffffff9cu);
            expectedLocationOffset += 42;
        }
        EXPECT_EQ(expectedLocationOffset, 501099u);
        EXPECT_FLOAT_EQ(high.mList25C[0].mCoords.mComponents[0].mValue, -68499.9609375f);
        EXPECT_EQ(high.mList25C[3].mByt01E_01C_01D[0].mValue, 1u);
        EXPECT_EQ(high.mList25C[3].mByt01E_01C_01D[1].mValue, 1u);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(high.mList25C[4].mTim018.mValue), 0xff7fffffu);
        EXPECT_EQ(high.mList260Count.mValue, 0u);
        EXPECT_EQ(high.mHasUnk3DC.mValue, 0u);
        EXPECT_EQ(high.mSubBuffer.mRange, (ESM4::FONVSaveRange{ 501103, 84 }));
        EXPECT_EQ(high.mSubBuffer.mLength.mValue, 81u);
        EXPECT_EQ(high.mSubBuffer.mLength.mRange, (ESM4::FONVSaveRange{ 501103, 2 }));
        EXPECT_EQ(high.mSubBuffer.mLength.mRaw, (std::vector<std::uint8_t>{ 0x45, 0x01 }));
        EXPECT_EQ(high.mSubBuffer.mData.mRange, (ESM4::FONVSaveRange{ 501106, 81 }));
        EXPECT_EQ(sha256Hex(high.mSubBuffer.mData.mRaw),
            "90d95c383e63a856126f8304163ac8224c604ef7db1dc2b15b2893c23acc6bd7");
        EXPECT_EQ(processState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 501187, 1397 }));
        EXPECT_EQ(processState.mUnparsedRemainder.mRaw.size(), 1397u);
        EXPECT_EQ(sha256Hex(processState.mUnparsedRemainder.mRaw),
            "6979bae30aeb0d49651a7629d159f4a2090cb57e9b7c28c96d8c22629bc852d1");
        EXPECT_EQ(processInventory.mUnparsedRemainder.mRange.mSize - processState.mRange.mSize,
            processState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerChangedCharacterState.has_value());
        const auto& characterState = *save.mPlayerChangedCharacterState;
        EXPECT_EQ(characterState.mRange, (ESM4::FONVSaveRange{ 501187, 510 }));
        EXPECT_EQ(characterState.mRaw.size(), 510u);
        EXPECT_EQ(sha256Hex(characterState.mRaw),
            "3802ba9e14fc6a31cba704aa523ea18205e06d65ec537815ee75425422175c7a");

        const auto& actorFixed = characterState.mActorFixed;
        EXPECT_EQ(actorFixed.mRange, (ESM4::FONVSaveRange{ 501187, 113 }));
        EXPECT_EQ(actorFixed.mRaw.size(), 113u);
        EXPECT_EQ(sha256Hex(actorFixed.mRaw),
            "7a0a1fbb0c4c198bca5d2c67d8cfaa84e5c83862fd026c89644910fa8ac35846");
        EXPECT_FLOAT_EQ(actorFixed.mUnk114.mValue, 0.010000228881835938f);
        EXPECT_EQ(actorFixed.mUnk114.mRange, (ESM4::FONVSaveRange{ 501187, 4 }));
        EXPECT_EQ(actorFixed.mByt124_125_0BC_0C4[2].mValue, 1u);
        EXPECT_EQ(actorFixed.mUnk110.mValue, 255u);
        EXPECT_EQ(actorFixed.mUnk150_154_158[1].mValue, 0xbc8310a5u);
        EXPECT_EQ(actorFixed.mByt174_175_18D[1].mValue, 1u);
        EXPECT_EQ(actorFixed.mForm0C0_ActorBase_Form070[0].mEncoded.mRange,
            (ESM4::FONVSaveRange{ 501288, 3 }));
        for (const auto& reference : actorFixed.mForm0C0_ActorBase_Form070)
            EXPECT_FALSE(reference.mResolvedFormId.has_value());

        const auto& actorMover = characterState.mActorMover;
        EXPECT_EQ(actorMover.mRange, (ESM4::FONVSaveRange{ 501300, 393 }));
        EXPECT_EQ(actorMover.mRaw.size(), 393u);
        EXPECT_EQ(sha256Hex(actorMover.mRaw),
            "d878b0a7165caaf80facf7e37f87491e44b938cdf5fbc651dc047dfdaf641165");
        EXPECT_EQ(actorMover.mWrd040.mValue, 0xffffu);
        EXPECT_EQ(actorMover.mWrd042.mValue, 0xffffu);
        EXPECT_EQ(actorMover.mUnk034.mValue, 257u);
        EXPECT_EQ(actorMover.mUnk06C.mValue, 4u);
        EXPECT_EQ(actorMover.mUnknown_Unk084[0].mValue, 0xe3b9u);
        EXPECT_EQ(actorMover.mPathingLocation.mRange, (ESM4::FONVSaveRange{ 501380, 37 }));
        for (const auto& component : actorMover.mPathingLocation.mCoords.mComponents)
            EXPECT_EQ(std::bit_cast<std::uint32_t>(component.mValue), 0x7f7fffffu);
        EXPECT_EQ(actorMover.mPathingLocation.mCoordXandY.mValue, 0xdeaddeadu);
        EXPECT_EQ(actorMover.mPathingLocation.mWrd024.mValue, -1);
        EXPECT_EQ(actorMover.mPathingLocation.mByt026.mValue, 2u);
        EXPECT_FALSE(actorMover.mForm02C.mResolvedFormId.has_value());
        EXPECT_EQ(actorMover.mContentFlags.mValue, 0x08u);
        EXPECT_EQ(actorMover.mContentFlags.mRange, (ESM4::FONVSaveRange{ 501421, 1 }));

        const auto& detailed = actorMover.mDetailedPathHandler;
        EXPECT_EQ(detailed.mRange, (ESM4::FONVSaveRange{ 501423, 242 }));
        EXPECT_EQ(detailed.mRaw.size(), 242u);
        EXPECT_EQ(sha256Hex(detailed.mRaw),
            "67d959fcbde2f94465cbecfd57de5be9e4f7c3de4d3f446cab24ea26c08038fd");
        EXPECT_EQ(std::bit_cast<std::uint32_t>(detailed.mCoords01C_028_034_040_04C[3].mComponents[0].mValue),
            0x7f7fffffu);
        EXPECT_EQ(detailed.mUnk060_064_068_06C_070_074_078_07C_080_084_088_08C_090_094_098_09C_0AC_0B0_0B4_0B8[2]
                      .mValue,
            0x3e8effadu);
        EXPECT_EQ(detailed.mUnk014[0].mValue, 0x41200000u);
        EXPECT_EQ(detailed.mUnk014[1].mValue, 0x41a00000u);
        EXPECT_EQ(detailed.mByt0DC_0DD_0DE_0DF_0E0_0E2_0E1[0].mValue, 1u);
        EXPECT_EQ(detailed.mByt0DC_0DD_0DE_0DF_0E0_0E2_0E1[1].mValue, 1u);
        EXPECT_FALSE(detailed.mForm0D8.mResolvedFormId.has_value());
        EXPECT_EQ(detailed.mList058Count.mValue, 0u);
        EXPECT_EQ(detailed.mList058Count.mRange, (ESM4::FONVSaveRange{ 501663, 1 }));
        EXPECT_TRUE(detailed.mList058.empty());

        EXPECT_EQ(actorMover.mPlayerMover.mRange, (ESM4::FONVSaveRange{ 501665, 28 }));
        EXPECT_FLOAT_EQ(actorMover.mPlayerMover.mCoords.mComponents[0].mValue, 308.f);
        EXPECT_FLOAT_EQ(actorMover.mPlayerMover.mCoords.mComponents[1].mValue, 0.f);
        EXPECT_FLOAT_EQ(actorMover.mPlayerMover.mCoords.mComponents[2].mValue, 0.f);
        EXPECT_EQ(actorMover.mPlayerMover.mUnk094_098_09C[0].mValue, 576u);
        EXPECT_EQ(actorMover.mPlayerMover.mUnk094_098_09C[1].mValue, 32u);
        EXPECT_EQ(actorMover.mPlayerMover.mUnk094_098_09C[2].mValue, 0xbc031273u);
        EXPECT_EQ(characterState.mByt1C0.mRange, (ESM4::FONVSaveRange{ 501693, 1 }));
        EXPECT_EQ(characterState.mByt1C1.mRange, (ESM4::FONVSaveRange{ 501695, 1 }));
        EXPECT_EQ(characterState.mByt1C0.mValue, 0u);
        EXPECT_EQ(characterState.mByt1C1.mValue, 0u);
        EXPECT_EQ(characterState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 501697, 887 }));
        EXPECT_EQ(characterState.mUnparsedRemainder.mRaw.size(), 887u);
        EXPECT_EQ(sha256Hex(characterState.mUnparsedRemainder.mRaw),
            "e2c332386e74a5114e27997356e9fe24cb4d49c876847b283604b4f56e4fc9d7");
        EXPECT_EQ(processState.mUnparsedRemainder.mRange.mSize - characterState.mRange.mSize,
            characterState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerCharacterAnimationState.has_value());
        const auto& animationState = *save.mPlayerCharacterAnimationState;
        EXPECT_EQ(animationState.mRange, (ESM4::FONVSaveRange{ 501697, 148 }));
        EXPECT_EQ(animationState.mRaw.size(), 148u);
        EXPECT_EQ(sha256Hex(animationState.mRaw),
            "eff4c16d299aa5c2b3b0385001d8340a165b4db3d13dc09ebe7886c98daf6206");
        EXPECT_EQ(animationState.mAnimation.mRange, animationState.mRange);
        EXPECT_EQ(animationState.mAnimation.mLength.mValue, 145u);
        EXPECT_EQ(animationState.mAnimation.mLength.mRange, (ESM4::FONVSaveRange{ 501697, 2 }));
        EXPECT_EQ(animationState.mAnimation.mLength.mRaw, (std::vector<std::uint8_t>{ 0x45, 0x02 }));
        EXPECT_EQ(animationState.mAnimation.mData.mRange, (ESM4::FONVSaveRange{ 501700, 145 }));
        EXPECT_EQ(animationState.mAnimation.mData.mRaw.size(), 145u);
        EXPECT_EQ(sha256Hex(animationState.mAnimation.mData.mRaw),
            "3b1a3de703fc484c1271076cbdac4e155531c2f794b0e7a7c49e418329090376");
        EXPECT_EQ(animationState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 501845, 739 }));
        EXPECT_EQ(animationState.mUnparsedRemainder.mRaw.size(), 739u);
        EXPECT_EQ(sha256Hex(animationState.mUnparsedRemainder.mRaw),
            "71e6d95f325b5d4b7a8db31b4abbf185944b7fb936d4a54f09d4243ecfa1021f");
        EXPECT_EQ(characterState.mUnparsedRemainder.mRange.mSize - animationState.mRange.mSize,
            animationState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerCharacterScalarReferenceState.has_value());
        const auto& scalarState = *save.mPlayerCharacterScalarReferenceState;
        EXPECT_EQ(scalarState.mRange, (ESM4::FONVSaveRange{ 501845, 287 }));
        EXPECT_EQ(scalarState.mRaw.size(), 287u);
        EXPECT_EQ(sha256Hex(scalarState.mRaw),
            "03c6a113c7aafe5b32d19038468e70d7a877670e6ef9a5ddcc35898c9c2c8c4f");
        EXPECT_EQ(scalarState.mFirstPersonMode.mValue, 0u);
        EXPECT_EQ(scalarState.mFirstPersonMode.mRange, (ESM4::FONVSaveRange{ 501845, 1 }));
        EXPECT_EQ(scalarState.mFirstPersonMode.mRaw, (std::vector<std::uint8_t>{ 0 }));
        EXPECT_EQ(scalarState.mByt651.mValue, 1u);
        EXPECT_EQ(scalarState.mUnk660.mValue, 1057501480u);
        EXPECT_EQ(scalarState.mRefr6F4Pos.mRange, (ESM4::FONVSaveRange{ 501907, 13 }));
        for (const auto& component : scalarState.mRefr6F4Pos.mComponents)
            EXPECT_FLOAT_EQ(component.mValue, 0.f);
        EXPECT_EQ(scalarState.mUnk65C.mValue, 1117126656u);
        EXPECT_FLOAT_EQ(scalarState.mFirstPersonModelFov.mValue, 55.f);
        EXPECT_EQ(scalarState.mFirstPersonModelFov.mRange, (ESM4::FONVSaveRange{ 501942, 4 }));
        EXPECT_EQ(scalarState.mFirstPersonModelFov.mRaw, (std::vector<std::uint8_t>{ 0, 0, 0x5c, 0x42 }));
        EXPECT_FLOAT_EQ(scalarState.mWorldFov.mValue, 75.f);
        EXPECT_EQ(scalarState.mWorldFov.mRange, (ESM4::FONVSaveRange{ 501947, 4 }));
        EXPECT_EQ(scalarState.mWorldFov.mRaw, (std::vector<std::uint8_t>{ 0, 0, 0x96, 0x42 }));
        EXPECT_EQ(scalarState.mUnk790.mValue, 1005724u);
        EXPECT_FLOAT_EQ(scalarState.mFlt11E0B5C.mValue, 120.f);
        EXPECT_EQ(scalarState.mFlt11E0B5C.mRange, (ESM4::FONVSaveRange{ 501999, 4 }));
        EXPECT_EQ(scalarState.mVersion21.mRange, (ESM4::FONVSaveRange{ 502046, 35 }));
        EXPECT_EQ(scalarState.mVersion21.mUnk684.mValue, 1084227584u);
        for (const auto& value : scalarState.mVersion21.mUnk744)
            EXPECT_EQ(value.mValue, 0u);
        EXPECT_EQ(scalarState.mUnk878.mRange, (ESM4::FONVSaveRange{ 502081, 7 }));
        EXPECT_EQ(scalarState.mUnk878.mUnk004.mValue, 275u);

        const std::array<const ESM4::FONVSaveResolvedReferenceId*, 11> scalarReferences = {
            &scalarState.mQuest,
            &scalarState.mClass,
            &scalarState.mRefr6F4ParentCell,
            &scalarState.mRegion,
            &scalarState.mRegionWeather,
            &scalarState.mForm208,
            &scalarState.mForm224,
            &scalarState.mForm638,
            &scalarState.mForm604,
            &scalarState.mFormD2C,
            &scalarState.mFormD44,
        };
        constexpr std::array<std::uint32_t, 11> scalarReferenceTokens
            = { 0x00403cu, 0u, 0u, 0x004f5fu, 0x004f59u, 0u, 0u, 0u, 0x004d2eu, 0u, 0u };
        constexpr std::array<std::uint32_t, 11> scalarResolvedFormIds
            = { 0x03002fcau, 0u, 0u, 0x00123ce3u, 0x001237d7u, 0u, 0u, 0u, 0x0010636fu, 0u, 0u };
        for (std::size_t i = 0; i < scalarReferences.size(); ++i)
        {
            const auto& reference = *scalarReferences[i];
            EXPECT_EQ(reference.mEncoded.mValue, scalarReferenceTokens[i]);
            EXPECT_EQ(reference.mEncoded.mRange, (ESM4::FONVSaveRange{ 502088 + i * 4, 3 }));
            EXPECT_EQ(reference.mKind, ESM4::FONVSaveReferenceKind::FormIdArray);
            EXPECT_EQ(reference.mPayload, scalarReferenceTokens[i]);
            if (scalarResolvedFormIds[i] == 0)
                EXPECT_FALSE(reference.mResolvedFormId.has_value());
            else
                EXPECT_EQ(reference.mResolvedFormId, scalarResolvedFormIds[i]);
        }
        EXPECT_EQ(scalarState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 502132, 452 }));
        EXPECT_EQ(scalarState.mUnparsedRemainder.mRaw.size(), 452u);
        EXPECT_EQ(sha256Hex(scalarState.mUnparsedRemainder.mRaw),
            "bb677eb06efd1a806ddc715269f3da7dee84353bbdb564a1fd4eee19bff9f6d3");
        EXPECT_EQ(animationState.mUnparsedRemainder.mRange.mSize - scalarState.mRange.mSize,
            scalarState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerCharacterListsState.has_value());
        const auto& listsState = *save.mPlayerCharacterListsState;
        EXPECT_EQ(listsState.mRange, (ESM4::FONVSaveRange{ 502132, 147 }));
        EXPECT_EQ(listsState.mRaw.size(), 147u);
        EXPECT_EQ(sha256Hex(listsState.mRaw),
            "cec00de438e97af6429ad69dc1b514b67a287e04a71146cbe114e24ac322da3c");
        EXPECT_EQ(listsState.mList6A8Count.mValue, 0u);
        EXPECT_EQ(listsState.mList6A8Count.mRange, (ESM4::FONVSaveRange{ 502132, 1 }));
        EXPECT_TRUE(listsState.mTopics.empty());
        EXPECT_EQ(listsState.mList5E4Count.mValue, 2u);
        EXPECT_EQ(listsState.mList5E4Count.mRange, (ESM4::FONVSaveRange{ 502134, 1 }));
        ASSERT_EQ(listsState.mNotes.size(), 2u);
        constexpr std::array<std::uint32_t, 2> noteTokens = { 0x000295u, 0x000297u };
        constexpr std::array<std::uint32_t, 2> noteFormIds = { 0x0001a7eau, 0x000744b7u };
        for (std::size_t i = 0; i < listsState.mNotes.size(); ++i)
        {
            EXPECT_EQ(listsState.mNotes[i].mEncoded.mValue, noteTokens[i]);
            EXPECT_EQ(listsState.mNotes[i].mEncoded.mRange, (ESM4::FONVSaveRange{ 502136 + i * 4, 3 }));
            EXPECT_EQ(listsState.mNotes[i].mResolvedFormId, noteFormIds[i]);
        }
        EXPECT_EQ(listsState.mInventoryEntryCount.mValue, 0u);
        EXPECT_EQ(listsState.mInventoryEntryCount.mRange, (ESM4::FONVSaveRange{ 502144, 1 }));
        EXPECT_TRUE(listsState.mInventoryEntries.empty());
        EXPECT_EQ(listsState.mListD48Count.mValue, 7u);
        EXPECT_EQ(listsState.mListD48Count.mRange, (ESM4::FONVSaveRange{ 502146, 1 }));
        ASSERT_EQ(listsState.mListD48.size(), 7u);
        constexpr std::array<std::uint32_t, 7> listD48Tokens
            = { 0x0029f9u, 0x00200bu, 0x004af1u, 0x001798u, 0x001ffcu, 0x002a48u, 0x0025f5u };
        constexpr std::array<std::uint32_t, 7> listD48FormIds
            = { 0x00104f03u, 0x00107077u, 0x0014c44au, 0x0015ef63u, 0x0010706eu, 0x0010769du,
                  0x001073e8u };
        for (std::size_t i = 0; i < listsState.mListD48.size(); ++i)
        {
            const auto& entry = listsState.mListD48[i];
            EXPECT_EQ(entry.mRange, (ESM4::FONVSaveRange{ 502148 + i * 8, 8 }));
            EXPECT_EQ(entry.mForm000.mEncoded.mValue, listD48Tokens[i]);
            EXPECT_EQ(entry.mForm000.mResolvedFormId, listD48FormIds[i]);
            EXPECT_EQ(entry.mByt004.mValue, 0u);
            EXPECT_EQ(entry.mByt005.mValue, i == 6 ? 1u : 0u);
        }
        EXPECT_EQ(listsState.mPerkCount.mValue, 0u);
        EXPECT_TRUE(listsState.mPerks.empty());
        EXPECT_EQ(listsState.mList60CCount.mValue, 0u);
        EXPECT_TRUE(listsState.mList60C.empty());
        EXPECT_EQ(listsState.mList610Count.mValue, 0u);
        EXPECT_TRUE(listsState.mList610.empty());
        EXPECT_EQ(listsState.mCards614Count.mValue, 0u);
        EXPECT_TRUE(listsState.mCards614.empty());
        EXPECT_EQ(listsState.mCards618Count.mValue, 0u);
        EXPECT_TRUE(listsState.mCards618.empty());
        EXPECT_EQ(listsState.mUnk61C.mValue, 0u);
        EXPECT_EQ(listsState.mUnk61C.mRange, (ESM4::FONVSaveRange{ 502214, 4 }));
        EXPECT_EQ(listsState.mUnk620.mValue, 0u);
        EXPECT_EQ(listsState.mUnk624.mValue, 0u);
        EXPECT_EQ(listsState.mUnk628.mValue, 0u);
        EXPECT_EQ(listsState.mUnk62C.mValue, 0u);
        EXPECT_EQ(listsState.mUnk62C.mRange, (ESM4::FONVSaveRange{ 502234, 4 }));
        EXPECT_EQ(listsState.mStageCount.mValue, 0u);
        EXPECT_EQ(listsState.mStageCount.mRange, (ESM4::FONVSaveRange{ 502239, 1 }));
        EXPECT_TRUE(listsState.mStages.empty());
        EXPECT_EQ(listsState.mObjectiveCount.mValue, 4u);
        EXPECT_EQ(listsState.mObjectiveCount.mRange, (ESM4::FONVSaveRange{ 502241, 1 }));
        ASSERT_EQ(listsState.mObjectives.size(), 4u);
        constexpr std::array<std::uint32_t, 4> objectiveQuestTokens
            = { 0x0031f1u, 0x002dd4u, 0x002c0fu, 0x00403cu };
        constexpr std::array<std::uint32_t, 4> objectiveQuestFormIds
            = { 0x01005229u, 0x02008891u, 0x04003603u, 0x03002fcau };
        for (std::size_t i = 0; i < listsState.mObjectives.size(); ++i)
        {
            const auto& entry = listsState.mObjectives[i];
            EXPECT_EQ(entry.mRange, (ESM4::FONVSaveRange{ 502243 + i * 9, 9 }));
            EXPECT_EQ(entry.mQuest.mEncoded.mValue, objectiveQuestTokens[i]);
            EXPECT_EQ(entry.mQuest.mResolvedFormId, objectiveQuestFormIds[i]);
            EXPECT_EQ(entry.mObjective.mValue, 10u);
        }
        EXPECT_EQ(listsState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 502279, 305 }));
        EXPECT_EQ(listsState.mUnparsedRemainder.mRaw.size(), 305u);
        EXPECT_EQ(sha256Hex(listsState.mUnparsedRemainder.mRaw),
            "54c70a2c054231d134ae65f1d26c23e8ed57b7385d7baaa6d6ff15a62264b07b");
        EXPECT_EQ(scalarState.mUnparsedRemainder.mRange.mSize - listsState.mRange.mSize,
            listsState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerCharacterMagicTargetState.has_value());
        const auto& magicTargetState = *save.mPlayerCharacterMagicTargetState;
        EXPECT_EQ(magicTargetState.mRange, (ESM4::FONVSaveRange{ 502279, 234 }));
        EXPECT_EQ(magicTargetState.mRaw.size(), 234u);
        EXPECT_EQ(sha256Hex(magicTargetState.mRaw),
            "2eb27ee12ad512bf549eddddd4d16f0b8b130a4c81dd550f636618c9becb03d4");
        EXPECT_EQ(magicTargetState.mMagicItemCount.mValue, 4u);
        EXPECT_EQ(magicTargetState.mMagicItemCount.mRange, (ESM4::FONVSaveRange{ 502279, 1 }));
        EXPECT_EQ(magicTargetState.mMagicItemCount.mRaw, (std::vector<std::uint8_t>{ 0x10 }));
        ASSERT_EQ(magicTargetState.mMagicItems.size(), 4u);
        constexpr std::array<std::uint32_t, 4> magicFormTokens
            = { 0x004af4u, 0x00029eu, 0x00029eu, 0x004af4u };
        constexpr std::array<std::uint32_t, 4> magicFormIds
            = { 0x0006533au, 0x00072e30u, 0x00072e30u, 0x0006533au };
        constexpr std::array<std::uint8_t, 4> magicArchTypes = { 0, 1, 0, 1 };
        constexpr std::array<std::uint32_t, 4> magicUnk098 = { 33, 0, 0, 0 };
        constexpr std::array<std::string_view, 4> magicEntryHashes = {
            "9a2bc6b3af6a49f454eee5bca492065903c4d228bea8cd68500f855e849f257c",
            "6fe8d9dc880f10818b396ab85d53bf9c1f597da25c20576ba69787b7e3c16377",
            "cf01b43d72eef0670fb6cd321a2c4084d252aaec916b5651a808607bd746801d",
            "8ccf825561c55cab2129646b44527cfd7923959edba44753094c807fa82210d4",
        };
        constexpr std::array<std::string_view, 4> magicEffectHashes = {
            "317dcdf61b6db55ecf3fcc4bde2173e50e9a7d9a8683076a8362bfc1622dbf2f",
            "c9d248425ffdd02fd59a9495ba7490871a23bfefe2855490465eae625a2289ae",
            "318b191a19ab9cbff11201560c68252040841cd2c3c634b48d294feeb105a103",
            "996c708d50c59c225cdfbe2db3b418f13160f9308dcfd938a718fa809dae4191",
        };
        for (std::size_t i = 0; i < magicTargetState.mMagicItems.size(); ++i)
        {
            const auto& entry = magicTargetState.mMagicItems[i];
            const std::size_t entryBegin = 502281 + i * 58;
            EXPECT_EQ(entry.mRange, (ESM4::FONVSaveRange{ entryBegin, 58 }));
            EXPECT_EQ(sha256Hex(entry.mRaw), magicEntryHashes[i]);
            EXPECT_EQ(entry.mMagicForm.mEncoded.mValue, magicFormTokens[i]);
            EXPECT_EQ(entry.mMagicForm.mEncoded.mRange, (ESM4::FONVSaveRange{ entryBegin, 3 }));
            EXPECT_EQ(entry.mMagicForm.mResolvedFormId, magicFormIds[i]);
            EXPECT_EQ(entry.mArchType.mValue, magicArchTypes[i]);
            EXPECT_EQ(entry.mUnk098.mValue, magicUnk098[i]);
            EXPECT_EQ(entry.mEffectItemCount.mValue, 48u);
            EXPECT_EQ(entry.mEffectItemCount.mRange, (ESM4::FONVSaveRange{ entryBegin + 8, 1 }));
            ASSERT_EQ(entry.mEffectItems.size(), 48u);
            std::vector<std::uint8_t> effectItems;
            effectItems.reserve(entry.mEffectItems.size());
            for (std::size_t effectIndex = 0; effectIndex < entry.mEffectItems.size(); ++effectIndex)
            {
                EXPECT_EQ(entry.mEffectItems[effectIndex].mRange,
                    (ESM4::FONVSaveRange{ entryBegin + 10 + effectIndex, 1 }));
                effectItems.push_back(entry.mEffectItems[effectIndex].mValue);
            }
            EXPECT_EQ(sha256Hex(effectItems), magicEffectHashes[i]);
        }
        EXPECT_EQ(magicTargetState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 502513, 71 }));
        EXPECT_EQ(magicTargetState.mUnparsedRemainder.mRaw.size(), 71u);
        EXPECT_EQ(sha256Hex(magicTargetState.mUnparsedRemainder.mRaw),
            "7e88d94efc5f64bbf8ca213e2d0759f4b3b3e4012a486145f9d5531af1de039f");
        EXPECT_EQ(listsState.mUnparsedRemainder.mRange.mSize - magicTargetState.mRange.mSize,
            magicTargetState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(save.mPlayerCharacterFinalState.has_value());
        const auto& finalState = *save.mPlayerCharacterFinalState;
        EXPECT_EQ(finalState.mRange, (ESM4::FONVSaveRange{ 502513, 71 }));
        EXPECT_EQ(finalState.mRaw.size(), 71u);
        EXPECT_EQ(sha256Hex(finalState.mRaw),
            "7e88d94efc5f64bbf8ca213e2d0759f4b3b3e4012a486145f9d5531af1de039f");
        EXPECT_EQ(finalState.mKeyForUnkD64.mValue, 0u);
        EXPECT_EQ(finalState.mKeyForUnkD64.mRange, (ESM4::FONVSaveRange{ 502513, 4 }));
        EXPECT_FLOAT_EQ(finalState.mUnknown11DFED4.mValue, 0.f);
        EXPECT_EQ(finalState.mUnknown11DFED4.mRange, (ESM4::FONVSaveRange{ 502518, 4 }));
        EXPECT_FLOAT_EQ(finalState.mUnknown11DFED8.mValue, 0.f);
        EXPECT_EQ(finalState.mUnknown11DFED8.mRange, (ESM4::FONVSaveRange{ 502523, 4 }));
        EXPECT_EQ(finalState.mPerksAD4Count.mValue, 0u);
        EXPECT_EQ(finalState.mPerksAD4Count.mRange, (ESM4::FONVSaveRange{ 502528, 1 }));
        EXPECT_TRUE(finalState.mPerksAD4.empty());
        EXPECT_EQ(finalState.mHardcoreMode.mValue, 0u);
        EXPECT_EQ(finalState.mHardcoreMode.mRange, (ESM4::FONVSaveRange{ 502530, 1 }));
        EXPECT_EQ(finalState.mClearsHardcoreFlag.mValue, 1u);
        EXPECT_EQ(finalState.mClearsHardcoreFlag.mRange, (ESM4::FONVSaveRange{ 502532, 1 }));
        EXPECT_EQ(finalState.mByt66E.mValue, 1u);
        EXPECT_EQ(finalState.mByt66E.mRange, (ESM4::FONVSaveRange{ 502534, 1 }));
        for (std::size_t i = 0; i < finalState.mUnknownE3C.size(); ++i)
        {
            EXPECT_EQ(finalState.mUnknownE3C[i].mEncoded.mValue, 0u);
            EXPECT_EQ(finalState.mUnknownE3C[i].mEncoded.mRange,
                (ESM4::FONVSaveRange{ 502536 + i * 5, 4 }));
            EXPECT_FALSE(finalState.mUnknownE3C[i].mResolvedFormId.has_value());
        }
        EXPECT_EQ(finalState.mEnabledCount.mValue, 0u);
        EXPECT_EQ(finalState.mEnabledCount.mRange, (ESM4::FONVSaveRange{ 502576, 1 }));
        EXPECT_TRUE(finalState.mEnabled.empty());
        EXPECT_EQ(finalState.mDisabledCount.mValue, 0u);
        EXPECT_EQ(finalState.mDisabledCount.mRange, (ESM4::FONVSaveRange{ 502578, 1 }));
        EXPECT_TRUE(finalState.mDisabled.empty());
        EXPECT_EQ(finalState.mUnknownCount.mValue, 0u);
        EXPECT_EQ(finalState.mUnknownCount.mRange, (ESM4::FONVSaveRange{ 502580, 1 }));
        EXPECT_TRUE(finalState.mUnknown.empty());
        EXPECT_EQ(finalState.mFadeOutCount.mValue, 0u);
        EXPECT_EQ(finalState.mFadeOutCount.mRange, (ESM4::FONVSaveRange{ 502582, 1 }));
        EXPECT_TRUE(finalState.mFadeOut.empty());
        EXPECT_EQ(finalState.mUnparsedRemainder.mRange, (ESM4::FONVSaveRange{ 502584, 0 }));
        EXPECT_TRUE(finalState.mUnparsedRemainder.mRaw.empty());
        EXPECT_EQ(magicTargetState.mUnparsedRemainder.mRange.mSize - finalState.mRange.mSize,
            finalState.mUnparsedRemainder.mRange.mSize);

        ASSERT_TRUE(movement.mCellOrWorldspace.mResolvedFormId.has_value());
        const ESM::RefId worldspace = ESM::RefId::formIdRefId(
            ESM::FormId::fromUint32(*movement.mCellOrWorldspace.mResolvedFormId));
        const ESM::ExteriorCellLocation playerCell = ESM::positionToExteriorCellLocation(
            movement.mPosition[0].mValue, movement.mPosition[1].mValue, worldspace);
        // The hash-pinned FalloutNV.esm has WastelandNV CELL 0x000E1AA7 at this grid. Global Data type 1 stores
        // (-18, 0), matching the named Goodsprings CELL, so it must not override the transform placement mapping.
        EXPECT_EQ(playerCell, (ESM::ExteriorCellLocation{ -18, -1, worldspace }));
        EXPECT_EQ(save.findChangedForm(ESM4::sFONVPlayerNpcFormId), nullptr)
            << "NPC_ FormID 0x7 is a FalloutNV.esm base-record relation, not serialized as a Save330 change form";

        ASSERT_EQ(save.mQuestChanges.size(), 468u);
        const std::uint64_t decodedQuestBytes = std::accumulate(save.mQuestChanges.begin(),
            save.mQuestChanges.end(), std::uint64_t{ 0 },
            [](std::uint64_t total, const ESM4::FONVSaveQuestChange& quest) {
                return total + quest.mRange.mSize;
            });
        EXPECT_EQ(decodedQuestBytes, 4776u);
        ASSERT_EQ(save.mFactionChanges.size(), 4u);
        const std::uint64_t decodedFactionBytes = std::accumulate(save.mFactionChanges.begin(),
            save.mFactionChanges.end(), std::uint64_t{ 0 },
            [](std::uint64_t total, const ESM4::FONVSaveFactionChange& faction) {
                return total + faction.mRange.mSize;
            });
        EXPECT_EQ(decodedFactionBytes, 792u);
        ASSERT_EQ(save.mWorldReferenceMovements.size(), 1462u);
        const std::uint64_t decodedWorldMovementBytes = std::accumulate(save.mWorldReferenceMovements.begin(),
            save.mWorldReferenceMovements.end(), std::uint64_t{ 0 },
            [](std::uint64_t total, const ESM4::FONVSaveWorldReferenceMovement& movement) {
                return total + movement.mMovement.mRange.mSize;
            });
        EXPECT_EQ(decodedWorldMovementBytes, 40936u);
        EXPECT_EQ(std::ranges::count_if(save.mWorldReferenceMovements,
                      [&](const ESM4::FONVSaveWorldReferenceMovement& movement) {
                          return (movement.mResolvedFormId >> 24) >= save.mMasters.size()
                              || !movement.mMovement.mCellOrWorldspace.mResolvedFormId
                              || (*movement.mMovement.mCellOrWorldspace.mResolvedFormId >> 24) >= save.mMasters.size();
                      }),
            0);
        EXPECT_EQ(save.mUnparsedSemanticPayloadRanges.size(), 6616u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 2685573u);
        EXPECT_EQ(save.mStructurallyAccountedRange, (ESM4::FONVSaveRange{ 0, 3395328 }));
        EXPECT_EQ(save.mParsedPrefixRange, save.mStructurallyAccountedRange);
        EXPECT_EQ(save.mUnparsedBodyRange, (ESM4::FONVSaveRange{ 3395328, 0 }));
    }

    TEST(FONVSaveGame, RejectsCorruptExternalSave330PlayerCharacterAnimationState)
    {
        const char* fixture = std::getenv("OPENMW_FNV_SAVE330_FIXTURE");
        if (fixture == nullptr || *fixture == '\0')
            GTEST_SKIP() << "Set OPENMW_FNV_SAVE330_FIXTURE to the read-only retail Save 330 path";

        const std::vector<std::uint8_t> fixtureBytes
            = readFixtureBytes(std::filesystem::u8path(fixture));
        ASSERT_EQ(sha256Hex(fixtureBytes), "07dbdd2d7c4abe3160628e5463a9603a40f4271042c1da1b89f1c4a4f7dbd81f")
            << "OPENMW_FNV_SAVE330_FIXTURE is not the pinned Save330 evidence file";

        std::vector<std::uint8_t> corrupted = fixtureBytes;
        corrupted[501697] = 0x46;
        corrupted[501698] = 0x02;
        corrupted[501699] = 0;
        corrupted[501700] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[501699] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[501697] = 0xe1;
        corrupted[501698] = 0x0d;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptExternalSave330PlayerCharacterScalarReferenceState)
    {
        const char* fixture = std::getenv("OPENMW_FNV_SAVE330_FIXTURE");
        if (fixture == nullptr || *fixture == '\0')
            GTEST_SKIP() << "Set OPENMW_FNV_SAVE330_FIXTURE to the read-only retail Save 330 path";

        const std::vector<std::uint8_t> fixtureBytes
            = readFixtureBytes(std::filesystem::u8path(fixture));
        ASSERT_EQ(sha256Hex(fixtureBytes), "07dbdd2d7c4abe3160628e5463a9603a40f4271042c1da1b89f1c4a4f7dbd81f")
            << "OPENMW_FNV_SAVE330_FIXTURE is not the pinned Save330 evidence file";

        std::vector<std::uint8_t> corrupted = fixtureBytes;
        corrupted[501846] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        overwriteU32(corrupted, 501907, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        overwriteU32(corrupted, 501942, 0x7fc00000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        overwriteU32(corrupted, 501947, 0xff800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        overwriteU32(corrupted, 501999, 0x7f800000u);
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502088] = 0xc0;
        corrupted[502089] = 0;
        corrupted[502090] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502088] = 0x3f;
        corrupted[502089] = 0xff;
        corrupted[502090] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502091] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[497486] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptExternalSave330PlayerCharacterListsState)
    {
        const char* fixture = std::getenv("OPENMW_FNV_SAVE330_FIXTURE");
        if (fixture == nullptr || *fixture == '\0')
            GTEST_SKIP() << "Set OPENMW_FNV_SAVE330_FIXTURE to the read-only retail Save 330 path";

        const std::vector<std::uint8_t> fixtureBytes
            = readFixtureBytes(std::filesystem::u8path(fixture));
        ASSERT_EQ(sha256Hex(fixtureBytes), "07dbdd2d7c4abe3160628e5463a9603a40f4271042c1da1b89f1c4a4f7dbd81f")
            << "OPENMW_FNV_SAVE330_FIXTURE is not the pinned Save330 evidence file";

        std::vector<std::uint8_t> corrupted = fixtureBytes;
        corrupted[502133] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502134] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502136] = 0xc0;
        corrupted[502137] = 0;
        corrupted[502138] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502136] = 0x3f;
        corrupted[502137] = 0xff;
        corrupted[502138] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502139] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502146] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502153] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502242] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502243] = 0xc0;
        corrupted[502244] = 0;
        corrupted[502245] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502246] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptExternalSave330PlayerCharacterMagicTargetState)
    {
        const char* fixture = std::getenv("OPENMW_FNV_SAVE330_FIXTURE");
        if (fixture == nullptr || *fixture == '\0')
            GTEST_SKIP() << "Set OPENMW_FNV_SAVE330_FIXTURE to the read-only retail Save 330 path";

        const std::vector<std::uint8_t> fixtureBytes
            = readFixtureBytes(std::filesystem::u8path(fixture));
        ASSERT_EQ(sha256Hex(fixtureBytes), "07dbdd2d7c4abe3160628e5463a9603a40f4271042c1da1b89f1c4a4f7dbd81f")
            << "OPENMW_FNV_SAVE330_FIXTURE is not the pinned Save330 evidence file";

        std::vector<std::uint8_t> corrupted = fixtureBytes;
        corrupted[502280] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502279] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502279] = 0xfc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502281] = 0xc0;
        corrupted[502282] = 0;
        corrupted[502283] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502281] = 0x3f;
        corrupted[502282] = 0xff;
        corrupted[502283] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502284] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502286] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502287] = 3;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502288] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502289] = 0xbc;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502290] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
    }

    TEST(FONVSaveGame, RejectsCorruptExternalSave330PlayerCharacterFinalState)
    {
        const char* fixture = std::getenv("OPENMW_FNV_SAVE330_FIXTURE");
        if (fixture == nullptr || *fixture == '\0')
            GTEST_SKIP() << "Set OPENMW_FNV_SAVE330_FIXTURE to the read-only retail Save 330 path";

        const std::vector<std::uint8_t> fixtureBytes
            = readFixtureBytes(std::filesystem::u8path(fixture));
        ASSERT_EQ(sha256Hex(fixtureBytes), "07dbdd2d7c4abe3160628e5463a9603a40f4271042c1da1b89f1c4a4f7dbd81f")
            << "OPENMW_FNV_SAVE330_FIXTURE is not the pinned Save330 evidence file";

        constexpr std::array<std::size_t, 19> delimiterOffsets = { 502517, 502522, 502527, 502529,
            502531, 502533, 502535, 502540, 502545, 502550, 502555, 502560, 502565, 502570, 502575,
            502577, 502579, 502581, 502583 };
        for (const std::size_t delimiterOffset : delimiterOffsets)
        {
            std::vector<std::uint8_t> corrupted = fixtureBytes;
            corrupted[delimiterOffset] = 0;
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
        }

        for (const std::size_t floatOffset : { 502518u, 502523u })
        {
            std::vector<std::uint8_t> corrupted = fixtureBytes;
            overwriteU32(corrupted, floatOffset, 0x7f800000u);
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
        }

        constexpr std::array<std::size_t, 5> countOffsets = { 502528, 502576, 502578, 502580, 502582 };
        for (const std::size_t countOffset : countOffsets)
        {
            std::vector<std::uint8_t> corrupted = fixtureBytes;
            corrupted[countOffset] = 3;
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

            corrupted = fixtureBytes;
            corrupted[countOffset] = 0xfc;
            EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
        }

        std::vector<std::uint8_t> corrupted = fixtureBytes;
        corrupted[502576] = 4;
        corrupted[502578] = 0xc0;
        corrupted[502579] = 0;
        corrupted[502580] = 1;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502576] = 4;
        corrupted[502578] = 0x3f;
        corrupted[502579] = 0xff;
        corrupted[502580] = 0xff;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[502576] = 4;
        corrupted[502578] = 0x40;
        corrupted[502579] = 0x01;
        corrupted[502580] = 0x23;
        corrupted[502581] = 0;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);

        corrupted = fixtureBytes;
        corrupted[497486] = 26;
        EXPECT_THROW(ESM4::parseFONVSaveGamePrefix(corrupted), ESM4::FONVSaveError);
    }
}

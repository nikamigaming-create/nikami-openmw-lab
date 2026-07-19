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
        std::size_t mGlobalData2Begin = 0;
        std::size_t mRefIdArrayBegin = 0;
        std::size_t mUnknownTableBegin = 0;
    };

    SaveBytes makeSave(bool hasLanguage = true, std::uint32_t width = 2, std::uint32_t height = 1,
        std::span<const std::string_view> masters = {}, bool includeScreenshot = true,
        std::string_view playerName = "Courier")
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

        constexpr std::array<std::uint8_t, 3> globalData1Payload = { 0xaa, 0xbb, 0xcc };
        std::vector<std::uint8_t> globalData1;
        appendGlobalDataEntry(globalData1, 0, globalData1Payload);

        std::vector<std::uint8_t> changedForms;
        std::vector<std::uint8_t> changedPayload1 = { 0, 0, 2 };
        appendF32(changedPayload1, -72392.84375f);
        appendF32(changedPayload1, -1240.19275f);
        appendF32(changedPayload1, 8137.58643f);
        appendF32(changedPayload1, -0.06439045f);
        appendF32(changedPayload1, -0.0f);
        appendF32(changedPayload1, 2.93332028f);
        appendDelimiter(changedPayload1);
        constexpr std::array<std::uint8_t, 3> changedPayload2 = { 0x20, 0x21, 0x22 };
        constexpr std::array<std::uint8_t, 4> changedPayload3 = { 0x30, 0x31, 0x32, 0x33 };
        const ChangedFormOffsets changed1 = appendChangedForm(
            changedForms, { 0, 0, 1 }, 0xb0000022, 1, 27, 1, changedPayload1);
        const ChangedFormOffsets changed2 = appendChangedForm(
            changedForms, { 0x40, 0x12, 0x34 }, 0x90abcdef, 2, 26, 2, changedPayload2);
        const ChangedFormOffsets changed3 = appendChangedForm(
            changedForms, { 0x80, 0x56, 0x78 }, 0x01020304, 3, 25, 4, changedPayload3);

        std::vector<std::uint8_t> globalData2;
        appendGlobalDataEntry(globalData2, 1000, std::span<const std::uint8_t>{});
        std::vector<std::uint8_t> refIdAndVisitedWorldspace;
        appendU32(refIdAndVisitedWorldspace, 2);
        appendU32(refIdAndVisitedWorldspace, ESM4::sFONVPlayerReferenceFormId);
        appendU32(refIdAndVisitedWorldspace, 0x000da726);
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
        appendU32(result.mBytes, 1);
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
        result.mBytes.insert(result.mBytes.end(), globalData1.begin(), globalData1.end());
        result.mBytes.insert(result.mBytes.end(), changedForms.begin(), changedForms.end());
        result.mBytes.insert(result.mBytes.end(), globalData2.begin(), globalData2.end());
        result.mBytes.insert(
            result.mBytes.end(), refIdAndVisitedWorldspace.begin(), refIdAndVisitedWorldspace.end());
        result.mBytes.insert(result.mBytes.end(), unknownTableAndTail.begin(), unknownTableAndTail.end());
        return result;
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
        EXPECT_EQ(save.mChangedForms.mEntries[0].mLengthWidth, 1u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mVersion.mValue, 27u);
        EXPECT_EQ(save.mChangedForms.mEntries[0].mDataLength.mValue, 28u);
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
            (ESM4::FONVSaveRange{ source.mChangedFormPayloads[0] + 28, 0 }));
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

    TEST(FONVSaveGame, RejectsCorruptCanonicalPlayerMovementWithoutInterpretingItsRemainder)
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
        EXPECT_EQ(changedCell.mUnparsedSemanticPayloadRanges.size(), 5u);
        EXPECT_EQ(changedCell.mUnparsedSemanticPayloadBytes, 43u);
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

        EXPECT_EQ(save.mUnparsedSemanticPayloadRanges.size(), 7091u);
        EXPECT_EQ(save.mUnparsedSemanticPayloadBytes, 2739018u);
        EXPECT_EQ(save.mStructurallyAccountedRange, (ESM4::FONVSaveRange{ 0, 3395328 }));
        EXPECT_EQ(save.mParsedPrefixRange, save.mStructurallyAccountedRange);
        EXPECT_EQ(save.mUnparsedBodyRange, (ESM4::FONVSaveRange{ 3395328, 0 }));
    }
}

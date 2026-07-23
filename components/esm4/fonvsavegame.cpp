#include "fonvsavegame.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
    constexpr std::string_view sMagic = "FO3SAVEGAME";
    constexpr std::uint8_t sDelimiter = 0x7c;
    constexpr std::size_t sLanguageBytes = 64;
    constexpr std::uint32_t sLanguageDiscriminator = 16384;
    constexpr std::size_t sFileLocationTableBytes = 0x6e;
    constexpr std::size_t sFileLocationFieldCount = 9;
    constexpr std::size_t sFileLocationUnusedBytes
        = sFileLocationTableBytes - sFileLocationFieldCount * sizeof(std::uint32_t);
    constexpr std::size_t sGlobalDataHeaderBytes = 2 * sizeof(std::uint32_t);
    constexpr std::size_t sChangedFormFixedHeaderBytes = 3 + sizeof(std::uint32_t) + 1 + 1;
    constexpr std::size_t sMinimumChangedFormEnvelopeBytes = sChangedFormFixedHeaderBytes + 1;

    std::string errorMessage(std::uint64_t offset, std::string_view message)
    {
        std::ostringstream stream;
        stream << "Invalid Fallout 3/New Vegas save at byte " << offset << ": " << message;
        return stream.str();
    }

    class Cursor
    {
    public:
        Cursor(std::span<const std::uint8_t> data, std::size_t begin, std::size_t end)
            : mData(data)
            , mPosition(begin)
            , mEnd(end)
        {
            if (begin > end || end > data.size())
                throw ESM4::FONVSaveError(begin, "invalid parser bounds");
        }

        std::size_t position() const { return mPosition; }
        std::size_t end() const { return mEnd; }

        std::span<const std::uint8_t> take(std::size_t count, std::string_view description)
        {
            if (count > mEnd - mPosition)
                throw ESM4::FONVSaveError(mPosition, std::string("truncated ") + std::string(description));
            const auto result = mData.subspan(mPosition, count);
            mPosition += count;
            return result;
        }

        std::uint8_t readU8(std::string_view description) { return take(1, description)[0]; }

        std::uint16_t readU16(std::string_view description)
        {
            const auto bytes = take(2, description);
            return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8);
        }

        std::uint32_t readU32(std::string_view description)
        {
            const auto bytes = take(4, description);
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
                | (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        }

        float readF32(std::string_view description) { return std::bit_cast<float>(readU32(description)); }

        double readF64(std::string_view description)
        {
            const std::uint64_t low = readU32(description);
            const std::uint64_t high = readU32(description);
            return std::bit_cast<double>(low | (high << 32));
        }

        std::uint32_t peekU32(std::string_view description)
        {
            const std::size_t saved = mPosition;
            const std::uint32_t result = readU32(description);
            mPosition = saved;
            return result;
        }

        void expectDelimiter(std::string_view description)
        {
            const std::size_t offset = mPosition;
            if (readU8(description) != sDelimiter)
                throw ESM4::FONVSaveError(
                    offset, std::string("bad delimiter before/after ") + std::string(description));
        }

    private:
        std::span<const std::uint8_t> mData;
        std::size_t mPosition;
        std::size_t mEnd;
    };

    ESM4::FONVSaveRange range(std::size_t begin, std::size_t end)
    {
        return { static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end - begin) };
    }

    std::vector<std::uint8_t> copyRange(std::span<const std::uint8_t> data, std::size_t begin, std::size_t end)
    {
        return { data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(end) };
    }

    template <class T, class Read>
    ESM4::FONVSaveField<T> readField(
        Cursor& cursor, std::span<const std::uint8_t> data, Read&& read, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        const T value = read(description);
        const std::size_t end = cursor.position();
        return { value, range(begin, end), copyRange(data, begin, end) };
    }

    template <class T, class Read>
    ESM4::FONVSaveField<T> readDelimitedField(
        Cursor& cursor, std::span<const std::uint8_t> data, Read&& read, std::string_view description)
    {
        ESM4::FONVSaveField<T> result = readField<T>(cursor, data, std::forward<Read>(read), description);
        cursor.expectDelimiter(description);
        return result;
    }

    ESM4::FONVSaveStringField readStringField(
        Cursor& cursor, std::span<const std::uint8_t> data, std::uint16_t maxBytes, std::string_view description)
    {
        const std::size_t encodedBegin = cursor.position();
        auto length = readField<std::uint16_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU16(label); }, description);
        if (length.mValue > maxBytes)
            throw ESM4::FONVSaveError(
                length.mRange.mOffset, std::string(description) + " length exceeds the configured bound");

        cursor.expectDelimiter(description);
        const std::size_t valueBegin = cursor.position();
        const auto valueBytes = cursor.take(length.mValue, description);
        const std::size_t valueEnd = cursor.position();
        // The retail writer omits the otherwise trailing delimiter for an empty string (Save 330's player name is
        // encoded as 00 00 7c immediately followed by the next field).
        if (length.mValue != 0)
            cursor.expectDelimiter(description);

        if (std::find(valueBytes.begin(), valueBytes.end(), 0) != valueBytes.end())
            throw ESM4::FONVSaveError(valueBegin, std::string(description) + " contains an embedded NUL");

        ESM4::FONVSaveStringField result;
        result.mLength = std::move(length);
        result.mValue.assign(reinterpret_cast<const char*>(valueBytes.data()), valueBytes.size());
        result.mValueRange = range(valueBegin, valueEnd);
        result.mEncodedRange = range(encodedBegin, cursor.position());
        result.mRawValue.assign(valueBytes.begin(), valueBytes.end());
        result.mRawEncoded = copyRange(data, encodedBegin, cursor.position());
        return result;
    }

    bool checkedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
    {
        if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
            return false;
        result = left * right;
        return true;
    }

    bool looksLikeLanguage(std::uint32_t firstWord)
    {
        // Fallout 3 puts the width here. New Vegas puts a fixed 64-byte language field here, whose first four ASCII
        // bytes are numerically far outside any retail screenshot dimension.
        return firstWord > sLanguageDiscriminator;
    }

    std::string decodeLanguage(std::span<const std::uint8_t> bytes, std::uint64_t offset)
    {
        const auto nul = std::find(bytes.begin(), bytes.end(), 0);
        if (nul == bytes.begin() || nul == bytes.end())
            throw ESM4::FONVSaveError(offset, "invalid fixed-width language field");

        for (auto it = bytes.begin(); it != nul; ++it)
        {
            if (*it < 0x20 || *it > 0x7e)
                throw ESM4::FONVSaveError(offset, "language contains a non-printable byte");
        }
        if (std::any_of(nul, bytes.end(), [](std::uint8_t value) { return value != 0; }))
            throw ESM4::FONVSaveError(offset, "language padding is not zero-filled");

        return { reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(nul - bytes.begin()) };
    }

    ESM4::FONVSaveRawField readRawField(Cursor& cursor, std::span<const std::uint8_t> data, std::size_t count,
        std::string_view description)
    {
        const std::size_t begin = cursor.position();
        cursor.take(count, description);
        const std::size_t end = cursor.position();
        return { range(begin, end), copyRange(data, begin, end) };
    }

    ESM4::FONVSaveField<std::uint32_t> readReferenceId(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        const auto bytes = cursor.take(3, description);
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 16)
            | (static_cast<std::uint32_t>(bytes[1]) << 8) | static_cast<std::uint32_t>(bytes[2]);
        return { value, range(begin, cursor.position()), std::vector<std::uint8_t>(bytes.begin(), bytes.end()) };
    }

    ESM4::FONVSaveField<std::uint32_t> readVariableWidthField(Cursor& cursor,
        std::span<const std::uint8_t> data, std::uint8_t width, std::string_view description)
    {
        switch (width)
        {
            case 1:
                return readField<std::uint32_t>(cursor, data,
                    [&](std::string_view label) { return static_cast<std::uint32_t>(cursor.readU8(label)); },
                    description);
            case 2:
                return readField<std::uint32_t>(cursor, data,
                    [&](std::string_view label) { return static_cast<std::uint32_t>(cursor.readU16(label)); },
                    description);
            case 4:
                return readField<std::uint32_t>(
                    cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, description);
            default:
                throw ESM4::FONVSaveError(cursor.position(), "invalid integer field width");
        }
    }

    ESM4::FONVSaveField<std::uint32_t> readPackedCount(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        const std::uint8_t first = cursor.readU8(description);
        const std::uint8_t tag = first & 3u;
        std::size_t width = 0;
        switch (tag)
        {
            case 0:
                width = 1;
                break;
            case 1:
                width = 2;
                break;
            case 2:
                width = 4;
                break;
            default:
                throw ESM4::FONVSaveError(begin, std::string(description) + " has an invalid U6to30 width tag");
        }

        std::uint32_t encoded = first;
        for (std::size_t i = 1; i < width; ++i)
            encoded |= static_cast<std::uint32_t>(cursor.readU8(description)) << (8 * i);
        const std::uint32_t value = encoded >> 2;
        if ((width == 2 && value <= 0x3fu) || (width == 4 && value <= 0x3fffu))
            throw ESM4::FONVSaveError(begin, std::string(description) + " uses a non-canonical U6to30 width");

        ESM4::FONVSaveField<std::uint32_t> result
            = { value, range(begin, cursor.position()), copyRange(data, begin, cursor.position()) };
        cursor.expectDelimiter(description);
        return result;
    }

    void validatePackedCountFits(const ESM4::FONVSaveField<std::uint32_t>& count, const Cursor& cursor,
        std::size_t minimumEntryBytes, std::string_view description)
    {
        const std::size_t remaining = cursor.end() - cursor.position();
        if (minimumEntryBytes == 0 || count.mValue > remaining / minimumEntryBytes)
            throw ESM4::FONVSaveError(count.mRange.mOffset, std::string(description) + " overruns its container");
    }

    ESM4::FONVSaveGlobalDataTable parseGlobalDataTable(std::span<const std::uint8_t> data, std::size_t begin,
        std::size_t end, std::uint32_t entryCount, const ESM4::FONVSaveLimits& limits, std::string_view description)
    {
        if (entryCount > limits.mMaxGlobalDataEntries)
            throw ESM4::FONVSaveError(begin, std::string(description) + " count exceeds the configured bound");
        const std::size_t sectionSize = end - begin;
        if (entryCount > sectionSize / sGlobalDataHeaderBytes)
            throw ESM4::FONVSaveError(begin, std::string(description) + " count cannot fit in its section");

        ESM4::FONVSaveGlobalDataTable result;
        result.mRange = range(begin, end);
        result.mEntries.reserve(entryCount);
        Cursor cursor(data, begin, end);
        for (std::uint32_t i = 0; i < entryCount; ++i)
        {
            ESM4::FONVSaveGlobalDataEntry entry;
            const std::size_t envelopeBegin = cursor.position();
            const std::size_t headerBegin = cursor.position();
            entry.mType = readField<std::uint32_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, "global data type");
            entry.mDataLength = readField<std::uint32_t>(cursor, data,
                [&](std::string_view label) { return cursor.readU32(label); }, "global data payload length");
            const std::size_t headerEnd = cursor.position();
            entry.mHeaderRange = range(headerBegin, headerEnd);
            entry.mRawHeader = copyRange(data, headerBegin, headerEnd);

            if (entry.mDataLength.mValue > limits.mMaxEntryPayloadBytes)
                throw ESM4::FONVSaveError(entry.mDataLength.mRange.mOffset,
                    std::string(description) + " payload exceeds the configured bound");
            if (entry.mDataLength.mValue > cursor.end() - cursor.position())
                throw ESM4::FONVSaveError(
                    cursor.position(), std::string(description) + " payload crosses its section boundary");
            entry.mUnparsedPayload
                = readRawField(cursor, data, entry.mDataLength.mValue, "global data payload");
            entry.mEnvelopeRange = range(envelopeBegin, cursor.position());
            result.mEntries.push_back(std::move(entry));
        }
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), std::string(description) + " contains unaccounted bytes");
        return result;
    }

    ESM4::FONVSaveChangedForms parseChangedForms(std::span<const std::uint8_t> data, std::size_t begin,
        std::size_t end, std::uint32_t entryCount, const ESM4::FONVSaveLimits& limits)
    {
        if (entryCount > limits.mMaxChangedForms)
            throw ESM4::FONVSaveError(begin, "changed-form count exceeds the configured bound");
        const std::size_t sectionSize = end - begin;
        if (entryCount > sectionSize / sMinimumChangedFormEnvelopeBytes)
            throw ESM4::FONVSaveError(begin, "changed-form count cannot fit in its section");

        ESM4::FONVSaveChangedForms result;
        result.mRange = range(begin, end);
        result.mEntries.reserve(entryCount);
        Cursor cursor(data, begin, end);
        for (std::uint32_t i = 0; i < entryCount; ++i)
        {
            ESM4::FONVSaveChangedFormEnvelope entry;
            const std::size_t envelopeBegin = cursor.position();
            const std::size_t headerBegin = cursor.position();
            entry.mEncodedReferenceId = readReferenceId(cursor, data, "changed-form reference ID");
            entry.mReferenceId = { entry.mEncodedReferenceId.mRange, entry.mEncodedReferenceId.mRaw };
            const std::uint32_t referenceKind = entry.mEncodedReferenceId.mValue >> 22;
            if (referenceKind == 3)
                throw ESM4::FONVSaveError(
                    entry.mEncodedReferenceId.mRange.mOffset, "invalid changed-form RefID namespace");
            entry.mReferenceKind = static_cast<ESM4::FONVSaveReferenceKind>(referenceKind);
            entry.mReferencePayload = entry.mEncodedReferenceId.mValue & 0x003fffffu;
            entry.mChangeFlags = readField<std::uint32_t>(cursor, data,
                [&](std::string_view label) { return cursor.readU32(label); }, "changed-form flags");
            entry.mRawType = readField<std::uint8_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "changed-form type");
            entry.mChangeType = entry.mRawType.mValue & 0x3f;
            const std::uint8_t lengthCode = entry.mRawType.mValue >> 6;
            if (lengthCode == 3)
                throw ESM4::FONVSaveError(entry.mRawType.mRange.mOffset, "invalid changed-form length-width code");
            entry.mLengthWidth = static_cast<std::uint8_t>(1u << lengthCode);
            entry.mVersion = readField<std::uint8_t>(cursor, data,
                [&](std::string_view label) { return cursor.readU8(label); }, "changed-form version");
            entry.mDataLength
                = readVariableWidthField(cursor, data, entry.mLengthWidth, "changed-form payload length");
            const std::size_t headerEnd = cursor.position();
            entry.mHeaderRange = range(headerBegin, headerEnd);
            entry.mRawHeader = copyRange(data, headerBegin, headerEnd);

            if (entry.mDataLength.mValue > limits.mMaxEntryPayloadBytes)
                throw ESM4::FONVSaveError(
                    entry.mDataLength.mRange.mOffset, "changed-form payload exceeds the configured bound");
            if (entry.mDataLength.mValue > cursor.end() - cursor.position())
                throw ESM4::FONVSaveError(cursor.position(), "changed-form payload crosses its section boundary");
            entry.mUnparsedPayload
                = readRawField(cursor, data, entry.mDataLength.mValue, "changed-form payload");
            entry.mEnvelopeRange = range(envelopeBegin, cursor.position());
            result.mEntries.push_back(std::move(entry));
        }
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), "changed-form section contains unaccounted bytes");
        return result;
    }

    ESM4::FONVSaveFormIdTable parseFormIdTable(Cursor& cursor, std::span<const std::uint8_t> data,
        std::size_t maximumCount, std::string_view description)
    {
        ESM4::FONVSaveFormIdTable result;
        const std::size_t begin = cursor.position();
        result.mCount = readField<std::uint32_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, description);
        if (result.mCount.mValue > maximumCount)
            throw ESM4::FONVSaveError(
                result.mCount.mRange.mOffset, std::string(description) + " count exceeds the configured bound");
        if (result.mCount.mValue > (cursor.end() - cursor.position()) / sizeof(std::uint32_t))
            throw ESM4::FONVSaveError(
                cursor.position(), std::string(description) + " entries cross the section boundary");

        result.mFormIds.reserve(result.mCount.mValue);
        for (std::uint32_t i = 0; i < result.mCount.mValue; ++i)
        {
            result.mFormIds.push_back(readField<std::uint32_t>(cursor, data,
                [&](std::string_view label) { return cursor.readU32(label); }, "FormID table entry"));
        }
        result.mRange = range(begin, cursor.position());
        return result;
    }

    std::optional<std::uint32_t> resolveReferenceId(ESM4::FONVSaveReferenceKind kind, std::uint32_t payload,
        const ESM4::FONVSaveFormIdTable& formIds, std::uint64_t sourceOffset, bool allowNull,
        std::string_view description, bool allowEmptyArraySlot = false)
    {
        switch (kind)
        {
            case ESM4::FONVSaveReferenceKind::FormIdArray:
                if (payload == 0)
                {
                    if (allowNull)
                        return std::nullopt;
                    throw ESM4::FONVSaveError(sourceOffset, std::string(description) + " is a null RefID");
                }
                if (payload > formIds.mFormIds.size())
                    throw ESM4::FONVSaveError(
                        sourceOffset, std::string(description) + " index lies beyond the FormID array");
                if (formIds.mFormIds[payload - 1].mValue == 0)
                {
                    if (allowNull && allowEmptyArraySlot)
                        return std::nullopt;
                    throw ESM4::FONVSaveError(
                        sourceOffset, std::string(description) + " resolves to an empty FormID-array slot");
                }
                return formIds.mFormIds[payload - 1].mValue;
            case ESM4::FONVSaveReferenceKind::DefaultForm:
                if (payload == 0)
                    throw ESM4::FONVSaveError(
                        sourceOffset, std::string(description) + " default RefID resolves to zero");
                return payload;
            case ESM4::FONVSaveReferenceKind::CreatedForm:
                return 0xff000000u | payload;
        }
        throw ESM4::FONVSaveError(sourceOffset, std::string(description) + " has an invalid namespace");
    }

    void resolveChangedFormReferences(
        ESM4::FONVSaveChangedForms& changedForms, const ESM4::FONVSaveFormIdTable& formIds)
    {
        for (ESM4::FONVSaveChangedFormEnvelope& entry : changedForms.mEntries)
        {
            entry.mResolvedFormId = resolveReferenceId(entry.mReferenceKind, entry.mReferencePayload, formIds,
                entry.mEncodedReferenceId.mRange.mOffset, true, "changed-form RefID");
        }
    }

    ESM4::FONVSaveResolvedReferenceId decodeResolvedReferenceId(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds, std::string_view description,
        bool allowNull = false, bool allowEmptyArraySlot = false)
    {
        ESM4::FONVSaveResolvedReferenceId result;
        result.mEncoded = readReferenceId(cursor, data, description);
        const std::uint32_t kind = result.mEncoded.mValue >> 22;
        if (kind == 3)
            throw ESM4::FONVSaveError(
                result.mEncoded.mRange.mOffset, std::string(description) + " has an invalid namespace");
        result.mKind = static_cast<ESM4::FONVSaveReferenceKind>(kind);
        result.mPayload = result.mEncoded.mValue & 0x003fffffu;
        result.mResolvedFormId = resolveReferenceId(
            result.mKind, result.mPayload, formIds, result.mEncoded.mRange.mOffset, allowNull, description,
            allowEmptyArraySlot);
        return result;
    }

    ESM4::FONVSaveResolvedReferenceId decodeDelimitedResolvedReferenceId(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds, std::string_view description,
        bool allowNull = false, bool allowEmptyArraySlot = false)
    {
        ESM4::FONVSaveResolvedReferenceId result
            = decodeResolvedReferenceId(cursor, data, formIds, description, allowNull, allowEmptyArraySlot);
        cursor.expectDelimiter(description);
        return result;
    }

    ESM4::FONVSaveQuestStageLog parseQuestStageLog(
        Cursor& cursor, std::span<const std::uint8_t> data)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSaveQuestStageLog result;
        result.mLogEntry = readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "quest-stage log-entry ID");
        result.mHasLogData = readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "quest-stage has-log-data");
        if (result.mHasLogData.mValue != 0)
        {
            result.mLogDataUnknown = readField<std::uint16_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU16(label); },
                "quest-stage log-data unknown");
            result.mLogDataValue = readDelimitedField<std::uint16_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU16(label); },
                "quest-stage log-data value");
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSaveQuestStage parseQuestStage(Cursor& cursor, std::span<const std::uint8_t> data)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSaveQuestStage result;
        result.mStage = readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "quest-stage ID");
        result.mStatus = readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "quest-stage status");
        result.mLogCount = readPackedCount(cursor, data, "quest-stage log count");
        validatePackedCountFits(result.mLogCount, cursor, 4, "quest-stage log count");
        result.mLogs.reserve(result.mLogCount.mValue);
        for (std::uint32_t i = 0; i < result.mLogCount.mValue; ++i)
            result.mLogs.push_back(parseQuestStageLog(cursor, data));
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSaveQuestScriptVariable parseQuestScriptVariable(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSaveQuestScriptVariable result;
        result.mFlagAndVariableId = readDelimitedField<std::uint32_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU32(label); },
            "quest script FlagAndVarID");
        if ((result.mFlagAndVariableId.mValue & 0x80000000u) != 0)
        {
            result.mReferenceValue = decodeDelimitedResolvedReferenceId(
                cursor, data, formIds, "quest script reference variable", true, true);
        }
        else
        {
            result.mNumericValue = readDelimitedField<double>(
                cursor, data, [&](std::string_view label) { return cursor.readF64(label); },
                "quest script numeric variable");
            if (!std::isfinite(result.mNumericValue->mValue))
            {
                throw ESM4::FONVSaveError(
                    result.mNumericValue->mRange.mOffset, "non-finite quest script numeric variable");
            }
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSaveQuestChange parseQuestChange(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& entry, const ESM4::FONVSaveFormIdTable& formIds)
    {
        constexpr std::uint32_t supportedFlags = 0xe0000007u;
        if (entry.mVersion.mValue < 0x15)
            throw ESM4::FONVSaveError(entry.mVersion.mRange.mOffset, "unsupported QUST change-form version");
        if (!entry.mResolvedFormId || (*entry.mResolvedFormId & 0x00ffffffu) == 0)
            throw ESM4::FONVSaveError(entry.mEncodedReferenceId.mRange.mOffset, "QUST change-form has no identity");
        if ((entry.mChangeFlags.mValue & ~supportedFlags) != 0)
            throw ESM4::FONVSaveError(entry.mChangeFlags.mRange.mOffset, "unsupported QUST change flags");

        const std::size_t begin = static_cast<std::size_t>(entry.mUnparsedPayload.mRange.mOffset);
        Cursor cursor(data, begin, static_cast<std::size_t>(entry.mUnparsedPayload.mRange.end()));
        ESM4::FONVSaveQuestChange result;
        result.mResolvedFormId = *entry.mResolvedFormId;
        result.mChangeFlags = entry.mChangeFlags.mValue;
        if ((result.mChangeFlags & 0x00000001u) != 0)
        {
            result.mFormFlags = readDelimitedField<std::uint32_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, "quest form flags");
        }
        if ((result.mChangeFlags & 0x00000002u) != 0)
        {
            result.mQuestFlags = readDelimitedField<std::uint8_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "quest flags");
        }
        if ((result.mChangeFlags & 0x00000004u) != 0)
        {
            result.mScriptDelay = readDelimitedField<float>(
                cursor, data, [&](std::string_view label) { return cursor.readF32(label); }, "quest script delay");
            if (!std::isfinite(result.mScriptDelay->mValue))
                throw ESM4::FONVSaveError(result.mScriptDelay->mRange.mOffset, "non-finite quest script delay");
        }
        if ((result.mChangeFlags & 0x80000000u) != 0)
        {
            result.mStageCount = readPackedCount(cursor, data, "quest-stage count");
            validatePackedCountFits(*result.mStageCount, cursor, 6, "quest-stage count");
            result.mStages.reserve(result.mStageCount->mValue);
            for (std::uint32_t i = 0; i < result.mStageCount->mValue; ++i)
                result.mStages.push_back(parseQuestStage(cursor, data));
        }
        if ((result.mChangeFlags & 0x40000000u) != 0)
        {
            result.mVariableCount = readPackedCount(cursor, data, "quest script-variable count");
            // A reference-valued entry is 9 bytes including its two field delimiters; numeric entries are 14.
            validatePackedCountFits(*result.mVariableCount, cursor, 9, "quest script-variable count");
            result.mVariables.reserve(result.mVariableCount->mValue);
            for (std::uint32_t i = 0; i < result.mVariableCount->mValue; ++i)
                result.mVariables.push_back(parseQuestScriptVariable(cursor, data, formIds));
            result.mHasEventState = readDelimitedField<std::uint8_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU8(label); },
                "quest script event-state presence");
            if (result.mHasEventState->mValue != 0)
            {
                result.mEventState = readRawField(cursor, data, 8, "quest script event state");
                cursor.expectDelimiter("quest script event state");
            }
            result.mOnLoad = readDelimitedField<std::uint8_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU8(label); },
                "quest script OnLoad state");
        }
        if ((result.mChangeFlags & 0x20000000u) != 0)
        {
            result.mObjectiveCount = readPackedCount(cursor, data, "quest-objective count");
            validatePackedCountFits(*result.mObjectiveCount, cursor, 10, "quest-objective count");
            result.mObjectives.reserve(result.mObjectiveCount->mValue);
            for (std::uint32_t i = 0; i < result.mObjectiveCount->mValue; ++i)
            {
                const std::size_t objectiveBegin = cursor.position();
                ESM4::FONVSaveQuestObjective objective;
                objective.mObjective = readDelimitedField<std::uint32_t>(
                    cursor, data, [&](std::string_view label) { return cursor.readU32(label); },
                    "quest-objective ID");
                objective.mData = readDelimitedField<std::uint32_t>(
                    cursor, data, [&](std::string_view label) { return cursor.readU32(label); },
                    "quest-objective data");
                objective.mRange = range(objectiveBegin, cursor.position());
                objective.mRaw = copyRange(data, objectiveBegin, cursor.position());
                result.mObjectives.push_back(std::move(objective));
            }
        }
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), "QUST change-form payload contains unaccounted bytes");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSaveFactionChange parseFactionChange(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& entry, const ESM4::FONVSaveFormIdTable& formIds)
    {
        constexpr std::uint32_t supportedFlags = 0x80000007u;
        if (entry.mVersion.mValue < 0x15)
            throw ESM4::FONVSaveError(entry.mVersion.mRange.mOffset, "unsupported FACT change-form version");
        if (!entry.mResolvedFormId || (*entry.mResolvedFormId & 0x00ffffffu) == 0)
            throw ESM4::FONVSaveError(entry.mEncodedReferenceId.mRange.mOffset, "FACT change-form has no identity");
        if ((entry.mChangeFlags.mValue & ~supportedFlags) != 0)
            throw ESM4::FONVSaveError(entry.mChangeFlags.mRange.mOffset, "unsupported FACT change flags");

        const std::size_t begin = static_cast<std::size_t>(entry.mUnparsedPayload.mRange.mOffset);
        Cursor cursor(data, begin, static_cast<std::size_t>(entry.mUnparsedPayload.mRange.end()));
        ESM4::FONVSaveFactionChange result;
        result.mResolvedFormId = *entry.mResolvedFormId;
        result.mChangeFlags = entry.mChangeFlags.mValue;
        if ((result.mChangeFlags & 0x00000001u) != 0)
        {
            result.mFormFlags = readDelimitedField<std::uint32_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, "faction form flags");
        }
        if ((result.mChangeFlags & 0x00000004u) != 0)
        {
            result.mReactionCount = readPackedCount(cursor, data, "faction reaction count");
            validatePackedCountFits(*result.mReactionCount, cursor, 14, "faction reaction count");
            result.mReactions.reserve(result.mReactionCount->mValue);
            for (std::uint32_t i = 0; i < result.mReactionCount->mValue; ++i)
            {
                const std::size_t reactionBegin = cursor.position();
                ESM4::FONVSaveFactionReaction reaction;
                reaction.mFaction
                    = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "faction reaction FACT RefID");
                reaction.mModifier = readDelimitedField<std::int32_t>(cursor, data,
                    [&](std::string_view label) { return std::bit_cast<std::int32_t>(cursor.readU32(label)); },
                    "faction reaction modifier");
                reaction.mReaction = readDelimitedField<std::uint32_t>(
                    cursor, data, [&](std::string_view label) { return cursor.readU32(label); },
                    "faction group-combat reaction");
                reaction.mRange = range(reactionBegin, cursor.position());
                reaction.mRaw = copyRange(data, reactionBegin, cursor.position());
                result.mReactions.push_back(std::move(reaction));
            }
        }
        if ((result.mChangeFlags & 0x00000002u) != 0)
        {
            result.mFactionFlags = readDelimitedField<std::uint32_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, "faction flags");
        }
        if ((result.mChangeFlags & 0x80000000u) != 0)
        {
            result.mCrimeCount44 = readDelimitedField<std::uint32_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, "faction crime count 44");
            result.mCrimeCount48 = readDelimitedField<std::uint32_t>(
                cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, "faction crime count 48");
        }
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), "FACT change-form payload contains unaccounted bytes");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSaveGlobalVariablesState parseGlobalVariablesState(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveGlobalDataEntry& entry, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = static_cast<std::size_t>(entry.mUnparsedPayload.mRange.mOffset);
        Cursor cursor(data, begin, static_cast<std::size_t>(entry.mUnparsedPayload.mRange.end()));
        ESM4::FONVSaveGlobalVariablesState result;
        result.mCount = readPackedCount(cursor, data, "global-variable count");
        validatePackedCountFits(result.mCount, cursor, 9, "global-variable count");
        result.mVariables.reserve(result.mCount.mValue);
        for (std::uint32_t i = 0; i < result.mCount.mValue; ++i)
        {
            const std::size_t variableBegin = cursor.position();
            ESM4::FONVSaveGlobalVariable variable;
            variable.mVariable = decodeDelimitedResolvedReferenceId(
                cursor, data, formIds, "global-variable RefID");
            variable.mValue = readDelimitedField<float>(cursor, data,
                [&](std::string_view label) { return cursor.readF32(label); }, "global-variable value");
            if (!std::isfinite(variable.mValue.mValue))
                throw ESM4::FONVSaveError(variable.mValue.mRange.mOffset, "non-finite global-variable value");
            variable.mRange = range(variableBegin, cursor.position());
            variable.mRaw = copyRange(data, variableBegin, cursor.position());
            result.mVariables.push_back(std::move(variable));
        }
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), "global-variable payload contains unaccounted bytes");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSaveSkyState parseSkyState(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveGlobalDataEntry& entry, const ESM4::FONVSaveFormIdTable& formIds)
    {
        constexpr std::size_t sSkyPayloadBytes = 4 * (3 + 1) + 11 * (sizeof(std::uint32_t) + 1);
        if (entry.mUnparsedPayload.mRange.mSize != sSkyPayloadBytes)
            throw ESM4::FONVSaveError(entry.mUnparsedPayload.mRange.mOffset, "unsupported Sky payload size");

        const std::size_t begin = static_cast<std::size_t>(entry.mUnparsedPayload.mRange.mOffset);
        Cursor cursor(data, begin, static_cast<std::size_t>(entry.mUnparsedPayload.mRange.end()));
        auto readWeather = [&](std::string_view description) {
            ESM4::FONVSaveResolvedReferenceId result
                = decodeResolvedReferenceId(cursor, data, formIds, description, true);
            cursor.expectDelimiter(description);
            return result;
        };
        auto readFloat = [&](std::string_view description) {
            ESM4::FONVSaveField<float> result = readDelimitedField<float>(cursor, data,
                [&](std::string_view label) { return cursor.readF32(label); }, description);
            if (!std::isfinite(result.mValue))
                throw ESM4::FONVSaveError(result.mRange.mOffset, std::string("non-finite ") + std::string(description));
            return result;
        };
        auto readUint = [&](std::string_view description) {
            return readDelimitedField<std::uint32_t>(cursor, data,
                [&](std::string_view label) { return cursor.readU32(label); }, description);
        };

        ESM4::FONVSaveSkyState result;
        result.mCurrentWeather = readWeather("Sky current-weather RefID");
        result.mTransitionWeather = readWeather("Sky transition-weather RefID");
        result.mDefaultWeather = readWeather("Sky default-weather RefID");
        result.mOverrideWeather = readWeather("Sky override-weather RefID");
        result.mGameHour = readFloat("Sky game hour");
        result.mLastUpdateHour = readFloat("Sky last-update hour");
        result.mWeatherPercent = readFloat("Sky weather percent");
        result.mFlags = readUint("Sky flags");
        result.mUnknown110 = readFloat("Sky field 0x110");
        for (ESM4::FONVSaveField<float>& value : result.mVectorB4)
            value = readFloat("Sky vector 0xB4 component");
        result.mUnknownE4 = readUint("Sky field 0xE4");
        result.mFogPower = readFloat("Sky fog power");
        result.mSkyMode = readUint("Sky mode");
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), "Sky payload contains unaccounted bytes");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerReferenceMovement parsePlayerReferenceMovement(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player, const ESM4::FONVSaveFormIdTable& formIds)
    {
        constexpr std::size_t movementBytes = 3 + 6 * sizeof(float) + 1;
        if (player.mUnparsedPayload.mRange.mSize < movementBytes)
            throw ESM4::FONVSaveError(
                player.mUnparsedPayload.mRange.mOffset, "truncated canonical player reference-movement block");

        const std::size_t begin = static_cast<std::size_t>(player.mUnparsedPayload.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(player.mUnparsedPayload.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerReferenceMovement result;
        result.mCellOrWorldspace
            = decodeResolvedReferenceId(cursor, data, formIds, "player movement CELL/WRLD RefID");
        for (ESM4::FONVSaveField<float>& coordinate : result.mPosition)
        {
            coordinate = readField<float>(cursor, data,
                [&](std::string_view label) { return cursor.readF32(label); }, "player movement position");
            if (!std::isfinite(coordinate.mValue))
                throw ESM4::FONVSaveError(coordinate.mRange.mOffset, "non-finite player movement position");
        }
        for (ESM4::FONVSaveField<float>& angle : result.mRotationRadians)
        {
            angle = readField<float>(cursor, data,
                [&](std::string_view label) { return cursor.readF32(label); }, "player movement rotation");
            if (!std::isfinite(angle.mValue))
                throw ESM4::FONVSaveError(angle.mRange.mOffset, "non-finite player movement rotation");
        }
        result.mTerminator = readField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "player movement terminator");
        if (result.mTerminator.mValue != sDelimiter)
            throw ESM4::FONVSaveError(result.mTerminator.mRange.mOffset, "bad player movement terminator");
        result.mRange = range(begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical player ACHR payload");
        return result;
    }

    ESM4::FONVSavePlayerActorValueData parsePlayerActorValueData(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player, const ESM4::FONVSavePlayerReferenceMovement& movement)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(
                player.mVersion.mRange.mOffset, "unsupported canonical player actor-value changed-form version");
        if (movement.mUnparsedRemainder.mRange.mSize < ESM4::sFONVPlayerActorValueDataBytes)
        {
            throw ESM4::FONVSaveError(
                movement.mUnparsedRemainder.mRange.mOffset, "truncated canonical player actor-value data");
        }

        const std::size_t begin = static_cast<std::size_t>(movement.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(movement.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerActorValueData result;
        const auto readActorValues = [&](auto& values, std::string_view description) {
            for (ESM4::FONVSaveField<float>& value : values)
            {
                value = readDelimitedField<float>(cursor, data,
                    [&](std::string_view label) { return cursor.readF32(label); }, description);
                if (!std::isfinite(value.mValue))
                {
                    throw ESM4::FONVSaveError(
                        value.mRange.mOffset, std::string("non-finite canonical player ") + std::string(description));
                }
            }
        };

        readActorValues(result.mActorValues244, "ActorValues244 value");
        readActorValues(result.mActorValues378, "ActorValues378 value");
        readActorValues(result.mActorValues4B0, "ActorValues4B0 value");
        result.mUnk4AC = readDelimitedField<std::uint32_t>(cursor, data,
            [&](std::string_view label) { return cursor.readU32(label); }, "player Unk4AC");
        result.mRange = range(begin, cursor.position());
        if (result.mRange.mSize != ESM4::sFONVPlayerActorValueDataBytes)
            throw ESM4::FONVSaveError(begin, "canonical player actor-value data has an invalid encoded size");
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical player ACHR payload");
        return result;
    }

    ESM4::FONVSavePlayerActorExtraData parsePlayerActorExtraData(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerActorExtraData result;
        result.mType = readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "player actor extra type");
        switch (result.mType.mValue)
        {
            case ESM4::sFONVExtraPackageStartLocationType:
            {
                result.mPackageStartCellOrWorldspace = decodeDelimitedResolvedReferenceId(
                    cursor, data, formIds, "player package-start CELL/WRLD RefID");
                std::array<ESM4::FONVSaveField<float>, 3> position;
                for (ESM4::FONVSaveField<float>& component : position)
                {
                    component = readField<float>(cursor, data,
                        [&](std::string_view label) { return cursor.readF32(label); },
                        "player package-start position");
                    if (!std::isfinite(component.mValue))
                    {
                        throw ESM4::FONVSaveError(
                            component.mRange.mOffset, "non-finite player package-start position");
                    }
                }
                cursor.expectDelimiter("player package-start position");
                result.mPackageStartPosition = std::move(position);
                result.mPackageStartUnknown = readDelimitedField<std::uint32_t>(cursor, data,
                    [&](std::string_view label) { return cursor.readU32(label); },
                    "player package-start unknown word");
                break;
            }
            case ESM4::sFONVExtraFollowerArrayType:
                result.mFollowerCount = readPackedCount(cursor, data, "player follower count");
                validatePackedCountFits(*result.mFollowerCount, cursor, 4, "player follower count");
                result.mFollowers.reserve(result.mFollowerCount->mValue);
                for (std::uint32_t i = 0; i < result.mFollowerCount->mValue; ++i)
                {
                    result.mFollowers.push_back(decodeDelimitedResolvedReferenceId(
                        cursor, data, formIds, "player follower RefID"));
                }
                break;
            case ESM4::sFONVExtraFactionChangesType:
            {
                result.mFactionChangeCount = readPackedCount(cursor, data, "player faction-change count");
                validatePackedCountFits(*result.mFactionChangeCount, cursor, 6, "player faction-change count");
                result.mFactionChanges.reserve(result.mFactionChangeCount->mValue);
                for (std::uint32_t i = 0; i < result.mFactionChangeCount->mValue; ++i)
                {
                    const std::size_t factionBegin = cursor.position();
                    ESM4::FONVSavePlayerFactionChange faction;
                    faction.mFaction = decodeDelimitedResolvedReferenceId(
                        cursor, data, formIds, "player faction-change faction RefID");
                    faction.mRank = readDelimitedField<std::int8_t>(cursor, data,
                        [&](std::string_view label) { return std::bit_cast<std::int8_t>(cursor.readU8(label)); },
                        "player faction-change rank");
                    faction.mRange = range(factionBegin, cursor.position());
                    faction.mRaw = copyRange(data, factionBegin, cursor.position());
                    result.mFactionChanges.push_back(std::move(faction));
                }
                break;
            }
            case ESM4::sFONVExtraActorCauseType:
                result.mActorCause = readDelimitedField<std::uint32_t>(cursor, data,
                    [&](std::string_view label) { return cursor.readU32(label); }, "player actor-cause word");
                break;
            case ESM4::sFONVExtraEncounterZoneType:
                result.mEncounterZone
                    = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player encounter-zone RefID");
                break;
            case ESM4::sFONVExtraSayToTopicInfoType:
                result.mSayToTopic
                    = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player SayTo topic RefID");
                result.mSayToTopicInfo
                    = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player SayTo topic-info RefID");
                result.mSayToUnknown = readDelimitedField<std::uint8_t>(
                    cursor, data, [&](std::string_view label) { return cursor.readU8(label); },
                    "player SayTo unknown byte");
                break;
            default:
                throw ESM4::FONVSaveError(
                    result.mType.mRange.mOffset, "unsupported canonical player actor extra type");
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerInventoryExtraData parsePlayerInventoryExtraData(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerInventoryExtraData result;
        result.mType = readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, "player inventory extra type");
        switch (result.mType.mValue)
        {
            case ESM4::sFONVExtraWornType:
                break;
            case ESM4::sFONVExtraCountType:
                result.mCount = readDelimitedField<std::int16_t>(cursor, data,
                    [&](std::string_view label) { return std::bit_cast<std::int16_t>(cursor.readU16(label)); },
                    "player inventory ExtraCount");
                break;
            case ESM4::sFONVExtraHealthType:
                result.mHealth = readDelimitedField<float>(cursor, data,
                    [&](std::string_view label) { return cursor.readF32(label); }, "player inventory ExtraHealth");
                if (!std::isfinite(result.mHealth->mValue))
                    throw ESM4::FONVSaveError(result.mHealth->mRange.mOffset, "non-finite player inventory ExtraHealth");
                break;
            case ESM4::sFONVExtraHotkeyType:
                result.mHotkey = readDelimitedField<std::uint8_t>(
                    cursor, data, [&](std::string_view label) { return cursor.readU8(label); },
                    "player inventory ExtraHotkey");
                if (result.mHotkey->mValue >= 8)
                    throw ESM4::FONVSaveError(result.mHotkey->mRange.mOffset, "invalid player inventory ExtraHotkey");
                break;
            case ESM4::sFONVExtraAmmoType:
                result.mAmmo
                    = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player inventory ExtraAmmo RefID");
                result.mAmmoCount = readDelimitedField<std::int32_t>(cursor, data,
                    [&](std::string_view label) { return std::bit_cast<std::int32_t>(cursor.readU32(label)); },
                    "player inventory ExtraAmmo count");
                break;
            default:
                throw ESM4::FONVSaveError(
                    result.mType.mRange.mOffset, "unsupported canonical player inventory extra type");
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerInventoryExtendData parsePlayerInventoryExtendData(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerInventoryExtendData result;
        result.mExtraDataCount = readPackedCount(cursor, data, "player inventory stack extra-data count");
        validatePackedCountFits(result.mExtraDataCount, cursor, 2, "player inventory stack extra-data count");
        result.mExtraData.reserve(result.mExtraDataCount.mValue);
        for (std::uint32_t i = 0; i < result.mExtraDataCount.mValue; ++i)
            result.mExtraData.push_back(parsePlayerInventoryExtraData(cursor, data, formIds));
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerInventoryEntry parsePlayerInventoryEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerInventoryEntry result;
        result.mType = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player inventory type RefID");
        result.mDelta = readDelimitedField<std::int32_t>(cursor, data,
            [&](std::string_view label) { return std::bit_cast<std::int32_t>(cursor.readU32(label)); },
            "player inventory delta");
        result.mExtendDataCount = readPackedCount(cursor, data, "player inventory extend-data count");
        validatePackedCountFits(result.mExtendDataCount, cursor, 2, "player inventory extend-data count");
        result.mExtendData.reserve(result.mExtendDataCount.mValue);
        for (std::uint32_t i = 0; i < result.mExtendDataCount.mValue; ++i)
            result.mExtendData.push_back(parsePlayerInventoryExtendData(cursor, data, formIds));
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerProcessInventoryData parsePlayerProcessInventoryData(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player, const ESM4::FONVSavePlayerActorValueData& actorValues,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(
                player.mVersion.mRange.mOffset, "unsupported canonical player process/inventory changed-form version");

        const std::size_t begin = static_cast<std::size_t>(actorValues.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(actorValues.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerProcessInventoryData result;
        result.mProcessLevel = readDelimitedField<std::int8_t>(cursor, data,
            [&](std::string_view label) { return std::bit_cast<std::int8_t>(cursor.readU8(label)); },
            "player process level");

        constexpr std::uint32_t changeReferenceScale = 0x00000010;
        if ((player.mChangeFlags.mValue & changeReferenceScale) != 0)
        {
            result.mReferenceScale = readDelimitedField<float>(cursor, data,
                [&](std::string_view label) { return cursor.readF32(label); }, "player reference scale");
            if (!std::isfinite(result.mReferenceScale->mValue) || result.mReferenceScale->mValue <= 0.f)
            {
                throw ESM4::FONVSaveError(
                    result.mReferenceScale->mRange.mOffset, "invalid player reference scale");
            }
        }
        result.mActorExtraDataCount = readPackedCount(cursor, data, "player actor extra-data count");
        validatePackedCountFits(result.mActorExtraDataCount, cursor, 2, "player actor extra-data count");
        result.mActorExtraData.reserve(result.mActorExtraDataCount.mValue);
        for (std::uint32_t i = 0; i < result.mActorExtraDataCount.mValue; ++i)
            result.mActorExtraData.push_back(parsePlayerActorExtraData(cursor, data, formIds));

        result.mInventoryEntryCount = readPackedCount(cursor, data, "player inventory entry count");
        validatePackedCountFits(result.mInventoryEntryCount, cursor, 11, "player inventory entry count");
        result.mInventoryEntries.reserve(result.mInventoryEntryCount.mValue);
        for (std::uint32_t i = 0; i < result.mInventoryEntryCount.mValue; ++i)
            result.mInventoryEntries.push_back(parsePlayerInventoryEntry(cursor, data, formIds));

        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical player ACHR payload");
        return result;
    }
    ESM4::FONVSaveField<std::uint8_t> readPlayerProcessU8(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        return readDelimitedField<std::uint8_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU8(label); }, description);
    }

    ESM4::FONVSaveField<std::int8_t> readPlayerProcessS8(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        return readDelimitedField<std::int8_t>(cursor, data,
            [&](std::string_view label) { return std::bit_cast<std::int8_t>(cursor.readU8(label)); }, description);
    }

    ESM4::FONVSaveField<std::uint16_t> readPlayerProcessU16(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        return readDelimitedField<std::uint16_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU16(label); }, description);
    }

    ESM4::FONVSaveField<std::int16_t> readPlayerProcessS16(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        return readDelimitedField<std::int16_t>(cursor, data,
            [&](std::string_view label) { return std::bit_cast<std::int16_t>(cursor.readU16(label)); }, description);
    }

    ESM4::FONVSaveField<std::uint32_t> readPlayerProcessU32(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        return readDelimitedField<std::uint32_t>(
            cursor, data, [&](std::string_view label) { return cursor.readU32(label); }, description);
    }

    ESM4::FONVSaveField<std::int32_t> readPlayerProcessS32(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        return readDelimitedField<std::int32_t>(cursor, data,
            [&](std::string_view label) { return std::bit_cast<std::int32_t>(cursor.readU32(label)); }, description);
    }

    ESM4::FONVSaveField<float> readPlayerProcessFloat(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        ESM4::FONVSaveField<float> result = readDelimitedField<float>(
            cursor, data, [&](std::string_view label) { return cursor.readF32(label); }, description);
        if (!std::isfinite(result.mValue))
            throw ESM4::FONVSaveError(result.mRange.mOffset, std::string("non-finite ") + std::string(description));
        return result;
    }

    ESM4::FONVSavePlayerProcessVector3 parsePlayerProcessVector3(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerProcessVector3 result;
        for (ESM4::FONVSaveField<float>& component : result.mComponents)
        {
            component = readField<float>(
                cursor, data, [&](std::string_view label) { return cursor.readF32(label); }, description);
            if (!std::isfinite(component.mValue))
            {
                throw ESM4::FONVSaveError(
                    component.mRange.mOffset, std::string("non-finite ") + std::string(description));
            }
        }
        cursor.expectDelimiter(description);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerProcessSubBuffer parsePlayerProcessSubBuffer(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerProcessSubBuffer result;
        result.mLength = readPackedCount(cursor, data, description);
        if (result.mLength.mValue > cursor.end() - cursor.position())
            throw ESM4::FONVSaveError(result.mLength.mRange.mOffset, std::string(description) + " overruns its container");
        result.mData = readRawField(cursor, data, result.mLength.mValue, description);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerProcessModifier parsePlayerProcessModifier(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerProcessModifier result;
        result.mActorValue = readPlayerProcessU8(cursor, data, description);
        result.mModifier = readPlayerProcessFloat(cursor, data, description);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    void rejectPlayerProcessPackageData(
        const ESM4::FONVSaveResolvedReferenceId& package, std::string_view description)
    {
        if (package.mResolvedFormId.has_value())
        {
            throw ESM4::FONVSaveError(
                package.mEncoded.mRange.mOffset, std::string("unsupported ") + std::string(description) + " data");
        }
    }

    ESM4::FONVSavePlayerMobileObjectBaseState parsePlayerMobileObjectBaseState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerMobileObjectBaseState result;
        for (ESM4::FONVSaveField<std::int8_t>& value : result.mBytes084_085_07C_07F_080_07D_07E_086)
            value = readPlayerProcessS8(cursor, data, "player MobileObject base byte");
        result.mUnk074 = readPlayerProcessU32(cursor, data, "player MobileObject Unk074");
        result.mUnk078 = readPlayerProcessU32(cursor, data, "player MobileObject Unk078");
        result.mByt081 = readPlayerProcessS8(cursor, data, "player MobileObject Byt081");
        result.mByt083 = readPlayerProcessU8(cursor, data, "player MobileObject Byt083");
        result.mUnk06C
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player MobileObject Unk06C RefID", true);
        result.mUnk070
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player MobileObject Unk070 RefID", true);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerBaseProcessState parsePlayerBaseProcessState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerBaseProcessState result;
        result.mUnk01C = readPlayerProcessU32(cursor, data, "player base-process Unk01C");
        result.mUnk020 = readPlayerProcessU32(cursor, data, "player base-process Unk020");
        result.mUnk024 = readPlayerProcessU32(cursor, data, "player base-process Unk024");
        result.mPackage
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player base-process package RefID", true);
        rejectPlayerProcessPackageData(result.mPackage, "player base-process package");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerLowProcessState parsePlayerLowProcessState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds, std::uint32_t changeFlags)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerLowProcessState result;
        result.mByt030 = readPlayerProcessU8(cursor, data, "player low-process Byt030");
        result.mUnk0A4 = readPlayerProcessU32(cursor, data, "player low-process Unk0A4");
        result.mBoundObject
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player low-process bound-object RefID", true);
        result.mUnk058 = readPlayerProcessU32(cursor, data, "player low-process Unk058");
        result.mUnk0A8 = readPlayerProcessU32(cursor, data, "player low-process Unk0A8");
        result.mUnk0AC = readPlayerProcessU32(cursor, data, "player low-process Unk0AC");
        result.mByt0B0 = readPlayerProcessU8(cursor, data, "player low-process Byt0B0");
        result.mWrd050 = readPlayerProcessU16(cursor, data, "player low-process Wrd050");
        for (ESM4::FONVSaveField<float>& value : result.mUnk038)
            value = readPlayerProcessFloat(cursor, data, "player low-process Unk038");
        for (ESM4::FONVSaveResolvedReferenceId& value : result.mUnk040_044_048_FormList_054)
            value = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player low-process RefID", true);
        result.mList006CCount = readPackedCount(cursor, data, "player low-process List006C count");
        validatePackedCountFits(result.mList006CCount, cursor, 4, "player low-process List006C count");
        result.mList006C.reserve(result.mList006CCount.mValue);
        for (std::uint32_t i = 0; i < result.mList006CCount.mValue; ++i)
            result.mList006C.push_back(
                decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player low-process List006C RefID", true));

        constexpr std::uint32_t damageModifiersChanged = 1u << 21;
        if ((changeFlags & damageModifiersChanged) != 0)
        {
            result.mDamageModifierCount = readPackedCount(cursor, data, "player damage-modifier count");
            validatePackedCountFits(*result.mDamageModifierCount, cursor, 7, "player damage-modifier count");
            result.mDamageModifiers.reserve(result.mDamageModifierCount->mValue);
            for (std::uint32_t i = 0; i < result.mDamageModifierCount->mValue; ++i)
                result.mDamageModifiers.push_back(parsePlayerProcessModifier(cursor, data, "player damage modifier"));
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerMiddleLowProcessState parsePlayerMiddleLowProcessState(
        Cursor& cursor, std::span<const std::uint8_t> data, std::uint32_t changeFlags)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerMiddleLowProcessState result;
        result.mUnk0B4 = readPlayerProcessU32(cursor, data, "player middle-low-process Unk0B4");
        constexpr std::uint32_t tempModifiersChanged = 1u << 20;
        if ((changeFlags & tempModifiersChanged) != 0)
        {
            result.mTempModifierCount = readPackedCount(cursor, data, "player temp-modifier count");
            validatePackedCountFits(*result.mTempModifierCount, cursor, 7, "player temp-modifier count");
            result.mTempModifiers.reserve(result.mTempModifierCount->mValue);
            for (std::uint32_t i = 0; i < result.mTempModifierCount->mValue; ++i)
                result.mTempModifiers.push_back(parsePlayerProcessModifier(cursor, data, "player temp modifier"));
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerMiddleHighList230Entry parsePlayerMiddleHighList230Entry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerMiddleHighList230Entry result;
        result.mBoundObject = decodeDelimitedResolvedReferenceId(
            cursor, data, formIds, "player middle-high-process List230 bound-object RefID", true);
        result.mUnknown = readPlayerProcessS32(cursor, data, "player middle-high-process List230 Unknown");
        result.mUnk008 = readPlayerProcessU32(cursor, data, "player middle-high-process List230 Unk008");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mBytes00C_00D_00E_00F_010_011)
            value = readPlayerProcessU8(cursor, data, "player middle-high-process List230 byte");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerMiddleHighProcessState parsePlayerMiddleHighProcessState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds, std::uint32_t changeFlags)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerMiddleHighProcessState result;
        result.mUnk134 = readPlayerProcessU8(cursor, data, "player middle-high-process Unk134");
        result.mWeaponOut = readPlayerProcessU8(cursor, data, "player middle-high-process weapon-out state");
        result.mUnk168 = readPlayerProcessU8(cursor, data, "player middle-high-process Unk168");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk170_174_108)
            value = readPlayerProcessU32(cursor, data, "player middle-high-process initial word");
        result.mUnk1DA = readPlayerProcessU8(cursor, data, "player middle-high-process Unk1DA");
        result.mCoords0E4 = parsePlayerProcessVector3(cursor, data, "player middle-high-process Coords0E4");
        result.mUnk0DC = readPlayerProcessU32(cursor, data, "player middle-high-process Unk0DC");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt13D_144_156)
            value = readPlayerProcessU8(cursor, data, "player middle-high-process control byte");
        result.mWrd154 = readPlayerProcessU16(cursor, data, "player middle-high-process Wrd154");
        result.mCoords148 = parsePlayerProcessVector3(cursor, data, "player middle-high-process Coords148");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mBool_Byt0E0_188_189)
            value = readPlayerProcessU8(cursor, data, "player middle-high-process state byte");
        result.mUnk0D8 = readPlayerProcessU32(cursor, data, "player middle-high-process Unk0D8");
        result.mByt18B = readPlayerProcessU8(cursor, data, "player middle-high-process Byt18B");
        result.mUnk1D0 = readPlayerProcessU32(cursor, data, "player middle-high-process Unk1D0");
        result.mUnk1D4 = readPlayerProcessU32(cursor, data, "player middle-high-process Unk1D4");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt1D8_1D9_228)
            value = readPlayerProcessU8(cursor, data, "player middle-high-process state byte");
        result.mWrd22A = readPlayerProcessU16(cursor, data, "player middle-high-process Wrd22A");
        result.mUnk1A8 = readPlayerProcessU32(cursor, data, "player middle-high-process Unk1A8");
        result.mByt0E1 = readPlayerProcessU8(cursor, data, "player middle-high-process Byt0E1");
        result.mUnk190 = readPlayerProcessU32(cursor, data, "player middle-high-process Unk190");
        result.mUnk198 = readPlayerProcessU32(cursor, data, "player middle-high-process Unk198");
        result.mByt19C = readPlayerProcessU8(cursor, data, "player middle-high-process Byt19C");
        result.mByt19D = readPlayerProcessU8(cursor, data, "player middle-high-process Byt19D");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk234_238_23C_244)
            value = readPlayerProcessU32(cursor, data, "player middle-high-process versioned word");
        result.mUnk110 = readPlayerProcessU8(cursor, data, "player middle-high-process Unk110");
        for (ESM4::FONVSaveResolvedReferenceId& value : result.mIdleForm10C_IdleForm194_Unk158_Unk140)
            value = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player middle-high-process RefID", true);
        result.mUnknown = readPlayerProcessU32(cursor, data, "player middle-high-process Unknown");
        result.mList0C8Count = readPackedCount(cursor, data, "player middle-high-process List0C8 count");
        validatePackedCountFits(result.mList0C8Count, cursor, 4, "player middle-high-process List0C8 count");
        result.mList0C8.reserve(result.mList0C8Count.mValue);
        for (std::uint32_t i = 0; i < result.mList0C8Count.mValue; ++i)
            result.mList0C8.push_back(decodeDelimitedResolvedReferenceId(
                cursor, data, formIds, "player middle-high-process List0C8 RefID", true));
        result.mPackage
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player middle-high-process package RefID", true);
        rejectPlayerProcessPackageData(result.mPackage, "player middle-high-process package");

        constexpr std::uint32_t animationChanged = 1u << 28;
        if ((changeFlags & animationChanged) != 0)
            result.mAnimation = parsePlayerProcessSubBuffer(cursor, data, "player actor animation sub-buffer");

        result.mMagicItemCount = readPackedCount(cursor, data, "player middle-high-process magic-item count");
        if (result.mMagicItemCount.mValue != 0)
        {
            throw ESM4::FONVSaveError(
                result.mMagicItemCount.mRange.mOffset, "unsupported player middle-high-process magic-item data");
        }
        for (ESM4::FONVSaveResolvedReferenceId& value : result.mUnk164_160_1BC)
            value = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player middle-high-process RefID", true);
        result.mList230Count = readPackedCount(cursor, data, "player middle-high-process List230 count");
        validatePackedCountFits(result.mList230Count, cursor, 26, "player middle-high-process List230 count");
        result.mList230.reserve(result.mList230Count.mValue);
        for (std::uint32_t i = 0; i < result.mList230Count.mValue; ++i)
            result.mList230.push_back(parsePlayerMiddleHighList230Entry(cursor, data, formIds));
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerHighLocationEntry parsePlayerHighLocationEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds, std::string_view description)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerHighLocationEntry result;
        result.mForm000 = decodeDelimitedResolvedReferenceId(cursor, data, formIds, description, true);
        result.mUnk004 = readPlayerProcessU8(cursor, data, description);
        result.mUnk008 = readPlayerProcessU32(cursor, data, description);
        result.mCoords = parsePlayerProcessVector3(cursor, data, description);
        result.mTim018 = readPlayerProcessFloat(cursor, data, description);
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt01E_01C_01D)
            value = readPlayerProcessU8(cursor, data, description);
        result.mUnk020 = readPlayerProcessU32(cursor, data, description);
        result.mByt01F = readPlayerProcessU8(cursor, data, description);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerHighProcessState parsePlayerHighProcessState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerHighProcessState result;
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mUnk32C_340_374_375)
            value = readPlayerProcessU8(cursor, data, "player high-process initial byte");
        result.mCurrentAction = readPlayerProcessS16(cursor, data, "player high-process current-action candidate");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk2B4_2F8_310_330_334_338_34C_294_2B8_2BC_298)
            value = readPlayerProcessU32(cursor, data, "player high-process initial word");
        for (ESM4::FONVSaveField<std::uint16_t>& value : result.mUnk2C0_2C2_2C4)
            value = readPlayerProcessU16(cursor, data, "player high-process initial short");
        result.mUnk349 = readPlayerProcessU8(cursor, data, "player high-process Unk349");
        result.mCoords = parsePlayerProcessVector3(cursor, data, "player high-process Coords");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk36C_3E8_3EC_33C_2A8_378)
            value = readPlayerProcessU32(cursor, data, "player high-process state word");
        result.mUnk3A0 = readPlayerProcessU8(cursor, data, "player high-process Unk3A0");
        result.mUnk39C = readPlayerProcessU32(cursor, data, "player high-process Unk39C");
        result.mUnk3A8 = readPlayerProcessU8(cursor, data, "player high-process Unk3A8");
        result.mUnk3A4 = readPlayerProcessU32(cursor, data, "player high-process Unk3A4");
        result.mUnk420 = readPlayerProcessU8(cursor, data, "player high-process Unk420");
        result.mUnk3BC = readPlayerProcessU32(cursor, data, "player high-process Unk3BC");
        result.mUnk3B0 = readPlayerProcessU32(cursor, data, "player high-process Unk3B0");
        result.mUnk2C6 = readPlayerProcessU8(cursor, data, "player high-process Unk2C6");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk2D0_2D4_2D8)
            value = readPlayerProcessU32(cursor, data, "player high-process state word");
        result.mUnk3B8 = readPlayerProcessU8(cursor, data, "player high-process Unk3B8");
        result.mUnk2DC1 = readPlayerProcessU8(cursor, data, "player high-process Unk2DC first");
        result.mUnk2E0 = readPlayerProcessU32(cursor, data, "player high-process Unk2E0");
        result.mUnk344 = readPlayerProcessU32(cursor, data, "player high-process Unk344");
        result.mUnk2DC2 = readPlayerProcessU8(cursor, data, "player high-process Unk2DC second");
        result.mUnk2DC3 = readPlayerProcessU8(cursor, data, "player high-process Unk2DC third");
        result.mUnk3D8Modulo12 = readPlayerProcessU32(cursor, data, "player high-process Unk3D8 modulo 12");
        if (result.mUnk3D8Modulo12.mValue >= 12)
            throw ESM4::FONVSaveError(result.mUnk3D8Modulo12.mRange.mOffset, "invalid player high-process modulo-12 value");
        result.mUnk448 = readPlayerProcessU32(cursor, data, "player high-process Unk448");
        result.mUnk29D = readPlayerProcessU8(cursor, data, "player high-process Unk29D");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk2B0_2C8_418_43C_440)
            value = readPlayerProcessU32(cursor, data, "player high-process state word");
        result.mUnk444 = readPlayerProcessU8(cursor, data, "player high-process Unk444");
        result.mUnk445 = readPlayerProcessU8(cursor, data, "player high-process Unk445");
        result.mUnk450 = readPlayerProcessU32(cursor, data, "player high-process Unk450");
        result.mUnk458 = readPlayerProcessU8(cursor, data, "player high-process Unk458");
        result.mUnk430 = readPlayerProcessU32(cursor, data, "player high-process Unk430");
        result.mUnk3E0 = readPlayerProcessU8(cursor, data, "player high-process Unk3E0");
        result.mUnk459 = readPlayerProcessU8(cursor, data, "player high-process Unk459");
        result.mUnk2A0 = readPlayerProcessU32(cursor, data, "player high-process Unk2A0");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mUnk3D0_3D1_348_Unknown)
            value = readPlayerProcessU8(cursor, data, "player high-process versioned byte");
        for (ESM4::FONVSaveResolvedReferenceId& value : result.mUnk30C_2A4_3F0_41C_37C_Idle_2AC)
            value = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player high-process RefID", true);
        for (ESM4::FONVSavePlayerHighUnknownEntry& entry : result.mUnknownEntries)
        {
            const std::size_t entryBegin = cursor.position();
            entry.mUnk3F8 = decodeDelimitedResolvedReferenceId(
                cursor, data, formIds, "player high-process fixed-array RefID", true);
            entry.mUnk410 = readPlayerProcessU8(cursor, data, "player high-process fixed-array byte");
            entry.mRange = range(entryBegin, cursor.position());
            entry.mRaw = copyRange(data, entryBegin, cursor.position());
        }

        const auto readReferenceList = [&](ESM4::FONVSaveField<std::uint32_t>& count,
                                           std::vector<ESM4::FONVSaveResolvedReferenceId>& values,
                                           std::string_view description) {
            count = readPackedCount(cursor, data, description);
            validatePackedCountFits(count, cursor, 4, description);
            values.reserve(count.mValue);
            for (std::uint32_t i = 0; i < count.mValue; ++i)
                values.push_back(decodeDelimitedResolvedReferenceId(cursor, data, formIds, description, true));
        };
        readReferenceList(result.mList38CCount, result.mList38C, "player high-process List38C count");
        readReferenceList(result.mList394Count, result.mList394, "player high-process List394 count");
        readReferenceList(result.mList264Count, result.mList264, "player high-process List264 count");

        result.mHasDialogueItems = readPlayerProcessU8(cursor, data, "player high-process Has Dialogue Items");
        if (result.mHasDialogueItems.mValue != 0)
            throw ESM4::FONVSaveError(result.mHasDialogueItems.mRange.mOffset, "unsupported player high-process dialogue data");
        result.mList44CCount = readPackedCount(cursor, data, "player high-process List44C count");
        if (result.mList44CCount.mValue != 0)
            throw ESM4::FONVSaveError(result.mList44CCount.mRange.mOffset, "unsupported player high-process List44C data");

        const auto readLocationList = [&](ESM4::FONVSaveField<std::uint32_t>& count,
                                          std::vector<ESM4::FONVSavePlayerHighLocationEntry>& values,
                                          std::string_view description) {
            count = readPackedCount(cursor, data, description);
            validatePackedCountFits(count, cursor, 42, description);
            values.reserve(count.mValue);
            for (std::uint32_t i = 0; i < count.mValue; ++i)
                values.push_back(parsePlayerHighLocationEntry(cursor, data, formIds, description));
        };
        readLocationList(result.mList25CCount, result.mList25C, "player high-process List25C");
        readLocationList(result.mList260Count, result.mList260, "player high-process List260");
        result.mHasUnk3DC = readPlayerProcessU8(cursor, data, "player high-process Has Unk3DC");
        if (result.mHasUnk3DC.mValue != 0)
            throw ESM4::FONVSaveError(result.mHasUnk3DC.mRange.mOffset, "unsupported player high-process Unk3DC data");
        result.mSubBuffer = parsePlayerProcessSubBuffer(cursor, data, "player high-process sub-buffer");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerMobileObjectProcessState parsePlayerMobileObjectProcessState(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerProcessInventoryData& processInventory,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(player.mVersion.mRange.mOffset,
                "unsupported canonical player MobileObject process-state changed-form version");
        if (processInventory.mProcessLevel.mValue != 0)
            throw ESM4::FONVSaveError(processInventory.mProcessLevel.mRange.mOffset,
                "unsupported canonical player MobileObject process level");

        const std::size_t begin = static_cast<std::size_t>(processInventory.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(processInventory.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerMobileObjectProcessState result;
        result.mMobileObjectBase = parsePlayerMobileObjectBaseState(cursor, data, formIds);
        result.mBaseProcess = parsePlayerBaseProcessState(cursor, data, formIds);
        result.mLowProcess = parsePlayerLowProcessState(cursor, data, formIds, player.mChangeFlags.mValue);
        result.mMiddleLowProcess = parsePlayerMiddleLowProcessState(cursor, data, player.mChangeFlags.mValue);
        result.mMiddleHighProcess
            = parsePlayerMiddleHighProcessState(cursor, data, formIds, player.mChangeFlags.mValue);
        result.mHighProcess = parsePlayerHighProcessState(cursor, data, formIds);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical player ACHR payload");
        return result;
    }

    ESM4::FONVSavePlayerChangedActorFixedState parsePlayerChangedActorFixedState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds, std::uint32_t changeFlags)
    {
        constexpr std::uint32_t unsupportedActorBranches
            = (1u << 10) | (1u << 19) | (1u << 22) | (1u << 23);
        if ((changeFlags & unsupportedActorBranches) != 0)
            throw ESM4::FONVSaveError(cursor.position(), "unsupported canonical player ChangedActor flag branch");

        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerChangedActorFixedState result;
        result.mUnk114 = readPlayerProcessFloat(cursor, data, "player ChangedActor Unk114");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt124_125_0BC_0C4)
            value = readPlayerProcessU8(cursor, data, "player ChangedActor initial byte");
        result.mUnk0C8 = readPlayerProcessU32(cursor, data, "player ChangedActor Unk0C8");
        result.mByt07D = readPlayerProcessU8(cursor, data, "player ChangedActor Byt07D");
        result.mUnk110 = readPlayerProcessU32(cursor, data, "player ChangedActor Unk110");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt118_126_145_146_14C_14D)
            value = readPlayerProcessU8(cursor, data, "player ChangedActor state byte");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk150_154_158)
            value = readPlayerProcessU32(cursor, data, "player ChangedActor state word");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt174_175_18D)
            value = readPlayerProcessU8(cursor, data, "player ChangedActor state byte");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk1A4_1A8)
            value = readPlayerProcessU32(cursor, data, "player ChangedActor state word");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt0F0_0F1)
            value = readPlayerProcessU8(cursor, data, "player ChangedActor state byte");
        result.mUnk10C = readPlayerProcessU32(cursor, data, "player ChangedActor Unk10C");
        result.mUnk0138Byt000 = readPlayerProcessU8(cursor, data, "player ChangedActor Unk0138 Byt000");
        result.mUnk0138Unk004 = readPlayerProcessU32(cursor, data, "player ChangedActor Unk0138 Unk004");
        result.mUnk0138Byt010 = readPlayerProcessU8(cursor, data, "player ChangedActor Unk0138 Byt010");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk0138Unk008_00C)
            value = readPlayerProcessU32(cursor, data, "player ChangedActor Unk0138 word");
        result.mUnk120 = readPlayerProcessU32(cursor, data, "player ChangedActor Unk120");
        for (ESM4::FONVSaveResolvedReferenceId& value : result.mForm0C0_ActorBase_Form070)
            value = decodeDelimitedResolvedReferenceId(
                cursor, data, formIds, "player ChangedActor fixed RefID", true);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerPathingLocation parsePlayerPathingLocation(
        Cursor& cursor, std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerPathingLocation result;
        result.mCoords = parsePlayerProcessVector3(cursor, data, "player ActorMover pathing-location coordinates");
        for (ESM4::FONVSaveResolvedReferenceId& value : result.mNavMesh_Cell_Worldspace)
        {
            value = decodeDelimitedResolvedReferenceId(
                cursor, data, formIds, "player ActorMover pathing-location RefID", true);
        }
        result.mCoordXandY
            = readPlayerProcessU32(cursor, data, "player ActorMover pathing-location CoordXandY");
        result.mWrd024 = readPlayerProcessS16(cursor, data, "player ActorMover pathing-location Wrd024");
        result.mByt026 = readPlayerProcessU8(cursor, data, "player ActorMover pathing-location Byt026");
        result.mUnknown = readPlayerProcessU8(cursor, data, "player ActorMover pathing-location Unknown");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerDetailedActorPathHandler parsePlayerDetailedActorPathHandler(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerDetailedActorPathHandler result;
        for (ESM4::FONVSavePlayerProcessVector3& value : result.mCoords01C_028_034_040_04C)
            value = parsePlayerProcessVector3(cursor, data, "player detailed actor-path coordinates");
        for (ESM4::FONVSaveField<std::uint32_t>& value :
            result.mUnk060_064_068_06C_070_074_078_07C_080_084_088_08C_090_094_098_09C_0AC_0B0_0B4_0B8)
        {
            value = readPlayerProcessU32(cursor, data, "player detailed actor-path word");
        }
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk014)
            value = readPlayerProcessU32(cursor, data, "player detailed actor-path Unk014 word");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk0C8_0CC_0D0_0D4)
            value = readPlayerProcessU32(cursor, data, "player detailed actor-path tail word");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt0DC_0DD_0DE_0DF_0E0_0E2_0E1)
            value = readPlayerProcessU8(cursor, data, "player detailed actor-path byte");
        result.mUnk0C0Byt000 = readPlayerProcessU8(cursor, data, "player detailed actor-path Unk0C0 Byt000");
        result.mUnk0C0Byt001 = readPlayerProcessU8(cursor, data, "player detailed actor-path Unk0C0 Byt001");
        result.mUnk0C0Unk004 = readPlayerProcessU32(cursor, data, "player detailed actor-path Unk0C0 Unk004");
        result.mUnknownCoords
            = parsePlayerProcessVector3(cursor, data, "player detailed actor-path unknown coordinates");
        result.mUnk0BC = readPlayerProcessU32(cursor, data, "player detailed actor-path Unk0BC");
        result.mForm0D8 = decodeDelimitedResolvedReferenceId(
            cursor, data, formIds, "player detailed actor-path Form0D8 RefID", true);
        result.mList058Count = readPackedCount(cursor, data, "player detailed actor-path List058 count");
        if (result.mList058Count.mValue != 0)
        {
            throw ESM4::FONVSaveError(
                result.mList058Count.mRange.mOffset, "unsupported player detailed actor-path List058 data");
        }
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerMoverState parsePlayerMoverState(
        Cursor& cursor, std::span<const std::uint8_t> data)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerMoverState result;
        result.mCoords = parsePlayerProcessVector3(cursor, data, "player-specific ActorMover coordinates");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk094_098_09C)
            value = readPlayerProcessU32(cursor, data, "player-specific ActorMover word");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerActorMoverState parsePlayerActorMoverState(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerActorMoverState result;
        result.mWrd040 = readPlayerProcessU16(cursor, data, "player ActorMover Wrd040");
        result.mWrd042 = readPlayerProcessU16(cursor, data, "player ActorMover Wrd042");
        result.mByt070 = readPlayerProcessU8(cursor, data, "player ActorMover Byt070");
        result.mUnk034 = readPlayerProcessU32(cursor, data, "player ActorMover Unk034");
        result.mByt071 = readPlayerProcessU8(cursor, data, "player ActorMover Byt071");
        result.mUnk038 = readPlayerProcessU32(cursor, data, "player ActorMover Unk038");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt072_073)
            value = readPlayerProcessU8(cursor, data, "player ActorMover initial byte");
        result.mCoords004 = parsePlayerProcessVector3(cursor, data, "player ActorMover Coords004");
        result.mCoords010 = parsePlayerProcessVector3(cursor, data, "player ActorMover Coords010");
        result.mUnk03C = readPlayerProcessU32(cursor, data, "player ActorMover Unk03C");
        for (ESM4::FONVSaveField<std::uint8_t>& value : result.mByt074_075_077_076_078)
            value = readPlayerProcessU8(cursor, data, "player ActorMover state byte");
        result.mUnk06C = readPlayerProcessU32(cursor, data, "player ActorMover Unk06C");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnknown_Unk084)
            value = readPlayerProcessU32(cursor, data, "player ActorMover versioned word");
        result.mPathingLocation = parsePlayerPathingLocation(cursor, data, formIds);
        result.mForm02C
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "player ActorMover Form02C RefID", true);
        result.mContentFlags = readPlayerProcessU8(cursor, data, "player ActorMover content flags");
        if (result.mContentFlags.mValue != 0x08)
            throw ESM4::FONVSaveError(result.mContentFlags.mRange.mOffset,
                "unsupported player ActorMover content-flag branch");
        result.mDetailedPathHandler = parsePlayerDetailedActorPathHandler(cursor, data, formIds);
        result.mPlayerMover = parsePlayerMoverState(cursor, data);
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerChangedCharacterState parsePlayerChangedCharacterState(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerMobileObjectProcessState& processState,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(player.mVersion.mRange.mOffset,
                "unsupported canonical player ChangedCharacter changed-form version");

        const std::size_t begin = static_cast<std::size_t>(processState.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(processState.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerChangedCharacterState result;
        result.mActorFixed
            = parsePlayerChangedActorFixedState(cursor, data, formIds, player.mChangeFlags.mValue);
        result.mActorMover = parsePlayerActorMoverState(cursor, data, formIds);
        result.mByt1C0 = readPlayerProcessU8(cursor, data, "player ChangedCharacter Byt1C0");
        result.mByt1C1 = readPlayerProcessU8(cursor, data, "player ChangedCharacter Byt1C1");
        if (cursor.position() - begin != ESM4::sFONVPlayerChangedCharacterStateBytes)
            throw ESM4::FONVSaveError(begin, "canonical player ChangedCharacter state has an unexpected size");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical player ACHR payload");
        return result;
    }

    ESM4::FONVSavePlayerCharacterAnimationState parsePlayerCharacterAnimationState(
        std::span<const std::uint8_t> data, const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerChangedCharacterState& characterState)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(player.mVersion.mRange.mOffset,
                "unsupported canonical player animation-buffer changed-form version");

        const std::size_t begin = static_cast<std::size_t>(characterState.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(characterState.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerCharacterAnimationState result;
        result.mAnimation = parsePlayerProcessSubBuffer(cursor, data, "second player animation sub-buffer");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical PlayerCharacter payload");
        return result;
    }

    ESM4::FONVSavePlayerCharacterVersion21State parsePlayerCharacterVersion21State(
        Cursor& cursor, std::span<const std::uint8_t> data)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterVersion21State result;
        result.mUnk1FC = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk1FC");
        result.mUnk684 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk684");
        for (ESM4::FONVSaveField<std::uint32_t>& value : result.mUnk744)
            value = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk744");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterUnk878State parsePlayerCharacterUnk878State(
        Cursor& cursor, std::span<const std::uint8_t> data)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterUnk878State result;
        result.mByt000 = readPlayerProcessU8(cursor, data, "PlayerCharacter Unk878 Byt000");
        result.mUnk004 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk878 Unk004");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterScalarReferenceState parsePlayerCharacterScalarReferenceState(
        std::span<const std::uint8_t> data, const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerCharacterAnimationState& animationState,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(player.mVersion.mRange.mOffset,
                "unsupported canonical PlayerCharacter scalar/reference changed-form version");

        const std::size_t begin = static_cast<std::size_t>(animationState.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(animationState.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerCharacterScalarReferenceState result;
        result.mFirstPersonMode = readPlayerProcessU8(cursor, data, "PlayerCharacter first-person mode (Byt64A)");
        result.mByt64D = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt64D");
        result.mByt651 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt651");
        result.mByt652 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt652");
        result.mUnk654 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk654");
        result.mUnk660 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk660");
        result.mUnk664 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk664");
        result.mUnk668 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk668");
        result.mByt66C = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt66C");
        result.mByt6CC = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt6CC");
        result.mUnk6D0 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk6D0");
        result.mUnk6D4 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk6D4");
        result.mByt6D8 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt6D8");
        result.mUnk6DC = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk6DC");
        result.mByt6E8 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt6E8");
        result.mByt681 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt681");
        result.mByt75CFaceGen = readPlayerProcessU8(cursor, data, "PlayerCharacter FaceGen-linked Byt75C");
        result.mByt7C6 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt7C6");
        result.mUnk6E4 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk6E4");
        result.mRefr6F4Pos = parsePlayerProcessVector3(cursor, data, "PlayerCharacter Refr6F4 position");
        result.mUnk698 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk698");
        result.mUnk67C = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk67C");
        result.mUnk738 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk738");
        result.mByt658 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt658");
        result.mUnk65C = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk65C");
        result.mFirstPersonModelFov
            = readPlayerProcessFloat(cursor, data, "PlayerCharacter first-person model FOV (Unk674)");
        result.mWorldFov = readPlayerProcessFloat(cursor, data, "PlayerCharacter world FOV (Unk670)");
        result.mByt75C = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt75C");
        result.mUnk730 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk730");
        result.mUnk790 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk790");
        result.mByt680 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt680");
        result.mByt7C4 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt7C4");
        result.mUnk63C = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk63C");
        result.mUnk640 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk640");
        result.mUnk644 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk644");
        result.mUnk200 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk200");
        result.mByt240 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt240");
        result.mByt64E = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt64E");
        result.mByt66D = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt66D");
        result.mUnk794 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk794");
        result.mFlt11E0B5C = readPlayerProcessFloat(cursor, data, "PlayerCharacter flt_11E0B5C");
        result.mUnkD6C = readPlayerProcessU32(cursor, data, "PlayerCharacter UnkD6C");
        result.mUnkD70 = readPlayerProcessU32(cursor, data, "PlayerCharacter UnkD70");
        result.mUnk228 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk228");
        result.mUnk22C = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk22C");
        result.mUnk230 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk230");
        result.mUnk234 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk234");
        result.mByt608 = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt608");
        result.mBytDF2 = readPlayerProcessU8(cursor, data, "PlayerCharacter BytDF2");
        result.mByt64F = readPlayerProcessU8(cursor, data, "PlayerCharacter version-13 Byt64F");
        result.mByt650 = readPlayerProcessU8(cursor, data, "PlayerCharacter version-13 Byt650");
        result.mByt7C7 = readPlayerProcessU8(cursor, data, "PlayerCharacter version-16 Byt7C7");
        result.mByt5F8 = readPlayerProcessU8(cursor, data, "PlayerCharacter version-18 Byt5F8");
        result.mVersion21 = parsePlayerCharacterVersion21State(cursor, data);
        result.mUnk878 = parsePlayerCharacterUnk878State(cursor, data);

        const auto readReference = [&](std::string_view description) {
            // xEdit resolves a nonzero FormID-array index whose slot contains zero as null. Preserve that encoded
            // index while retaining strict namespace and bounds validation.
            return decodeDelimitedResolvedReferenceId(cursor, data, formIds, description, true, true);
        };
        result.mQuest = readReference("PlayerCharacter Quest RefID");
        result.mClass = readReference("PlayerCharacter Class RefID");
        result.mRefr6F4ParentCell = readReference("PlayerCharacter Refr6F4 parent-cell RefID");
        result.mRegion = readReference("PlayerCharacter Region RefID");
        result.mRegionWeather = readReference("PlayerCharacter Region Weather RefID");
        result.mForm208 = readReference("PlayerCharacter Form208 RefID");
        result.mForm224 = readReference("PlayerCharacter version-18 Form224 RefID");
        result.mForm638 = readReference("PlayerCharacter Form638 RefID");
        result.mForm604 = readReference("PlayerCharacter Form604 RefID");
        result.mFormD2C = readReference("PlayerCharacter FormD2C RefID");
        result.mFormD44 = readReference("PlayerCharacter FormD44 RefID");

        if (cursor.position() - begin != ESM4::sFONVPlayerCharacterScalarReferenceStateBytes)
            throw ESM4::FONVSaveError(
                begin, "canonical PlayerCharacter scalar/reference state has an unexpected size");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical PlayerCharacter payload");
        return result;
    }

    std::vector<ESM4::FONVSaveResolvedReferenceId> parsePlayerCharacterReferenceList(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds,
        ESM4::FONVSaveField<std::uint32_t>& count, std::string_view description)
    {
        count = readPackedCount(cursor, data, description);
        validatePackedCountFits(count, cursor, 4, description);
        std::vector<ESM4::FONVSaveResolvedReferenceId> result;
        result.reserve(count.mValue);
        for (std::uint32_t i = 0; i < count.mValue; ++i)
            result.push_back(decodeDelimitedResolvedReferenceId(cursor, data, formIds, description));
        return result;
    }

    ESM4::FONVSavePlayerCharacterListD48Entry parsePlayerCharacterListD48Entry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterListD48Entry result;
        result.mForm000
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "PlayerCharacter ListD48 Form000 RefID");
        result.mByt004 = readPlayerProcessU8(cursor, data, "PlayerCharacter ListD48 Byt004");
        result.mByt005 = readPlayerProcessU8(cursor, data, "PlayerCharacter ListD48 Byt005");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterPerkEntry parsePlayerCharacterPerkEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterPerkEntry result;
        result.mPerk = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "PlayerCharacter Perk RefID");
        result.mByt004 = readPlayerProcessU8(cursor, data, "PlayerCharacter Perk Byt004");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterList60CEntry parsePlayerCharacterList60CEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterList60CEntry result;
        result.mUnk000 = readPlayerProcessU32(cursor, data, "PlayerCharacter List60C Unk000");
        result.mFlt004 = readPlayerProcessFloat(cursor, data, "PlayerCharacter List60C Flt004");
        result.mFormId
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "PlayerCharacter List60C FormID RefID");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterList610Entry parsePlayerCharacterList610Entry(
        Cursor& cursor, std::span<const std::uint8_t> data)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterList610Entry result;
        result.mUnk000 = readPlayerProcessU32(cursor, data, "PlayerCharacter List610 Unk000");
        result.mUnk004 = readPlayerProcessU32(cursor, data, "PlayerCharacter List610 Unk004");
        result.mWrd008 = readPlayerProcessU16(cursor, data, "PlayerCharacter List610 Wrd008");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterStageEntry parsePlayerCharacterStageEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterStageEntry result;
        result.mQuest
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "PlayerCharacter Stage Quest RefID");
        result.mStage = readPlayerProcessU8(cursor, data, "PlayerCharacter Stage");
        result.mLogEntry = readPlayerProcessU8(cursor, data, "PlayerCharacter Stage LogEntry");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterObjectiveEntry parsePlayerCharacterObjectiveEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterObjectiveEntry result;
        result.mQuest
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "PlayerCharacter Objective Quest RefID");
        result.mObjective = readPlayerProcessU32(cursor, data, "PlayerCharacter Objective");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterListsState parsePlayerCharacterListsState(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerCharacterScalarReferenceState& scalarState,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(
                player.mVersion.mRange.mOffset, "unsupported canonical PlayerCharacter lists changed-form version");

        const std::size_t begin = static_cast<std::size_t>(scalarState.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(scalarState.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerCharacterListsState result;
        result.mTopics = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mList6A8Count, "PlayerCharacter List6A8 Topic count");
        result.mNotes = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mList5E4Count, "PlayerCharacter List5E4 Note count");

        result.mInventoryEntryCount = readPackedCount(cursor, data, "PlayerCharacter inventory entry count");
        validatePackedCountFits(
            result.mInventoryEntryCount, cursor, 11, "PlayerCharacter inventory entry count");
        result.mInventoryEntries.reserve(result.mInventoryEntryCount.mValue);
        for (std::uint32_t i = 0; i < result.mInventoryEntryCount.mValue; ++i)
            result.mInventoryEntries.push_back(parsePlayerInventoryEntry(cursor, data, formIds));

        result.mListD48Count = readPackedCount(cursor, data, "PlayerCharacter ListD48 count");
        validatePackedCountFits(result.mListD48Count, cursor, 8, "PlayerCharacter ListD48 count");
        result.mListD48.reserve(result.mListD48Count.mValue);
        for (std::uint32_t i = 0; i < result.mListD48Count.mValue; ++i)
            result.mListD48.push_back(parsePlayerCharacterListD48Entry(cursor, data, formIds));

        result.mPerkCount = readPackedCount(cursor, data, "PlayerCharacter Perks count");
        validatePackedCountFits(result.mPerkCount, cursor, 6, "PlayerCharacter Perks count");
        result.mPerks.reserve(result.mPerkCount.mValue);
        for (std::uint32_t i = 0; i < result.mPerkCount.mValue; ++i)
            result.mPerks.push_back(parsePlayerCharacterPerkEntry(cursor, data, formIds));

        result.mList60CCount = readPackedCount(cursor, data, "PlayerCharacter List60C count");
        validatePackedCountFits(result.mList60CCount, cursor, 14, "PlayerCharacter List60C count");
        result.mList60C.reserve(result.mList60CCount.mValue);
        for (std::uint32_t i = 0; i < result.mList60CCount.mValue; ++i)
            result.mList60C.push_back(parsePlayerCharacterList60CEntry(cursor, data, formIds));

        result.mList610Count = readPackedCount(cursor, data, "PlayerCharacter List610 count");
        validatePackedCountFits(result.mList610Count, cursor, 13, "PlayerCharacter List610 count");
        result.mList610.reserve(result.mList610Count.mValue);
        for (std::uint32_t i = 0; i < result.mList610Count.mValue; ++i)
            result.mList610.push_back(parsePlayerCharacterList610Entry(cursor, data));

        result.mCards614 = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mCards614Count, "PlayerCharacter Cards 614 count");
        result.mCards618 = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mCards618Count, "PlayerCharacter Cards 618 count");
        result.mUnk61C = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk61C");
        result.mUnk620 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk620");
        result.mUnk624 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk624");
        result.mUnk628 = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk628");
        result.mUnk62C = readPlayerProcessU32(cursor, data, "PlayerCharacter Unk62C");

        result.mStageCount = readPackedCount(cursor, data, "PlayerCharacter Stages count");
        validatePackedCountFits(result.mStageCount, cursor, 8, "PlayerCharacter Stages count");
        result.mStages.reserve(result.mStageCount.mValue);
        for (std::uint32_t i = 0; i < result.mStageCount.mValue; ++i)
            result.mStages.push_back(parsePlayerCharacterStageEntry(cursor, data, formIds));

        result.mObjectiveCount = readPackedCount(cursor, data, "PlayerCharacter Objectives count");
        validatePackedCountFits(result.mObjectiveCount, cursor, 9, "PlayerCharacter Objectives count");
        result.mObjectives.reserve(result.mObjectiveCount.mValue);
        for (std::uint32_t i = 0; i < result.mObjectiveCount.mValue; ++i)
            result.mObjectives.push_back(parsePlayerCharacterObjectiveEntry(cursor, data, formIds));

        // This state is a sequence of counted retail lists. Its serialized size changes as topics, notes,
        // inventory, perks, stages, and objectives change between saves; Save 330's 147 bytes are not a format
        // invariant. Every count and entry is already parsed with bounds checks above, so the cursor position is
        // the authoritative boundary for the following PlayerCharacter state.
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical PlayerCharacter payload");
        return result;
    }

    ESM4::FONVSavePlayerCharacterMagicTargetEntry parsePlayerCharacterMagicTargetEntry(Cursor& cursor,
        std::span<const std::uint8_t> data, const ESM4::FONVSaveFormIdTable& formIds)
    {
        constexpr std::size_t sEntryBytes = 58;
        const std::size_t begin = cursor.position();
        ESM4::FONVSavePlayerCharacterMagicTargetEntry result;
        result.mMagicForm
            = decodeDelimitedResolvedReferenceId(cursor, data, formIds, "PlayerCharacter magic-target form RefID");
        result.mArchType = readPlayerProcessU8(cursor, data, "PlayerCharacter magic-target ArchType");
        result.mUnk098 = readPackedCount(cursor, data, "PlayerCharacter magic-target Unk098");
        result.mEffectItemCount = readPackedCount(cursor, data, "PlayerCharacter magic-target effect-item count");
        validatePackedCountFits(
            result.mEffectItemCount, cursor, 1, "PlayerCharacter magic-target effect-item count");
        result.mEffectItems.reserve(result.mEffectItemCount.mValue);
        for (std::uint32_t i = 0; i < result.mEffectItemCount.mValue; ++i)
        {
            result.mEffectItems.push_back(readField<std::uint8_t>(cursor, data,
                [&](std::string_view label) { return cursor.readU8(label); },
                "PlayerCharacter magic-target effect item"));
        }
        if (cursor.position() - begin != sEntryBytes)
            throw ESM4::FONVSaveError(begin, "canonical PlayerCharacter magic-target entry has an unexpected size");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        return result;
    }

    ESM4::FONVSavePlayerCharacterMagicTargetState parsePlayerCharacterMagicTargetState(
        std::span<const std::uint8_t> data, const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerCharacterListsState& listsState,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        constexpr std::size_t sEntryBytes = 58;
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(player.mVersion.mRange.mOffset,
                "unsupported canonical PlayerCharacter magic-target changed-form version");

        const std::size_t begin = static_cast<std::size_t>(listsState.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(listsState.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerCharacterMagicTargetState result;
        result.mMagicItemCount = readPackedCount(cursor, data, "PlayerCharacter magic-target count");
        validatePackedCountFits(result.mMagicItemCount, cursor, sEntryBytes, "PlayerCharacter magic-target count");
        result.mMagicItems.reserve(result.mMagicItemCount.mValue);
        for (std::uint32_t i = 0; i < result.mMagicItemCount.mValue; ++i)
            result.mMagicItems.push_back(parsePlayerCharacterMagicTargetEntry(cursor, data, formIds));

        if (cursor.position() - begin != ESM4::sFONVPlayerCharacterMagicTargetStateBytes)
            throw ESM4::FONVSaveError(begin, "canonical PlayerCharacter magic-target state has an unexpected size");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder = readRawField(
            cursor, data, cursor.end() - cursor.position(), "remaining canonical PlayerCharacter payload");
        return result;
    }

    ESM4::FONVSaveFullFormId decodePlayerCharacterFullFormId(
        Cursor& cursor, std::span<const std::uint8_t> data, std::string_view description)
    {
        ESM4::FONVSaveFullFormId result;
        result.mEncoded = readPlayerProcessU32(cursor, data, description);
        if (result.mEncoded.mValue != 0)
            result.mResolvedFormId = result.mEncoded.mValue;
        return result;
    }

    ESM4::FONVSavePlayerCharacterFinalState parsePlayerCharacterFinalState(std::span<const std::uint8_t> data,
        const ESM4::FONVSaveChangedFormEnvelope& player,
        const ESM4::FONVSavePlayerCharacterMagicTargetState& magicTargetState,
        const ESM4::FONVSaveFormIdTable& formIds)
    {
        if (player.mVersion.mValue != 27)
            throw ESM4::FONVSaveError(
                player.mVersion.mRange.mOffset, "unsupported canonical PlayerCharacter final changed-form version");

        const std::size_t begin = static_cast<std::size_t>(magicTargetState.mUnparsedRemainder.mRange.mOffset);
        const std::size_t end = static_cast<std::size_t>(magicTargetState.mUnparsedRemainder.mRange.end());
        Cursor cursor(data, begin, end);
        ESM4::FONVSavePlayerCharacterFinalState result;
        result.mKeyForUnkD64 = readPlayerProcessU32(cursor, data, "PlayerCharacter KeyForUnkD64");
        result.mUnknown11DFED4 = readPlayerProcessFloat(cursor, data, "PlayerCharacter Unknown 11DFED4");
        result.mUnknown11DFED8 = readPlayerProcessFloat(cursor, data, "PlayerCharacter Unknown 11DFED8");

        result.mPerksAD4Count = readPackedCount(cursor, data, "PlayerCharacter Perks AD4 count");
        validatePackedCountFits(result.mPerksAD4Count, cursor, 6, "PlayerCharacter Perks AD4 count");
        result.mPerksAD4.reserve(result.mPerksAD4Count.mValue);
        for (std::uint32_t i = 0; i < result.mPerksAD4Count.mValue; ++i)
            result.mPerksAD4.push_back(parsePlayerCharacterPerkEntry(cursor, data, formIds));

        result.mHardcoreMode = readPlayerProcessU8(cursor, data, "PlayerCharacter HardcoreMode");
        result.mClearsHardcoreFlag = readPlayerProcessU8(cursor, data, "PlayerCharacter Clears HardcoreFlag");
        result.mByt66E = readPlayerProcessU8(cursor, data, "PlayerCharacter Byt66E");
        for (ESM4::FONVSaveFullFormId& form : result.mUnknownE3C)
            form = decodePlayerCharacterFullFormId(cursor, data, "PlayerCharacter Unknown E3C FormID");

        result.mEnabled = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mEnabledCount, "PlayerCharacter Lists11CAC98 Enabled count");
        result.mDisabled = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mDisabledCount, "PlayerCharacter Lists11CAC98 Disabled count");
        result.mUnknown = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mUnknownCount, "PlayerCharacter Lists11CAC98 Unknown count");
        result.mFadeOut = parsePlayerCharacterReferenceList(
            cursor, data, formIds, result.mFadeOutCount, "PlayerCharacter Lists11CAC98 FadeOut count");

        if (cursor.position() - begin != ESM4::sFONVPlayerCharacterFinalStateBytes)
            throw ESM4::FONVSaveError(begin, "canonical PlayerCharacter final state has an unexpected size");
        if (cursor.position() != cursor.end())
            throw ESM4::FONVSaveError(cursor.position(), "canonical PlayerCharacter payload has trailing bytes");
        result.mRange = range(begin, cursor.position());
        result.mRaw = copyRange(data, begin, cursor.position());
        result.mUnparsedRemainder
            = readRawField(cursor, data, 0, "remaining canonical PlayerCharacter payload");
        return result;
    }

    void appendUnparsedSemanticPayload(ESM4::FONVSaveGamePrefix& save, const ESM4::FONVSaveRange& payload)
    {
        if (payload.empty())
            return;
        save.mUnparsedSemanticPayloadRanges.push_back(payload);
        save.mUnparsedSemanticPayloadBytes += payload.mSize;
    }
}

namespace ESM4
{
    FONVSaveError::FONVSaveError(std::uint64_t offset, std::string message)
        : std::runtime_error(errorMessage(offset, message))
        , mOffset(offset)
    {
    }

    bool isFONVSaveGame(std::span<const std::uint8_t> data)
    {
        return data.size() >= sMagic.size()
            && std::equal(sMagic.begin(), sMagic.end(), reinterpret_cast<const char*>(data.data()));
    }

    const FONVSaveChangedFormEnvelope* FONVSaveGamePrefix::findChangedForm(
        std::uint32_t resolvedFormId, std::optional<std::uint8_t> changeType) const
    {
        const auto found = std::find_if(mChangedForms.mEntries.begin(), mChangedForms.mEntries.end(),
            [&](const FONVSaveChangedFormEnvelope& entry) {
                return entry.mResolvedFormId == resolvedFormId
                    && (!changeType.has_value() || entry.mChangeType == *changeType);
            });
        return found == mChangedForms.mEntries.end() ? nullptr : &*found;
    }

    const FONVSaveChangedFormEnvelope& FONVSaveGamePrefix::requirePlayerReferenceChangeForm() const
    {
        const FONVSaveChangedFormEnvelope* result = nullptr;
        for (const FONVSaveChangedFormEnvelope& entry : mChangedForms.mEntries)
        {
            if (entry.mResolvedFormId != sFONVPlayerReferenceFormId
                || entry.mChangeType != sFONVActorReferenceChangeType)
                continue;
            if (result != nullptr)
                throw FONVSaveError(entry.mEncodedReferenceId.mRange.mOffset,
                    "multiple canonical player ACHR change forms are present");
            result = &entry;
        }
        if (result == nullptr)
            throw FONVSaveError(mChangedForms.mRange.mOffset,
                "canonical player ACHR reference FormID 0x00000014 is absent; NPC_ FormID 0x00000007 cannot be "
                "located");
        return *result;
    }

    FONVSaveGamePrefix parseFONVSaveGamePrefix(
        std::span<const std::uint8_t> data, const std::filesystem::path& sourcePath, const FONVSaveLimits& limits)
    {
        if (data.size() > limits.mMaxFileBytes)
            throw FONVSaveError(0, "file exceeds the configured bound");
        if (!isFONVSaveGame(data))
            throw FONVSaveError(0, "expected FO3SAVEGAME magic");

        FONVSaveGamePrefix result;
        result.mSourcePath = sourcePath;
        result.mFileSize = data.size();
        result.mMagicRange = { 0, sMagic.size() };
        result.mRawMagic = copyRange(data, 0, sMagic.size());

        Cursor prefix(data, sMagic.size(), data.size());
        result.mHeaderSize = readField<std::uint32_t>(
            prefix, data, [&](std::string_view label) { return prefix.readU32(label); }, "header size");
        if (result.mHeaderSize.mValue == 0 || result.mHeaderSize.mValue > limits.mMaxHeaderBytes)
            throw FONVSaveError(result.mHeaderSize.mRange.mOffset, "header size exceeds the configured bound");
        if (result.mHeaderSize.mValue > data.size() - prefix.position())
            throw FONVSaveError(prefix.position(), "truncated header");

        const std::size_t headerBegin = prefix.position();
        const std::size_t headerEnd = headerBegin + result.mHeaderSize.mValue;
        Cursor header(data, headerBegin, headerEnd);
        result.mHeaderRange = range(headerBegin, headerEnd);
        result.mRawHeader = copyRange(data, headerBegin, headerEnd);

        result.mHeader.mVersion = readField<std::uint32_t>(
            header, data, [&](std::string_view label) { return header.readU32(label); }, "header version");
        header.expectDelimiter("header version");

        if (looksLikeLanguage(header.peekU32("screenshot width or language")))
        {
            const std::size_t begin = header.position();
            const auto raw = header.take(sLanguageBytes, "language");
            FONVSaveField<std::string> language;
            language.mValue = decodeLanguage(raw, begin);
            language.mRange = range(begin, header.position());
            language.mRaw.assign(raw.begin(), raw.end());
            result.mHeader.mLanguage = std::move(language);
            header.expectDelimiter("language");
        }

        result.mHeader.mScreenshotWidth = readField<std::uint32_t>(
            header, data, [&](std::string_view label) { return header.readU32(label); }, "screenshot width");
        if (result.mHeader.mScreenshotWidth.mValue == 0
            || result.mHeader.mScreenshotWidth.mValue > limits.mMaxScreenshotDimension)
            throw FONVSaveError(
                result.mHeader.mScreenshotWidth.mRange.mOffset, "screenshot width exceeds the configured bound");
        header.expectDelimiter("screenshot width");

        result.mHeader.mScreenshotHeight = readField<std::uint32_t>(
            header, data, [&](std::string_view label) { return header.readU32(label); }, "screenshot height");
        if (result.mHeader.mScreenshotHeight.mValue == 0
            || result.mHeader.mScreenshotHeight.mValue > limits.mMaxScreenshotDimension)
            throw FONVSaveError(
                result.mHeader.mScreenshotHeight.mRange.mOffset, "screenshot height exceeds the configured bound");
        header.expectDelimiter("screenshot height");

        result.mHeader.mSaveNumber = readField<std::uint32_t>(
            header, data, [&](std::string_view label) { return header.readU32(label); }, "save number");
        header.expectDelimiter("save number");
        result.mHeader.mPlayerName
            = readStringField(header, data, std::numeric_limits<std::uint16_t>::max(), "player name");
        result.mHeader.mPlayerKarmaTitle
            = readStringField(header, data, std::numeric_limits<std::uint16_t>::max(), "player karma title");
        result.mHeader.mPlayerLevel = readField<std::uint32_t>(
            header, data, [&](std::string_view label) { return header.readU32(label); }, "player level");
        header.expectDelimiter("player level");
        result.mHeader.mPlayerLocation
            = readStringField(header, data, std::numeric_limits<std::uint16_t>::max(), "player location");
        result.mHeader.mPlayTime
            = readStringField(header, data, std::numeric_limits<std::uint16_t>::max(), "play time");
        if (header.position() != header.end())
            throw FONVSaveError(header.position(), "header contains unaccounted bytes");

        std::uint64_t screenshotPixels = 0;
        std::uint64_t screenshotBytes = 0;
        if (!checkedMultiply(
                result.mHeader.mScreenshotWidth.mValue, result.mHeader.mScreenshotHeight.mValue, screenshotPixels)
            || !checkedMultiply(screenshotPixels, result.mScreenshotBytesPerPixel, screenshotBytes)
            || screenshotBytes > limits.mMaxScreenshotBytes)
            throw FONVSaveError(headerEnd, "screenshot byte count overflows or exceeds the configured bound");
        if (screenshotBytes > data.size() - headerEnd)
            throw FONVSaveError(headerEnd, "truncated screenshot payload");

        const std::size_t screenshotEnd = headerEnd + static_cast<std::size_t>(screenshotBytes);
        result.mScreenshotRange = range(headerEnd, screenshotEnd);
        result.mRawScreenshot = copyRange(data, headerEnd, screenshotEnd);

        Cursor postScreenshot(data, screenshotEnd, data.size());
        result.mFormVersion = readField<std::uint8_t>(
            postScreenshot, data, [&](std::string_view label) { return postScreenshot.readU8(label); }, "form version");
        result.mMasterTableSize = readField<std::uint32_t>(
            postScreenshot, data, [&](std::string_view label) { return postScreenshot.readU32(label); },
            "master table size");
        if (result.mMasterTableSize.mValue == 0 || result.mMasterTableSize.mValue > limits.mMaxMasterTableBytes)
            throw FONVSaveError(
                result.mMasterTableSize.mRange.mOffset, "master table size exceeds the configured bound");
        if (result.mMasterTableSize.mValue > data.size() - postScreenshot.position())
            throw FONVSaveError(postScreenshot.position(), "truncated master table");

        const std::size_t masterTableBegin = postScreenshot.position();
        const std::size_t masterTableEnd = masterTableBegin + result.mMasterTableSize.mValue;
        Cursor masterTable(data, masterTableBegin, masterTableEnd);
        result.mMasterTableRange = range(masterTableBegin, masterTableEnd);
        result.mRawMasterTable = copyRange(data, masterTableBegin, masterTableEnd);
        result.mMasterCount = readField<std::uint8_t>(
            masterTable, data, [&](std::string_view label) { return masterTable.readU8(label); }, "master count");
        if (result.mMasterCount.mValue == 0)
            throw FONVSaveError(result.mMasterCount.mRange.mOffset, "master table is empty");
        if (result.mMasterCount.mValue > limits.mMaxMasterCount)
            throw FONVSaveError(result.mMasterCount.mRange.mOffset, "master count exceeds the configured bound");
        masterTable.expectDelimiter("master count");

        result.mMasters.reserve(result.mMasterCount.mValue);
        for (std::size_t i = 0; i < result.mMasterCount.mValue; ++i)
        {
            FONVSaveMaster master;
            master.mFileName = readStringField(masterTable, data, limits.mMaxMasterNameBytes, "master file name");
            if (master.mFileName.mValue.empty())
                throw FONVSaveError(master.mFileName.mValueRange.mOffset, "master file name is empty");
            result.mMasters.push_back(std::move(master));
        }
        if (masterTable.position() != masterTable.end())
            throw FONVSaveError(masterTable.position(), "master table contains unaccounted bytes");

        result.mHeaderAndMastersRange = { 0, masterTableEnd };
        if (sFileLocationTableBytes > data.size() - masterTableEnd)
            throw FONVSaveError(masterTableEnd, "truncated file-location table");
        const std::size_t fileLocationEnd = masterTableEnd + sFileLocationTableBytes;
        Cursor fileLocation(data, masterTableEnd, fileLocationEnd);
        result.mFileLocationTable.mRange = range(masterTableEnd, fileLocationEnd);
        result.mFileLocationTable.mRaw = copyRange(data, masterTableEnd, fileLocationEnd);
        result.mFileLocationTable.mRefIdArrayCountOffset = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "RefID-array count offset");
        result.mFileLocationTable.mUnknownTableOffset = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "unknown-table offset");
        result.mFileLocationTable.mGlobalDataTable1Offset = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "global-data table 1 offset");
        result.mFileLocationTable.mChangedFormsOffset = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "changed-forms offset");
        result.mFileLocationTable.mGlobalDataTable2Offset = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "global-data table 2 offset");
        result.mFileLocationTable.mGlobalDataTable1Count = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "global-data table 1 count");
        result.mFileLocationTable.mGlobalDataTable2Count = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "global-data table 2 count");
        result.mFileLocationTable.mChangedFormsCount = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "changed-forms count");
        result.mFileLocationTable.mUnknownCount = readField<std::uint32_t>(fileLocation, data,
            [&](std::string_view label) { return fileLocation.readU32(label); }, "unknown-table count");
        result.mFileLocationTable.mUnused
            = readRawField(fileLocation, data, sFileLocationUnusedBytes, "file-location unused bytes");
        if (fileLocation.position() != fileLocation.end())
            throw FONVSaveError(fileLocation.position(), "file-location table contains unaccounted bytes");

        const std::uint64_t fileSize = data.size();
        const auto validateOffset = [&](const FONVSaveField<std::uint32_t>& field, std::string_view description) {
            if (field.mValue > fileSize)
                throw FONVSaveError(field.mRange.mOffset, std::string(description) + " lies beyond EOF");
        };
        validateOffset(result.mFileLocationTable.mRefIdArrayCountOffset, "RefID-array count offset");
        validateOffset(result.mFileLocationTable.mUnknownTableOffset, "unknown-table offset");
        validateOffset(result.mFileLocationTable.mGlobalDataTable1Offset, "global-data table 1 offset");
        validateOffset(result.mFileLocationTable.mChangedFormsOffset, "changed-forms offset");
        validateOffset(result.mFileLocationTable.mGlobalDataTable2Offset, "global-data table 2 offset");

        const std::size_t globalData1Offset = result.mFileLocationTable.mGlobalDataTable1Offset.mValue;
        const std::size_t changedFormsOffset = result.mFileLocationTable.mChangedFormsOffset.mValue;
        const std::size_t globalData2Offset = result.mFileLocationTable.mGlobalDataTable2Offset.mValue;
        const std::size_t refIdArrayOffset = result.mFileLocationTable.mRefIdArrayCountOffset.mValue;
        const std::size_t unknownTableOffset = result.mFileLocationTable.mUnknownTableOffset.mValue;
        if (globalData1Offset != fileLocationEnd)
            throw FONVSaveError(result.mFileLocationTable.mGlobalDataTable1Offset.mRange.mOffset,
                "global-data table 1 does not immediately follow the file-location table");
        if (changedFormsOffset < globalData1Offset || globalData2Offset < changedFormsOffset
            || refIdArrayOffset < globalData2Offset || unknownTableOffset < refIdArrayOffset)
            throw FONVSaveError(masterTableEnd, "file-location sections overlap or are out of retail FNV order");

        result.mGlobalDataTable1 = parseGlobalDataTable(data, globalData1Offset, changedFormsOffset,
            result.mFileLocationTable.mGlobalDataTable1Count.mValue, limits, "global-data table 1");
        result.mChangedForms = parseChangedForms(data, changedFormsOffset, globalData2Offset,
            result.mFileLocationTable.mChangedFormsCount.mValue, limits);
        result.mGlobalDataTable2 = parseGlobalDataTable(data, globalData2Offset, refIdArrayOffset,
            result.mFileLocationTable.mGlobalDataTable2Count.mValue, limits, "global-data table 2");

        Cursor formIdAndWorldspaceTables(data, refIdArrayOffset, unknownTableOffset);
        result.mFormIdTable = parseFormIdTable(
            formIdAndWorldspaceTables, data, limits.mMaxFormIds, "FormID-array count");
        result.mVisitedWorldspaces = parseFormIdTable(
            formIdAndWorldspaceTables, data, limits.mMaxVisitedWorldspaces, "visited-worldspace count");
        if (formIdAndWorldspaceTables.position() != formIdAndWorldspaceTables.end())
            throw FONVSaveError(
                formIdAndWorldspaceTables.position(), "FormID and visited-worldspace tables contain unaccounted bytes");
        resolveChangedFormReferences(result.mChangedForms, result.mFormIdTable);

        for (const FONVSaveChangedFormEnvelope& entry : result.mChangedForms.mEntries)
        {
            if (entry.mChangeType == 9)
                result.mQuestChanges.push_back(parseQuestChange(data, entry, result.mFormIdTable));
            else if (entry.mChangeType == 34)
                result.mFactionChanges.push_back(parseFactionChange(data, entry, result.mFormIdTable));
        }

        for (const FONVSaveGlobalDataEntry& entry : result.mGlobalDataTable1.mEntries)
        {
            if (entry.mType.mValue == 3)
            {
                if (result.mGlobalVariables.has_value())
                    throw FONVSaveError(entry.mType.mRange.mOffset,
                        "multiple global-variable global-data entries are present");
                result.mGlobalVariables = parseGlobalVariablesState(data, entry, result.mFormIdTable);
                continue;
            }
            if (entry.mType.mValue != 8)
                continue;
            if (result.mSky.has_value())
                throw FONVSaveError(entry.mType.mRange.mOffset, "multiple Sky global-data entries are present");
            result.mSky = parseSkyState(data, entry, result.mFormIdTable);
        }

        const FONVSaveChangedFormEnvelope* playerReference = nullptr;
        for (const FONVSaveChangedFormEnvelope& entry : result.mChangedForms.mEntries)
        {
            if (entry.mResolvedFormId != sFONVPlayerReferenceFormId
                || entry.mChangeType != sFONVActorReferenceChangeType)
                continue;
            if (playerReference != nullptr)
                throw FONVSaveError(entry.mEncodedReferenceId.mRange.mOffset,
                    "multiple canonical player ACHR change forms are present");
            playerReference = &entry;
        }
        if (playerReference != nullptr)
        {
            constexpr std::uint32_t movedOrHavokMoved = 0x00000006;
            constexpr std::uint32_t changedCell = 0x00000008;
            if ((playerReference->mChangeFlags.mValue & changedCell) == 0
                && (playerReference->mChangeFlags.mValue & movedOrHavokMoved) != 0)
            {
                result.mPlayerReferenceMovement
                    = parsePlayerReferenceMovement(data, *playerReference, result.mFormIdTable);
                result.mPlayerActorValueData
                    = parsePlayerActorValueData(data, *playerReference, *result.mPlayerReferenceMovement);
                result.mPlayerProcessInventoryData = parsePlayerProcessInventoryData(
                    data, *playerReference, *result.mPlayerActorValueData, result.mFormIdTable);
                result.mPlayerMobileObjectProcessState = parsePlayerMobileObjectProcessState(
                    data, *playerReference, *result.mPlayerProcessInventoryData, result.mFormIdTable);
                result.mPlayerChangedCharacterState = parsePlayerChangedCharacterState(
                    data, *playerReference, *result.mPlayerMobileObjectProcessState, result.mFormIdTable);
                result.mPlayerCharacterAnimationState = parsePlayerCharacterAnimationState(
                    data, *playerReference, *result.mPlayerChangedCharacterState);
                result.mPlayerCharacterScalarReferenceState = parsePlayerCharacterScalarReferenceState(
                    data, *playerReference, *result.mPlayerCharacterAnimationState, result.mFormIdTable);
                result.mPlayerCharacterListsState = parsePlayerCharacterListsState(
                    data, *playerReference, *result.mPlayerCharacterScalarReferenceState, result.mFormIdTable);
                result.mPlayerCharacterMagicTargetState = parsePlayerCharacterMagicTargetState(
                    data, *playerReference, *result.mPlayerCharacterListsState, result.mFormIdTable);
                result.mPlayerCharacterFinalState = parsePlayerCharacterFinalState(
                    data, *playerReference, *result.mPlayerCharacterMagicTargetState, result.mFormIdTable);
            }
        }

        Cursor unknownTable(data, unknownTableOffset, data.size());
        const std::size_t unknownTableBegin = unknownTable.position();
        result.mUnknownTable.mCount = readField<std::uint32_t>(unknownTable, data,
            [&](std::string_view label) { return unknownTable.readU32(label); }, "unknown-table byte count");
        if (result.mUnknownTable.mCount.mValue > limits.mMaxUnknownTableBytes)
            throw FONVSaveError(result.mUnknownTable.mCount.mRange.mOffset,
                "unknown-table byte count exceeds the configured bound");
        if (result.mUnknownTable.mCount.mValue != unknownTable.end() - unknownTable.position())
            throw FONVSaveError(
                unknownTable.position(), "unknown-table byte count does not account exactly for the file tail");
        result.mUnknownTable.mUnparsedEntries
            = readRawField(unknownTable, data, result.mUnknownTable.mCount.mValue, "unknown-table entries");
        result.mUnknownTable.mRange = range(unknownTableBegin, unknownTable.position());

        for (const FONVSaveGlobalDataEntry& entry : result.mGlobalDataTable1.mEntries)
        {
            if (result.mGlobalVariables.has_value()
                && entry.mUnparsedPayload.mRange == result.mGlobalVariables->mRange)
                continue;
            if (result.mSky.has_value() && entry.mUnparsedPayload.mRange == result.mSky->mRange)
                continue;
            appendUnparsedSemanticPayload(result, entry.mUnparsedPayload.mRange);
        }
        for (const FONVSaveChangedFormEnvelope& entry : result.mChangedForms.mEntries)
        {
            const bool parsedChange
                = (entry.mChangeType == 9
                      && std::ranges::any_of(result.mQuestChanges,
                          [&](const FONVSaveQuestChange& quest) {
                              return quest.mRange == entry.mUnparsedPayload.mRange;
                          }))
                || (entry.mChangeType == 34
                    && std::ranges::any_of(result.mFactionChanges,
                        [&](const FONVSaveFactionChange& faction) {
                            return faction.mRange == entry.mUnparsedPayload.mRange;
                        }));
            if (parsedChange)
                continue;
            if (result.mPlayerCharacterFinalState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerCharacterFinalState->mUnparsedRemainder.mRange);
            else if (result.mPlayerCharacterMagicTargetState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(
                    result, result.mPlayerCharacterMagicTargetState->mUnparsedRemainder.mRange);
            else if (result.mPlayerCharacterListsState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerCharacterListsState->mUnparsedRemainder.mRange);
            else if (result.mPlayerCharacterScalarReferenceState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(
                    result, result.mPlayerCharacterScalarReferenceState->mUnparsedRemainder.mRange);
            else if (result.mPlayerCharacterAnimationState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(
                    result, result.mPlayerCharacterAnimationState->mUnparsedRemainder.mRange);
            else if (result.mPlayerChangedCharacterState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerChangedCharacterState->mUnparsedRemainder.mRange);
            else if (result.mPlayerMobileObjectProcessState.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerMobileObjectProcessState->mUnparsedRemainder.mRange);
            else if (result.mPlayerProcessInventoryData.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerProcessInventoryData->mUnparsedRemainder.mRange);
            else if (result.mPlayerActorValueData.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerActorValueData->mUnparsedRemainder.mRange);
            else if (result.mPlayerReferenceMovement.has_value() && &entry == playerReference)
                appendUnparsedSemanticPayload(result, result.mPlayerReferenceMovement->mUnparsedRemainder.mRange);
            else
                appendUnparsedSemanticPayload(result, entry.mUnparsedPayload.mRange);
        }
        for (const FONVSaveGlobalDataEntry& entry : result.mGlobalDataTable2.mEntries)
            appendUnparsedSemanticPayload(result, entry.mUnparsedPayload.mRange);
        appendUnparsedSemanticPayload(result, result.mUnknownTable.mUnparsedEntries.mRange);

        result.mStructurallyAccountedRange = { 0, data.size() };
        result.mParsedPrefixRange = result.mStructurallyAccountedRange;
        result.mUnparsedBodyRange = { data.size(), 0 };
        return result;
    }

    FONVSaveGamePrefix readFONVSaveGamePrefix(const std::filesystem::path& path, const FONVSaveLimits& limits)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            throw FONVSaveError(0, "could not open source file");
        const std::streampos end = stream.tellg();
        if (end < 0)
            throw FONVSaveError(0, "could not determine source file size");
        const auto fileSize = static_cast<std::uint64_t>(end);
        if (fileSize > limits.mMaxFileBytes || fileSize > std::numeric_limits<std::size_t>::max()
            || fileSize > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            throw FONVSaveError(0, "file exceeds the configured or platform bound");

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
        stream.seekg(0);
        if (!bytes.empty())
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size())
            throw FONVSaveError(0, "source file changed or was truncated while reading");
        return parseFONVSaveGamePrefix(bytes, path, limits);
    }
}

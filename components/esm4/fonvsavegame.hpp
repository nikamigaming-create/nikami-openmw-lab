#ifndef OPENMW_COMPONENTS_ESM4_FONVSAVEGAME
#define OPENMW_COMPONENTS_ESM4_FONVSAVEGAME

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ESM4
{
    inline constexpr std::uint32_t sFONVPlayerNpcFormId = 0x00000007;
    inline constexpr std::uint32_t sFONVPlayerReferenceFormId = 0x00000014;
    inline constexpr std::uint8_t sFONVActorReferenceChangeType = 1;

    struct FONVSaveRange
    {
        std::uint64_t mOffset = 0;
        std::uint64_t mSize = 0;

        std::uint64_t end() const { return mOffset + mSize; }
        bool empty() const { return mSize == 0; }

        friend bool operator==(const FONVSaveRange&, const FONVSaveRange&) = default;
    };

    template <class T>
    struct FONVSaveField
    {
        T mValue{};
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSaveRawField
    {
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSaveStringField
    {
        FONVSaveField<std::uint16_t> mLength;
        std::string mValue;
        FONVSaveRange mValueRange;
        FONVSaveRange mEncodedRange;
        std::vector<std::uint8_t> mRawValue;
        std::vector<std::uint8_t> mRawEncoded;
    };

    struct FONVSaveHeader
    {
        FONVSaveField<std::uint32_t> mVersion;
        std::optional<FONVSaveField<std::string>> mLanguage;
        FONVSaveField<std::uint32_t> mScreenshotWidth;
        FONVSaveField<std::uint32_t> mScreenshotHeight;
        FONVSaveField<std::uint32_t> mSaveNumber;
        FONVSaveStringField mPlayerName;
        FONVSaveStringField mPlayerKarmaTitle;
        FONVSaveField<std::uint32_t> mPlayerLevel;
        FONVSaveStringField mPlayerLocation;
        FONVSaveStringField mPlayTime;
    };

    struct FONVSaveMaster
    {
        FONVSaveStringField mFileName;
    };

    // Fallout New Vegas 1.4.0.525 writes this fixed 0x6e-byte table immediately after plugin information. All
    // offsets are absolute file offsets. The order below is the retail FNV order, which differs from later games.
    struct FONVSaveFileLocationTable
    {
        FONVSaveField<std::uint32_t> mRefIdArrayCountOffset;
        FONVSaveField<std::uint32_t> mUnknownTableOffset;
        FONVSaveField<std::uint32_t> mGlobalDataTable1Offset;
        FONVSaveField<std::uint32_t> mChangedFormsOffset;
        FONVSaveField<std::uint32_t> mGlobalDataTable2Offset;
        FONVSaveField<std::uint32_t> mGlobalDataTable1Count;
        FONVSaveField<std::uint32_t> mGlobalDataTable2Count;
        FONVSaveField<std::uint32_t> mChangedFormsCount;
        // Retail keeps this location-table field at zero in Save330. It is distinct from the counted byte table at
        // mUnknownTableOffset.
        FONVSaveField<std::uint32_t> mUnknownCount;
        FONVSaveRawField mUnused;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSaveGlobalDataEntry
    {
        FONVSaveField<std::uint32_t> mType;
        FONVSaveField<std::uint32_t> mDataLength;
        FONVSaveRange mHeaderRange;
        std::vector<std::uint8_t> mRawHeader;
        FONVSaveRawField mUnparsedPayload;
        FONVSaveRange mEnvelopeRange;
    };

    struct FONVSaveGlobalDataTable
    {
        FONVSaveRange mRange;
        std::vector<FONVSaveGlobalDataEntry> mEntries;
    };

    enum class FONVSaveReferenceKind : std::uint8_t
    {
        FormIdArray = 0,
        DefaultForm = 1,
        CreatedForm = 2,
    };

    struct FONVSaveChangedFormEnvelope
    {
        FONVSaveRawField mReferenceId;
        // RefIDs are unsigned 24-bit big-endian values. The high two bits select the namespace and the low 22
        // bits are either a one-based FormID-array index, a default FormID, or the low part of an FF-created ID.
        FONVSaveField<std::uint32_t> mEncodedReferenceId;
        FONVSaveReferenceKind mReferenceKind = FONVSaveReferenceKind::FormIdArray;
        std::uint32_t mReferencePayload = 0;
        std::optional<std::uint32_t> mResolvedFormId;
        FONVSaveField<std::uint32_t> mChangeFlags;
        FONVSaveField<std::uint8_t> mRawType;
        std::uint8_t mChangeType = 0;
        std::uint8_t mLengthWidth = 0;
        FONVSaveField<std::uint8_t> mVersion;
        FONVSaveField<std::uint32_t> mDataLength;
        FONVSaveRange mHeaderRange;
        std::vector<std::uint8_t> mRawHeader;
        FONVSaveRawField mUnparsedPayload;
        FONVSaveRange mEnvelopeRange;
    };

    struct FONVSaveChangedForms
    {
        FONVSaveRange mRange;
        std::vector<FONVSaveChangedFormEnvelope> mEntries;
    };

    struct FONVSaveFormIdTable
    {
        FONVSaveField<std::uint32_t> mCount;
        std::vector<FONVSaveField<std::uint32_t>> mFormIds;
        FONVSaveRange mRange;
    };

    struct FONVSaveUnknownTable
    {
        // This count is physically stored at mUnknownTableOffset and covers the remaining opaque bytes exactly.
        FONVSaveField<std::uint32_t> mCount;
        FONVSaveRawField mUnparsedEntries;
        FONVSaveRange mRange;
    };

    struct FONVSaveLimits
    {
        std::uint64_t mMaxFileBytes = 512ull * 1024 * 1024;
        std::uint32_t mMaxHeaderBytes = 64 * 1024;
        std::uint32_t mMaxScreenshotDimension = 8192;
        std::uint64_t mMaxScreenshotBytes = 192ull * 1024 * 1024;
        std::uint32_t mMaxMasterTableBytes = 1024 * 1024;
        std::uint16_t mMaxMasterNameBytes = 1024;
        std::size_t mMaxMasterCount = 255;
        std::size_t mMaxGlobalDataEntries = 1'000'000;
        std::size_t mMaxChangedForms = 1'000'000;
        std::size_t mMaxFormIds = 4'000'000;
        std::size_t mMaxVisitedWorldspaces = 1'000'000;
        std::size_t mMaxUnknownTableBytes = 16 * 1024 * 1024;
        std::uint64_t mMaxEntryPayloadBytes = 256ull * 1024 * 1024;
    };

    class FONVSaveError : public std::runtime_error
    {
    public:
        FONVSaveError(std::uint64_t offset, std::string message);

        std::uint64_t offset() const { return mOffset; }

    private:
        std::uint64_t mOffset;
    };

    // This deliberately represents only the structures that this reader validates. Global-data and change-form
    // envelopes are bounded exactly, but their payload bytes remain explicitly uninterpreted.
    struct FONVSaveGamePrefix
    {
        std::filesystem::path mSourcePath;
        std::uint64_t mFileSize = 0;

        FONVSaveRange mMagicRange;
        std::vector<std::uint8_t> mRawMagic;
        FONVSaveField<std::uint32_t> mHeaderSize;
        FONVSaveRange mHeaderRange;
        std::vector<std::uint8_t> mRawHeader;
        FONVSaveHeader mHeader;

        std::uint8_t mScreenshotBytesPerPixel = 3;
        FONVSaveRange mScreenshotRange;
        std::vector<std::uint8_t> mRawScreenshot;

        FONVSaveField<std::uint8_t> mFormVersion;
        FONVSaveField<std::uint32_t> mMasterTableSize;
        FONVSaveRange mMasterTableRange;
        std::vector<std::uint8_t> mRawMasterTable;
        FONVSaveField<std::uint8_t> mMasterCount;
        std::vector<FONVSaveMaster> mMasters;

        FONVSaveRange mHeaderAndMastersRange;
        FONVSaveFileLocationTable mFileLocationTable;
        FONVSaveGlobalDataTable mGlobalDataTable1;
        FONVSaveChangedForms mChangedForms;
        FONVSaveGlobalDataTable mGlobalDataTable2;
        FONVSaveFormIdTable mFormIdTable;
        FONVSaveFormIdTable mVisitedWorldspaces;
        FONVSaveUnknownTable mUnknownTable;

        // The parser accounts for every byte structurally. These ranges are the gameplay payload bytes whose
        // internal meaning is still deliberately unknown: every global-data payload, every changed-form payload,
        // and the trailing unknown-table entries. Envelope coverage is never reported as semantic coverage.
        std::vector<FONVSaveRange> mUnparsedSemanticPayloadRanges;
        std::uint64_t mUnparsedSemanticPayloadBytes = 0;
        FONVSaveRange mStructurallyAccountedRange;

        // Compatibility names retained for callers of the first reader slice. Once this slice succeeds, the
        // parsed range is the complete file and the unparsed body is empty; semantic gaps are listed above.
        FONVSaveRange mParsedPrefixRange;
        FONVSaveRange mUnparsedBodyRange;

        const FONVSaveChangedFormEnvelope* findChangedForm(
            std::uint32_t resolvedFormId, std::optional<std::uint8_t> changeType = std::nullopt) const;

        // FNV serializes player state under the canonical ACHR reference 0x14. Its base NPC_ FormID 0x7 is a
        // FalloutNV.esm relationship and is not duplicated in this save structure; plugin lookup remains required.
        const FONVSaveChangedFormEnvelope& requirePlayerReferenceChangeForm() const;
    };

    bool isFONVSaveGame(std::span<const std::uint8_t> data);

    FONVSaveGamePrefix parseFONVSaveGamePrefix(std::span<const std::uint8_t> data,
        const std::filesystem::path& sourcePath = {}, const FONVSaveLimits& limits = {});

    FONVSaveGamePrefix readFONVSaveGamePrefix(const std::filesystem::path& path, const FONVSaveLimits& limits = {});
}

#endif // OPENMW_COMPONENTS_ESM4_FONVSAVEGAME

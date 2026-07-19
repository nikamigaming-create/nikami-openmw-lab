#ifndef OPENMW_COMPONENTS_ESM4_FONVSAVEGAME
#define OPENMW_COMPONENTS_ESM4_FONVSAVEGAME

#include <array>
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
    inline constexpr std::size_t sFONVPlayerActorValueCount = 77;
    inline constexpr std::size_t sFONVPlayerActorValueDataBytes
        = 3 * sFONVPlayerActorValueCount * (sizeof(float) + 1) + sizeof(std::uint32_t) + 1;
    static_assert(sFONVPlayerActorValueDataBytes == 1160);
    inline constexpr std::size_t sFONVPlayerChangedCharacterStateBytes = 510;
    inline constexpr std::uint8_t sFONVExtraWornType = 0x16;
    inline constexpr std::uint8_t sFONVExtraCountType = 0x24;
    inline constexpr std::uint8_t sFONVExtraHealthType = 0x25;
    inline constexpr std::uint8_t sFONVExtraFactionChangesType = 0x5e;
    inline constexpr std::uint8_t sFONVExtraEncounterZoneType = 0x74;

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

    struct FONVSaveResolvedReferenceId
    {
        FONVSaveField<std::uint32_t> mEncoded;
        FONVSaveReferenceKind mKind = FONVSaveReferenceKind::FormIdArray;
        std::uint32_t mPayload = 0;
        std::optional<std::uint32_t> mResolvedFormId;
    };

    // Global-data table 1 type 8. Retail serializes every member with a trailing 0x7c delimiter. The four weather
    // identities use the save RefID namespace; null transition/override weather is valid.
    struct FONVSaveSkyState
    {
        FONVSaveResolvedReferenceId mCurrentWeather;
        FONVSaveResolvedReferenceId mTransitionWeather;
        FONVSaveResolvedReferenceId mDefaultWeather;
        FONVSaveResolvedReferenceId mOverrideWeather;
        FONVSaveField<float> mGameHour;
        FONVSaveField<float> mLastUpdateHour;
        FONVSaveField<float> mWeatherPercent;
        FONVSaveField<std::uint32_t> mFlags;
        FONVSaveField<float> mUnknown110;
        std::array<FONVSaveField<float>, 3> mVectorB4;
        FONVSaveField<std::uint32_t> mUnknownE4;
        FONVSaveField<float> mFogPower;
        FONVSaveField<std::uint32_t> mSkyMode;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // This is only the type-4 "Reference moved" prefix selected by ACHR change flags 1/2. Other initial-data
    // layouts remain opaque. The container can be either a CELL or WRLD; save bytes alone cannot distinguish them,
    // and this block carries no separate exterior cell-grid coordinates.
    struct FONVSavePlayerReferenceMovement
    {
        FONVSaveResolvedReferenceId mCellOrWorldspace;
        std::array<FONVSaveField<float>, 3> mPosition;
        // Bethesda reference placement stores these angles in radians, matching ESM::Position.
        std::array<FONVSaveField<float>, 3> mRotationRadians;
        FONVSaveField<std::uint8_t> mTerminator;
        FONVSaveRange mRange;
        FONVSaveRawField mUnparsedRemainder;
    };

    // Fixed player-only prefix immediately after the ACHR reference-movement block in changed-form version 27.
    // The three arrays are intentionally kept under xEdit's structural names: their broader runtime semantics are
    // not established. Each value and Unk4AC is followed by a retail 0x7c delimiter.
    struct FONVSavePlayerActorValueData
    {
        std::array<FONVSaveField<float>, sFONVPlayerActorValueCount> mActorValues244;
        std::array<FONVSaveField<float>, sFONVPlayerActorValueCount> mActorValues378;
        std::array<FONVSaveField<float>, sFONVPlayerActorValueCount> mActorValues4B0;
        FONVSaveField<std::uint32_t> mUnk4AC;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
        FONVSaveRawField mUnparsedRemainder;
    };

    struct FONVSavePlayerFactionChange
    {
        FONVSaveResolvedReferenceId mFaction;
        FONVSaveField<std::int8_t> mRank;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // Only the engine-authored ExtraFactionChanges and ExtraEncounterZone layouts are decoded by this slice.
    struct FONVSavePlayerActorExtraData
    {
        FONVSaveField<std::uint8_t> mType;
        std::optional<FONVSaveField<std::uint32_t>> mFactionChangeCount;
        std::vector<FONVSavePlayerFactionChange> mFactionChanges;
        std::optional<FONVSaveResolvedReferenceId> mEncounterZone;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // The type byte is authoritative. Payloads are populated only for ExtraCount and ExtraHealth; ExtraWorn has
    // no payload bytes and is represented solely by its type.
    struct FONVSavePlayerInventoryExtraData
    {
        FONVSaveField<std::uint8_t> mType;
        std::optional<FONVSaveField<std::int16_t>> mCount;
        std::optional<FONVSaveField<float>> mHealth;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerInventoryExtendData
    {
        FONVSaveField<std::uint32_t> mExtraDataCount;
        std::vector<FONVSavePlayerInventoryExtraData> mExtraData;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerInventoryEntry
    {
        FONVSaveResolvedReferenceId mType;
        FONVSaveField<std::int32_t> mDelta;
        FONVSaveField<std::uint32_t> mExtendDataCount;
        std::vector<FONVSavePlayerInventoryExtendData> mExtendData;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // ChangedMobileObject process level, ChangedREFR actor extras, then ChangedInventory. Array counts retain their
    // raw U6to30 encodings; the raw continuation is handed to the following MobileObject process-state parser.
    struct FONVSavePlayerProcessInventoryData
    {
        FONVSaveField<std::int8_t> mProcessLevel;
        FONVSaveField<std::uint32_t> mActorExtraDataCount;
        std::vector<FONVSavePlayerActorExtraData> mActorExtraData;
        FONVSaveField<std::uint32_t> mInventoryEntryCount;
        std::vector<FONVSavePlayerInventoryEntry> mInventoryEntries;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
        FONVSaveRawField mUnparsedRemainder;
    };
    // wbCoordXYZ is three adjacent little-endian floats followed by one save-field delimiter. The component ranges
    // cover the float bytes; the aggregate range/raw carrier also preserves the shared delimiter.
    struct FONVSavePlayerProcessVector3
    {
        std::array<FONVSaveField<float>, 3> mComponents;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // xEdit deliberately leaves both actor animation and high-process sub-buffers opaque. Their U6to30 lengths are
    // still decoded canonically and the exact body bytes remain range-addressable.
    struct FONVSavePlayerProcessSubBuffer
    {
        FONVSaveField<std::uint32_t> mLength;
        FONVSaveRawField mData;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerProcessModifier
    {
        FONVSaveField<std::uint8_t> mActorValue;
        FONVSaveField<float> mModifier;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // The Changed MobileObject members following ChangedREFR. Names intentionally mirror xEdit's authoritative FNV
    // save definition; unknown runtime semantics are not invented.
    struct FONVSavePlayerMobileObjectBaseState
    {
        std::array<FONVSaveField<std::int8_t>, 8> mBytes084_085_07C_07F_080_07D_07E_086;
        FONVSaveField<std::uint32_t> mUnk074;
        FONVSaveField<std::uint32_t> mUnk078;
        FONVSaveField<std::int8_t> mByt081;
        FONVSaveField<std::uint8_t> mByt083;
        FONVSaveResolvedReferenceId mUnk06C;
        FONVSaveResolvedReferenceId mUnk070;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerBaseProcessState
    {
        FONVSaveField<std::uint32_t> mUnk01C;
        FONVSaveField<std::uint32_t> mUnk020;
        FONVSaveField<std::uint32_t> mUnk024;
        FONVSaveResolvedReferenceId mPackage;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerLowProcessState
    {
        FONVSaveField<std::uint8_t> mByt030;
        FONVSaveField<std::uint32_t> mUnk0A4;
        FONVSaveResolvedReferenceId mBoundObject;
        FONVSaveField<std::uint32_t> mUnk058;
        FONVSaveField<std::uint32_t> mUnk0A8;
        FONVSaveField<std::uint32_t> mUnk0AC;
        FONVSaveField<std::uint8_t> mByt0B0;
        FONVSaveField<std::uint16_t> mWrd050;
        std::array<FONVSaveField<float>, 2> mUnk038;
        std::array<FONVSaveResolvedReferenceId, 5> mUnk040_044_048_FormList_054;
        FONVSaveField<std::uint32_t> mList006CCount;
        std::vector<FONVSaveResolvedReferenceId> mList006C;
        std::optional<FONVSaveField<std::uint32_t>> mDamageModifierCount;
        std::vector<FONVSavePlayerProcessModifier> mDamageModifiers;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerMiddleLowProcessState
    {
        FONVSaveField<std::uint32_t> mUnk0B4;
        std::optional<FONVSaveField<std::uint32_t>> mTempModifierCount;
        std::vector<FONVSavePlayerProcessModifier> mTempModifiers;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerMiddleHighList230Entry
    {
        FONVSaveResolvedReferenceId mBoundObject;
        FONVSaveField<std::int32_t> mUnknown;
        FONVSaveField<std::uint32_t> mUnk008;
        std::array<FONVSaveField<std::uint8_t>, 6> mBytes00C_00D_00E_00F_010_011;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerMiddleHighProcessState
    {
        std::array<FONVSaveField<std::uint8_t>, 3> mUnk134_135_168;
        std::array<FONVSaveField<std::uint32_t>, 3> mUnk170_174_108;
        FONVSaveField<std::uint8_t> mUnk1DA;
        FONVSavePlayerProcessVector3 mCoords0E4;
        FONVSaveField<std::uint32_t> mUnk0DC;
        std::array<FONVSaveField<std::uint8_t>, 3> mByt13D_144_156;
        FONVSaveField<std::uint16_t> mWrd154;
        FONVSavePlayerProcessVector3 mCoords148;
        std::array<FONVSaveField<std::uint8_t>, 4> mBool_Byt0E0_188_189;
        FONVSaveField<std::uint32_t> mUnk0D8;
        FONVSaveField<std::uint8_t> mByt18B;
        FONVSaveField<std::uint32_t> mUnk1D0;
        FONVSaveField<std::uint32_t> mUnk1D4;
        std::array<FONVSaveField<std::uint8_t>, 3> mByt1D8_1D9_228;
        FONVSaveField<std::uint16_t> mWrd22A;
        FONVSaveField<std::uint32_t> mUnk1A8;
        FONVSaveField<std::uint8_t> mByt0E1;
        FONVSaveField<std::uint32_t> mUnk190;
        FONVSaveField<std::uint32_t> mUnk198;
        FONVSaveField<std::uint8_t> mByt19C;
        FONVSaveField<std::uint8_t> mByt19D;
        std::array<FONVSaveField<std::uint32_t>, 4> mUnk234_238_23C_244;
        FONVSaveField<std::uint8_t> mUnk110;
        std::array<FONVSaveResolvedReferenceId, 4> mIdleForm10C_IdleForm194_Unk158_Unk140;
        FONVSaveField<std::uint32_t> mUnknown;
        FONVSaveField<std::uint32_t> mList0C8Count;
        std::vector<FONVSaveResolvedReferenceId> mList0C8;
        FONVSaveResolvedReferenceId mPackage;
        std::optional<FONVSavePlayerProcessSubBuffer> mAnimation;
        FONVSaveField<std::uint32_t> mMagicItemCount;
        std::array<FONVSaveResolvedReferenceId, 3> mUnk164_160_1BC;
        FONVSaveField<std::uint32_t> mList230Count;
        std::vector<FONVSavePlayerMiddleHighList230Entry> mList230;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerHighUnknownEntry
    {
        FONVSaveResolvedReferenceId mUnk3F8;
        FONVSaveField<std::uint8_t> mUnk410;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerHighLocationEntry
    {
        FONVSaveResolvedReferenceId mForm000;
        FONVSaveField<std::uint8_t> mUnk004;
        FONVSaveField<std::uint32_t> mUnk008;
        FONVSavePlayerProcessVector3 mCoords;
        FONVSaveField<float> mTim018;
        std::array<FONVSaveField<std::uint8_t>, 3> mByt01E_01C_01D;
        FONVSaveField<std::uint32_t> mUnk020;
        FONVSaveField<std::uint8_t> mByt01F;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerHighProcessState
    {
        std::array<FONVSaveField<std::uint8_t>, 4> mUnk32C_340_374_375;
        FONVSaveField<std::uint16_t> mUnk2FC;
        std::array<FONVSaveField<std::uint32_t>, 11> mUnk2B4_2F8_310_330_334_338_34C_294_2B8_2BC_298;
        std::array<FONVSaveField<std::uint16_t>, 3> mUnk2C0_2C2_2C4;
        FONVSaveField<std::uint8_t> mUnk349;
        FONVSavePlayerProcessVector3 mCoords;
        std::array<FONVSaveField<std::uint32_t>, 6> mUnk36C_3E8_3EC_33C_2A8_378;
        FONVSaveField<std::uint8_t> mUnk3A0;
        FONVSaveField<std::uint32_t> mUnk39C;
        FONVSaveField<std::uint8_t> mUnk3A8;
        FONVSaveField<std::uint32_t> mUnk3A4;
        FONVSaveField<std::uint8_t> mUnk420;
        FONVSaveField<std::uint32_t> mUnk3BC;
        FONVSaveField<std::uint32_t> mUnk3B0;
        FONVSaveField<std::uint8_t> mUnk2C6;
        std::array<FONVSaveField<std::uint32_t>, 3> mUnk2D0_2D4_2D8;
        FONVSaveField<std::uint8_t> mUnk3B8;
        FONVSaveField<std::uint8_t> mUnk2DC1;
        FONVSaveField<std::uint32_t> mUnk2E0;
        FONVSaveField<std::uint32_t> mUnk344;
        FONVSaveField<std::uint8_t> mUnk2DC2;
        FONVSaveField<std::uint8_t> mUnk2DC3;
        FONVSaveField<std::uint32_t> mUnk3D8Modulo12;
        FONVSaveField<std::uint32_t> mUnk448;
        FONVSaveField<std::uint8_t> mUnk29D;
        std::array<FONVSaveField<std::uint32_t>, 5> mUnk2B0_2C8_418_43C_440;
        FONVSaveField<std::uint8_t> mUnk444;
        FONVSaveField<std::uint8_t> mUnk445;
        FONVSaveField<std::uint32_t> mUnk450;
        FONVSaveField<std::uint8_t> mUnk458;
        FONVSaveField<std::uint32_t> mUnk430;
        FONVSaveField<std::uint8_t> mUnk3E0;
        FONVSaveField<std::uint8_t> mUnk459;
        FONVSaveField<std::uint32_t> mUnk2A0;
        std::array<FONVSaveField<std::uint8_t>, 4> mUnk3D0_3D1_348_Unknown;
        std::array<FONVSaveResolvedReferenceId, 7> mUnk30C_2A4_3F0_41C_37C_Idle_2AC;
        std::array<FONVSavePlayerHighUnknownEntry, 6> mUnknownEntries;
        FONVSaveField<std::uint32_t> mList38CCount;
        std::vector<FONVSaveResolvedReferenceId> mList38C;
        FONVSaveField<std::uint32_t> mList394Count;
        std::vector<FONVSaveResolvedReferenceId> mList394;
        FONVSaveField<std::uint32_t> mList264Count;
        std::vector<FONVSaveResolvedReferenceId> mList264;
        FONVSaveField<std::uint8_t> mHasDialogueItems;
        FONVSaveField<std::uint32_t> mList44CCount;
        FONVSaveField<std::uint32_t> mList25CCount;
        std::vector<FONVSavePlayerHighLocationEntry> mList25C;
        FONVSaveField<std::uint32_t> mList260Count;
        std::vector<FONVSavePlayerHighLocationEntry> mList260;
        FONVSaveField<std::uint8_t> mHasUnk3DC;
        FONVSavePlayerProcessSubBuffer mSubBuffer;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // Exact changed-form version-27, process-level-0 continuation after ChangedInventory. Any xEdit branch not
    // represented above is rejected instead of being guessed. Its raw continuation is handed to ChangedActor.
    struct FONVSavePlayerMobileObjectProcessState
    {
        FONVSavePlayerMobileObjectBaseState mMobileObjectBase;
        FONVSavePlayerBaseProcessState mBaseProcess;
        FONVSavePlayerLowProcessState mLowProcess;
        FONVSavePlayerMiddleLowProcessState mMiddleLowProcess;
        FONVSavePlayerMiddleHighProcessState mMiddleHighProcess;
        FONVSavePlayerHighProcessState mHighProcess;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
        FONVSaveRawField mUnparsedRemainder;
    };

    // ChangedActor's fixed members after ChangedMobileObject. Version-gated members are present because the
    // enclosing canonical player ACHR is required to be changed-form version 27. Names deliberately follow the
    // authoritative FNV save definition; unknown runtime meanings are not inferred.
    struct FONVSavePlayerChangedActorFixedState
    {
        FONVSaveField<float> mUnk114;
        std::array<FONVSaveField<std::uint8_t>, 4> mByt124_125_0BC_0C4;
        FONVSaveField<std::uint32_t> mUnk0C8;
        FONVSaveField<std::uint8_t> mByt07D;
        FONVSaveField<std::uint32_t> mUnk110;
        std::array<FONVSaveField<std::uint8_t>, 6> mByt118_126_145_146_14C_14D;
        std::array<FONVSaveField<std::uint32_t>, 3> mUnk150_154_158;
        std::array<FONVSaveField<std::uint8_t>, 3> mByt174_175_18D;
        std::array<FONVSaveField<std::uint32_t>, 2> mUnk1A4_1A8;
        std::array<FONVSaveField<std::uint8_t>, 2> mByt0F0_0F1;
        FONVSaveField<std::uint32_t> mUnk10C;
        FONVSaveField<std::uint8_t> mUnk0138Byt000;
        FONVSaveField<std::uint32_t> mUnk0138Unk004;
        FONVSaveField<std::uint8_t> mUnk0138Byt010;
        std::array<FONVSaveField<std::uint32_t>, 2> mUnk0138Unk008_00C;
        FONVSaveField<std::uint32_t> mUnk120;
        std::array<FONVSaveResolvedReferenceId, 3> mForm0C0_ActorBase_Form070;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerPathingLocation
    {
        FONVSavePlayerProcessVector3 mCoords;
        std::array<FONVSaveResolvedReferenceId, 3> mNavMesh_Cell_Worldspace;
        FONVSaveField<std::uint32_t> mCoordXandY;
        FONVSaveField<std::int16_t> mWrd024;
        FONVSaveField<std::uint8_t> mByt026;
        FONVSaveField<std::uint8_t> mUnknown;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // Save330 selects ActorMover content bit 3, the detailed actor path handler. Other content branches have no
    // enclosing byte length, so accepting them here would move the boundary by guesswork and is rejected.
    struct FONVSavePlayerDetailedActorPathHandler
    {
        std::array<FONVSavePlayerProcessVector3, 5> mCoords01C_028_034_040_04C;
        std::array<FONVSaveField<std::uint32_t>, 20>
            mUnk060_064_068_06C_070_074_078_07C_080_084_088_08C_090_094_098_09C_0AC_0B0_0B4_0B8;
        std::array<FONVSaveField<std::uint32_t>, 2> mUnk014;
        std::array<FONVSaveField<std::uint32_t>, 4> mUnk0C8_0CC_0D0_0D4;
        std::array<FONVSaveField<std::uint8_t>, 7> mByt0DC_0DD_0DE_0DF_0E0_0E2_0E1;
        FONVSaveField<std::uint8_t> mUnk0C0Byt000;
        FONVSaveField<std::uint8_t> mUnk0C0Byt001;
        FONVSaveField<std::uint32_t> mUnk0C0Unk004;
        FONVSavePlayerProcessVector3 mUnknownCoords;
        FONVSaveField<std::uint32_t> mUnk0BC;
        FONVSaveResolvedReferenceId mForm0D8;
        FONVSaveField<std::uint32_t> mList058Count;
        std::vector<FONVSaveResolvedReferenceId> mList058;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerMoverState
    {
        FONVSavePlayerProcessVector3 mCoords;
        std::array<FONVSaveField<std::uint32_t>, 3> mUnk094_098_09C;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    struct FONVSavePlayerActorMoverState
    {
        FONVSaveField<std::uint16_t> mWrd040;
        FONVSaveField<std::uint16_t> mWrd042;
        FONVSaveField<std::uint8_t> mByt070;
        FONVSaveField<std::uint32_t> mUnk034;
        FONVSaveField<std::uint8_t> mByt071;
        FONVSaveField<std::uint32_t> mUnk038;
        std::array<FONVSaveField<std::uint8_t>, 2> mByt072_073;
        FONVSavePlayerProcessVector3 mCoords004;
        FONVSavePlayerProcessVector3 mCoords010;
        FONVSaveField<std::uint32_t> mUnk03C;
        std::array<FONVSaveField<std::uint8_t>, 5> mByt074_075_077_076_078;
        FONVSaveField<std::uint32_t> mUnk06C;
        std::array<FONVSaveField<std::uint32_t>, 2> mUnknown_Unk084;
        FONVSavePlayerPathingLocation mPathingLocation;
        FONVSaveResolvedReferenceId mForm02C;
        FONVSaveField<std::uint8_t> mContentFlags;
        FONVSavePlayerDetailedActorPathHandler mDetailedPathHandler;
        FONVSavePlayerMoverState mPlayerMover;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
    };

    // Exact 510-byte ChangedActor/ActorMover/ChangedCharacter continuation in pinned Save330. Its raw continuation
    // is handed to the second player animation-buffer parser.
    struct FONVSavePlayerChangedCharacterState
    {
        FONVSavePlayerChangedActorFixedState mActorFixed;
        FONVSavePlayerActorMoverState mActorMover;
        FONVSaveField<std::uint8_t> mByt1C0;
        FONVSaveField<std::uint8_t> mByt1C1;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
        FONVSaveRawField mUnparsedRemainder;
    };

    // The second player animation buffer is an authoritative U6to30-sized field after ChangedCharacter. xEdit
    // deliberately leaves the animation body opaque, so its exact bytes and absolute range are preserved without
    // inventing an internal layout. The following PlayerCharacter bytes remain explicitly unparsed.
    struct FONVSavePlayerCharacterAnimationState
    {
        FONVSavePlayerProcessSubBuffer mAnimation;
        FONVSaveRange mRange;
        std::vector<std::uint8_t> mRaw;
        FONVSaveRawField mUnparsedRemainder;
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
        // Raw carrier for the complete payload. A proven semantic subset may also be exposed separately; consult
        // FONVSaveGamePrefix::mUnparsedSemanticPayloadRanges for the bytes that remain semantically uncovered.
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

    // This deliberately represents only the structures that this reader validates. Recognized semantic payloads
    // are exposed explicitly; every remaining global-data and change-form payload stays byte-accounted and opaque.
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
        std::optional<FONVSavePlayerReferenceMovement> mPlayerReferenceMovement;
        std::optional<FONVSavePlayerActorValueData> mPlayerActorValueData;
        std::optional<FONVSavePlayerProcessInventoryData> mPlayerProcessInventoryData;
        std::optional<FONVSavePlayerMobileObjectProcessState> mPlayerMobileObjectProcessState;
        std::optional<FONVSavePlayerChangedCharacterState> mPlayerChangedCharacterState;
        std::optional<FONVSavePlayerCharacterAnimationState> mPlayerCharacterAnimationState;
        std::optional<FONVSaveSkyState> mSky;

        // The parser accounts for every byte structurally. These ranges are gameplay payload bytes whose internal
        // meaning is still deliberately unknown: unrecognized global data, undecoded change-form bytes, and the
        // trailing unknown-table entries. Envelope coverage is never reported as semantic coverage.
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

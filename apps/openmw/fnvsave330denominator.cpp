#include <components/esm4/fonvsavegame.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/reader.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/files/collections.hpp>
#include <components/files/openfile.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include "mwworld/esmstore.hpp"
#include "mwworld/fnvplayerstate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    struct Arguments
    {
        std::filesystem::path mSave;
        std::filesystem::path mContentProfile;
        std::filesystem::path mOutput;
        std::optional<std::filesystem::path> mInventoryJoinOutput;
    };

    struct ContentProfile
    {
        std::filesystem::path mConfigPath;
        Files::PathContainer mDataDirectories;
        std::vector<std::string> mContentFiles;
    };

    std::string trim(std::string value)
    {
        const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
            return !isSpace(static_cast<unsigned char>(ch));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
                        return !isSpace(static_cast<unsigned char>(ch));
                    }).base(),
            value.end());
        return value;
    }

    std::string unquote(std::string value)
    {
        value = trim(std::move(value));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            return value.substr(1, value.size() - 2);
        return value;
    }

    Arguments parseArguments(int argc, char** argv)
    {
        Arguments result;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view option(argv[index]);
            if (option == "--save" || option == "--content-profile" || option == "--output"
                || option == "--inventory-join-output")
            {
                if (index + 1 >= argc)
                    throw std::runtime_error(std::string(option) + " requires a value");
                const std::filesystem::path value = std::filesystem::u8path(argv[++index]);
                if (option == "--save")
                    result.mSave = value;
                else if (option == "--content-profile")
                    result.mContentProfile = value;
                else
                {
                    if (option == "--output")
                        result.mOutput = value;
                    else
                        result.mInventoryJoinOutput = value;
                }
            }
            else
            {
                throw std::runtime_error("unknown option: " + std::string(option));
            }
        }
        if (result.mSave.empty() || result.mContentProfile.empty() || result.mOutput.empty())
            throw std::runtime_error(
                "usage: fnv-save330-denominator --save <save.fos> --content-profile <profile> --output <json> "
                "[--inventory-join-output <json>]");
        return result;
    }

    ContentProfile readContentProfile(const std::filesystem::path& requested)
    {
        ContentProfile result;
        result.mConfigPath = std::filesystem::is_directory(requested) ? requested / "openmw.cfg" : requested;
        std::ifstream stream(result.mConfigPath);
        if (!stream)
            throw std::runtime_error("could not open content profile: " + result.mConfigPath.generic_string());

        std::string line;
        while (std::getline(stream, line))
        {
            const std::size_t comment = line.find('#');
            if (comment != std::string::npos)
                line.resize(comment);
            line = trim(std::move(line));
            if (line.empty())
                continue;
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, separator));
            const std::string value = unquote(line.substr(separator + 1));
            if (value.empty())
                continue;

            if (key == "content")
                result.mContentFiles.push_back(value);
            else if (key == "data" || key == "data-local")
            {
                std::filesystem::path directory = std::filesystem::u8path(value);
                if (directory.is_relative())
                    directory = result.mConfigPath.parent_path() / directory;
                if (std::filesystem::is_directory(directory))
                    result.mDataDirectories.push_back(std::move(directory));
            }
        }

        if (result.mDataDirectories.empty())
            throw std::runtime_error("content profile has no existing data/data-local directory");
        if (result.mContentFiles.empty())
            throw std::runtime_error("content profile has no content files");
        return result;
    }

    std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            throw std::runtime_error("could not open save: " + path.generic_string());
        const std::streampos end = stream.tellg();
        if (end < 0)
            throw std::runtime_error("could not size save: " + path.generic_string());
        std::vector<std::uint8_t> result(static_cast<std::size_t>(end));
        stream.seekg(0);
        if (!result.empty())
            stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
        if (!stream || static_cast<std::size_t>(stream.gcount()) != result.size())
            throw std::runtime_error("save changed or was truncated while being read");
        return result;
    }

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
            for (std::size_t index = 0; index < 16; ++index)
            {
                const std::size_t wordOffset = offset + index * 4;
                words[index] = (static_cast<std::uint32_t>(padded[wordOffset]) << 24)
                    | (static_cast<std::uint32_t>(padded[wordOffset + 1]) << 16)
                    | (static_cast<std::uint32_t>(padded[wordOffset + 2]) << 8)
                    | static_cast<std::uint32_t>(padded[wordOffset + 3]);
            }
            for (std::size_t index = 16; index < words.size(); ++index)
            {
                const std::uint32_t s0
                    = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^ (words[index - 15] >> 3);
                const std::uint32_t s1
                    = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^ (words[index - 2] >> 10);
                words[index] = words[index - 16] + s0 + words[index - 7] + s1;
            }

            std::uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
            std::uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
            for (std::size_t index = 0; index < words.size(); ++index)
            {
                const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
                const std::uint32_t choose = (e & f) ^ (~e & g);
                const std::uint32_t temp1 = h + sum1 + choose + constants[index] + words[index];
                const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
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

    void writeJsonString(std::ostream& stream, std::string_view value)
    {
        stream << '"';
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
                case '"': stream << "\\\""; break;
                case '\\': stream << "\\\\"; break;
                case '\b': stream << "\\b"; break;
                case '\f': stream << "\\f"; break;
                case '\n': stream << "\\n"; break;
                case '\r': stream << "\\r"; break;
                case '\t': stream << "\\t"; break;
                default:
                    if (ch < 0x20)
                        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<unsigned int>(ch) << std::dec;
                    else
                        stream << static_cast<char>(ch);
            }
        }
        stream << '"';
    }

    std::string formIdString(ESM::FormId value)
    {
        std::ostringstream stream;
        stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value.toUint32();
        return stream.str();
    }

    void writeRange(std::ostream& stream, const ESM4::FONVSaveRange& range)
    {
        stream << "{\"offset\":" << range.mOffset << ",\"bytes\":" << range.mSize << '}';
    }

    void writeSaveProvenance(std::ostream& stream, const ESM4::FONVSaveRange& range)
    {
        stream << "{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, range);
        stream << '}';
    }

    template <class T>
    void writeValueWithProvenance(std::ostream& stream, T value, const ESM4::FONVSaveRange& range)
    {
        stream << "{\"value\":";
        if constexpr (std::is_same_v<T, bool>)
            stream << (value ? "true" : "false");
        else if constexpr (std::is_floating_point_v<T>)
            stream << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
        else
            stream << +value;
        stream << ",\"provenance\":";
        writeSaveProvenance(stream, range);
        stream << '}';
    }

    void writeFormIdValue(std::ostream& stream, ESM::FormId value, const ESM4::FONVSaveRange& range)
    {
        stream << "{\"value\":";
        writeJsonString(stream, formIdString(value));
        stream << ",\"provenance\":";
        writeSaveProvenance(stream, range);
        stream << '}';
    }

    std::string kindName(MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind kind)
    {
        switch (kind)
        {
            case MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind::Permanent: return "permanent";
            case MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind::Damage: return "damage";
            case MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind::Temporary: return "temporary";
        }
        throw std::runtime_error("unknown normalized actor-value modifier kind");
    }

    std::string contentFileForForm(ESM::FormId id, std::span<const std::string> contentFiles)
    {
        if (id.hasContentFile() && id.mContentFile >= 0
            && static_cast<std::size_t>(id.mContentFile) < contentFiles.size())
        {
            return contentFiles[static_cast<std::size_t>(id.mContentFile)];
        }
        return "<no-content-file>";
    }

    struct InventoryRecordDetails
    {
        std::string mFamily;
        std::string mEditorId;
        std::string mDisplayName;
        std::string mIcon;
        std::string mContentFile;
        std::optional<ESM::FormId> mWeaponAmmo;
        std::vector<ESM::FormId> mWeaponAmmoList;
        std::optional<std::uint8_t> mWeaponClipSize;
        std::vector<ESM::FormId> mAlchEffectReferences;
    };

    bool isSetFormId(ESM::FormId id)
    {
        return !id.isZeroOrUnset();
    }

    InventoryRecordDetails resolveInventoryRecord(const MWWorld::ESMStore& store, ESM::FormId id,
        std::span<const std::string> contentFiles)
    {
        const ESM::RefId refId(id);
        InventoryRecordDetails details;
        details.mContentFile = contentFileForForm(id, contentFiles);

        if (const auto* record = store.get<ESM4::Weapon>().search(refId))
        {
            details.mFamily = "WEAP";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            details.mWeaponClipSize = record->mData.clipSize;
            if (isSetFormId(record->mAmmo))
            {
                details.mWeaponAmmo = record->mAmmo;
                if (const auto* list = store.get<ESM4::FormIdList>().search(ESM::RefId(record->mAmmo)))
                    details.mWeaponAmmoList = list->mObjects;
            }
            return details;
        }
        if (const auto* record = store.get<ESM4::Armor>().search(refId))
        {
            details.mFamily = "ARMO";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = !record->mIconMale.empty() ? record->mIconMale : record->mIconFemale;
            return details;
        }
        if (const auto* record = store.get<ESM4::Clothing>().search(refId))
        {
            details.mFamily = "CLOT";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = !record->mIconMale.empty() ? record->mIconMale : record->mIconFemale;
            return details;
        }
        if (const auto* record = store.get<ESM4::Ammunition>().search(refId))
        {
            details.mFamily = "AMMO";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            return details;
        }
        if (const auto* record = store.get<ESM4::Potion>().search(refId))
        {
            details.mFamily = "ALCH";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            const ESM::FormId effect = ESM::FormId::fromUint32(record->mEffect.formId);
            if (isSetFormId(effect))
                details.mAlchEffectReferences.push_back(effect);
            return details;
        }
        if (const auto* record = store.get<ESM4::Ingredient>().search(refId))
        {
            details.mFamily = "INGR";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            const ESM::FormId effect = ESM::FormId::fromUint32(record->mEffect.formId);
            if (isSetFormId(effect))
                details.mAlchEffectReferences.push_back(effect);
            return details;
        }
        if (const auto* record = store.get<ESM4::MiscItem>().search(refId))
        {
            details.mFamily = "MISC";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            return details;
        }
        if (const auto* record = store.get<ESM4::Key>().search(refId))
        {
            details.mFamily = "KEYM";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            return details;
        }
        if (const auto* record = store.get<ESM4::Book>().search(refId))
        {
            details.mFamily = "BOOK";
            details.mEditorId = record->mEditorId;
            details.mDisplayName = record->mFullName;
            details.mIcon = record->mIcon;
            return details;
        }

        throw std::runtime_error("positive normalized inventory FormID is unresolved in official content: "
            + formIdString(id));
    }

    std::optional<ESM4::FONVSaveRange> findInventoryExtraRange(const ESM4::FONVSaveGamePrefix& save,
        std::uint64_t sourceOffset, std::uint32_t extraType)
    {
        if (!save.mPlayerProcessInventoryData)
            return std::nullopt;
        for (const auto& entry : save.mPlayerProcessInventoryData->mInventoryEntries)
        {
            for (const auto& extend : entry.mExtendData)
            {
                for (const auto& extra : extend.mExtraData)
                {
                    if (extra.mType.mValue != extraType)
                        continue;
                    if (extra.mRange.mOffset == sourceOffset || extra.mType.mRange.mOffset == sourceOffset
                        || (extra.mHotkey && extra.mHotkey->mRange.mOffset == sourceOffset)
                        || (extra.mAmmo && extra.mAmmo->mEncoded.mRange.mOffset == sourceOffset)
                        || (extra.mAmmoCount && extra.mAmmoCount->mRange.mOffset == sourceOffset))
                    {
                        return extra.mRange;
                    }
                }
            }
        }
        return std::nullopt;
    }

    std::optional<ESM4::FONVSaveRange> findInventoryExtendRange(const ESM4::FONVSaveGamePrefix& save,
        std::uint64_t sourceOffset)
    {
        if (!save.mPlayerProcessInventoryData)
            return std::nullopt;
        for (const auto& entry : save.mPlayerProcessInventoryData->mInventoryEntries)
            for (const auto& extend : entry.mExtendData)
                if (extend.mRange.mOffset == sourceOffset)
                    return extend.mRange;
        return std::nullopt;
    }

    std::optional<ESM4::FONVSaveRange> findActorModifierRange(const ESM4::FONVSaveGamePrefix& save,
        const MWWorld::FalloutSavePlayerHeaderState::ActorValueModifier& modifier)
    {
        if (modifier.mKind == MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind::Permanent
            && save.mPlayerActorValueData && modifier.mActorValue < save.mPlayerActorValueData->mActorValues378.size())
        {
            const auto& field = save.mPlayerActorValueData->mActorValues378[modifier.mActorValue];
            if (field.mRange.mOffset == modifier.mSourceOffset)
                return field.mRange;
        }
        if (modifier.mKind == MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind::Damage
            && save.mPlayerMobileObjectProcessState)
        {
            for (const auto& candidate : save.mPlayerMobileObjectProcessState->mLowProcess.mDamageModifiers)
                if (candidate.mRange.mOffset == modifier.mSourceOffset)
                    return candidate.mRange;
        }
        if (modifier.mKind == MWWorld::FalloutSavePlayerHeaderState::ActorValueModifierKind::Temporary
            && save.mPlayerMobileObjectProcessState)
        {
            for (const auto& candidate : save.mPlayerMobileObjectProcessState->mMiddleLowProcess.mTempModifiers)
                if (candidate.mRange.mOffset == modifier.mSourceOffset)
                    return candidate.mRange;
        }
        return std::nullopt;
    }

    const ESM4::FONVSaveGlobalVariable* findRawGlobal(const ESM4::FONVSaveGamePrefix& save, std::uint64_t sourceOffset)
    {
        if (!save.mGlobalVariables)
            return nullptr;
        for (const auto& value : save.mGlobalVariables->mVariables)
            if (value.mRange.mOffset == sourceOffset)
                return &value;
        return nullptr;
    }

    void validateSaveMasterOrder(const ESM4::FONVSaveGamePrefix& save, std::span<const std::string> contentFiles)
    {
        if (save.mMasters.size() != 10 || contentFiles.size() != save.mMasters.size())
            throw std::runtime_error("Save330 denominator requires exactly the ten saved official masters");
        for (std::size_t index = 0; index < save.mMasters.size(); ++index)
        {
            if (!Misc::StringUtils::ciEqual(save.mMasters[index].mFileName.mValue, contentFiles[index]))
                throw std::runtime_error("content profile order does not match the Save330 master table at index "
                    + std::to_string(index));
        }
    }

    void loadOfficialContent(const ContentProfile& profile, MWWorld::ESMStore& store)
    {
        Files::Collections collections(profile.mDataDirectories);
        std::map<std::string, int> fileToModIndex;
        Loading::Listener listener;
        for (std::size_t index = 0; index < profile.mContentFiles.size(); ++index)
        {
            const std::string& contentFile = profile.mContentFiles[index];
            const std::filesystem::path filename = collections.getPath(contentFile);
            auto input = Files::openBinaryInputFileStream(filename);
            ESM4::Reader reader(std::move(input), filename, nullptr, nullptr, true);
            reader.setModIndex(static_cast<std::uint32_t>(index));
            reader.updateModIndices(fileToModIndex);
            store.loadESM4(reader, &listener);
            fileToModIndex[Misc::StringUtils::lowerCase(filename.filename().string())]
                = static_cast<int>(index);
        }
        store.setUp();
    }

    void writeProfile(std::ostream& stream, const ContentProfile& profile)
    {
        stream << "{\"path\":";
        writeJsonString(stream, profile.mConfigPath.generic_string());
        stream << ",\"provenance\":{\"kind\":\"content-profile\",\"path\":";
        writeJsonString(stream, profile.mConfigPath.generic_string());
        stream << "},\"contentFiles\":[";
        for (std::size_t index = 0; index < profile.mContentFiles.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << "{\"loadOrder\":" << index << ",\"name\":";
            writeJsonString(stream, profile.mContentFiles[index]);
            stream << "}";
        }
        stream << "]}";
    }

    void writeInventory(std::ostream& stream, const ESM4::FONVSaveGamePrefix& save,
        const MWWorld::FalloutSavePlayerHeaderState& player, std::span<const std::string> contentFiles)
    {
        stream << "{\"finalTotals\":[";
        for (std::size_t index = 0; index < player.mInventoryItems.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& item = player.mInventoryItems[index];
            stream << "{\"formId\":";
            writeJsonString(stream, formIdString(item.mRecord));
            stream << ",\"count\":" << item.mCount << ",\"provenance\":[";
            bool first = true;
            for (const auto& contribution : player.mInventoryContributions)
            {
                if (contribution.mRecord != item.mRecord)
                    continue;
                if (!first)
                    stream << ',';
                first = false;
                if (!contribution.mFromSave)
                {
                    stream << "{\"kind\":\"content-record\",\"contentFile\":";
                    writeJsonString(stream, contentFileForForm(contribution.mSourceRecord, contentFiles));
                    stream << ",\"formId\":";
                    writeJsonString(stream, formIdString(contribution.mSourceRecord));
                    stream << ",\"delta\":" << contribution.mDelta << '}';
                }
                else
                {
                    stream << "{\"kind\":\"save-bytes\",\"sourceFormId\":";
                    writeJsonString(stream, formIdString(contribution.mSourceRecord));
                    stream << ",\"delta\":" << contribution.mDelta << ",\"range\":{\"offset\":"
                           << contribution.mSourceOffset << ",\"bytes\":" << contribution.mSourceBytes
                           << "},\"formIdRange\":{\"offset\":" << contribution.mFormIdOffset
                           << ",\"bytes\":" << contribution.mFormIdBytes << "}}";
                }
            }
            stream << "]}";
        }
        stream << "],\"contributions\":[";
        for (std::size_t index = 0; index < player.mInventoryContributions.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& contribution = player.mInventoryContributions[index];
            stream << "{\"record\":";
            writeJsonString(stream, formIdString(contribution.mRecord));
            stream << ",\"sourceRecord\":";
            writeJsonString(stream, formIdString(contribution.mSourceRecord));
            stream << ",\"delta\":" << contribution.mDelta << ",\"fromSave\":"
                   << (contribution.mFromSave ? "true" : "false");
            if (contribution.mFromSave)
            {
                stream << ",\"range\":{\"offset\":" << contribution.mSourceOffset << ",\"bytes\":"
                       << contribution.mSourceBytes << "},\"formIdRange\":{\"offset\":"
                       << contribution.mFormIdOffset << ",\"bytes\":" << contribution.mFormIdBytes << '}';
            }
            else
            {
                stream << ",\"provenance\":{\"kind\":\"content-record\",\"contentFile\":";
                writeJsonString(stream, contentFileForForm(contribution.mSourceRecord, contentFiles));
                stream << '}';
            }
            stream << '}';
        }
        stream << "],\"conditionedStacks\":[";
        for (std::size_t index = 0; index < player.mConditionedStacks.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& stack = player.mConditionedStacks[index];
            const auto range = findInventoryExtendRange(save, stack.mSourceOffset);
            if (!range)
                throw std::runtime_error("normalized conditioned stack has no exact inventory extend-data range");
            stream << "{\"formId\":";
            writeJsonString(stream, formIdString(stack.mRecord));
            stream << ",\"count\":" << stack.mCount << ",\"health\":" << std::setprecision(9) << stack.mHealth
                   << ",\"provenance\":";
            writeSaveProvenance(stream, *range);
            stream << "}";
        }
        stream << "]}";
    }

    void writeEquipped(std::ostream& stream, const ESM4::FONVSaveGamePrefix& save,
        const MWWorld::FalloutSavePlayerHeaderState& player)
    {
        stream << "{\"wornVisualItems\":[";
        for (std::size_t index = 0; index < player.mWornVisualItems.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& worn = player.mWornVisualItems[index];
            const auto range = findInventoryExtraRange(save, worn.mSourceOffset, ESM4::sFONVExtraWornType);
            if (!range)
                throw std::runtime_error("normalized worn item has no exact ExtraWorn source range");
            stream << "{\"formId\":";
            writeJsonString(stream, formIdString(worn.mRecord));
            stream << ",\"health\":";
            if (worn.mHealth)
                stream << std::setprecision(9) << *worn.mHealth;
            else
                stream << "null";
            stream << ",\"sourceRange\":";
            writeRange(stream, *range);
            stream << ",\"provenance\":";
            writeSaveProvenance(stream, *range);
            stream << '}';
        }
        stream << "],\"hotkeys\":[";
        for (std::size_t index = 0; index < player.mHotkeyItems.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& hotkey = player.mHotkeyItems[index];
            const auto range = findInventoryExtraRange(save, hotkey.mSourceOffset, ESM4::sFONVExtraHotkeyType);
            if (!range)
                throw std::runtime_error("normalized hotkey has no exact ExtraHotkey source range");
            stream << "{\"index\":" << +hotkey.mIndex << ",\"formId\":";
            writeJsonString(stream, formIdString(hotkey.mRecord));
            stream << ",\"provenance\":";
            writeSaveProvenance(stream, *range);
            stream << '}';
        }
        stream << "],\"ammoSelections\":[";
        for (std::size_t index = 0; index < player.mAmmoSelections.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& selection = player.mAmmoSelections[index];
            const auto range = findInventoryExtraRange(save, selection.mSourceOffset, ESM4::sFONVExtraAmmoType);
            if (!range)
                throw std::runtime_error("normalized ammo selection has no exact ExtraAmmo source range");
            stream << "{\"weapon\":";
            writeJsonString(stream, formIdString(selection.mWeapon));
            stream << ",\"ammo\":";
            writeJsonString(stream, formIdString(selection.mAmmo));
            stream << ",\"savedCount\":" << selection.mSavedCount << ",\"provenance\":";
            writeSaveProvenance(stream, *range);
            stream << '}';
        }
        stream << "],\"weaponState\":{";
        stream << "\"drawn\":" << (player.mWeaponDrawn ? "true" : "false") << ",\"currentAction\":"
               << player.mCurrentWeaponAction << ",\"currentActionSourceOffset\":"
               << player.mCurrentWeaponActionSourceOffset;
        if (save.mPlayerMobileObjectProcessState)
        {
            stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":{\"offset\":"
                   << save.mPlayerMobileObjectProcessState->mMiddleHighProcess.mWeaponOut.mRange.mOffset
                   << ",\"bytes\":" << save.mPlayerMobileObjectProcessState->mMiddleHighProcess.mWeaponOut.mRange.mSize
                   << "}}";
        }
        stream << "}}";
    }

    void writeActorValues(std::ostream& stream, const ESM4::FONVSaveGamePrefix& save,
        const MWWorld::FalloutSavePlayerHeaderState& player)
    {
        stream << "{\"modifiers\":[";
        for (std::size_t index = 0; index < player.mActorValueModifiers.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& modifier = player.mActorValueModifiers[index];
            const auto range = findActorModifierRange(save, modifier);
            if (!range)
                throw std::runtime_error("normalized actor-value modifier has no exact source range");
            stream << "{\"actorValue\":" << +modifier.mActorValue << ",\"modifier\":"
                   << std::setprecision(std::numeric_limits<float>::max_digits10) << modifier.mModifier
                   << ",\"kind\":";
            writeJsonString(stream, kindName(modifier.mKind));
            stream << ",\"sourceOffset\":" << modifier.mSourceOffset << ",\"provenance\":";
            writeSaveProvenance(stream, *range);
            stream << '}';
        }
        stream << "],\"arrays\":{";
        constexpr std::array names{ "unidentified244", "permanent378", "unidentified4B0" };
        const std::array arrays{ &save.mPlayerActorValueData->mActorValues244,
            &save.mPlayerActorValueData->mActorValues378, &save.mPlayerActorValueData->mActorValues4B0 };
        for (std::size_t arrayIndex = 0; arrayIndex < arrays.size(); ++arrayIndex)
        {
            if (arrayIndex != 0)
                stream << ',';
            writeJsonString(stream, names[arrayIndex]);
            stream << ":[";
            for (std::size_t valueIndex = 0; valueIndex < arrays[arrayIndex]->size(); ++valueIndex)
            {
                if (valueIndex != 0)
                    stream << ',';
                stream << "{\"actorValue\":" << valueIndex << ",\"sample\":";
                writeValueWithProvenance(stream, (*arrays[arrayIndex])[valueIndex].mValue,
                    (*arrays[arrayIndex])[valueIndex].mRange);
                stream << '}';
            }
            stream << ']';
        }
        stream << "}}";
    }

    void writeGlobals(std::ostream& stream, const ESM4::FONVSaveGamePrefix& save,
        const MWWorld::FalloutSaveLoadPlan& plan)
    {
        stream << '[';
        for (std::size_t index = 0; index < plan.mGlobals.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& global = plan.mGlobals[index];
            const auto* raw = findRawGlobal(save, global.mSourceOffset);
            if (raw == nullptr)
                throw std::runtime_error("normalized global has no exact raw source entry");
            stream << "{\"formId\":";
            writeJsonString(stream, formIdString(global.mVariable));
            stream << ",\"value\":";
            writeValueWithProvenance(stream, global.mValue, raw->mValue.mRange);
            stream << ",\"entryRange\":";
            writeRange(stream, raw->mRange);
            stream << ",\"formIdRange\":";
            writeRange(stream, raw->mVariable.mEncoded.mRange);
            stream << '}';
        }
        stream << ']';
    }

    void writeQuestProgress(std::ostream& stream, const ESM4::FONVSaveGamePrefix& save,
        const MWWorld::FalloutSaveLoadPlan& plan)
    {
        if (!plan.mQuestProgress || !save.mPlayerCharacterListsState)
        {
            stream << "{\"status\":\"unsupported\",\"reason\":\"native quest carrier absent\"}";
            return;
        }
        const auto& progress = *plan.mQuestProgress;
        const auto& raw = *save.mPlayerCharacterListsState;
        stream << "{\"status\":\"normalized\",\"activeQuest\":";
        if (progress.mActiveQuest)
        {
            stream << "{\"value\":";
            writeJsonString(stream, formIdString(*progress.mActiveQuest));
            stream << ",\"provenance\":";
            writeSaveProvenance(stream, save.mPlayerCharacterScalarReferenceState->mQuest.mEncoded.mRange);
            stream << "}";
        }
        else
            stream << "null";
        stream << ",\"stages\":[";
        for (std::size_t index = 0; index < progress.mStages.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& stage = progress.mStages[index];
            const auto& source = raw.mStages.at(index);
            stream << "{\"quest\":{\"value\":";
            writeJsonString(stream, formIdString(stage.mQuest));
            stream << ",\"provenance\":";
            writeSaveProvenance(stream, source.mQuest.mEncoded.mRange);
            stream << "}";
            stream << ",\"stage\":";
            writeValueWithProvenance(stream, stage.mStage, source.mStage.mRange);
            stream << ",\"logEntry\":";
            writeValueWithProvenance(stream, stage.mLogEntry, source.mLogEntry.mRange);
            stream << ",\"entryRange\":";
            writeRange(stream, source.mRange);
            stream << '}';
        }
        stream << "],\"objectives\":[";
        for (std::size_t index = 0; index < progress.mObjectives.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& objective = progress.mObjectives[index];
            const auto& source = raw.mObjectives.at(index);
            stream << "{\"quest\":{\"value\":";
            writeJsonString(stream, formIdString(objective.mQuest));
            stream << ",\"provenance\":";
            writeSaveProvenance(stream, source.mQuest.mEncoded.mRange);
            stream << "}";
            stream << ",\"objective\":";
            writeValueWithProvenance(stream, objective.mObjective, source.mObjective.mRange);
            stream << ",\"entryRange\":";
            writeRange(stream, source.mRange);
            stream << '}';
        }
        stream << "]}";
    }

    void writeContentForm(std::ostream& stream, ESM::FormId id, std::span<const std::string> contentFiles)
    {
        stream << "{\"formId\":";
        writeJsonString(stream, formIdString(id));
        stream << ",\"contentFile\":";
        writeJsonString(stream, contentFileForForm(id, contentFiles));
        stream << "}";
    }

    void writeInventoryJoin(const std::filesystem::path& output, const std::filesystem::path& savePath,
        const std::vector<std::uint8_t>& saveBytes, const std::string& saveHash, const ContentProfile& profile,
        const ESM4::FONVSaveGamePrefix& save, const MWWorld::FalloutSaveLoadPlan& plan,
        const MWWorld::ESMStore& store)
    {
        std::error_code error;
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path(), error);
        if (error)
            throw std::runtime_error("could not create inventory join output directory: " + error.message());
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("could not create inventory join output: " + output.generic_string());

        stream << "{\n  \"schema\":\"nikami-fnv-save-inventory-join/v1\",\n"
                  "  \"status\":\"resolved-official-inventory-join\",\n  \"source\":{\"path\":";
        writeJsonString(stream, savePath.generic_string());
        stream << ",\"bytes\":" << saveBytes.size() << ",\"sha256\":";
        writeJsonString(stream, saveHash);
        stream << "},\n  \"contentProfile\":";
        writeProfile(stream, profile);
        stream << ",\n  \"rows\":[";

        for (std::size_t index = 0; index < plan.mPlayer.mInventoryItems.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto& item = plan.mPlayer.mInventoryItems[index];
            const InventoryRecordDetails details
                = resolveInventoryRecord(store, item.mRecord, profile.mContentFiles);
            stream << "{\"formId\":";
            writeJsonString(stream, formIdString(item.mRecord));
            stream << ",\"count\":" << item.mCount << ",\"record\":{\"family\":";
            writeJsonString(stream, details.mFamily);
            stream << ",\"editorId\":";
            writeJsonString(stream, details.mEditorId);
            stream << ",\"displayName\":";
            writeJsonString(stream, details.mDisplayName);
            stream << ",\"icon\":";
            writeJsonString(stream, details.mIcon);
            stream << ",\"contentFile\":";
            writeJsonString(stream, details.mContentFile);
            stream << ",\"provenance\":{\"kind\":\"content-record\",\"contentFile\":";
            writeJsonString(stream, details.mContentFile);
            stream << ",\"formId\":";
            writeJsonString(stream, formIdString(item.mRecord));
            stream << "}},\"sourceContributions\":[";

            bool firstContribution = true;
            for (const auto& contribution : plan.mPlayer.mInventoryContributions)
            {
                if (contribution.mRecord != item.mRecord)
                    continue;
                if (!firstContribution)
                    stream << ',';
                firstContribution = false;
                stream << "{\"record\":";
                writeJsonString(stream, formIdString(contribution.mRecord));
                stream << ",\"sourceRecord\":";
                writeJsonString(stream, formIdString(contribution.mSourceRecord));
                stream << ",\"delta\":" << contribution.mDelta << ",\"fromSave\":"
                       << (contribution.mFromSave ? "true" : "false");
                if (contribution.mFromSave)
                {
                    stream << ",\"range\":{\"offset\":" << contribution.mSourceOffset << ",\"bytes\":"
                           << contribution.mSourceBytes << "},\"formIdRange\":{\"offset\":"
                           << contribution.mFormIdOffset << ",\"bytes\":" << contribution.mFormIdBytes << '}';
                }
                else
                {
                    stream << ",\"provenance\":{\"kind\":\"content-record\",\"contentFile\":";
                    writeJsonString(stream, contentFileForForm(contribution.mSourceRecord, profile.mContentFiles));
                    stream << ",\"formId\":";
                    writeJsonString(stream, formIdString(contribution.mSourceRecord));
                    stream << "}";
                }
                stream << "}";
            }

            stream << "],\"equipped\":{\"wornVisual\":"
                   << (std::any_of(plan.mPlayer.mWornVisualItems.begin(), plan.mPlayer.mWornVisualItems.end(),
                           [&](const auto& worn) { return worn.mRecord == item.mRecord; })
                           ? "true"
                           : "false")
                   << ",\"hotkeySlots\":[";
            bool firstHotkey = true;
            for (const auto& hotkey : plan.mPlayer.mHotkeyItems)
            {
                if (hotkey.mRecord != item.mRecord)
                    continue;
                if (!firstHotkey)
                    stream << ',';
                firstHotkey = false;
                stream << +hotkey.mIndex;
            }
            stream << "],\"ammoSelections\":[";
            bool firstAmmoSelection = true;
            for (const auto& selection : plan.mPlayer.mAmmoSelections)
            {
                if (selection.mWeapon != item.mRecord)
                    continue;
                if (!firstAmmoSelection)
                    stream << ',';
                firstAmmoSelection = false;
                stream << "{\"ammo\":";
                writeJsonString(stream, formIdString(selection.mAmmo));
                stream << ",\"savedCount\":" << selection.mSavedCount << ",\"sourceOffset\":"
                       << selection.mSourceOffset << "}";
            }
            stream << "]},\"condition\":{\"stacks\":[";
            bool firstCondition = true;
            for (const auto& stack : plan.mPlayer.mConditionedStacks)
            {
                if (stack.mRecord != item.mRecord)
                    continue;
                const auto range = findInventoryExtendRange(save, stack.mSourceOffset);
                if (!range)
                    throw std::runtime_error("conditioned inventory row has no exact source range for "
                        + formIdString(item.mRecord));
                if (!firstCondition)
                    stream << ',';
                firstCondition = false;
                stream << "{\"count\":" << stack.mCount << ",\"health\":" << std::setprecision(9) << stack.mHealth
                       << ",\"sourceRange\":";
                writeRange(stream, *range);
                stream << ",\"provenance\":";
                writeSaveProvenance(stream, *range);
                stream << "}";
            }
            stream << "]},\"weapon\":{\"ammo\":";
            if (details.mWeaponAmmo)
                writeContentForm(stream, *details.mWeaponAmmo, profile.mContentFiles);
            else
                stream << "null";
            stream << ",\"ammoList\":[";
            for (std::size_t ammoIndex = 0; ammoIndex < details.mWeaponAmmoList.size(); ++ammoIndex)
            {
                if (ammoIndex != 0)
                    stream << ',';
                writeContentForm(stream, details.mWeaponAmmoList[ammoIndex], profile.mContentFiles);
            }
            stream << "],\"clipSize\":";
            if (details.mWeaponClipSize)
                stream << +*details.mWeaponClipSize;
            else
                stream << "null";
            stream << "},\"alch\":{\"effectReferences\":[";
            for (std::size_t effectIndex = 0; effectIndex < details.mAlchEffectReferences.size(); ++effectIndex)
            {
                if (effectIndex != 0)
                    stream << ',';
                writeContentForm(stream, details.mAlchEffectReferences[effectIndex], profile.mContentFiles);
            }
            stream << "]}}";
        }
        stream << "],\n  \"unresolved\":[]\n}\n";
        stream.close();
        if (!stream)
            throw std::runtime_error("failed while writing inventory join output");
    }

    void writeOutput(const std::filesystem::path& output, const std::filesystem::path& savePath,
        const std::vector<std::uint8_t>& saveBytes, const std::string& saveHash, const ContentProfile& profile,
        const ESM4::FONVSaveGamePrefix& save, const MWWorld::FalloutSaveLoadPlan& plan,
        const MWWorld::ESMStore& store)
    {
        std::error_code error;
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path(), error);
        if (error)
            throw std::runtime_error("could not create denominator output directory: " + error.message());
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("could not create denominator output: " + output.generic_string());

        stream << "{\n  \"schema\":\"nikami-fnv-save-player-denominator/v2\",\n"
                  "  \"status\":\"normalized-save-denominator\",\n  \"source\":{\"path\":";
        writeJsonString(stream, savePath.generic_string());
        stream << ",\"bytes\":" << saveBytes.size() << ",\"sha256\":";
        writeJsonString(stream, saveHash);
        stream << "},\n  \"contentProfile\":";
        writeProfile(stream, profile);
        stream << ",\n  \"masters\":[";
        for (std::size_t index = 0; index < save.mMasters.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << "{\"name\":";
            writeJsonString(stream, save.mMasters[index].mFileName.mValue);
            stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
            writeRange(stream, save.mMasters[index].mFileName.mEncodedRange);
            stream << "}}";
        }
        stream << "],\n  \"normalizedLoadPlan\":{\"status\":\"resolved\",\"uncoveredState\":[";
        for (std::size_t index = 0; index < plan.mUncoveredState.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            writeJsonString(stream, plan.mUncoveredState[index]);
        }
        stream << "]},\n  \"player\":{";
        stream << "\"baseRecord\":{\"value\":";
        writeJsonString(stream, formIdString(plan.mPlayer.mBaseRecord));
        stream << ",\"provenance\":{\"kind\":\"content-record\",\"contentFile\":";
        writeJsonString(stream, contentFileForForm(plan.mPlayer.mBaseRecord, profile.mContentFiles));
        stream << "}},\"referenceRecord\":{\"value\":";
        writeJsonString(stream, formIdString(plan.mPlayer.mReferenceRecord));
        stream << ",\"provenance\":{\"kind\":\"content-record\",\"contentFile\":";
        writeJsonString(stream, contentFileForForm(plan.mPlayer.mReferenceRecord, profile.mContentFiles));
        stream << "}},\"saveNumber\":";
        writeValueWithProvenance(stream, plan.mPlayer.mSaveNumber, save.mHeader.mSaveNumber.mRange);
        stream << ",\"name\":{\"value\":";
        writeJsonString(stream, plan.mPlayer.mName);
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mHeader.mPlayerName.mEncodedRange);
        stream << "}},\"karmaTitle\":{\"value\":";
        writeJsonString(stream, plan.mPlayer.mKarmaTitle);
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mHeader.mPlayerKarmaTitle.mEncodedRange);
        stream << "}},\"level\":";
        writeValueWithProvenance(stream, plan.mPlayer.mLevel, save.mHeader.mPlayerLevel.mRange);
        stream << ",\"locationLabel\":{\"value\":";
        writeJsonString(stream, plan.mPlayer.mLocationLabel);
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mHeader.mPlayerLocation.mEncodedRange);
        stream << "}},\"playTimeLabel\":{\"value\":";
        writeJsonString(stream, plan.mPlayer.mPlayTimeLabel);
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mHeader.mPlayTime.mEncodedRange);
        stream << "}},\"processLevel\":";
        writeValueWithProvenance(stream, plan.mPlayer.mProcessLevel, save.mPlayerProcessInventoryData->mProcessLevel.mRange);
        stream << ",\"weaponDrawn\":";
        writeValueWithProvenance(stream, plan.mPlayer.mWeaponDrawn,
            save.mPlayerMobileObjectProcessState->mMiddleHighProcess.mWeaponOut.mRange);
        stream << ",\"currentWeaponAction\":";
        writeValueWithProvenance(stream, plan.mPlayer.mCurrentWeaponAction,
            save.mPlayerMobileObjectProcessState->mHighProcess.mCurrentAction.mRange);
        stream << ",\"referenceChangeFlags\":";
        const auto& playerChange = save.requirePlayerReferenceChangeForm();
        writeValueWithProvenance(stream, plan.mPlayer.mReferenceChangeFlags, playerChange.mChangeFlags.mRange);
        stream << ",\"referencePayloadRange\":";
        writeRange(stream, ESM4::FONVSaveRange{ plan.mPlayer.mReferencePayloadOffset, plan.mPlayer.mReferencePayloadBytes });
        stream << "},\n  \"transform\":{";
        stream << "\"cellOrWorldspace\":{\"value\":";
        writeJsonString(stream, formIdString(plan.mTransform.mCellOrWorldspaceRecord));
        stream << ",\"recordFamily\":";
        if (store.get<ESM4::World>().search(ESM::RefId(plan.mTransform.mCellOrWorldspaceRecord)) != nullptr)
            writeJsonString(stream, "worldspace");
        else if (store.get<ESM4::Cell>().search(ESM::RefId(plan.mTransform.mCellOrWorldspaceRecord)) != nullptr)
            writeJsonString(stream, "cell");
        else
            writeJsonString(stream, "unresolved");
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mPlayerReferenceMovement->mCellOrWorldspace.mEncoded.mRange);
        stream << "}},\"position\":[";
        for (std::size_t index = 0; index < 3; ++index)
        {
            if (index != 0)
                stream << ',';
            writeValueWithProvenance(stream, plan.mTransform.mPosition[index],
                save.mPlayerReferenceMovement->mPosition[index].mRange);
        }
        stream << "],\"rotationRadians\":[";
        for (std::size_t index = 0; index < 3; ++index)
        {
            if (index != 0)
                stream << ',';
            writeValueWithProvenance(stream, plan.mTransform.mRotationRadians[index],
                save.mPlayerReferenceMovement->mRotationRadians[index].mRange);
        }
        stream << "]},\n  \"camera\":{";
        stream << "\"firstPersonMode\":";
        writeValueWithProvenance(stream, plan.mCamera.mThirdPersonMode,
            save.mPlayerCharacterScalarReferenceState->mFirstPersonMode.mRange);
        stream << ",\"firstPerson\":";
        writeValueWithProvenance(stream, plan.mCamera.mFirstPerson,
            save.mPlayerCharacterScalarReferenceState->mFirstPersonMode.mRange);
        stream << ",\"firstPersonModelFov\":";
        writeValueWithProvenance(stream, plan.mCamera.mFirstPersonModelFov,
            save.mPlayerCharacterScalarReferenceState->mFirstPersonModelFov.mRange);
        stream << ",\"worldFov\":";
        writeValueWithProvenance(stream, plan.mCamera.mWorldFov, save.mPlayerCharacterScalarReferenceState->mWorldFov.mRange);
        stream << "},\n  \"scene\":{";
        stream << "\"gameHour\":";
        writeValueWithProvenance(stream, plan.mScene.mGameHour, save.mSky->mGameHour.mRange);
        stream << ",\"lastUpdateHour\":";
        writeValueWithProvenance(stream, plan.mScene.mLastUpdateHour, save.mSky->mLastUpdateHour.mRange);
        stream << ",\"weatherPercent\":";
        writeValueWithProvenance(stream, plan.mScene.mWeatherPercent, save.mSky->mWeatherPercent.mRange);
        stream << ",\"currentWeather\":{\"value\":";
        writeJsonString(stream, formIdString(plan.mScene.mCurrentWeather));
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mSky->mCurrentWeather.mEncoded.mRange);
        stream << "}},\"defaultWeather\":{\"value\":";
        writeJsonString(stream, formIdString(plan.mScene.mDefaultWeather));
        stream << ",\"provenance\":{\"kind\":\"save-bytes\",\"range\":";
        writeRange(stream, save.mSky->mDefaultWeather.mEncoded.mRange);
        stream << "}},\"payloadRange\":{\"offset\":" << plan.mScene.mPayloadOffset << ",\"bytes\":"
               << plan.mScene.mPayloadBytes << "}},\n  \"inventory\":";
        writeInventory(stream, save, plan.mPlayer, profile.mContentFiles);
        stream << ",\n  \"equippedRows\":";
        writeEquipped(stream, save, plan.mPlayer);
        stream << ",\n  \"actorValues\":";
        writeActorValues(stream, save, plan.mPlayer);
        stream << ",\n  \"globals\":";
        writeGlobals(stream, save, plan);
        stream << ",\n  \"questProgress\":";
        writeQuestProgress(stream, save, plan);
        stream << ",\n  \"discoveredMarkerStates\":{\"status\":\"unsupported\",\"reason\":\"normalized FalloutSaveLoadPlan and native Save330 decoder do not expose map-marker discovery semantics; no plugin-default or unlock-all state was synthesized\"},\n  \"unsupportedOpaqueRanges\":[";
        for (std::size_t index = 0; index < save.mUnparsedSemanticPayloadRanges.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << "{\"range\":";
            writeRange(stream, save.mUnparsedSemanticPayloadRanges[index]);
            stream << ",\"provenance\":";
            writeSaveProvenance(stream, save.mUnparsedSemanticPayloadRanges[index]);
            stream << '}';
        }
        stream << "],\n  \"unsupportedOpaqueBytes\":" << save.mUnparsedSemanticPayloadBytes << "\n}\n";
        stream.close();
        if (!stream)
            throw std::runtime_error("failed while writing denominator output");
    }
}

int main(int argc, char** argv)
{
    try
    {
        const Arguments arguments = parseArguments(argc, argv);
        const ContentProfile profile = readContentProfile(arguments.mContentProfile);
        const std::vector<std::uint8_t> saveBytes = readBytes(arguments.mSave);
        const std::string saveHash = sha256Hex(saveBytes);
        const ESM4::FONVSaveGamePrefix save = ESM4::parseFONVSaveGamePrefix(saveBytes, arguments.mSave);
        validateSaveMasterOrder(save, profile.mContentFiles);

        MWWorld::ESMStore store;
        loadOfficialContent(profile, store);
        ESM::ReadersCache readers;
        store.validateRecords(readers);
        const MWWorld::FalloutSaveLoadPlanResolution resolution
            = MWWorld::resolveFalloutSaveLoadPlan(save, store.getFalloutPlayerState(), store.get<ESM4::FormIdList>(),
                store.get<ESM4::Ammunition>(), profile.mContentFiles);
        if (!resolution)
            throw std::runtime_error("Save330 load-plan resolution failed: " + resolution.mError);

        writeOutput(arguments.mOutput, arguments.mSave, saveBytes, saveHash, profile, save, *resolution.mPlan, store);
        if (arguments.mInventoryJoinOutput)
        {
            writeInventoryJoin(*arguments.mInventoryJoinOutput, arguments.mSave, saveBytes, saveHash, profile, save,
                *resolution.mPlan, store);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "fnv-save330-denominator: " << error.what() << '\n';
        return 1;
    }
}

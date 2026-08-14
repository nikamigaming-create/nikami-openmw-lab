#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadperk.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        appendPod(result, value);
        return result;
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string bytes(std::initializer_list<std::uint8_t> values)
    {
        std::string result;
        for (const std::uint8_t value : values)
            result.push_back(static_cast<char>(value));
        return result;
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags = 0)
    {
        ASSERT_EQ(type.size(), 4);
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(payload);
    }

    std::string perkPayload(std::string_view editorId, std::string_view description, std::uint32_t ability)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        appendSubRecord(payload, "FULL", zString("Stored Perk"));
        appendSubRecord(payload, "DESC", zString(description));
        appendSubRecord(payload, "DATA", bytes({ 0, 8, 1, 1, 0 }));
        appendSubRecord(payload, "PRKE", bytes({ 1, 0, 0 }));
        appendSubRecord(payload, "DATA", pod(ability));
        appendSubRecord(payload, "PRKF", {});
        return payload;
    }

    std::string makePlugin(const std::vector<std::pair<std::uint32_t, std::string>>& records,
        std::string_view master = {}, std::uint32_t flags = 0)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, static_cast<std::int32_t>(records.size() + 1));
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        if (!master.empty())
        {
            appendSubRecord(headerPayload, "MAST", zString(master));
            appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));
        }

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        for (const auto& [formId, payload] : records)
            appendRecord(plugin, "PERK", formId, payload, flags);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename,
        std::uint32_t modIndex, const std::map<std::string, int>& masters = {})
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (!masters.empty())
            reader->updateModIndices(masters);
        return reader;
    }

    TEST(Esm4PerkStoreTest, loadsFnvPerksIntoTheTypedStoreWithAdjustedHeaderAndEntryFormIds)
    {
        const std::string plugin = makePlugin(
            { { 0x010003e8, perkPayload("StoredPerk", "Winning record", 0x00005678) } }, "Master.esm",
            ESM4::Rec_Constant | ESM4::Rec_Ignored);
        auto reader = makeReader(plugin, "FalloutNV.esm", 5, { { "master.esm", 2 } });

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);
        store.setUp();

        const auto& perks = store.get<ESM4::Perk>();
        ASSERT_EQ(perks.getSize(), 1);
        const ESM4::Perk* perk = perks.search(ESM::RefId(ESM::FormId::fromUint32(0x050003e8)));
        ASSERT_NE(perk, nullptr);
        EXPECT_EQ(perk->mFlags, ESM4::Rec_Constant | ESM4::Rec_Ignored);
        EXPECT_EQ(perk->mEditorId, "StoredPerk");
        ASSERT_EQ(perk->mEntries.size(), 1);
        const auto& ability = std::get<ESM4::Perk::AbilityEntry>(perk->mEntries[0].mData);
        ASSERT_TRUE(ability.mAbility.has_value());
        EXPECT_EQ(*ability.mAbility, ESM::FormId::fromUint32(0x02005678));
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4PerkStoreTest, reproducesTheFrozenOfficialWinningLiveCensusAndOverridePrecedence)
    {
        constexpr std::size_t frozenPhysicalRecords = 259;
        constexpr std::size_t frozenWinningLiveRecords = 257;
        const auto uniqueRecords = [](std::string_view source, std::size_t count, std::uint32_t objectBase) {
            std::vector<std::pair<std::uint32_t, std::string>> records;
            records.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                const std::string editorId = std::string(source) + std::to_string(index);
                records.emplace_back(0x01000000u | (objectBase + static_cast<std::uint32_t>(index)),
                    perkPayload(editorId, source, 0x00005678));
            }
            return records;
        };

        std::vector<std::pair<std::uint32_t, std::string>> baseRecords;
        baseRecords.reserve(176);
        baseRecords.emplace_back(0x00031dac, perkPayload("HereandNow", "FalloutNV base", 0x00001234));
        for (std::size_t index = 0; index < 175; ++index)
        {
            const std::string editorId = "FalloutNVPerk" + std::to_string(index);
            baseRecords.emplace_back(0x00010000u + static_cast<std::uint32_t>(index),
                perkPayload(editorId, "FalloutNV", 0x00005678));
        }

        auto deadMoney = uniqueRecords("DeadMoney", 16, 0x00020000);
        deadMoney.emplace_back(0x00031dac, perkPayload("HereandNow", "Dead Money override", 0x00001234));
        auto honestHearts = uniqueRecords("HonestHearts", 9, 0x00030000);
        honestHearts.emplace_back(0x00031dac, perkPayload("HereandNow", "Honest Hearts winner", 0x00001234));
        auto oldWorldBlues = uniqueRecords("OldWorldBlues", 31, 0x00040000);
        auto lonesomeRoad = uniqueRecords("LonesomeRoad", 24, 0x00050000);
        auto gunRunners = uniqueRecords("GunRunnersArsenal", 1, 0x00060000);
        const std::size_t physicalRecords = baseRecords.size() + deadMoney.size() + honestHearts.size()
            + oldWorldBlues.size() + lonesomeRoad.size() + gunRunners.size();
        ASSERT_EQ(physicalRecords, frozenPhysicalRecords);

        const auto load = [](MWWorld::ESMStore& store,
                              const std::vector<std::pair<std::uint32_t, std::string>>& records,
                              std::string_view filename, std::uint32_t modIndex, bool hasMaster) {
            const std::string plugin = makePlugin(records, hasMaster ? "FalloutNV.esm" : std::string_view{});
            auto reader = makeReader(plugin, filename, modIndex,
                hasMaster ? std::map<std::string, int>{ { "falloutnv.esm", 0 } } : std::map<std::string, int>{});
            store.loadESM4(*reader, nullptr);
        };

        MWWorld::ESMStore store;
        load(store, baseRecords, "FalloutNV.esm", 0, false);
        load(store, deadMoney, "DeadMoney.esm", 1, true);
        load(store, honestHearts, "HonestHearts.esm", 2, true);
        load(store, oldWorldBlues, "OldWorldBlues.esm", 3, true);
        load(store, lonesomeRoad, "LonesomeRoad.esm", 4, true);
        load(store, gunRunners, "GunRunnersArsenal.esm", 5, true);

        const auto& perks = store.get<ESM4::Perk>();
        ASSERT_EQ(perks.getSize(), frozenWinningLiveRecords);
        const ESM4::Perk* winner = perks.search(ESM::RefId(ESM::FormId::fromUint32(0x00031dac)));
        ASSERT_NE(winner, nullptr);
        EXPECT_EQ(winner->mDescription, "Honest Hearts winner");
    }

    TEST(Esm4PerkStoreTest, doesNotApplyTheFnvSchemaToAnotherGamesPerkRecords)
    {
        const std::string plugin
            = makePlugin({ { 0x000003e8, perkPayload("WrongGamePerk", "Skipped", 0x00005678) } });
        auto reader = makeReader(plugin, "Skyrim.esm", 0);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::Perk>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4PerkStoreTest, rejectsMalformedFnvRecordsBeforeStoreInsertion)
    {
        std::string invalid = perkPayload("InvalidPerk", "Must not insert", 0x00005678);
        appendSubRecord(invalid, "MICO", zString("not-an-fnv-perk-field.dds"));
        const std::string plugin = makePlugin({ { 0x000003e8, std::move(invalid) } });
        auto reader = makeReader(plugin, "FalloutNV.esm", 0);

        MWWorld::ESMStore store;
        EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
        EXPECT_EQ(store.get<ESM4::Perk>().getSize(), 0);
    }
}

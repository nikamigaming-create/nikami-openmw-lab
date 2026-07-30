#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    struct Record
    {
        std::string mType;
        std::uint32_t mFormId = 0;
        std::string mPayload;
    };

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

    std::string commonPayload(std::string_view editorId, std::string_view name)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        appendSubRecord(payload, "OBND", std::string(12, '\0'));
        appendSubRecord(payload, "FULL", zString(name));
        appendSubRecord(payload, "MODL", zString("clutter\\currency.nif"));
        appendSubRecord(payload, "ICON", zString("interface\\icons\\currency.dds"));
        return payload;
    }

    void appendRecord(std::string& output, const Record& record, std::uint32_t flags = 0)
    {
        output.append(record.mType);
        appendPod(output, static_cast<std::uint32_t>(record.mPayload.size()));
        appendPod(output, flags);
        appendPod(output, record.mFormId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(record.mPayload);
    }

    std::string makePlugin(const std::vector<Record>& records)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, static_cast<std::int32_t>(records.size() + 1));
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, { "TES4", 0, std::move(headerPayload) }, ESM4::Rec_ESM);
        for (const Record& record : records)
            appendRecord(plugin, record);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(
        const std::string& plugin, std::string_view filename = "FalloutNV.esm")
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(
            std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(0);
        return reader;
    }

    std::vector<Record> makeSpecialInventoryRecords()
    {
        std::string misc = commonPayload("OrdinaryMisc", "Ordinary Misc");
        ESM4::MiscItem::Data miscData{ 12, 0.5f };
        appendSubRecord(misc, "DATA", pod(miscData));

        std::string chip = commonPayload("SierraMadreChip", "Sierra Madre Chip");
        appendSubRecord(chip, "YNAM", pod(std::uint32_t{ 0x100 }));
        appendSubRecord(chip, "ZNAM", pod(std::uint32_t{ 0x101 }));

        std::string card = commonPayload("CardSpades10", "10 of Spades");
        appendSubRecord(card, "SCRI", pod(std::uint32_t{ 0x102 }));
        appendSubRecord(card, "TX00", zString("terminals\\card_front.dds"));
        appendSubRecord(card, "TX01", zString("terminals\\card_back.dds"));
        appendSubRecord(card, "INTV", pod(std::uint32_t{ 2 }));
        appendSubRecord(card, "INTV", pod(std::uint32_t{ 10 }));
        appendSubRecord(card, "DATA", pod(std::uint32_t{ 2 }));

        std::string money = commonPayload("MoneyNCR100", "$100 NCR");
        appendSubRecord(money, "MICO", zString("interface\\icons\\currency_small.dds"));
        appendSubRecord(money, "DATA", pod(std::uint32_t{ 40 }));

        return {
            { "MISC", 0x100, std::move(misc) },
            { "CHIP", 0x101, std::move(chip) },
            { "CCRD", 0x102, std::move(card) },
            { "CMNY", 0x103, std::move(money) },
        };
    }

    TEST(Esm4SpecialInventoryStoreTest, aliasesNativeFnvCurrencyAndCardsIntoThePersistentMiscStore)
    {
        auto reader = makeReader(makePlugin(makeSpecialInventoryRecords()));
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);
        store.setUp();

        const auto& items = store.get<ESM4::MiscItem>();
        ASSERT_EQ(items.getSize(), 4);

        const ESM4::MiscItem* ordinary = items.search(
            ESM::RefId(ESM::FormId::fromUint32(0x100)));
        const ESM4::MiscItem* chip = items.search(
            ESM::RefId(ESM::FormId::fromUint32(0x101)));
        const ESM4::MiscItem* card = items.search(
            ESM::RefId(ESM::FormId::fromUint32(0x102)));
        const ESM4::MiscItem* money = items.search(
            ESM::RefId(ESM::FormId::fromUint32(0x103)));
        ASSERT_NE(ordinary, nullptr);
        ASSERT_NE(chip, nullptr);
        ASSERT_NE(card, nullptr);
        ASSERT_NE(money, nullptr);

        EXPECT_EQ(ordinary->mOriginalRecordType, ESM4::REC_MISC);
        EXPECT_EQ(ordinary->mData.value, 12u);
        EXPECT_FLOAT_EQ(ordinary->mData.weight, 0.5f);

        EXPECT_EQ(chip->mOriginalRecordType, ESM4::REC_CHIP);
        EXPECT_EQ(chip->mData.value, 0u);
        EXPECT_FLOAT_EQ(chip->mData.weight, 0.f);
        EXPECT_EQ(chip->mPickUpSound, ESM::FormId::fromUint32(0x100));

        EXPECT_EQ(card->mOriginalRecordType, ESM4::REC_CCRD);
        EXPECT_EQ(card->mCardSuit, 2u);
        EXPECT_EQ(card->mCardRank, 10u);
        EXPECT_EQ(card->mData.value, 2u);
        EXPECT_EQ(card->mCardFaceTexture, "terminals\\card_front.dds");
        EXPECT_EQ(card->mCardBackTexture, "terminals\\card_back.dds");

        EXPECT_EQ(money->mOriginalRecordType, ESM4::REC_CMNY);
        EXPECT_EQ(money->mData.value, 40u);
        EXPECT_FLOAT_EQ(money->mData.weight, 0.f);
        EXPECT_EQ(money->mMiniIcon, "interface\\icons\\currency_small.dds");

        for (std::uint32_t id = 0x100; id <= 0x103; ++id)
            EXPECT_EQ(store.find(ESM::RefId(ESM::FormId::fromUint32(id))), ESM::REC_MISC4);
    }

    TEST(Esm4SpecialInventoryStoreTest, doesNotApplyTheFnvAliasesToAnotherGame)
    {
        auto reader = makeReader(makePlugin(makeSpecialInventoryRecords()), "Skyrim.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        // Ordinary MISC is shared ESM4 content; the three FNV-only aliases are skipped.
        ASSERT_EQ(store.get<ESM4::MiscItem>().getSize(), 1);
        const ESM4::MiscItem& ordinary = *store.get<ESM4::MiscItem>().begin();
        EXPECT_EQ(ordinary.mOriginalRecordType, ESM4::REC_MISC);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }
}

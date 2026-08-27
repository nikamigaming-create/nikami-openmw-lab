#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadmisc.hpp>
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

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4u);
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4u);
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(data.size()));
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(data);
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string miscPayload(std::string_view editorId, std::uint32_t value, bool ordinary)
    {
        std::string result;
        appendSubRecord(result, "EDID", zString(editorId));
        appendSubRecord(result, "FULL", zString(editorId));
        if (ordinary)
        {
            ESM4::MiscItem::Data data{ value, 0.f };
            appendSubRecord(result, "DATA", std::string(reinterpret_cast<const char*>(&data), sizeof(data)));
        }
        else
        {
            appendSubRecord(result, "DATA", std::string(reinterpret_cast<const char*>(&value), sizeof(value)));
        }
        return result;
    }

    std::string makePlugin()
    {
        std::string header;
        appendPod(header, std::bit_cast<float>(static_cast<std::uint32_t>(ESM::VER_134)));
        appendPod(header, std::int32_t{ 4 });
        appendPod(header, std::uint32_t{ 0x800 });

        std::string result;
        appendRecord(result, "TES4", 0, [&] {
            std::string data;
            appendSubRecord(data, "HEDR", header);
            return data;
        }());
        appendRecord(result, "MISC", 0x100, miscPayload("Ordinary", 12, true));
        appendRecord(result, "CHIP", 0x101, miscPayload("Chip", 25, false));
        appendRecord(result, "CCRD", 0x102, miscPayload("Card", 2, false));
        appendRecord(result, "CMNY", 0x103, miscPayload("Money", 40, false));
        return result;
    }

    std::unique_ptr<ESM4::Reader> makeReader(std::string plugin, std::string_view filename)
    {
        auto stream = std::make_unique<std::istringstream>(std::move(plugin), std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(0);
        return reader;
    }

    ESM::RefId formId(std::uint32_t value)
    {
        return ESM::RefId(ESM::FormId::fromUint32(value));
    }
}

TEST(Esm4SpecialInventoryStoreTest, aliasesFnvMiscRecordsIntoTheNativeMiscStore)
{
    auto reader = makeReader(makePlugin(), "FalloutNV.esm");
    MWWorld::ESMStore store;
    store.loadESM4(*reader, nullptr);
    store.setUp();

    const auto& items = store.get<ESM4::MiscItem>();
    ASSERT_EQ(items.getSize(), 4u);
    const ESM4::MiscItem* chip = items.search(formId(0x101));
    const ESM4::MiscItem* card = items.search(formId(0x102));
    const ESM4::MiscItem* money = items.search(formId(0x103));
    ASSERT_NE(chip, nullptr);
    ASSERT_NE(card, nullptr);
    ASSERT_NE(money, nullptr);
    EXPECT_EQ(chip->mOriginalRecordType, ESM4::REC_CHIP);
    EXPECT_EQ(card->mOriginalRecordType, ESM4::REC_CCRD);
    EXPECT_EQ(money->mOriginalRecordType, ESM4::REC_CMNY);
    EXPECT_EQ(money->mData.value, 40u);
    EXPECT_EQ(store.find(formId(0x103)), ESM::REC_MISC4);
}

TEST(Esm4SpecialInventoryStoreTest, rejectsFnvMiscAliasesForAnotherGame)
{
    auto reader = makeReader(makePlugin(), "Skyrim.esm");
    MWWorld::ESMStore store;
    store.loadESM4(*reader, nullptr);

    EXPECT_EQ(store.get<ESM4::MiscItem>().getSize(), 1u);
    EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
}

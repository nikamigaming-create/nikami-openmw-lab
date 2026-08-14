#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadavif.hpp>
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
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags)
    {
        ASSERT_EQ(type.size(), 4);
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(payload);
    }

    std::string makePlugin(std::string_view actorValuePayload, std::uint32_t actorValueFlags)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "AVIF", 0x3e8, actorValuePayload, actorValueFlags);
        return plugin;
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::unique_ptr<ESM4::Reader> makeReader(
        const std::string& plugin, std::string_view filename, std::uint32_t modIndex)
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        return reader;
    }

    std::string makeCompleteActorValuePayload()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("AVStrength"));
        appendSubRecord(payload, "FULL", zString("Strength"));
        appendSubRecord(payload, "DESC", zString("Raw physical power."));
        appendSubRecord(payload, "ICON", zString("Interface\\Icons\\Stats\\Strength.dds"));
        appendSubRecord(payload, "MICO", zString("Interface\\Icons\\Stats\\Small\\Strength.dds"));
        appendSubRecord(payload, "ANAM", zString("STR"));
        return payload;
    }

    TEST(Esm4ActorValueStoreTest, shouldLoadExactFnvFieldsFlagsAndFormIdIntoTheTypedStore)
    {
        constexpr std::uint32_t flags = ESM4::Rec_Constant | ESM4::Rec_Ignored;
        const std::string plugin = makePlugin(makeCompleteActorValuePayload(), flags);
        auto reader = makeReader(plugin, "FalloutNV.esm", 2);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        const ESM::FormId adjustedId = ESM::FormId::fromUint32(0x020003e8);
        const ESM::RefId lookupId(adjustedId);
        const auto& actorValues = store.get<ESM4::ActorValueInformation>();
        ASSERT_EQ(actorValues.getSize(), 1);
        const ESM4::ActorValueInformation* actorValue = actorValues.search(lookupId);
        ASSERT_NE(actorValue, nullptr);
        EXPECT_EQ(actorValue->mId, adjustedId);
        EXPECT_EQ(actorValue->mFlags, flags);
        EXPECT_EQ(actorValue->mEditorId, "AVStrength");
        EXPECT_EQ(actorValue->mFullName, "Strength");
        EXPECT_EQ(actorValue->mDescription, "Raw physical power.");
        EXPECT_EQ(actorValue->mLargeIcon, "Interface\\Icons\\Stats\\Strength.dds");
        EXPECT_EQ(actorValue->mSmallIcon, "Interface\\Icons\\Stats\\Small\\Strength.dds");
        EXPECT_EQ(actorValue->mShortName, "STR");
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4ActorValueStoreTest, shouldNotApplyTheFnvSchemaToAnotherGamesAvifRecords)
    {
        const std::string plugin = makePlugin(makeCompleteActorValuePayload(), 0);
        auto reader = makeReader(plugin, "Skyrim.esm", 0);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::ActorValueInformation>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4ActorValueStoreTest, shouldRejectUnprovenFnvActorValueSubrecords)
    {
        std::string payload = makeCompleteActorValuePayload();
        appendSubRecord(payload, "AVSK", std::string_view("\0\0\0\0", 4));
        const std::string plugin = makePlugin(payload, 0);
        auto reader = makeReader(plugin, "FalloutNV.esm", 0);

        MWWorld::ESMStore store;
        EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
    }
}

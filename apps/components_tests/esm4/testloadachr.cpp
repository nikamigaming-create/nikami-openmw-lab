#include <components/esm4/loadachr.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendFourCC(std::string& output, std::string_view value)
    {
        ASSERT_EQ(value.size(), 4);
        output.append(value);
    }

    void appendSubRecord(std::string& output, std::string_view type, std::uint32_t value)
    {
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint16_t>(sizeof(value)));
        appendPod(output, value);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload)
    {
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, std::uint32_t{ 0 }); // flags
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 }); // revision
        appendPod(output, std::uint16_t{ 0 }); // form version
        appendPod(output, std::uint16_t{ 0 }); // unknown
        output.append(payload);
    }

    std::unique_ptr<ESM4::Reader> makeReader(std::string_view recordType, const std::string& payload)
    {
        std::string headerPayload;
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 1 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        appendFourCC(headerPayload, "HEDR");
        appendPod(headerPayload, static_cast<std::uint16_t>(hedr.size()));
        headerPayload.append(hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, recordType, 0x157b3b, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "synthetic.esm", nullptr, nullptr, true);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4ActorPlacementTest, retainsFalloutLinkedPatrolReference)
    {
        // A plugin-local FormID has no load-order byte until Reader resolves its file mapping.
        constexpr std::uint32_t linkedPatrolMarker = 0x000e19b4;
        std::string payload;
        appendSubRecord(payload, "XLKR", linkedPatrolMarker);

        for (std::string_view recordType : { std::string_view("ACHR"), std::string_view("ACRE") })
        {
            SCOPED_TRACE(recordType);
            auto reader = makeReader(recordType, payload);
            ESM4::ActorCharacter actor;
            actor.load(*reader);

            EXPECT_EQ(actor.mLinkedReference, ESM::FormId::fromUint32(linkedPatrolMarker));
        }
    }
}

#include <components/esm4/loadmesg.hpp>
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

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        appendPod(result, value);
        return result;
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
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

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload)
    {
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(payload);
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, "MESG", 0x000abc58, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(0);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4MessageTest, LoadsExactGoodspringsSneakTutorial)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("CGTutorialSneak"));
        appendSubRecord(payload, "DESC", zString("To sneak or crouch, &sUActnCrouch;."));
        appendSubRecord(payload, "FULL", zString("Tutorial - Sneak"));
        appendSubRecord(payload, "INAM", pod(std::uint32_t{ 0 }));
        appendSubRecord(payload, "DNAM", pod(std::uint32_t{ 0 }));
        appendSubRecord(payload, "TNAM", pod(std::uint32_t{ 15 }));

        auto reader = makeReader(payload);
        ESM4::Message message;
        message.load(*reader);

        EXPECT_EQ(message.mId, ESM::FormId::fromUint32(0x000abc58));
        EXPECT_EQ(message.mEditorId, "CGTutorialSneak");
        EXPECT_EQ(message.mDescription, "To sneak or crouch, &sUActnCrouch;.");
        EXPECT_EQ(message.mFullName, "Tutorial - Sneak");
        EXPECT_TRUE(message.mIcon.isZeroOrUnset());
        EXPECT_EQ(message.mMessageFlags, 0u);
        EXPECT_EQ(message.mDisplayTime, 15u);
        EXPECT_TRUE(message.mButtons.empty());
    }
}

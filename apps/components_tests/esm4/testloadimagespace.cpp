#include <components/esm4/imagespacecomposition.hpp>
#include <components/esm4/loadimad.hpp>
#include <components/esm4/loadimgs.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
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

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    template <class T>
    void appendSubRecord(std::string& output, std::string_view type, const T& data)
    {
        appendSubRecord(output, type, std::string_view(reinterpret_cast<const char*>(&data), sizeof(data)));
    }

    void appendSubRecord(std::string& output, std::string_view type, const std::string& data)
    {
        appendSubRecord(output, type, std::string_view(data));
    }

    std::unique_ptr<ESM4::Reader> makeReader(
        std::string_view recordType, std::uint32_t formId, std::string payload, std::uint32_t modIndex = 0)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        const auto appendRecord = [](std::string& output, std::string_view type, std::uint32_t id,
                                      std::string_view body) {
            output.append(type);
            appendPod(output, static_cast<std::uint32_t>(body.size()));
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, id);
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, std::uint16_t{ 0 });
            appendPod(output, std::uint16_t{ 0 });
            output.append(body);
        };

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, recordType, formId, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "imagespace.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4ImageSpaceTest, shouldParseRetailFNVBaseTraits)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", std::string_view("NVDefaultExterior\0", 18));
        std::array<float, 38> data{};
        data[ESM4::ImageSpace::Trait_TargetLuminance] = 1.4f;
        data[ESM4::ImageSpace::Trait_SunlightDimmer] = 1.1f;
        data[ESM4::ImageSpace::Trait_SkinDimmer] = 0.55f;
        data[ESM4::ImageSpace::Trait_CinematicSaturation] = 1.1f;
        data[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance] = 0.2f;
        data[ESM4::ImageSpace::Trait_CinematicContrast] = 1.1f;
        data[ESM4::ImageSpace::Trait_CinematicBrightness] = 1.f;
        data[ESM4::ImageSpace::Trait_CinematicTintRed] = 0.984313726f;
        data[ESM4::ImageSpace::Trait_CinematicTintGreen] = 0.568627477f;
        data[ESM4::ImageSpace::Trait_CinematicTintStrength] = 0.330000013f;
        appendSubRecord(payload, "DNAM", data);

        auto reader = makeReader("IMGS", 0x8809d, payload, 2);
        ESM4::ImageSpace imageSpace;
        imageSpace.load(*reader);

        EXPECT_EQ(imageSpace.mId, ESM::FormId::fromUint32(0x0208809d));
        EXPECT_EQ(imageSpace.mEditorId, "NVDefaultExterior");
        EXPECT_FLOAT_EQ(imageSpace.mTraits[ESM4::ImageSpace::Trait_TargetLuminance], 1.4f);
        EXPECT_FLOAT_EQ(imageSpace.mTraits[ESM4::ImageSpace::Trait_SkinDimmer], 0.55f);
        EXPECT_FLOAT_EQ(imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintStrength], 0.330000013f);
    }

    TEST(Esm4ImageSpaceTest, shouldParseRetailFNVModifierKeys)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", std::string_view("NVWastelandIS\0", 14));
        std::array<std::uint8_t, 244> data{};
        const float duration = 1.f;
        std::memcpy(data.data() + 4, &duration, sizeof(duration));
        appendSubRecord(payload, "DNAM", data);

        const std::array<ESM4::ImageSpaceModifier::FloatKey, 2> skin
            = { { { 0.f, 0.35f }, { 1.f, 0.6f } } };
        const std::array<ESM4::ImageSpaceModifier::FloatKey, 2> brightness
            = { { { 0.f, 1.3f }, { 1.f, 1.1f } } };
        appendSubRecord(payload, std::string_view("\2IAD", 4), skin);
        appendSubRecord(payload, std::string_view("\24IAD", 4), brightness);

        const std::array<ESM4::ImageSpaceModifier::ColorKey, 2> tint = { { { 0.f,
            { 1.f, 0.737254918f, 0.050980393f, 0.392156869f } }, { 1.f, { 1.f, 1.f, 1.f, 0.f } } } };
        appendSubRecord(payload, "TNAM", tint);

        auto reader = makeReader("IMAD", 0x0cee18, payload, 2);
        ESM4::ImageSpaceModifier modifier;
        modifier.load(*reader);

        EXPECT_EQ(modifier.mId, ESM::FormId::fromUint32(0x020cee18));
        EXPECT_EQ(modifier.mEditorId, "NVWastelandIS");
        EXPECT_FLOAT_EQ(modifier.mDuration, 1.f);
        ASSERT_EQ(modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_SkinDimmer].size(), 2);
        EXPECT_FLOAT_EQ(modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_SkinDimmer][0].value, 0.35f);
        ASSERT_EQ(modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicBrightness].size(), 2);
        EXPECT_FLOAT_EQ(
            modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicBrightness][0].value, 1.3f);
        ASSERT_EQ(modifier.mTint.size(), 2);
        EXPECT_FLOAT_EQ(modifier.mTint[0].value[3], 0.392156869f);
    }

    TEST(Esm4ImageSpaceTest, shouldReproduceCapturedRetailFNVFinalConstants)
    {
        ESM4::ImageSpace base;
        base.mTraits[ESM4::ImageSpace::Trait_TargetLuminance] = 1.4f;
        base.mTraits[ESM4::ImageSpace::Trait_SunlightDimmer] = 1.1f;
        base.mTraits[ESM4::ImageSpace::Trait_SkinDimmer] = 0.55f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicSaturation] = 1.1f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance] = 0.2f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicContrast] = 1.1f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness] = 1.f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicTintRed] = 0.984313726f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicTintGreen] = 0.568627477f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicTintBlue] = 0.f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicTintStrength] = 0.330000013f;

        ESM4::ImageSpaceModifier modifier;
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_SkinDimmer].push_back({ 0.f, 0.35f });
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_SunlightDimmer].push_back({ 0.f, 1.1f });
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicSaturation].push_back({ 0.f, 1.f });
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicContrast].push_back({ 0.f, 1.f });
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicContrastAverageLuminance].push_back(
            { 0.f, 1.f });
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicBrightness].push_back({ 0.f, 1.3f });
        modifier.mTint.push_back({ 0.f, { 1.f, 0.737254918f, 0.050980393f, 0.392156869f } });

        // Retail Sky exposed the same static IMAD as complementary fade-in/fade-out instances.
        const auto result = ESM4::composeImageSpace(base,
            { { &modifier, 0.f, 0.401982009f }, { &modifier, 0.f, 0.598017991f } });

        EXPECT_NEAR(result.mTraits[ESM4::ImageSpace::Trait_SkinDimmer], 0.1925f, 1e-6f);
        EXPECT_NEAR(result.mTraits[ESM4::ImageSpace::Trait_SunlightDimmer], 1.21f, 1e-6f);
        EXPECT_NEAR(result.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness], 1.3f, 1e-6f);
        EXPECT_NEAR(result.mTint[0], 0.992831886f, 1e-6f);
        EXPECT_NEAR(result.mTint[1], 0.660198152f, 1e-6f);
        EXPECT_NEAR(result.mTint[2], 0.0276841652f, 1e-6f);
        EXPECT_NEAR(result.mTint[3], 0.392156869f, 1e-6f);
    }
}

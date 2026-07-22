#include <components/esm4/imagespacecomposition.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadimad.hpp>
#include <components/esm4/loadimgs.hpp>
#include <components/esm4/loadwrld.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#ifndef OPENMW_PROJECT_SOURCE_DIR
#define OPENMW_PROJECT_SOURCE_DIR "."
#endif

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

    TEST(Esm4ImageSpaceTest, shouldKeepTes5SplitFieldsOutOfFalloutDnamTraits)
    {
        std::string payload;
        std::array<float, 9> hdr{ 3.f, 7.f, 0.6f, 0.5f, 0.15f, 0.15f, 1.8f, 1.5f, 3.f };
        std::array<float, 3> cinematic{ 0.9f, 1.5f, 1.1f };
        std::array<float, 4> tint{ 0.25f, 0.2f, 0.4f, 0.6f };
        ESM4::ImageSpace::DepthOfField depth;
        depth.strength = 0.75f;
        depth.distance = 1200.f;
        depth.range = 300.f;
        depth.skyBlurRadius = 16384;
        appendSubRecord(payload, "HNAM", hdr);
        appendSubRecord(payload, "CNAM", cinematic);
        appendSubRecord(payload, "TNAM", tint);
        appendSubRecord(payload, "DNAM",
            std::string_view(reinterpret_cast<const char*>(&depth), 16));
        appendSubRecord(payload, "TX00", std::string_view("textures/effects/test_lut.dds\0", 30));

        auto reader = makeReader("IMGS", 0x1234, payload);
        ESM4::ImageSpace imageSpace;
        ASSERT_NO_THROW(imageSpace.load(*reader));

        EXPECT_FLOAT_EQ(imageSpace.mHdr[6], 1.8f);
        EXPECT_FLOAT_EQ(imageSpace.mCinematic[0], 0.9f);
        EXPECT_FLOAT_EQ(imageSpace.mTint[3], 0.6f);
        EXPECT_FLOAT_EQ(imageSpace.mDepthOfField.strength, 0.75f);
        EXPECT_EQ(imageSpace.mDepthOfField.skyBlurRadius, 16384);
        EXPECT_EQ(imageSpace.mLut, "textures/effects/test_lut.dds");
        EXPECT_FLOAT_EQ(imageSpace.mTraits[ESM4::ImageSpace::Trait_SunlightDimmer], 1.8f);
        EXPECT_FLOAT_EQ(imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintStrength], 0.25f);
        EXPECT_FLOAT_EQ(imageSpace.mTraits[ESM4::ImageSpace::Trait_TargetLuminance], 0.f);
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
        const std::array<ESM4::ImageSpaceModifier::FloatKey, 2> vignetteRadius
            = { { { 0.f, 0.25f }, { 1.f, 0.75f } } };
        const std::array<ESM4::ImageSpaceModifier::FloatKey, 1> vignetteStrength = { { { 0.f, 0.4f } } };
        appendSubRecord(payload, "NAM5", vignetteRadius);
        appendSubRecord(payload, "NAM6", vignetteStrength);

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
        ASSERT_EQ(modifier.mVignetteRadius.size(), 2);
        EXPECT_FLOAT_EQ(modifier.mVignetteRadius[1].value, 0.75f);
        ASSERT_EQ(modifier.mVignetteStrength.size(), 1);
        EXPECT_FLOAT_EQ(modifier.mVignetteStrength[0].value, 0.4f);
    }

    TEST(Esm4ImageSpaceTest, shouldReproduceCapturedRetailFNVFinalConstants)
    {
        ESM4::ImageSpace base;
        base.mTraits[ESM4::ImageSpace::Trait_TargetLuminance] = 1.4f;
        base.mTraits[ESM4::ImageSpace::Trait_LuminanceRampNoTexture] = 1.1f;
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
        modifier.mMultiply[ESM4::ImageSpaceModifier::Channel_LuminanceRampNoTexture].push_back({ 0.f, 0.8f });
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
        EXPECT_NEAR(result.mTraits[ESM4::ImageSpace::Trait_LuminanceRampNoTexture], 0.88f, 1e-6f);
        EXPECT_NEAR(result.mTraits[ESM4::ImageSpace::Trait_SunlightDimmer], 1.21f, 1e-6f);
        EXPECT_NEAR(result.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness], 1.3f, 1e-6f);
        EXPECT_NEAR(result.mTint[0], 0.992831886f, 1e-6f);
        EXPECT_NEAR(result.mTint[1], 0.660198152f, 1e-6f);
        EXPECT_NEAR(result.mTint[2], 0.0276841652f, 1e-6f);
        EXPECT_NEAR(result.mTint[3], 0.392156869f, 1e-6f);
    }

    TEST(Esm4ImageSpaceTest, shouldComposeRetailFragExplosionModifierOverAuthoredDuration)
    {
        ESM4::ImageSpace base;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicSaturation] = 1.f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance] = 0.2f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicContrast] = 1.1f;
        base.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness] = 1.f;

        ESM4::ImageSpaceModifier frag;
        frag.mDuration = 1.5f;
        frag.mBlurRadius = { { 0.f, 0.f }, { 0.04f, 1.f }, { 0.74f, 0.f }, { 1.f, 0.f } };
        frag.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicContrast]
            = { { 0.f, 1.f }, { 0.04f, 2.f }, { 0.74f, 1.f }, { 1.f, 1.f } };
        frag.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicContrastAverageLuminance]
            = { { 0.f, 1.f }, { 0.04f, 0.7f }, { 0.74f, 1.f }, { 1.f, 1.f } };
        frag.mMultiply[ESM4::ImageSpaceModifier::Channel_CinematicBrightness]
            = { { 0.f, 1.f }, { 0.04f, 2.f }, { 0.74f, 1.f }, { 1.f, 1.f } };

        const float peakTime = ESM4::normalizeImageSpaceModifierTime(0.06f, frag.mDuration);
        EXPECT_NEAR(peakTime, 0.04f, 1e-6f);
        const ESM4::ComposedImageSpace peak = ESM4::composeImageSpace(base, { { &frag, peakTime, 1.f } });
        EXPECT_FLOAT_EQ(peak.mBlurRadius, 1.f);
        EXPECT_FLOAT_EQ(peak.mTraits[ESM4::ImageSpace::Trait_CinematicContrast], 2.2f);
        EXPECT_FLOAT_EQ(
            peak.mTraits[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance], 0.14f);
        EXPECT_FLOAT_EQ(peak.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness], 2.f);

        const float endTime = ESM4::normalizeImageSpaceModifierTime(frag.mDuration, frag.mDuration);
        const ESM4::ComposedImageSpace end = ESM4::composeImageSpace(base, { { &frag, endTime, 1.f } });
        EXPECT_FLOAT_EQ(end.mBlurRadius, 0.f);
        EXPECT_FLOAT_EQ(end.mTraits[ESM4::ImageSpace::Trait_CinematicContrast], 1.1f);
        EXPECT_FLOAT_EQ(end.mTraits[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance], 0.2f);
        EXPECT_FLOAT_EQ(end.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness], 1.f);
    }

    TEST(Esm4ImageSpaceTest, shouldNotRenormalizeOpenMwLdrSceneByRetailTargetLuminance)
    {
        const std::filesystem::path shaderPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "data"
            / "shaders" / "internal_fallout_imagespace.omwfx";
        std::ifstream stream(shaderPath);
        ASSERT_TRUE(stream) << shaderPath;
        const std::string shader{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };

        // The post-process intercept is RGBA8. The retail target-luminance
        // denominator belongs to its HDR source and must not scale this LDR
        // scene contribution; it remains available for authored bloom.
        EXPECT_NE(shader.find("vec3 color = source.rgb;"), std::string::npos);
        EXPECT_EQ(shader.find("color = max(color /"), std::string::npos);
        EXPECT_EQ(shader.find("color /= "), std::string::npos);
        EXPECT_NE(shader.find("* (uFalloutHdr.x / hdrDenominator)"), std::string::npos);
    }

    TEST(Esm4ImageSpaceTest, shouldUseRetailFNVImageSpaceBlurSurfaceAndKernel)
    {
        const std::filesystem::path shaderPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "data"
            / "shaders" / "internal_fallout_imagespace.omwfx";
        std::ifstream stream(shaderPath);
        ASSERT_TRUE(stream) << shaderPath;
        const std::string shader{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };

        EXPECT_NE(shader.find("uniform_float uFalloutBlurRadius"), std::string::npos);
        EXPECT_NE(shader.find("width = 256;"), std::string::npos);
        EXPECT_NE(shader.find("height = 256;"), std::string::npos);
        EXPECT_NE(shader.find("uFalloutBlurRadius / (9.0 * 256.0)"), std::string::npos);
        EXPECT_NE(shader.find("* 0.025"), std::string::npos);
        EXPECT_NE(shader.find("* 0.05"), std::string::npos);
        EXPECT_NE(shader.find("* 0.075"), std::string::npos);
        EXPECT_NE(shader.find("* 0.15"), std::string::npos);
        EXPECT_NE(shader.find("* 0.3"), std::string::npos);
        EXPECT_NE(shader.find("if (uFalloutBlurRadius > 0.0)"), std::string::npos);
    }

    TEST(Esm4ImageSpaceTest, shouldPreferAuthoredInteriorCellImageSpace)
    {
        ESM4::World world;
        world.mImageSpace = ESM::FormId::fromUint32(0x0008809d);
        ESM4::Cell interior;
        interior.mCellFlags = ESM4::CELL_Interior;
        interior.mImageSpace = ESM::FormId::fromUint32(0x0001507a);

        EXPECT_EQ(ESM4::resolveCellImageSpace(interior, &world), interior.mImageSpace);
    }

    TEST(Esm4ImageSpaceTest, shouldInheritWorldImageSpaceOnlyForExteriorCells)
    {
        ESM4::World world;
        world.mImageSpace = ESM::FormId::fromUint32(0x0008809d);
        ESM4::Cell exterior;

        EXPECT_EQ(ESM4::resolveCellImageSpace(exterior, &world), world.mImageSpace);
    }

    TEST(Esm4ImageSpaceTest, shouldNotCarryExteriorImageSpaceIntoInteriorWithoutXcim)
    {
        ESM4::World world;
        world.mImageSpace = ESM::FormId::fromUint32(0x0008809d);
        ESM4::Cell interior;
        interior.mCellFlags = ESM4::CELL_Interior;

        EXPECT_TRUE(ESM4::resolveCellImageSpace(interior, &world).isZeroOrUnset());
    }

    TEST(Esm4ImageSpaceTest, shouldClearStaleFalloutImageSpaceWhenNoBaseIsActive)
    {
        const std::filesystem::path postProcessorPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "apps"
            / "openmw" / "mwrender" / "postprocessor.cpp";
        std::ifstream postProcessorStream(postProcessorPath);
        ASSERT_TRUE(postProcessorStream) << postProcessorPath;
        const std::string postProcessorSource{ std::istreambuf_iterator<char>{ postProcessorStream },
            std::istreambuf_iterator<char>{} };

        const std::size_t clearStart
            = postProcessorSource.find("void PostProcessor::clearFalloutImageSpace()");
        ASSERT_NE(clearStart, std::string::npos);
        const std::size_t clearEnd = postProcessorSource.find("void PostProcessor::traverse", clearStart);
        ASSERT_NE(clearEnd, std::string::npos);
        const std::string_view clearBody(postProcessorSource.data() + clearStart, clearEnd - clearStart);
        EXPECT_NE(clearBody.find("if (!mFalloutImageSpaceTechnique)"), std::string_view::npos);
        EXPECT_NE(clearBody.find("setIdentityUniform(\"uFalloutHdr\", osg::Vec4f(1.f, 0.f, 1.f, 0.f))"),
            std::string_view::npos);
        EXPECT_NE(clearBody.find("setIdentityUniform(\"uFalloutCinematic\", osg::Vec4f(1.f, 0.f, 1.f, 1.f))"),
            std::string_view::npos);
        EXPECT_NE(clearBody.find("setIdentityUniform(\"uFalloutTint\", osg::Vec4f(1.f, 1.f, 1.f, 0.f))"),
            std::string_view::npos);
        EXPECT_NE(clearBody.find("setIdentityUniform(\"uFalloutFade\", osg::Vec4f(0.f, 0.f, 0.f, 0.f))"),
            std::string_view::npos);
        EXPECT_NE(clearBody.find("setIdentityUniform(\"uFalloutBlurRadius\", 0.f)"), std::string_view::npos);

        const std::filesystem::path weatherPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "apps"
            / "openmw" / "mwworld" / "weather.cpp";
        std::ifstream weatherStream(weatherPath);
        ASSERT_TRUE(weatherStream) << weatherPath;
        const std::string weatherSource{ std::istreambuf_iterator<char>{ weatherStream },
            std::istreambuf_iterator<char>{} };
        constexpr std::string_view clearCall = "mRendering.getPostProcessor()->clearFalloutImageSpace();";

        const std::size_t applyStart = weatherSource.find("const auto applyFalloutImageSpace");
        ASSERT_NE(applyStart, std::string::npos);
        const std::size_t interiorStart = weatherSource.find("if (!isExterior)", applyStart);
        ASSERT_NE(interiorStart, std::string::npos);
        const std::size_t interiorReturn = weatherSource.find("return;", interiorStart);
        ASSERT_NE(interiorReturn, std::string::npos);
        const std::size_t interiorApply = weatherSource.find("applyFalloutImageSpace();", interiorStart);
        ASSERT_NE(interiorApply, std::string::npos);
        EXPECT_LT(interiorApply, interiorReturn);
        const std::string_view interiorBody(
            weatherSource.data() + interiorStart, interiorReturn - interiorStart);
        EXPECT_EQ(interiorBody.find(clearCall), std::string_view::npos);

        const std::size_t missingBaseStart = weatherSource.find("if (base == nullptr)", applyStart);
        ASSERT_NE(missingBaseStart, std::string::npos);
        const std::size_t missingBaseReturn = weatherSource.find("return;", missingBaseStart);
        ASSERT_NE(missingBaseReturn, std::string::npos);
        const std::size_t missingBaseClear = weatherSource.find(clearCall, missingBaseStart);
        ASSERT_NE(missingBaseClear, std::string::npos);
        EXPECT_LT(missingBaseClear, missingBaseReturn);
    }
}

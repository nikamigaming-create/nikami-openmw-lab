#include <components/esm4/imagespacecomposition.hpp>
#include <components/esm4/lightingcomposition.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadwrld.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
    ESM4::Lighting makeLighting(std::uint32_t colorBase, float distanceBase)
    {
        ESM4::Lighting result;
        result.ambient = colorBase + 1;
        result.directional = colorBase + 2;
        result.fogColor = colorBase + 3;
        result.fogNear = distanceBase + 1.f;
        result.fogFar = distanceBase + 2.f;
        result.rotationXY = static_cast<std::int32_t>(colorBase + 4);
        result.rotationZ = static_cast<std::int32_t>(colorBase + 5);
        result.fogDirFade = distanceBase + 3.f;
        result.fogClipDist = distanceBase + 4.f;
        result.fogPower = distanceBase + 5.f;
        return result;
    }

    TEST(Esm4LightingCompositionTest, shouldApplyDocMitchellLightingTemplateInheritanceMask)
    {
        // Captured directly from FalloutNV.esm CELL 0x103df9 XCLL and LGTM 0x8f58b DATA.
        const ESM4::Lighting cell = {
            .ambient = 0x0045462f,
            .directional = 0x00000000,
            .fogColor = 0x00203e4d,
            .fogNear = 100.f,
            .fogFar = 1500.f,
            .rotationXY = 35,
            .rotationZ = 270,
            .fogDirFade = 1.f,
            .fogClipDist = 1500.f,
            .fogPower = 0.6f,
        };
        const ESM4::Lighting lightingTemplate = {
            .ambient = 0x002d1f1f,
            .directional = 0x00151e20,
            .fogColor = 0x00304449,
            .fogNear = 64.f,
            .fogFar = 5000.f,
            .rotationXY = 0,
            .rotationZ = 267,
            .fogDirFade = 1.f,
            .fogClipDist = 5000.f,
            .fogPower = 0.84f,
        };

        // GSDocMitchellHouse inherits these LGTM fields through LNAM 0x9f.
        const ESM4::Lighting result = ESM4::composeLighting(cell, lightingTemplate, 0x9f);

        EXPECT_EQ(result.ambient, 0x002d1f1f);
        EXPECT_EQ(result.directional, 0x00151e20);
        EXPECT_EQ(result.fogColor, 0x00304449);
        EXPECT_FLOAT_EQ(result.fogNear, 64.f);
        EXPECT_FLOAT_EQ(result.fogFar, 5000.f);
        EXPECT_FLOAT_EQ(result.fogClipDist, 5000.f);
        EXPECT_EQ(result.rotationXY, 35);
        EXPECT_EQ(result.rotationZ, 270);
        EXPECT_FLOAT_EQ(result.fogDirFade, 1.f);
        EXPECT_FLOAT_EQ(result.fogPower, 0.6f);
    }

    TEST(Esm4LightingCompositionTest, shouldKeepNonInheritedCellFields)
    {
        const ESM4::Lighting cell = makeLighting(0x100, 10.f);
        const ESM4::Lighting lightingTemplate = makeLighting(0x200, 20.f);
        const std::uint32_t flags = ESM4::LightingTemplateInherit_DirectionalRotation
            | ESM4::LightingTemplateInherit_DirectionalFade | ESM4::LightingTemplateInherit_FogPower;

        const ESM4::Lighting result = ESM4::composeLighting(cell, lightingTemplate, flags);

        EXPECT_EQ(result.ambient, cell.ambient);
        EXPECT_EQ(result.directional, cell.directional);
        EXPECT_EQ(result.fogColor, cell.fogColor);
        EXPECT_FLOAT_EQ(result.fogNear, cell.fogNear);
        EXPECT_FLOAT_EQ(result.fogFar, cell.fogFar);
        EXPECT_FLOAT_EQ(result.fogClipDist, cell.fogClipDist);
        EXPECT_EQ(result.rotationXY, lightingTemplate.rotationXY);
        EXPECT_EQ(result.rotationZ, lightingTemplate.rotationZ);
        EXPECT_FLOAT_EQ(result.fogDirFade, lightingTemplate.fogDirFade);
        EXPECT_FLOAT_EQ(result.fogPower, lightingTemplate.fogPower);
    }

    TEST(Esm4LightingCompositionTest, shouldReplaceExteriorWorldGradeWithInteriorCellGrade)
    {
        const ESM::FormId exteriorImageSpace = ESM::FormId::fromUint32(0x0008809d);
        const ESM::FormId interiorImageSpace = ESM::FormId::fromUint32(0x0001507a);
        ESM4::World world{};
        world.mImageSpace = exteriorImageSpace;

        ESM4::Cell exterior{};
        EXPECT_EQ(ESM4::resolveCellImageSpace(exterior, &world), exteriorImageSpace);

        ESM4::Cell interior{};
        interior.mCellFlags = ESM4::CELL_Interior;
        interior.mImageSpace = interiorImageSpace;
        EXPECT_EQ(ESM4::resolveCellImageSpace(interior, &world), interiorImageSpace);

        // An interior without XCIM must not retain or inherit the previous exterior WRLD grade.
        interior.mImageSpace = {};
        EXPECT_TRUE(ESM4::resolveCellImageSpace(interior, &world).isZeroOrUnset());
    }
}

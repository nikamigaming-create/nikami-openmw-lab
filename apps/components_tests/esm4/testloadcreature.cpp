#include <components/esm4/loadcrea.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(Esm4CreatureTemplateTest, resolvesPartialVisualFieldsIndependently)
    {
        ESM4::Creature concrete;
        concrete.mModel = "actors/robot/skeleton.nif";
        concrete.mBaseTemplate = ESM::FormId::fromUint32(0x100);

        ESM4::Creature middle;
        middle.mNif = { "actors/robot/screen.nif" };
        middle.mBaseTemplate = ESM::FormId::fromUint32(0x200);

        ESM4::Creature root;
        root.mBodyParts = { ESM::FormId::fromUint32(0x300) };
        root.mKf = { "actors/robot/idle.kf" };

        const ESM4::CreatureVisualTemplate result
            = ESM4::resolveCreatureVisualTemplate({ &concrete, &middle, &root });
        EXPECT_EQ(result.mModel, &concrete);
        EXPECT_EQ(result.mNif, &middle);
        EXPECT_EQ(result.mKf, &root);
        EXPECT_EQ(result.mBodyParts, &root);
    }

    TEST(Esm4CreatureTemplateTest, honorsUseModelFlagBeforeResolvingEachVisualField)
    {
        ESM4::Creature concrete;
        concrete.mModel = "wrong/local/skeleton.nif";
        concrete.mNif = { "wrong/local/screen.nif" };
        concrete.mBodyParts = { ESM::FormId::fromUint32(0x111) };
        concrete.mBaseTemplate = ESM::FormId::fromUint32(0x100);
        concrete.mBaseConfig.fo3.templateFlags = ESM4::Creature::Template_UseModel;

        ESM4::Creature templated;
        templated.mModel = "actors/robot/skeleton.nif";
        templated.mNif = { "actors/robot/screen.nif" };
        templated.mBodyParts = { ESM::FormId::fromUint32(0x222) };

        const ESM4::CreatureVisualTemplate result
            = ESM4::resolveCreatureVisualTemplate({ &concrete, &templated });
        EXPECT_EQ(result.mModel, &templated);
        EXPECT_EQ(result.mNif, &templated);
        EXPECT_EQ(result.mBodyParts, &templated);
    }
}

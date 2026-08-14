#include <gtest/gtest.h>

#include "apps/openmw/mwclass/esm4creature.hpp"
#include "apps/openmw/mwrender/fallouthitreaction.hpp"
#include "apps/openmw/mwrender/fonvpackageanimation.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace
{
    void expectProcedureCandidates(std::int32_t packageType, std::string_view furnitureModel,
        std::initializer_list<std::string_view> expected)
    {
        EXPECT_EQ(MWRender::getFonvPackageProcedureAnimationCandidates(packageType, furnitureModel),
            std::vector<std::string_view>(expected));
    }

    TEST(FonvPackageAnimationCoverageTest, PinsGoodspringsHumanoidChoreMappings)
    {
        // FalloutNV.esm 0016A9B0 EasyPeteEat12x2: type 3, diner table seat.
        expectProcedureCandidates(3, "Meshes\\Furniture\\DinerBoothTable.NIF",
            {
                "meshes/characters/_male/idleanims/chair_forwardenter.kf",
                "meshes/characters/_male/idleanims/chair_forwardexit.kf",
                "meshes/characters/_male/idleanims/chair_backenter.kf",
                "meshes/characters/_male/idleanims/chair_backexit.kf",
                "meshes/characters/_male/idleanims/chair_leftenter.kf",
                "meshes/characters/_male/idleanims/chair_leftexit.kf",
                "meshes/characters/_male/idleanims/chair_rightenter.kf",
                "meshes/characters/_male/idleanims/chair_rightexit.kf",
                "meshes/characters/_male/idleanims/sittablechaireata.kf",
                "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf",
            });

        // FalloutNV.esm 00176ACF EasyPeteSleepPackage and 00168FAD GSChetSleep22x8: type 4.
        expectProcedureCandidates(
            4, {}, { "meshes/characters/_male/idleanims/dynamicidle_sleep.kf" });

        // FalloutNV.esm 0010655E EasyPeteChairPackage8x4: type 6, chair reference.
        expectProcedureCandidates(6, "meshes/furniture/chair01.nif",
            {
                "meshes/characters/_male/idleanims/chair_forwardenter.kf",
                "meshes/characters/_male/idleanims/chair_forwardexit.kf",
                "meshes/characters/_male/idleanims/chair_backenter.kf",
                "meshes/characters/_male/idleanims/chair_backexit.kf",
                "meshes/characters/_male/idleanims/chair_leftenter.kf",
                "meshes/characters/_male/idleanims/chair_leftexit.kf",
                "meshes/characters/_male/idleanims/chair_rightenter.kf",
                "meshes/characters/_male/idleanims/chair_rightexit.kf",
                "meshes/characters/_male/idleanims/sitchairlistena.kf",
                "meshes/characters/_male/idleanims/sitchairtalktoplayera.kf",
                "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf",
            });
        EXPECT_EQ(MWRender::getFonvPackageProcedureAnimationCandidates(
                      8, "meshes/furniture/chair01.nif"),
            MWRender::getFonvPackageProcedureAnimationCandidates(
                6, "meshes/furniture/chair01.nif"));

        // Travel without resolved furniture and sandbox use locomotion/IDLA paths, not chair procedure KFs.
        expectProcedureCandidates(6, {}, {});
        expectProcedureCandidates(12, "meshes/furniture/chair01.nif", {});
    }

    TEST(FonvPackageAnimationCoverageTest, PinsGoodspringsCreaturePackageAndRigCoverage)
    {
        struct CoverageRow
        {
            std::string_view mCategory;
            std::string_view mActor;
            std::uint32_t mActorFormId;
            std::uint32_t mPackageFormId;
            std::int32_t mPackageType;
            std::string_view mAnimationDirectory;
        };

        // These are exact FalloutNV.esm records. Creature packages supply movement while each rig supplies
        // authored/discovered locomotion KFs; the audited directories also have deterministic hit semantics.
        static constexpr std::array<CoverageRow, 6> coverage = { {
            { "animal", "GSCheyenne", 0x0010588d, 0x00152afa, 7, "meshes/creatures/dog" },
            { "animal", "GSBigHorner", 0x0010ab79, 0x00026ad5, 12, "meshes/creatures/nvbighorner" },
            { "robot", "Victor", 0x00103dfd, 0x0016addc, 13, "meshes/creatures/nvsecuritron" },
            { "hostile", "VFreeformGoodspringsGecko", 0x0010a1f8, 0x00025482, 13,
                "meshes/creatures/nvgecko" },
            { "hostile", "GSGiantMantisNymph", 0x0011d584, 0x000410af, 12,
                "meshes/creatures/nvmantis" },
            { "hostile", "GSCrBarkScorpion", 0x0016aefa, 0x00025482, 13,
                "meshes/creatures/radscorpion" },
        } };

        for (const CoverageRow& row : coverage)
        {
            SCOPED_TRACE(testing::Message() << row.mCategory << ':' << row.mActor << " actor=0x" << std::hex
                                            << row.mActorFormId << " package=0x" << row.mPackageFormId);
            EXPECT_TRUE(MWClass::fnvCreatureAiPackageProcedureSupported(row.mPackageType));
            EXPECT_TRUE(MWRender::isAuditedFonvCreatureHitReactionDirectory(row.mAnimationDirectory));
            EXPECT_FALSE(MWRender::getFonvCreatureHitReactionCandidates(row.mAnimationDirectory).front().empty());
        }

        // FalloutNV.esm 00174BAB VSpecialBonnieHornerCalfFollow uses unsupported PTDT linked-ref type 3.
        EXPECT_FALSE(MWClass::fnvCreatureFollowTargetSupported(1, 3, 0));
    }
}

#include <apps/openmw/mwsound/falloutsoundpath.hpp>

#include <components/testing/util.hpp>
#include <components/vfs/manager.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(FalloutSoundPathTest, DistinguishesAnimationAssetPathsFromEditorIds)
    {
        EXPECT_TRUE(MWSound::isFalloutSoundAssetPath("sound/fx/npc/human/rake/long/"));
        EXPECT_TRUE(MWSound::isFalloutSoundAssetPath("sound\\fx\\npc\\human\\sit\\chair\\down\\"));
        EXPECT_FALSE(MWSound::isFalloutSoundAssetPath("SomeSoundEDID"));
    }

    TEST(FalloutSoundPathTest, ResolvesConcreteAuthoredFileWithoutChangingIt)
    {
        TestingOpenMW::VFSTestFile wav("wav");
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("sound/fx/wpn/pistol/fire.wav"), &wav },
        });

        const auto resolved = MWSound::resolveFalloutSoundPath("FX\\WPN\\Pistol\\Fire.wav", *vfs);
        ASSERT_TRUE(resolved);
        EXPECT_EQ(resolved->value(), "sound/fx/wpn/pistol/fire.wav");
    }

    TEST(FalloutSoundPathTest, UsesMp3OnlyAsConcreteFileExtensionFallback)
    {
        TestingOpenMW::VFSTestFile mp3("mp3");
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("sound/fx/wpn/pistol/equip.mp3"), &mp3 },
        });

        const auto resolved = MWSound::resolveFalloutSoundPath("fx/wpn/pistol/equip.wav", *vfs);
        ASSERT_TRUE(resolved);
        EXPECT_EQ(resolved->value(), "sound/fx/wpn/pistol/equip.mp3");
    }

    TEST(FalloutSoundPathTest, ResolvesDirectoryPrefixToFirstSupportedVariantDeterministically)
    {
        TestingOpenMW::VFSTestFile first("first");
        TestingOpenMW::VFSTestFile second("second");
        TestingOpenMW::VFSTestFile metadata("metadata");
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("sound/fx/wpn/rifle/fire_3d/z_fire.wav"), &second },
            { VFS::Path::NormalizedView("sound/fx/wpn/rifle/fire_3d/readme.txt"), &metadata },
            { VFS::Path::NormalizedView("sound/fx/wpn/rifle/fire_3d/a_fire.wav"), &first },
        });

        const auto resolved = MWSound::resolveFalloutSoundPath("fx\\wpn\\rifle\\fire_3d\\", *vfs);
        ASSERT_TRUE(resolved);
        EXPECT_EQ(resolved->value(), "sound/fx/wpn/rifle/fire_3d/a_fire.wav");
    }

    TEST(FalloutSoundPathTest, ResolvesRetainedGoodspringsAnimationDirectoryKeys)
    {
        TestingOpenMW::VFSTestFile rake("rake");
        TestingOpenMW::VFSTestFile chair("chair");
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("sound/fx/npc/human/rake/long/npc_human_rake_long_01.wav"), &rake },
            { VFS::Path::NormalizedView("sound/fx/npc/human/sit/chair/down/npc_human_sit_chair_down_01.wav"),
                &chair },
        });

        const auto resolvedRake = MWSound::resolveFalloutSoundPath("sound/fx/npc/human/rake/long/", *vfs);
        ASSERT_TRUE(resolvedRake);
        EXPECT_EQ(resolvedRake->value(), "sound/fx/npc/human/rake/long/npc_human_rake_long_01.wav");

        const auto resolvedChair
            = MWSound::resolveFalloutSoundPath("sound\\fx\\npc\\human\\sit\\chair\\down\\", *vfs);
        ASSERT_TRUE(resolvedChair);
        EXPECT_EQ(resolvedChair->value(), "sound/fx/npc/human/sit/chair/down/npc_human_sit_chair_down_01.wav");
    }

    TEST(FalloutSoundPathTest, NeverTurnsDirectoryPrefixIntoSiblingMp3)
    {
        TestingOpenMW::VFSTestFile sibling("sibling");
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("sound/fx/wpn/rifle/fire_3d.mp3"), &sibling },
        });

        EXPECT_FALSE(MWSound::resolveFalloutSoundPath("fx/wpn/rifle/fire_3d/", *vfs));
    }

    TEST(FalloutSoundPathTest, MissingAuthoredPathReturnsNothing)
    {
        const auto vfs = TestingOpenMW::createTestVFS(VFS::FileMap{});
        EXPECT_FALSE(MWSound::resolveFalloutSoundPath("fx/wpn/missing.wav", *vfs));
        EXPECT_FALSE(MWSound::resolveFalloutSoundPath({}, *vfs));
    }
}

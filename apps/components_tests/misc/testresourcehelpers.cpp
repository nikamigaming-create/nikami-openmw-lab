#include <components/misc/resourcehelpers.hpp>
#include <components/testing/util.hpp>

#include <components/esm/common.hpp>

#include <gtest/gtest.h>

namespace
{
    using namespace Misc::ResourceHelpers;
    TEST(CorrectSoundPath, wav_files_not_overridden_with_mp3_in_vfs_are_not_corrected)
    {
        constexpr VFS::Path::NormalizedView path("sound/bar.wav");
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({ { path, nullptr } });
        EXPECT_EQ(correctSoundPath(path, *mVFS), "sound/bar.wav");
    }

    TEST(CorrectSoundPath, wav_files_overridden_with_mp3_in_vfs_are_corrected)
    {
        constexpr VFS::Path::NormalizedView mp3("sound/foo.mp3");
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({ { mp3, nullptr } });
        constexpr VFS::Path::NormalizedView wav("sound/foo.wav");
        EXPECT_EQ(correctSoundPath(wav, *mVFS), "sound/foo.mp3");
    }

    TEST(CorrectSoundPath, corrected_path_does_not_check_existence_in_vfs)
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({});

        {
            constexpr VFS::Path::NormalizedView path("sound/foo.wav");
            EXPECT_EQ(correctSoundPath(path, *mVFS), "sound/foo.mp3");
        }

        auto correctESM4SoundPath = [](auto path, auto* vfs) {
            return Misc::ResourceHelpers::correctResourcePath({ { "sound" } }, path, vfs, ".mp3");
        };

        EXPECT_EQ(correctESM4SoundPath("foo.WAV", mVFS.get()), "sound\\foo.mp3");
        EXPECT_EQ(correctESM4SoundPath("SOUND/foo.WAV", mVFS.get()), "sound\\foo.mp3");
        EXPECT_EQ(correctESM4SoundPath("DATA\\SOUND\\foo.WAV", mVFS.get()), "sound\\foo.mp3");
        EXPECT_EQ(correctESM4SoundPath("\\Data/Sound\\foo.WAV", mVFS.get()), "sound\\foo.mp3");
    }

    namespace
    {
        std::string checkChangeExtensionToDds(std::string path)
        {
            changeExtensionToDds(path);
            return path;
        }
    }

    TEST(ChangeExtensionToDds, original_extension_with_same_size_as_dds)
    {
        EXPECT_EQ(checkChangeExtensionToDds("texture/bar.tga"), "texture/bar.dds");
    }

    TEST(ChangeExtensionToDds, original_extension_greater_than_dds)
    {
        EXPECT_EQ(checkChangeExtensionToDds("texture/bar.jpeg"), "texture/bar.dds");
    }

    TEST(ChangeExtensionToDds, original_extension_smaller_than_dds)
    {
        EXPECT_EQ(checkChangeExtensionToDds("texture/bar.xx"), "texture/bar.dds");
    }

    TEST(ChangeExtensionToDds, does_not_change_dds_extension)
    {
        std::string path = "texture/bar.dds";
        EXPECT_FALSE(changeExtensionToDds(path));
    }

    TEST(ChangeExtensionToDds, does_not_change_when_no_extension)
    {
        std::string path = "texture/bar";
        EXPECT_FALSE(changeExtensionToDds(path));
    }

        TEST(MiscResourceHelpersCorrectResourcePath, shouldPrefixWithGivenTopDirectory)
        {
            constexpr VFS::Path::NormalizedView path("foo.mp3");
            const std::unique_ptr<const VFS::Manager> vfs = TestingOpenMW::createTestVFS({});
            EXPECT_EQ(correctResourcePath({ { sound } }, path, *vfs, mp3), "sound/foo.mp3");
        }

        TEST(MiscResourceHelpersCorrectResourcePath, shouldChangeTopDirectoryAndKeepExtensionIfOriginalExistInVfs)
        {
            constexpr VFS::Path::NormalizedView path("bookart/foo.a");
            constexpr VFS::Path::NormalizedView aPath("textures/foo.a");
            const std::unique_ptr<const VFS::Manager> vfs = TestingOpenMW::createTestVFS({
                { aPath, nullptr },
            });
            EXPECT_EQ(correctResourcePath({ { textures, bookart } }, path, *vfs, b), "textures/foo.a");
        }

        TEST(MiscResourceHelpersCorrectResourcePath, shouldChangeTopDirectoryAndChangeExtensionIfFallbackExistInVfs)
        {
            constexpr VFS::Path::NormalizedView path("bookart/foo.a");
            constexpr VFS::Path::NormalizedView bPath("textures/foo.b");
            const std::unique_ptr<const VFS::Manager> vfs = TestingOpenMW::createTestVFS({
                { bPath, nullptr },
            });
            EXPECT_EQ(correctResourcePath({ { textures, bookart } }, path, *vfs, b), "textures/foo.b");
        }

        TEST(MiscResourceHelpersCorrectResourcePath, shouldHandlePathEqualToDirectory)
        {
            constexpr VFS::Path::NormalizedView path("sound");
            const std::unique_ptr<const VFS::Manager> vfs = TestingOpenMW::createTestVFS({});
            EXPECT_EQ(correctResourcePath({ { sound } }, path, *vfs, mp3), "sound/sound");
        }

        TEST(MiscResourceHelpersCorrectIconPath, shouldResolveFalloutTextureRelativeIcon)
        {
            constexpr VFS::Path::NormalizedView recordPath(
                "interface/icons/pipboyimages/weapons/weapons_10mm_pistol.dds");
            constexpr VFS::Path::NormalizedView archivePath(
                "textures/interface/icons/pipboyimages/weapons/weapons_10mm_pistol.dds");
            const std::unique_ptr<const VFS::Manager> vfs
                = TestingOpenMW::createTestVFS({ { archivePath, nullptr } });

            EXPECT_EQ(correctIconPath(recordPath, *vfs), archivePath);
        }

        TEST(MiscResourceHelpersCorrectIconPath, shouldRetainMorrowindIconLookup)
        {
            constexpr VFS::Path::NormalizedView recordPath("k/health.tga");
            constexpr VFS::Path::NormalizedView archivePath("icons/k/health.dds");
            const std::unique_ptr<const VFS::Manager> vfs
                = TestingOpenMW::createTestVFS({ { archivePath, nullptr } });

            EXPECT_EQ(correctIconPath(recordPath, *vfs), archivePath);
        }

        struct MiscResourceHelpersCorrectResourcePathShouldRemoveExtraPrefix : TestWithParam<VFS::Path::NormalizedView>
        {
        };

        TEST_P(MiscResourceHelpersCorrectResourcePathShouldRemoveExtraPrefix, shouldMatchExpected)
        {
            const std::unique_ptr<const VFS::Manager> vfs = TestingOpenMW::createTestVFS({});
            EXPECT_EQ(correctResourcePath({ { sound } }, GetParam(), *vfs, mp3), "sound/foo.mp3");
        }

        const std::vector<VFS::Path::NormalizedView> pathsWithPrefix = {
            VFS::Path::NormalizedView("data/sound/foo.mp3"),
            VFS::Path::NormalizedView("data/notsound/sound/foo.mp3"),
            VFS::Path::NormalizedView("data/soundnot/sound/foo.mp3"),
            VFS::Path::NormalizedView("data/notsoundnot/sound/foo.mp3"),
        };

        INSTANTIATE_TEST_SUITE_P(
            PathsWithPrefix, MiscResourceHelpersCorrectResourcePathShouldRemoveExtraPrefix, ValuesIn(pathsWithPrefix));

        TEST(MiscResourceHelpersChangeExtensionToDds, original_extension_with_same_size_as_dds)
        {
            std::string path = "texture/bar.tga";
            ASSERT_TRUE(changeExtensionToDds(path));
            EXPECT_EQ(path, "texture/bar.dds");
        }

        TEST(MiscResourceHelpersChangeExtensionToDds, original_extension_greater_than_dds)
        {
            std::string path = "texture/bar.jpeg";
            ASSERT_TRUE(changeExtensionToDds(path));
            EXPECT_EQ(path, "texture/bar.dds");
        }

        TEST(MiscResourceHelpersChangeExtensionToDds, original_extension_smaller_than_dds)
        {
            std::string path = "texture/bar.xx";
            ASSERT_TRUE(changeExtensionToDds(path));
            EXPECT_EQ(path, "texture/bar.dds");
        }

        TEST(MiscResourceHelpersChangeExtensionToDds, does_not_change_dds_extension)
        {
            std::string path = "texture/bar.dds";
            EXPECT_FALSE(changeExtensionToDds(path));
            EXPECT_EQ(path, "texture/bar.dds");
        }

        TEST(MiscResourceHelpersChangeExtensionToDds, does_not_change_when_no_extension)
        {
            std::string path = "texture/bar";
            EXPECT_FALSE(changeExtensionToDds(path));
            EXPECT_EQ(path, "texture/bar");
        }
    }

    TEST(GetLODMeshName, fallout_new_vegas_versions_use_lod_meshes)
    {
        constexpr VFS::Path::NormalizedView source("meshes/architecture/building.nif");
        constexpr VFS::Path::NormalizedView lod("meshes/architecture/building_lod.nif");
        std::unique_ptr<VFS::Manager> vfs = TestingOpenMW::createTestVFS({ { source, nullptr }, { lod, nullptr } });

        EXPECT_EQ(getLODMeshName(ESM::VER_132, source, *vfs), lod);
        EXPECT_EQ(getLODMeshName(ESM::VER_133, source, *vfs), lod);
        EXPECT_EQ(getLODMeshName(ESM::VER_134, source, *vfs), lod);
    }

    TEST(GetLODMeshName, other_versions_keep_their_existing_mesh_selection)
    {
        constexpr VFS::Path::NormalizedView source("meshes/architecture/building.nif");
        constexpr VFS::Path::NormalizedView dist("meshes/architecture/building_dist.nif");
        constexpr VFS::Path::NormalizedView far("meshes/architecture/building_far.nif");
        constexpr VFS::Path::NormalizedView lod("meshes/architecture/building_lod.nif");
        std::unique_ptr<VFS::Manager> vfs = TestingOpenMW::createTestVFS(
            { { source, nullptr }, { dist, nullptr }, { far, nullptr }, { lod, nullptr } });

        EXPECT_EQ(getLODMeshName(ESM::VER_120, source, *vfs), dist);
        EXPECT_EQ(getLODMeshName(ESM::VER_080, source, *vfs), far);
        EXPECT_EQ(getLODMeshName(ESM::VER_094, source, *vfs), lod);
        EXPECT_EQ(getLODMeshName(ESM::VER_171, source, *vfs), source);
    }
}

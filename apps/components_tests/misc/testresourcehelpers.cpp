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

    TEST(ChangeExtensionToDds, change_when_there_is_an_extension)
    {
        std::string path = "texture/bar.jpeg";
        EXPECT_TRUE(changeExtensionToDds(path));
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

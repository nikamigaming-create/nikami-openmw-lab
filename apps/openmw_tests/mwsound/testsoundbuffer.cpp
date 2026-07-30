#include <apps/openmw/mwsound/soundbuffer.hpp>

#include <components/testing/util.hpp>

#include <gtest/gtest.h>

namespace MWSound
{
    TEST(ESM4SoundResourcePaths, ResolvesEveryAuthoredImmediateDirectoryVariant)
    {
        const std::unique_ptr<VFS::Manager> vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::Normalized("sound/fx/qst/baby/cry/qst_babycry_02.wav"), nullptr },
            { VFS::Path::Normalized("sound/fx/qst/baby/cry/qst_babycry_01.wav"), nullptr },
            { VFS::Path::Normalized("sound/fx/qst/baby/cry/readme.txt"), nullptr },
            { VFS::Path::Normalized("sound/fx/qst/baby/cry/nested/not_a_direct_child.wav"), nullptr },
            { VFS::Path::Normalized("sound/fx/qst/baby/crying/not_in_the_authored_directory.wav"), nullptr },
        });

        const std::vector<VFS::Path::Normalized> actual
            = resolveESM4SoundResourcePaths(R"(fx\qst\baby\cry\)", *vfs);

        ASSERT_EQ(actual.size(), 2);
        EXPECT_EQ(actual[0], "sound/fx/qst/baby/cry/qst_babycry_01.wav");
        EXPECT_EQ(actual[1], "sound/fx/qst/baby/cry/qst_babycry_02.wav");
    }

    TEST(ESM4SoundResourcePaths, PreservesOrdinarySingleFileResolution)
    {
        const std::unique_ptr<VFS::Manager> vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::Normalized("sound/fx/qst/fade.mp3"), nullptr },
        });

        const std::vector<VFS::Path::Normalized> actual
            = resolveESM4SoundResourcePaths(R"(fx\qst\fade.wav)", *vfs);

        ASSERT_EQ(actual.size(), 1);
        EXPECT_EQ(actual[0], "sound/fx/qst/fade.mp3");
    }
}

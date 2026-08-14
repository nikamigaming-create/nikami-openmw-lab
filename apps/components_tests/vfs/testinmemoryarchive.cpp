#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include <components/vfs/inmemoryarchive.hpp>
#include <components/vfs/manager.hpp>

namespace
{
    using namespace testing;

    std::string readAll(Files::IStreamPtr&& stream)
    {
        std::stringstream buf;
        buf << stream->rdbuf();
        return buf.str();
    }

    TEST(InMemoryArchiveTest, ShouldServeAddedFile)
    {
        VFS::Manager vfs;
        auto archive = std::make_unique<VFS::InMemoryArchive>("generated");
        VFS::InMemoryArchive* generated = archive.get();
        vfs.addArchive(std::move(archive));

        generated->addFile(VFS::Path::Normalized("generated/test.lua"), "return 42");
        vfs.buildIndex();

        constexpr VFS::Path::NormalizedView path("generated/test.lua");
        ASSERT_TRUE(vfs.exists(path));
        EXPECT_EQ(readAll(vfs.get(path)), "return 42");
    }

    TEST(InMemoryArchiveTest, FilesAddedAfterIndexingShouldAppearAfterRebuild)
    {
        VFS::Manager vfs;
        auto archive = std::make_unique<VFS::InMemoryArchive>("generated");
        VFS::InMemoryArchive* generated = archive.get();
        vfs.addArchive(std::move(archive));
        vfs.buildIndex();

        constexpr VFS::Path::NormalizedView path("generated/late.lua");
        EXPECT_FALSE(vfs.exists(path));

        generated->addFile(VFS::Path::Normalized("generated/late.lua"), "late content");
        EXPECT_FALSE(vfs.exists(path)); // not visible until the index is rebuilt
        vfs.buildIndex();
        ASSERT_TRUE(vfs.exists(path));
        EXPECT_EQ(readAll(vfs.get(path)), "late content");
    }

    TEST(InMemoryArchiveTest, AddingSamePathShouldReplaceContent)
    {
        VFS::Manager vfs;
        auto archive = std::make_unique<VFS::InMemoryArchive>("generated");
        VFS::InMemoryArchive* generated = archive.get();
        vfs.addArchive(std::move(archive));

        generated->addFile(VFS::Path::Normalized("generated/replaced.lua"), "old");
        generated->addFile(VFS::Path::Normalized("generated/replaced.lua"), "new");
        vfs.buildIndex();

        EXPECT_EQ(readAll(vfs.get(VFS::Path::Normalized("generated/replaced.lua"))), "new");
    }

    TEST(InMemoryArchiveTest, FileShouldReportStem)
    {
        VFS::InMemoryArchive archive("generated");
        archive.addFile(VFS::Path::Normalized("generated/obscript/somescript.lua"), "");

        VFS::FileMap files;
        archive.listResources(files);
        ASSERT_EQ(files.size(), 1);
        EXPECT_EQ(files.begin()->second->getStem(), "somescript");
    }

    TEST(InMemoryArchiveTest, EachOpenShouldGetIndependentStream)
    {
        VFS::InMemoryArchive archive("generated");
        archive.addFile(VFS::Path::Normalized("generated/shared.lua"), "abc");

        VFS::FileMap files;
        archive.listResources(files);
        VFS::File* file = files.begin()->second;
        Files::IStreamPtr first = file->open();
        Files::IStreamPtr second = file->open();
        char c = 0;
        first->read(&c, 1);
        EXPECT_EQ(c, 'a');
        second->read(&c, 1);
        EXPECT_EQ(c, 'a'); // second stream unaffected by reads on the first
    }
}

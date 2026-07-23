#ifndef OPENMW_COMPONENTS_VFS_INMEMORYARCHIVE_H
#define OPENMW_COMPONENTS_VFS_INMEMORYARCHIVE_H

#include <map>
#include <string>

#include "archive.hpp"
#include "file.hpp"
#include "pathutil.hpp"

namespace VFS
{
    class InMemoryArchiveFile : public File
    {
    public:
        explicit InMemoryArchiveFile(std::string&& content);

        Files::IStreamPtr open() override;

        std::filesystem::file_time_type getLastModified() const override { return {}; }

        std::string getStem() const override;

        void setStem(std::string stem) { mStem = std::move(stem); }

    private:
        std::string mContent;
        std::string mStem;
    };

    // An archive whose files are created at runtime and stored in memory,
    // e.g. for engine-generated scripts. Files added after the index was
    // built become visible after the next Manager::buildIndex().
    class InMemoryArchive : public Archive
    {
    public:
        explicit InMemoryArchive(std::string description);

        // Adds a file with the given content, replacing any previous file at
        // the same path.
        void addFile(Path::Normalized path, std::string content);

        void listResources(FileMap& out) override;

        bool contains(Path::NormalizedView file) const override;

        std::string getDescription() const override { return mDescription; }

    private:
        std::map<Path::Normalized, InMemoryArchiveFile, std::less<>> mFiles;
        std::string mDescription;
    };
}

#endif

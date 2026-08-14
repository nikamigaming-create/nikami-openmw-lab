#include "inmemoryarchive.hpp"

#include <sstream>
#include <utility>

namespace VFS
{
    InMemoryArchiveFile::InMemoryArchiveFile(std::string&& content)
        : mContent(std::move(content))
    {
    }

    Files::IStreamPtr InMemoryArchiveFile::open()
    {
        return std::make_unique<std::stringstream>(mContent, std::ios_base::in);
    }

    std::string InMemoryArchiveFile::getStem() const
    {
        return mStem;
    }

    InMemoryArchive::InMemoryArchive(std::string description)
        : mDescription(std::move(description))
    {
    }

    void InMemoryArchive::addFile(Path::Normalized path, std::string content)
    {
        const std::string_view value = path.value();
        const std::size_t separator = value.find_last_of('/');
        const std::string_view filename = separator == std::string_view::npos ? value : value.substr(separator + 1);
        const std::size_t extension = filename.find_last_of('.');

        InMemoryArchiveFile file(std::move(content));
        file.setStem(std::string(extension == std::string_view::npos ? filename : filename.substr(0, extension)));

        mFiles.insert_or_assign(std::move(path), std::move(file));
    }

    void InMemoryArchive::listResources(FileMap& out)
    {
        for (auto& [path, file] : mFiles)
            out[path] = &file;
    }

    bool InMemoryArchive::contains(Path::NormalizedView file) const
    {
        return mFiles.find(file) != mFiles.end();
    }
}

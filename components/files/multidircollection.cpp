#include "multidircollection.hpp"
#include "conversion.hpp"

#include <filesystem>

#include <components/debug/debuglog.hpp>

namespace Files
{

<<<<<<< HEAD
    MultiDirCollection::MultiDirCollection(const Files::PathContainer& directories, std::string_view extension)
=======
    MultiDirCollection::MultiDirCollection(const Files::PathContainer& directories, const std::string& extension)
>>>>>>> origin/main
    {
        for (const auto& directory : directories)
        {
            if (!std::filesystem::is_directory(directory))
            {
                Log(Debug::Info) << "Skipping invalid directory: " << directory;
                continue;
            }

            for (const auto& dirIter : std::filesystem::directory_iterator(directory))
            {
                const auto& path = dirIter.path();

<<<<<<< HEAD
                std::string ext = Files::pathToUnicodeString(path.extension());
                if (ext.size() != extension.size() + 1 || !Misc::StringUtils::ciEndsWith(ext, extension))
=======
                if (!Misc::StringUtils::ciEqual(extension, Files::pathToUnicodeString(path.extension())))
>>>>>>> origin/main
                    continue;

                const auto filename = Files::pathToUnicodeString(path.filename());

                TIter result = mFiles.find(filename);

                if (result == mFiles.end())
                {
                    mFiles.insert(std::make_pair(filename, path));
                }
                else if (result->first == filename)
                {
                    mFiles[filename] = path;
                }
                else
                {
                    // handle case folding
                    mFiles.erase(result->first);
<<<<<<< HEAD
                    mFiles.emplace(filename, path);
=======
                    mFiles.insert(std::make_pair(filename, path));
>>>>>>> origin/main
                }
            }
        }
    }

<<<<<<< HEAD
    std::filesystem::path MultiDirCollection::getPath(std::string_view file) const
=======
    std::filesystem::path MultiDirCollection::getPath(const std::string& file) const
>>>>>>> origin/main
    {
        TIter iter = mFiles.find(file);

        if (iter == mFiles.end())
<<<<<<< HEAD
            throw std::runtime_error("file " + std::string(file) + " not found");
=======
            throw std::runtime_error("file " + file + " not found");
>>>>>>> origin/main

        return iter->second;
    }

<<<<<<< HEAD
    bool MultiDirCollection::doesExist(std::string_view file) const
    {
        return mFiles.contains(file);
=======
    bool MultiDirCollection::doesExist(const std::string& file) const
    {
        return mFiles.find(file) != mFiles.end();
>>>>>>> origin/main
    }

    MultiDirCollection::TIter MultiDirCollection::begin() const
    {
        return mFiles.begin();
    }

    MultiDirCollection::TIter MultiDirCollection::end() const
    {
        return mFiles.end();
    }
}

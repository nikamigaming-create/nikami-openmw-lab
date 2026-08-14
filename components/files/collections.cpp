#include "collections.hpp"
#include "conversion.hpp"

#include <components/misc/strings/algorithm.hpp>
<<<<<<< HEAD
=======
#include <components/misc/strings/lower.hpp>
>>>>>>> origin/main

namespace Files
{
    Collections::Collections()
        : mDirectories()
        , mCollections()
    {
    }

    Collections::Collections(const Files::PathContainer& directories)
        : mDirectories(directories)
        , mCollections()
    {
    }

    const MultiDirCollection& Collections::getCollection(std::string_view extension) const
    {
<<<<<<< HEAD
        auto iter = mCollections.find(extension);
        if (iter == mCollections.end())
        {
            auto result = mCollections.emplace(extension, MultiDirCollection(mDirectories, extension));
=======
        std::string ext = Misc::StringUtils::lowerCase(extension);
        auto iter = mCollections.find(ext);
        if (iter == mCollections.end())
        {
            std::pair<MultiDirCollectionContainer::iterator, bool> result
                = mCollections.emplace(ext, MultiDirCollection(mDirectories, ext));
>>>>>>> origin/main

            iter = result.first;
        }

        return iter->second;
    }

<<<<<<< HEAD
    std::filesystem::path Collections::getPath(std::string_view file) const
=======
    std::filesystem::path Collections::getPath(const std::string& file) const
>>>>>>> origin/main
    {
        for (auto iter = mDirectories.rbegin(); iter != mDirectories.rend(); iter++)
        {
            for (const auto& iter2 : std::filesystem::directory_iterator(*iter))
            {
                const auto& path = iter2.path();
                const auto str = Files::pathToUnicodeString(path.filename());

                if (Misc::StringUtils::ciEqual(file, str))
                    return path;
            }
        }

<<<<<<< HEAD
        throw std::runtime_error("file " + std::string(file) + " not found");
=======
        throw std::runtime_error("file " + file + " not found");
>>>>>>> origin/main
    }

    bool Collections::doesExist(std::string_view file) const
    {
        for (auto iter = mDirectories.rbegin(); iter != mDirectories.rend(); iter++)
        {
            for (const auto& iter2 : std::filesystem::directory_iterator(*iter))
            {
                const auto& path = iter2.path();
                const auto str = Files::pathToUnicodeString(path.filename());

                if (Misc::StringUtils::ciEqual(file, str))
                    return true;
            }
        }

        return false;
    }

    const Files::PathContainer& Collections::getPaths() const
    {
        return mDirectories;
    }
}

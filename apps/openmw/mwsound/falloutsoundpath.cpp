#include "falloutsoundpath.hpp"

#include <algorithm>
#include <array>
#include <string>

#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

namespace MWSound
{
    namespace
    {
        VFS::Path::Normalized makeFalloutSoundPath(std::string_view authoredPath)
        {
            std::string path = VFS::Path::normalizeFilename(authoredPath);
            path.erase(std::unique(path.begin(), path.end(),
                           [](char left, char right) {
                               return left == VFS::Path::separator && right == VFS::Path::separator;
                           }),
                path.end());
            while (!path.empty() && path.front() == VFS::Path::separator)
                path.erase(path.begin());

            constexpr std::string_view soundPrefix = "sound/";
            if (!path.starts_with(soundPrefix))
                path.insert(0, soundPrefix);
            return VFS::Path::Normalized(std::move(path));
        }

        bool hasSupportedAudioExtension(VFS::Path::NormalizedView path)
        {
            constexpr std::array<std::string_view, 3> extensions{ ".wav", ".mp3", ".ogg" };
            return std::any_of(extensions.begin(), extensions.end(),
                [value = path.value()](std::string_view extension) { return value.ends_with(extension); });
        }
    }

    bool isFalloutSoundAssetPath(std::string_view authoredPath)
    {
        return authoredPath.find_first_of("/\\") != std::string_view::npos;
    }

    std::optional<VFS::Path::Normalized> resolveFalloutSoundPath(std::string_view authoredPath, const VFS::Manager& vfs)
    {
        if (authoredPath.empty())
            return std::nullopt;

        VFS::Path::Normalized path = makeFalloutSoundPath(authoredPath);
        const bool directory = path.value().ends_with(VFS::Path::separator);
        if (!directory)
        {
            if (vfs.exists(path))
                return path;

            VFS::Path::Normalized mp3Path = path;
            if (mp3Path.changeExtension("mp3") && vfs.exists(mp3Path))
                return mp3Path;
        }

        std::string prefix = path.value();
        if (!prefix.ends_with(VFS::Path::separator))
            prefix.push_back(VFS::Path::separator);

        // VFS indices are ordered by normalized path, making the first accepted variant stable across runs and
        // independent of archive registration order. Random-variant playback can later consume the same range
        // without changing how authored directory records are recognized.
        for (VFS::Path::NormalizedView candidate : vfs.getRecursiveDirectoryIterator(prefix))
        {
            if (hasSupportedAudioExtension(candidate))
                return VFS::Path::Normalized(candidate);
        }
        return std::nullopt;
    }
}

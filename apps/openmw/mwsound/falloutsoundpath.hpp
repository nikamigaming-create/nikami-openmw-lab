#ifndef OPENMW_MWSOUND_FALLOUTSOUNDPATH_H
#define OPENMW_MWSOUND_FALLOUTSOUNDPATH_H

#include <optional>
#include <string_view>

#include <components/vfs/pathutil.hpp>

namespace VFS
{
    class Manager;
}

namespace MWSound
{
    /// Resolve an authored Fallout SOUN/SNDR path against the VFS. Fallout sound records may name either one
    /// concrete audio file or a directory containing randomized variants. Directory records are resolved to the
    /// lexicographically first supported file so the current one-buffer-per-FormID cache remains deterministic.
    /// Missing authored resources return no path; this function never invents a legacy/default sound.
    [[nodiscard]] std::optional<VFS::Path::Normalized> resolveFalloutSoundPath(
        std::string_view authoredPath, const VFS::Manager& vfs);
}

#endif

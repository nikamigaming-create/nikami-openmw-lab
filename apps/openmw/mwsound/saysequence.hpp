#ifndef GAME_SOUND_SAYSEQUENCE_H
#define GAME_SOUND_SAYSEQUENCE_H

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <utility>

#include <components/vfs/pathutil.hpp>

namespace MWSound
{
    /// Tracks the single active line and the remaining authored lines for one actor.
    /// Stream ownership stays in SoundManager; this class only defines deterministic
    /// replacement and advancement semantics.
    class SaySequence
    {
    public:
        void replace(std::span<const VFS::Path::Normalized> voices)
        {
            clear();
            mPending.assign(voices.begin(), voices.end());
        }

        const VFS::Path::Normalized* beginNext()
        {
            if (mCurrent || mPending.empty())
                return nullptr;

            mCurrent.emplace(std::move(mPending.front()));
            mPending.pop_front();
            return &*mCurrent;
        }

        void finishCurrent() { mCurrent.reset(); }

        void clear()
        {
            mCurrent.reset();
            mPending.clear();
        }

        const VFS::Path::Normalized* getCurrent() const
        {
            return mCurrent ? &*mCurrent : nullptr;
        }

        std::size_t getPendingCount() const { return mPending.size(); }

    private:
        std::optional<VFS::Path::Normalized> mCurrent;
        std::deque<VFS::Path::Normalized> mPending;
    };
}

#endif

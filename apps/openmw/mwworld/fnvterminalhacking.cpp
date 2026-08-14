#include "fnvterminalhacking.hpp"

#include "fnvterminalruntime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace MWWorld
{
    namespace
    {
        std::pair<std::size_t, std::size_t> getWordLengthRange(FnvTerminalDifficulty difficulty)
        {
            switch (difficulty)
            {
                case FnvTerminalDifficulty::VeryEasy:
                    return { 4, 5 };
                case FnvTerminalDifficulty::Easy:
                    return { 6, 8 };
                case FnvTerminalDifficulty::Average:
                    return { 9, 10 };
                case FnvTerminalDifficulty::Hard:
                    return { 11, 12 };
                case FnvTerminalDifficulty::VeryHard:
                    return { 13, 15 };
                case FnvTerminalDifficulty::RequiresKey:
                    return { 0, 0 };
            }
            return { 0, 0 };
        }

        std::uint32_t nextRandom(std::uint32_t& state)
        {
            if (state == 0)
                state = 0x6d2b79f5u;
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }

        constexpr std::string_view HackingPunctuation = "!@#$%^&*()-_=+[]{}<>?/\\|;:,.`~";

        std::string makeBoardRow(std::uint16_t address, std::string_view characters)
        {
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result(FnvTerminalBoardDisplayCharactersPerRow, ' ');
            result[0] = '0';
            result[1] = 'x';
            result[2] = hex[(address >> 12) & 0xf];
            result[3] = hex[(address >> 8) & 0xf];
            result[4] = hex[(address >> 4) & 0xf];
            result[5] = hex[address & 0xf];
            std::copy(characters.begin(), characters.end(), result.begin() + 7);
            return result;
        }
    }

    bool FnvTerminalBoard::isValid() const
    {
        if (mRows.size() != FnvTerminalBoardColumnCount * FnvTerminalBoardRowCount
            || std::any_of(mRows.begin(), mRows.end(), [](const std::string& row) {
                   return row.size() != FnvTerminalBoardDisplayCharactersPerRow || row[0] != '0' || row[1] != 'x'
                       || row[6] != ' ';
               }))
            return false;
        const std::size_t capacity = FnvTerminalBoardColumnCount * FnvTerminalBoardRowCount
            * FnvTerminalBoardDataCharactersPerRow;
        return std::all_of(mWords.begin(), mWords.end(), [capacity](const FnvTerminalBoardWord& word) {
            return word.mBegin <= word.mEnd && word.mEnd < capacity;
        }) && std::all_of(mTargets.begin(), mTargets.end(), [capacity](const FnvTerminalBoardTarget& target) {
            return target.mBegin <= target.mEnd && target.mEnd < capacity;
        });
    }

    std::size_t getFnvTerminalWordLikeness(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
            return 0;
        std::size_t result = 0;
        for (std::size_t i = 0; i < left.size(); ++i)
            result += left[i] == right[i] ? 1u : 0u;
        return result;
    }

    std::size_t getFnvTerminalWordCount(float science, FnvTerminalDifficulty difficulty,
        int minimumWords, int maximumWords, float hackLevelMultiplier)
    {
        if (!std::isfinite(science) || !std::isfinite(hackLevelMultiplier) || minimumWords < 0
            || maximumWords < minimumWords || difficulty == FnvTerminalDifficulty::RequiresKey)
            return 0;

        // Fallout's terminal formula uses the authored difficulty ordinal as the lock-level setting.
        // Very Hard produces a zero lock offset with the stock multiplier; the engine substitutes 0.5.
        const float lockLevel = static_cast<float>(difficulty) * hackLevelMultiplier * 100.f;
        const float lockOffset = 100.f - lockLevel;
        const float scienceRatio = std::abs(lockOffset) <= std::numeric_limits<float>::epsilon()
            ? 0.5f
            : (science - lockLevel) / lockOffset;
        const float count = (1.f - scienceRatio) * static_cast<float>(maximumWords - minimumWords)
            + static_cast<float>(minimumWords);

        // The original always caps this minigame at twenty words, even when a GMST requests more.
        const int engineMaximum = std::min(maximumWords, 20);
        return static_cast<std::size_t>(std::clamp(static_cast<int>(count), minimumWords, engineMaximum));
    }

    std::string_view getFnvTerminalXpRewardGameSetting(FnvTerminalDifficulty difficulty)
    {
        constexpr std::array<std::string_view, 5> settings{
            "iXPRewardHackComputerVeryEasy",
            "iXPRewardHackComputerEasy",
            "iXPRewardHackComputerAverage",
            "iXPRewardHackComputerHard",
            "iXPRewardHackComputerVeryHard",
        };
        const std::size_t index = static_cast<std::size_t>(difficulty);
        return index < settings.size() ? settings[index] : std::string_view{};
    }

    std::vector<FnvTerminalBracketPair> findFnvTerminalBracketPairs(std::string_view row)
    {
        constexpr std::array<char, 4> openers{ '(', '[', '{', '<' };
        constexpr std::array<char, 4> closers{ ')', ']', '}', '>' };
        std::vector<FnvTerminalBracketPair> result;
        for (std::size_t begin = 0; begin < row.size(); ++begin)
        {
            const auto opener = std::find(openers.begin(), openers.end(), row[begin]);
            if (opener == openers.end())
                continue;
            const char closer = closers[static_cast<std::size_t>(opener - openers.begin())];
            const std::size_t end = row.find(closer, begin + 1);
            if (end != std::string_view::npos)
                result.push_back({ begin, end });
        }
        return result;
    }

    std::vector<std::string> parseFnvTerminalDictionary(std::string_view contents)
    {
        std::istringstream input{ std::string(contents) };
        std::vector<std::string> words;
        std::unordered_set<std::string> unique;
        for (std::string word; input >> word;)
        {
            if (!std::all_of(word.begin(), word.end(), [](unsigned char value) {
                    return value >= 'A' && value <= 'Z';
                }))
                continue;
            if (unique.insert(word).second)
                words.push_back(std::move(word));
        }
        return words;
    }

    std::vector<std::string> buildFnvTerminalWordSet(std::span<const std::string> dictionary,
        FnvTerminalDifficulty difficulty, std::uint32_t seed, std::size_t maximumWords)
    {
        const auto [minimumLength, maximumLength] = getWordLengthRange(difficulty);
        if (minimumLength == 0 || maximumWords < 2)
            return {};

        std::vector<std::string> candidates;
        std::unordered_set<std::string_view> unique;
        for (const std::string& word : dictionary)
        {
            if (word.size() >= minimumLength && word.size() <= maximumLength && unique.insert(word).second)
                candidates.push_back(word);
        }
        for (std::size_t end = candidates.size(); end > 1; --end)
        {
            const std::size_t selected = nextRandom(seed) % end;
            std::swap(candidates[end - 1], candidates[selected]);
        }
        if (candidates.empty())
            return {};

        const std::size_t selectedLength = candidates.front().size();
        std::erase_if(candidates, [selectedLength](const std::string& word) { return word.size() != selectedLength; });
        if (candidates.size() > maximumWords)
            candidates.resize(maximumWords);
        return candidates.size() >= 2 ? candidates : std::vector<std::string>{};
    }

    FnvTerminalBoard buildFnvTerminalHackingBoard(std::span<const std::string> words, std::uint32_t seed)
    {
        constexpr std::size_t columnCapacity
            = FnvTerminalBoardRowCount * FnvTerminalBoardDataCharactersPerRow;
        constexpr std::size_t boardCapacity = FnvTerminalBoardColumnCount * columnCapacity;
        FnvTerminalBoard result;
        if (words.size() < 2 || std::any_of(words.begin(), words.end(), [](const std::string& word) {
                return word.empty() || word.size() > columnCapacity
                    || !std::all_of(word.begin(), word.end(), [](unsigned char value) {
                           return value >= 'A' && value <= 'Z';
                       });
            }))
            return result;

        std::string memory(boardCapacity, ' ');
        for (char& value : memory)
            value = HackingPunctuation[nextRandom(seed) % HackingPunctuation.size()];
        std::vector<bool> occupied(boardCapacity, false);
        std::vector<std::size_t> order(words.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&words](std::size_t left, std::size_t right) {
            return words[left].size() > words[right].size();
        });

        for (const std::size_t wordIndex : order)
        {
            const std::string& word = words[wordIndex];
            std::vector<std::size_t> starts;
            starts.reserve(boardCapacity);
            for (std::size_t column = 0; column < FnvTerminalBoardColumnCount; ++column)
            {
                const std::size_t columnBegin = column * columnCapacity;
                const std::size_t columnEnd = columnBegin + columnCapacity;
                for (std::size_t begin = columnBegin; begin + word.size() <= columnEnd; ++begin)
                {
                    const std::size_t guardedBegin = begin == columnBegin ? begin : begin - 1;
                    const std::size_t guardedEnd = std::min(columnEnd, begin + word.size() + 1);
                    if (std::none_of(occupied.begin() + static_cast<std::ptrdiff_t>(guardedBegin),
                            occupied.begin() + static_cast<std::ptrdiff_t>(guardedEnd), [](bool value) {
                                return value;
                            }))
                        starts.push_back(begin);
                }
            }
            if (starts.empty())
                return {};
            const std::size_t begin = starts[nextRandom(seed) % starts.size()];
            const std::size_t end = begin + word.size() - 1;
            std::copy(word.begin(), word.end(), memory.begin() + static_cast<std::ptrdiff_t>(begin));
            std::fill(occupied.begin() + static_cast<std::ptrdiff_t>(begin),
                occupied.begin() + static_cast<std::ptrdiff_t>(end + 1), true);
            result.mWords.push_back({ wordIndex, begin, end });
            result.mTargets.push_back(
                { FnvTerminalBoardTargetKind::Word, begin, end, wordIndex });
        }
        std::sort(result.mWords.begin(), result.mWords.end(), [](const auto& left, const auto& right) {
            return left.mBegin < right.mBegin;
        });

        // hacking_menu.xml declares two columns of 28 rows, each 19 glyphs wide. Seven glyphs are the address and
        // separator, leaving twelve contiguous memory characters per row. Words can cross a row boundary, but not
        // the boundary between the two memory columns.
        const std::uint16_t maximumStart = static_cast<std::uint16_t>(0x10000u - boardCapacity);
        const std::uint16_t baseAddress
            = static_cast<std::uint16_t>((0xf000u + nextRandom(seed) % (maximumStart - 0xf000u + 1u)) & ~3u);
        result.mRows.reserve(FnvTerminalBoardColumnCount * FnvTerminalBoardRowCount);
        for (std::size_t column = 0; column < FnvTerminalBoardColumnCount; ++column)
        {
            for (std::size_t row = 0; row < FnvTerminalBoardRowCount; ++row)
            {
                const std::size_t offset = column * columnCapacity + row * FnvTerminalBoardDataCharactersPerRow;
                result.mRows.push_back(makeBoardRow(static_cast<std::uint16_t>(baseAddress + offset),
                    std::string_view(memory).substr(offset, FnvTerminalBoardDataCharactersPerRow)));
                const std::string_view rowMemory
                    = std::string_view(memory).substr(offset, FnvTerminalBoardDataCharactersPerRow);
                for (const FnvTerminalBracketPair& pair : findFnvTerminalBracketPairs(rowMemory))
                {
                    const std::string_view sequence = rowMemory.substr(pair.mBegin, pair.mEnd - pair.mBegin + 1);
                    if (std::none_of(sequence.begin(), sequence.end(), [](unsigned char value) {
                            return std::isalpha(value) != 0;
                        }))
                        result.mTargets.push_back({ FnvTerminalBoardTargetKind::Bracket,
                            offset + pair.mBegin, offset + pair.mEnd, 0 });
                }
            }
        }
        std::sort(result.mTargets.begin(), result.mTargets.end(), [](const auto& left, const auto& right) {
            if (left.mBegin != right.mBegin)
                return left.mBegin < right.mBegin;
            return left.mEnd < right.mEnd;
        });
        return result;
    }

    FnvTerminalHackingSession::FnvTerminalHackingSession(std::vector<std::string> words, std::size_t answer)
        : mWords(std::move(words))
        , mActive(mWords.size(), true)
        , mAnswer(answer)
    {
    }

    bool FnvTerminalHackingSession::isValid() const
    {
        if (mWords.size() < 2 || mAnswer >= mWords.size() || mWords[mAnswer].empty())
            return false;
        const std::size_t length = mWords[mAnswer].size();
        std::unordered_set<std::string_view> unique;
        for (const std::string& word : mWords)
        {
            if (word.size() != length || !std::all_of(word.begin(), word.end(), [](unsigned char value) {
                    return value >= 'A' && value <= 'Z';
                })
                || !unique.insert(word).second)
                return false;
        }
        return true;
    }

    bool FnvTerminalHackingSession::isWordActive(std::size_t index) const
    {
        return index < mActive.size() && mActive[index];
    }

    FnvTerminalGuessOutcome FnvTerminalHackingSession::guess(std::size_t index)
    {
        FnvTerminalGuessOutcome outcome{ FnvTerminalGuessResult::Invalid, 0, mAttemptsRemaining };
        if (!isValid() || mSolved || mLockedOut || !isWordActive(index))
            return outcome;

        outcome.mLikeness = getFnvTerminalWordLikeness(mWords[index], mWords[mAnswer]);
        if (index == mAnswer)
        {
            mSolved = true;
            outcome.mResult = FnvTerminalGuessResult::Correct;
        }
        else
        {
            --mAttemptsRemaining;
            mLockedOut = mAttemptsRemaining == 0;
            outcome.mResult = mLockedOut ? FnvTerminalGuessResult::LockedOut : FnvTerminalGuessResult::Incorrect;
        }
        outcome.mAttemptsRemaining = mAttemptsRemaining;
        return outcome;
    }

    bool FnvTerminalHackingSession::removeDud(std::size_t index)
    {
        if (!isValid() || mSolved || mLockedOut || index == mAnswer || !isWordActive(index))
            return false;
        mActive[index] = false;
        return true;
    }

    bool FnvTerminalHackingSession::resetAllowance()
    {
        if (!isValid() || mSolved || mLockedOut || mAttemptsRemaining == FnvTerminalInitialAttempts)
            return false;
        mAttemptsRemaining = FnvTerminalInitialAttempts;
        return true;
    }

    FnvTerminalBracketOutcome FnvTerminalHackingSession::activateBracket(std::uint32_t seed)
    {
        FnvTerminalBracketOutcome outcome{ FnvTerminalBracketResult::Invalid, 0, mAttemptsRemaining };
        if (!isValid() || mSolved || mLockedOut)
            return outcome;

        std::vector<std::size_t> duds;
        for (std::size_t index = 0; index < mWords.size(); ++index)
        {
            if (index != mAnswer && isWordActive(index))
                duds.push_back(index);
        }
        const bool replenish = duds.empty() || (nextRandom(seed) & 1u) == 0;
        if (replenish)
        {
            // Retail consumes the bracket and reports an allowance reset even when the allowance is already full.
            if (mAttemptsRemaining != FnvTerminalInitialAttempts)
                mAttemptsRemaining = FnvTerminalInitialAttempts;
            outcome.mResult = FnvTerminalBracketResult::AllowanceReset;
        }
        else
        {
            outcome.mRemovedWord = duds[nextRandom(seed) % duds.size()];
            mActive[outcome.mRemovedWord] = false;
            outcome.mResult = FnvTerminalBracketResult::DudRemoved;
        }
        outcome.mAttemptsRemaining = mAttemptsRemaining;
        return outcome;
    }
}

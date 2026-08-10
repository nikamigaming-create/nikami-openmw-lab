#ifndef OPENMW_MWWORLD_FNVTERMINALHACKING_H
#define OPENMW_MWWORLD_FNVTERMINALHACKING_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MWWorld
{
    enum class FnvTerminalDifficulty : std::uint8_t;

    inline constexpr int FnvTerminalInitialAttempts = 4;
    inline constexpr std::size_t FnvTerminalBoardColumnCount = 2;
    inline constexpr std::size_t FnvTerminalBoardRowCount = 28;
    inline constexpr std::size_t FnvTerminalBoardDataCharactersPerRow = 12;
    inline constexpr std::size_t FnvTerminalBoardDisplayCharactersPerRow = 19;

    enum class FnvTerminalGuessResult
    {
        Invalid,
        Incorrect,
        Correct,
        LockedOut,
    };

    struct FnvTerminalGuessOutcome
    {
        FnvTerminalGuessResult mResult = FnvTerminalGuessResult::Invalid;
        std::size_t mLikeness = 0;
        int mAttemptsRemaining = FnvTerminalInitialAttempts;
    };

    struct FnvTerminalBracketPair
    {
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;

        bool operator==(const FnvTerminalBracketPair&) const = default;
    };

    struct FnvTerminalBoardWord
    {
        std::size_t mWord = 0;
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;

        bool operator==(const FnvTerminalBoardWord&) const = default;
    };

    enum class FnvTerminalBoardTargetKind
    {
        Word,
        Bracket,
    };

    struct FnvTerminalBoardTarget
    {
        FnvTerminalBoardTargetKind mKind = FnvTerminalBoardTargetKind::Word;
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;
        std::size_t mWord = 0;

        bool operator==(const FnvTerminalBoardTarget&) const = default;
    };

    struct FnvTerminalBoard
    {
        std::vector<std::string> mRows;
        std::vector<FnvTerminalBoardWord> mWords;
        std::vector<FnvTerminalBoardTarget> mTargets;

        [[nodiscard]] bool isValid() const;
    };

    enum class FnvTerminalBracketResult
    {
        Invalid,
        DudRemoved,
        AllowanceReset,
    };

    struct FnvTerminalBracketOutcome
    {
        FnvTerminalBracketResult mResult = FnvTerminalBracketResult::Invalid;
        std::size_t mRemovedWord = 0;
        int mAttemptsRemaining = FnvTerminalInitialAttempts;
    };

    [[nodiscard]] std::size_t getFnvTerminalWordLikeness(std::string_view left, std::string_view right);
    [[nodiscard]] std::size_t getFnvTerminalWordCount(float science, FnvTerminalDifficulty difficulty,
        int minimumWords, int maximumWords, float hackLevelMultiplier);
    [[nodiscard]] std::string_view getFnvTerminalXpRewardGameSetting(FnvTerminalDifficulty difficulty);
    [[nodiscard]] std::vector<FnvTerminalBracketPair> findFnvTerminalBracketPairs(std::string_view row);
    [[nodiscard]] std::vector<std::string> parseFnvTerminalDictionary(std::string_view contents);
    [[nodiscard]] std::vector<std::string> buildFnvTerminalWordSet(
        std::span<const std::string> dictionary, FnvTerminalDifficulty difficulty, std::uint32_t seed,
        std::size_t maximumWords = 12);
    [[nodiscard]] FnvTerminalBoard buildFnvTerminalHackingBoard(
        std::span<const std::string> words, std::uint32_t seed);

    class FnvTerminalHackingSession
    {
        std::vector<std::string> mWords;
        std::vector<bool> mActive;
        std::size_t mAnswer = 0;
        int mAttemptsRemaining = FnvTerminalInitialAttempts;
        bool mSolved = false;
        bool mLockedOut = false;

    public:
        FnvTerminalHackingSession(std::vector<std::string> words, std::size_t answer);

        bool isValid() const;
        bool isSolved() const { return mSolved; }
        bool isLockedOut() const { return mLockedOut; }
        int getAttemptsRemaining() const { return mAttemptsRemaining; }
        std::span<const std::string> getWords() const { return mWords; }
        bool isWordActive(std::size_t index) const;

        FnvTerminalGuessOutcome guess(std::size_t index);
        bool removeDud(std::size_t index);
        bool resetAllowance();
        FnvTerminalBracketOutcome activateBracket(std::uint32_t seed);
    };
}

#endif

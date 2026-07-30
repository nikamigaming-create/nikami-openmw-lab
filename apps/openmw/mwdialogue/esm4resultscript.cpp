#include "esm4resultscript.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <components/misc/strings/algorithm.hpp>

namespace MWDialogue
{
    namespace
    {
        std::string_view trim(std::string_view value)
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.remove_prefix(1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.remove_suffix(1);
            return value;
        }

        std::string_view firstToken(std::string_view line)
        {
            const std::size_t end = line.find_first_of(" \t\r\n");
            return line.substr(0, end);
        }

        bool isControlKeyword(std::string_view line, std::string_view keyword, bool allowOpeningParenthesis = false)
        {
            if (line.size() < keyword.size()
                || !Misc::StringUtils::ciEqual(line.substr(0, keyword.size()), keyword))
                return false;
            if (line.size() == keyword.size())
                return true;
            const char next = line[keyword.size()];
            return std::isspace(static_cast<unsigned char>(next)) || (allowOpeningParenthesis && next == '(');
        }

        Esm4ResultCommand parseTopLevelCommand(std::string_view line)
        {
            Esm4ResultCommand result;
            result.mSource = std::string(line);

            const std::string_view token = firstToken(line);
            if (Misc::StringUtils::ciEqual(token, "ShowBarterMenu"))
            {
                result.mType = Esm4ResultCommandType::ShowBarterMenu;
                result.mSource.clear();
                return result;
            }

            const std::size_t separator = token.rfind('.');
            if (separator == std::string_view::npos || separator == 0 || separator + 1 == token.size())
                return result;

            const std::string_view command = token.substr(separator + 1);
            if (Misc::StringUtils::ciEqual(command, "Enable"))
                result.mType = Esm4ResultCommandType::Enable;
            else if (Misc::StringUtils::ciEqual(command, "Disable"))
                result.mType = Esm4ResultCommandType::Disable;
            else if (Misc::StringUtils::ciEqual(command, "Unlock"))
                result.mType = Esm4ResultCommandType::Unlock;
            else if (Misc::StringUtils::ciEqual(command, "evp")
                || Misc::StringUtils::ciEqual(command, "EvaluatePackage"))
                result.mType = Esm4ResultCommandType::EvaluatePackage;
            else
                return result;

            result.mTarget = std::string(token.substr(0, separator));
            result.mSource.clear();
            return result;
        }

        struct ConditionalFrame
        {
            bool mAllClosedBranchesShowBarter = true;
            bool mCurrentBranchShowsBarter = false;
            bool mHasElse = false;
            bool mSawElse = false;
        };
    }

    Esm4ResultScript parseEsm4ResultScript(std::string_view source)
    {
        Esm4ResultScript result;
        std::vector<ConditionalFrame> conditionals;
        bool rootShowsBarter = false;
        bool hasTopLevelShowBarter = false;

        std::istringstream stream{ std::string(source) };
        for (std::string storage; std::getline(stream, storage);)
        {
            std::string_view line = trim(storage);
            if (const std::size_t comment = line.find(';'); comment != std::string_view::npos)
                line = trim(line.substr(0, comment));
            if (line.empty())
                continue;

            if (isControlKeyword(line, "if", true))
            {
                conditionals.emplace_back();
                continue;
            }
            if (isControlKeyword(line, "elseif", true))
            {
                if (conditionals.empty() || conditionals.back().mSawElse)
                {
                    result.mMalformedControlFlow = true;
                    continue;
                }
                ConditionalFrame& frame = conditionals.back();
                frame.mAllClosedBranchesShowBarter &= frame.mCurrentBranchShowsBarter;
                frame.mCurrentBranchShowsBarter = false;
                continue;
            }
            if (isControlKeyword(line, "else"))
            {
                if (conditionals.empty() || conditionals.back().mSawElse)
                {
                    result.mMalformedControlFlow = true;
                    continue;
                }
                ConditionalFrame& frame = conditionals.back();
                frame.mAllClosedBranchesShowBarter &= frame.mCurrentBranchShowsBarter;
                frame.mCurrentBranchShowsBarter = false;
                frame.mHasElse = true;
                frame.mSawElse = true;
                continue;
            }
            if (isControlKeyword(line, "endif"))
            {
                if (conditionals.empty())
                {
                    result.mMalformedControlFlow = true;
                    continue;
                }
                ConditionalFrame frame = conditionals.back();
                conditionals.pop_back();
                const bool everyBranchShowsBarter
                    = frame.mHasElse && frame.mAllClosedBranchesShowBarter && frame.mCurrentBranchShowsBarter;
                if (everyBranchShowsBarter)
                {
                    if (conditionals.empty())
                        rootShowsBarter = true;
                    else
                        conditionals.back().mCurrentBranchShowsBarter = true;
                }
                continue;
            }

            const Esm4ResultCommand command = parseTopLevelCommand(line);
            if (!conditionals.empty())
            {
                if (command.mType == Esm4ResultCommandType::ShowBarterMenu)
                    conditionals.back().mCurrentBranchShowsBarter = true;
                else
                    ++result.mSkippedConditionalCommands;
                continue;
            }

            if (command.mType == Esm4ResultCommandType::ShowBarterMenu)
            {
                rootShowsBarter = true;
                hasTopLevelShowBarter = true;
            }
            result.mCommands.push_back(command);
        }

        if (!conditionals.empty())
            result.mMalformedControlFlow = true;
        if (rootShowsBarter && !hasTopLevelShowBarter && !result.mMalformedControlFlow)
            result.mCommands.push_back({ Esm4ResultCommandType::ShowBarterMenu, {}, {} });
        return result;
    }
}

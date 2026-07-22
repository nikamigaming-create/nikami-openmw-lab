#ifndef OPENMW_MWDIALOGUE_ESM4RESULTSCRIPT_H
#define OPENMW_MWDIALOGUE_ESM4RESULTSCRIPT_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace MWDialogue
{
    enum class Esm4ResultCommandType
    {
        Quest,
        ShowBarterMenu,
        Enable,
        Disable,
        Unlock,
        EvaluatePackage,
    };

    struct Esm4ResultCommand
    {
        Esm4ResultCommandType mType = Esm4ResultCommandType::Quest;
        std::string mTarget;
        std::string mSource;
    };

    struct Esm4ResultScript
    {
        std::vector<Esm4ResultCommand> mCommands;
        std::size_t mSkippedConditionalCommands = 0;
        bool mMalformedControlFlow = false;
    };

    // Extract commands that are safe to execute without guessing the value of
    // an unsupported script condition. Commands at the top level are retained,
    // while conditional quest source is kept intact for the native quest runtime.
    // ShowBarterMenu is also retained when every branch of a complete if/else
    // tree opens barter (the pattern used by retail Chet dialogue).
    Esm4ResultScript parseEsm4ResultScript(std::string_view source);
}

#endif

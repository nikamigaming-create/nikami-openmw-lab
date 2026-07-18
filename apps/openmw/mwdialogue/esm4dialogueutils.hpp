#ifndef OPENMW_MWDIALOGUE_ESM4DIALOGUEUTILS_H
#define OPENMW_MWDIALOGUE_ESM4DIALOGUEUTILS_H

#include <string_view>

#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>

namespace MWDialogue
{
    inline std::string_view getEsm4DialoguePrompt(
        const ESM4::Dialogue& dialogue, const ESM4::DialogInfo& info)
    {
        if (!info.mPrompt.empty())
            return info.mPrompt;
        if (!dialogue.mTopicName.empty())
            return dialogue.mTopicName;
        return dialogue.mEditorId;
    }
}

#endif

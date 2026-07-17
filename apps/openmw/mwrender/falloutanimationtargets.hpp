#ifndef GAME_RENDER_FALLOUTANIMATIONTARGETS_H
#define GAME_RENDER_FALLOUTANIMATIONTARGETS_H

#include <string_view>

namespace MWRender
{
    inline bool isFonvOptionalVisualControllerTarget(std::string_view lowerTarget)
    {
        // These targets are authored into shared KFs for model variants which are
        // not necessarily present on the current actor. Keep this list positive
        // and evidence-based: an unknown missing target must remain a hard error.
        return lowerTarget.starts_with("##") || lowerTarget == "eyes" || lowerTarget == "eyesblue"
            || lowerTarget == "eyesoneblue" || lowerTarget.starts_with("projectilenode_")
            || lowerTarget == "screenstatic:0" || lowerTarget == "voicebox_talk:0"
            // Shared creature KFs also animate optional model-variant geometry. These exact names are backed by
            // the retail assets: Super Mutant two-hand automatic KFs own AssualtCarbine, Protectron talking owns
            // Dome's SELF_ILLUM property channel, and Mister Gutsy melee KFs own the detachable buzz-saw blade.
            || lowerTarget == "assualtcarbine" || lowerTarget == "dome" || lowerTarget == "buzzsawblad";
    }

    inline bool isFonvRequiredSkeletonControllerTarget(std::string_view lowerTarget)
    {
        return !isFonvOptionalVisualControllerTarget(lowerTarget);
    }

    inline std::string_view getFonvExactDuplicateControllerTarget(std::string_view lowerTarget)
    {
        // Both retail Securitron get-up KFs author Spin1 and Spine1 with byte-equivalent transform channels. The
        // skeleton has Spine1, Spin01 and Spin02 but no Spin1. This is a duplicate track, not an alias: it may be
        // skipped only when the canonical Spine1 track is also authored by the same KF.
        if (lowerTarget == "bip01 spin1")
            return "bip01 spine1";
        return {};
    }
}

#endif // GAME_RENDER_FALLOUTANIMATIONTARGETS_H

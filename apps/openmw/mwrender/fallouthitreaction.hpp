#ifndef GAME_RENDER_FALLOUTHITREACTION_H
#define GAME_RENDER_FALLOUTHITREACTION_H

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace MWRender
{
    inline constexpr std::string_view FonvHitReactionSemanticGroup = "hit1";
    inline constexpr std::string_view FonvCreatureHitReactionSemanticGroup = FonvHitReactionSemanticGroup;
    inline constexpr std::string_view FonvNpcHitReactionSemanticGroup = FonvHitReactionSemanticGroup;
    inline constexpr std::string_view FonvNpcHitReactionSource
        = "meshes/characters/_male/idleanims/mt_hittorso.kf";
    inline constexpr std::string_view FonvNpcHitReactionSkeleton = "meshes/characters/_male/skeleton.nif";

    enum class FonvNpcHitReactionResolution
    {
        NotApplicable,
        AlreadyBound,
        BindExact,
        MissingSource,
        IncompatibleSkeleton,
    };

    inline bool shouldClearHitRecoveryDuringActiveAction(
        bool isNpc, bool isFonvCreature, bool hasBoundHitReaction)
    {
        return !isNpc && (!isFonvCreature || !hasBoundHitReaction);
    }

    inline std::string normalizeFonvHitReactionPath(std::string_view path)
    {
        std::string normalized(path);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
            return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
        });
        return normalized;
    }

    inline FonvNpcHitReactionResolution resolveFonvNpcHitReaction(bool isStrictFonvNpc,
        std::string_view baseModel, std::string_view selectedSource, bool exactSourceExists)
    {
        if (!isStrictFonvNpc)
            return FonvNpcHitReactionResolution::NotApplicable;
        if (normalizeFonvHitReactionPath(baseModel) != FonvNpcHitReactionSkeleton)
            return FonvNpcHitReactionResolution::IncompatibleSkeleton;
        if (normalizeFonvHitReactionPath(selectedSource) == FonvNpcHitReactionSource)
            return FonvNpcHitReactionResolution::AlreadyBound;
        return exactSourceExists ? FonvNpcHitReactionResolution::BindExact
                                 : FonvNpcHitReactionResolution::MissingSource;
    }

    inline bool isPreparedFonvNpcHitReaction(std::string_view selectedSource, unsigned int controllerMask)
    {
        return controllerMask != 0 && normalizeFonvHitReactionPath(selectedSource) == FonvNpcHitReactionSource;
    }

    inline std::string normalizeFonvCreatureAnimationDirectory(std::string_view animationDirectory)
    {
        std::string directory = normalizeFonvHitReactionPath(animationDirectory);
        while (!directory.empty() && directory.back() == '/')
            directory.pop_back();
        return directory;
    }

    inline bool isAuditedFonvCreatureHitReactionDirectory(std::string_view animationDirectory)
    {
        static constexpr std::array<std::string_view, 6> auditedDirectories = {
            "meshes/creatures/nvsecuritron",
            "meshes/creatures/dog",
            "meshes/creatures/nvbighorner",
            "meshes/creatures/nvmantis",
            "meshes/creatures/radscorpion",
            "meshes/creatures/nvgecko",
        };
        const std::string directory = normalizeFonvCreatureAnimationDirectory(animationDirectory);
        return std::find(auditedDirectories.begin(), auditedDirectories.end(), directory) != auditedDirectories.end();
    }

    inline std::array<std::string, 7> getFonvCreatureHitReactionCandidates(std::string_view animationDirectory)
    {
        std::string directory = normalizeFonvCreatureAnimationDirectory(animationDirectory);
        if (!isAuditedFonvCreatureHitReactionDirectory(directory))
            return {};
        directory.push_back('/');

        static constexpr std::array<std::string_view, 7> relativeCandidates = {
            "idleanims/hitreaction_torso.kf",
            "idleanims/hitreaction_head.kf",
            "idleanims/specialidle_hittorso.kf",
            "idleanims/specialidle_hithead.kf",
            "idleanims/mtspecialidle_hithead.kf",
            "recoil.kf",
            "h2hrecoil.kf",
        };

        std::array<std::string, relativeCandidates.size()> result;
        std::transform(relativeCandidates.begin(), relativeCandidates.end(), result.begin(),
            [&](std::string_view candidate) { return directory + std::string(candidate); });
        return result;
    }

    template <class Exists>
    std::string resolveFonvCreatureHitReaction(std::string_view animationDirectory, Exists&& exists)
    {
        for (std::string& candidate : getFonvCreatureHitReactionCandidates(animationDirectory))
        {
            if (!candidate.empty() && exists(candidate))
                return std::move(candidate);
        }
        return {};
    }
}

#endif // GAME_RENDER_FALLOUTHITREACTION_H

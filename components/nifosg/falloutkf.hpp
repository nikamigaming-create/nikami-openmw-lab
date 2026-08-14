#ifndef OPENMW_COMPONENTS_NIFOSG_FALLOUTKF_HPP
#define OPENMW_COMPONENTS_NIFOSG_FALLOUTKF_HPP

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include <components/sceneutil/textkeymap.hpp>

namespace NifOsg
{
    inline std::string getFalloutKfStem(std::string_view filename)
    {
        std::string stem(filename);
        const std::size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos)
            stem.erase(0, slash + 1);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos)
            stem.resize(dot);
        for (char& c : stem)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
        }
        return stem;
    }

    /// Map a Fallout KF filename to the movement group requested by CharacterController.
    /// Swimming clips must remain disjoint from land locomotion because animation sources are
    /// searched in reverse insertion order and creature directories are discovered recursively.
    inline std::string_view getFalloutKfMovementGroup(std::string_view filename)
    {
        const std::string stem = getFalloutKfStem(filename);
        const auto endsWith = [&](std::string_view suffix) { return stem.ends_with(suffix); };

        if (stem == "swimidle")
            return "idleswim";
        if (stem.starts_with("swim"))
        {
            if (endsWith("turnleft"))
                return "swimturnleft";
            if (endsWith("turnright"))
                return "swimturnright";
            if (endsWith("fastforward") || endsWith("runforward"))
                return "swimrunforward";
            if (endsWith("fastbackward") || endsWith("runbackward") || endsWith("runback"))
                return "swimrunback";
            if (endsWith("fastleft") || endsWith("runleft"))
                return "swimrunleft";
            if (endsWith("fastright") || endsWith("runright"))
                return "swimrunright";
            if (endsWith("forward"))
                return "swimwalkforward";
            if (endsWith("backward") || endsWith("walkback"))
                return "swimwalkback";
            if (endsWith("left"))
                return "swimwalkleft";
            if (endsWith("right"))
                return "swimwalkright";
            return {};
        }

        if (stem == "mtturnleft" || endsWith("turnleft"))
            return "turnleft";
        if (stem == "mtturnright" || endsWith("turnright"))
            return "turnright";
        if (stem == "mtforward")
            return "walkforward";
        // The retail dog rig misspells this filename, while its sequence is correctly named Backward.
        if (stem == "mtbackward" || stem == "mtbackrward")
            return "walkback";
        if (stem == "mtleft")
            return "walkleft";
        if (stem == "mtright")
            return "walkright";
        if (endsWith("fastforward") || endsWith("runforward"))
            return "runforward";
        if (endsWith("fastbackward") || endsWith("runbackward"))
            return "runback";
        if (endsWith("fastleft") || endsWith("runleft"))
            return "runleft";
        if (endsWith("fastright") || endsWith("runright"))
            return "runright";
        if (endsWith("forward") || endsWith("walkforward"))
            return "walkforward";
        if (endsWith("backward") || endsWith("walkbackward"))
            return "walkback";
        if (endsWith("left") || endsWith("walkleft"))
            return "walkleft";
        if (endsWith("right") || endsWith("walkright"))
            return "walkright";
        return {};
    }

    inline std::vector<std::string_view> getFalloutKfLoopGroups(std::string_view filename)
    {
        const std::string stem = getFalloutKfStem(filename);
        if (stem.find("flyaway") != std::string::npos)
            return { "idle", "idle2", "flyforward", "walkforward" };
        if (stem.find("specialidle") != std::string::npos)
            return { "idle2", "idle" };
        if (stem == "2hrcrouch")
            return { "kneel" };
        if (stem == "floorsleepdynamicidle")
            return { "prone" };
        if (stem == "talk_handsatside_moving")
            return { "talk" };
        if (stem == "wavehello")
            return { "wave" };
        // Directional words on action clips describe action variants, not locomotion.
        if (stem.find("attack") != std::string::npos || stem.find("reload") != std::string::npos
            || stem.find("equip") != std::string::npos)
            return {};
        if (const std::string_view movement = getFalloutKfMovementGroup(stem); !movement.empty())
            return { movement };
        if (stem == "mtidle" || stem == "pamtidle" || stem == "talk_handsatside_still2"
            || stem == "2hrloiter" || stem == "2hrloiteronehanded"
            || stem == "3rdp_specialidle_1hmidlela" || stem == "3rdp_specialidle_1hmidlelb"
            || stem == "dlcanch1hpistolpose" || stem.ends_with("idle"))
            return { "idle" };
        return {};
    }

    inline void addFalloutKfLoopingTextKeys(
        SceneUtil::TextKeyMap& textKeys, float start, float stop, std::string_view group)
    {
        textKeys.emplace(start, std::string(group) + ": start");
        textKeys.emplace(start, std::string(group) + ": loop start");
        textKeys.emplace(stop, std::string(group) + ": loop stop");
        textKeys.emplace(stop, std::string(group) + ": stop");
    }

    /// Synthesize OpenMW groups over the complete authored NiControllerSequence interval.
    inline bool synthesizeFalloutKfTextKeys(
        std::string_view filename, float sequenceStart, float sequenceStop, SceneUtil::TextKeyMap& textKeys)
    {
        const std::vector<std::string_view> groups = getFalloutKfLoopGroups(filename);
        if (groups.empty())
            return false;

        const float start = std::isfinite(sequenceStart) ? sequenceStart : 0.f;
        float stop = std::isfinite(sequenceStop) ? sequenceStop : start;
        if (stop <= start)
            stop = start + 1.f;

        bool changed = false;
        for (std::string_view group : groups)
        {
            if (textKeys.hasGroupStart(group))
                continue;
            addFalloutKfLoopingTextKeys(textKeys, start, stop, group);
            changed = true;
        }
        return changed;
    }

    /// A selected creature hit KF is appended after the ordinary animation sources. Fallout's
    /// SpecialIdle filename synthesis gives that hit clip idle/idle2 aliases, which would otherwise
    /// make reverse source lookup choose the short hit reaction for ordinary idle playback.
    inline bool isolateFalloutCreatureHitReactionTextKeys(
        SceneUtil::TextKeyMap& textKeys, std::string_view semanticGroup)
    {
        if (semanticGroup != "hit1")
            return false;

        const bool removedIdle = textKeys.eraseGroup("idle");
        const bool removedIdle2 = textKeys.eraseGroup("idle2");
        return removedIdle || removedIdle2;
    }
}

#endif // OPENMW_COMPONENTS_NIFOSG_FALLOUTKF_HPP

#ifndef OPENMW_MWMECHANICS_FALLOUTACTORSTATE_H
#define OPENMW_MWMECHANICS_FALLOUTACTORSTATE_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <components/esm/formid.hpp>
#include <components/esm4/actor.hpp>

namespace MWWorld
{
    class FalloutPlayerRuntimeState;
    class Ptr;
}

namespace MWMechanics
{
    enum class FalloutActorValueOperation : std::uint8_t
    {
        Set,
        Mod,
        Restore,
    };

    enum class FalloutActorFlag : std::uint32_t
    {
        Unconscious = 0x01,
        Restrained = 0x02,
        PlayerTeammate = 0x04,
        IgnoreCrime = 0x08,
        Ghost = 0x10,
        IgnoreFriendlyHits = 0x20,
    };

    struct FalloutFactionMembership
    {
        bool mMember = false;
        std::int8_t mRank = -1;
    };

    /// Resolve Creation Engine actor-value names and the aliases used by Fallout 3/FNV quest source.
    [[nodiscard]] std::optional<std::uint8_t> resolveFalloutActorValue(std::string_view name);

    /// Read or mutate a live actor value. Passing playerState enables the exact mutable Player payload for
    /// health, AP, XP, SPECIAL, skills, and Karma; all other values remain per-reference CreatureStats state.
    [[nodiscard]] std::optional<float> getFalloutActorValue(const MWWorld::Ptr& actor, std::uint8_t actorValue,
        const MWWorld::FalloutPlayerRuntimeState* playerState = nullptr);
    [[nodiscard]] bool applyFalloutActorValue(const MWWorld::Ptr& actor, std::uint8_t actorValue,
        FalloutActorValueOperation operation, float value,
        MWWorld::FalloutPlayerRuntimeState* playerState = nullptr);

    /// Merge authored NPC_/CREA faction membership with per-reference AddToFaction/RemoveFromFaction overrides.
    [[nodiscard]] std::vector<ESM4::ActorFaction> getFalloutActorFactions(const MWWorld::Ptr& actor,
        const MWWorld::FalloutPlayerRuntimeState* playerState = nullptr);
    [[nodiscard]] FalloutFactionMembership getFalloutFactionMembership(const MWWorld::Ptr& actor,
        ESM::FormId faction, const MWWorld::FalloutPlayerRuntimeState* playerState = nullptr);
    [[nodiscard]] bool setFalloutActorFaction(
        const MWWorld::Ptr& actor, ESM::FormId faction, std::optional<int> rank);
    [[nodiscard]] bool getFalloutActorFlag(const MWWorld::Ptr& actor, FalloutActorFlag flag);
    [[nodiscard]] bool setFalloutActorFlag(
        const MWWorld::Ptr& actor, FalloutActorFlag flag, bool enabled);
}

#endif

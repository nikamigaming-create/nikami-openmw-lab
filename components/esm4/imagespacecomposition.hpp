#ifndef OPENMW_COMPONENTS_ESM4_IMAGESPACECOMPOSITION_H
#define OPENMW_COMPONENTS_ESM4_IMAGESPACECOMPOSITION_H

#include "loadimad.hpp"
#include "loadimgs.hpp"

#include <components/esm/formid.hpp>

#include <array>
#include <vector>

namespace ESM4
{
    struct Cell;
    struct World;

    struct ImageSpaceModifierContribution
    {
        const ImageSpaceModifier* mModifier = nullptr;
        float mTime = 0.f;
        float mStrength = 0.f;
    };

    struct ComposedImageSpace
    {
        std::array<float, ImageSpace::sTraitCount> mTraits{};
        float mBlurRadius = 0.f;
        std::array<float, 4> mTint{ 1.f, 1.f, 1.f, 0.f };
        std::array<float, 4> mFade{ 0.f, 0.f, 0.f, 0.f };
    };

    /// IMAD key times are normalized over the authored DNAM duration.
    [[nodiscard]] float normalizeImageSpaceModifierTime(float elapsedSeconds, float durationSeconds);

    /// Compose an IMGS with active IMAD instances using the Gamebryo neutral-delta convention.
    /// A multiplier contributes (value - 1) * strength and an additive channel contributes value * strength.
    ComposedImageSpace composeImageSpace(
        const ImageSpace& base, const std::vector<ImageSpaceModifierContribution>& modifiers);

    /// Resolve the authored base image space for a cell. Interior CELL XCIM is authoritative;
    /// only an exterior cell without XCIM inherits its parent WRLD image space.
    [[nodiscard]] ESM::FormId resolveCellImageSpace(const Cell& cell, const World* parentWorld);
}

#endif

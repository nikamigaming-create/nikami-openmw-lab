#include "controlsstate.hpp"

#include "esmreader.hpp"
#include "esmwriter.hpp"

namespace ESM
{

    ControlsState::ControlsState()
        : mViewSwitchDisabled(false)
        , mControlsDisabled(false)
        , mJumpingDisabled(false)
        , mLookingDisabled(false)
        , mVanityModeDisabled(false)
        , mWeaponDrawingDisabled(false)
        , mSpellDrawingDisabled(false)
        , mMovementDisabled(false)
        , mInterfaceDisabled(false)
        , mSneakingDisabled(false)
        , mRolloverDisabled(false)
    {
    }

    void ControlsState::load(ESMReader& esm)
    {
        int flags;
        esm.getHNT(flags, "CFLG");

        mViewSwitchDisabled = flags & ViewSwitchDisabled;
        mControlsDisabled = flags & ControlsDisabled;
        mJumpingDisabled = flags & JumpingDisabled;
        mLookingDisabled = flags & LookingDisabled;
        mVanityModeDisabled = flags & VanityModeDisabled;
        mWeaponDrawingDisabled = flags & WeaponDrawingDisabled;
        mSpellDrawingDisabled = flags & SpellDrawingDisabled;
        mMovementDisabled = flags & MovementDisabled;
        mInterfaceDisabled = flags & InterfaceDisabled;
        mSneakingDisabled = flags & SneakingDisabled;
        mRolloverDisabled = flags & RolloverDisabled;
    }

    void ControlsState::save(ESMWriter& esm) const
    {
        int flags = 0;
        if (mViewSwitchDisabled)
            flags |= ViewSwitchDisabled;
        if (mControlsDisabled)
            flags |= ControlsDisabled;
        if (mJumpingDisabled)
            flags |= JumpingDisabled;
        if (mLookingDisabled)
            flags |= LookingDisabled;
        if (mVanityModeDisabled)
            flags |= VanityModeDisabled;
        if (mWeaponDrawingDisabled)
            flags |= WeaponDrawingDisabled;
        if (mSpellDrawingDisabled)
            flags |= SpellDrawingDisabled;
        if (mMovementDisabled)
            flags |= MovementDisabled;
        if (mInterfaceDisabled)
            flags |= InterfaceDisabled;
        if (mSneakingDisabled)
            flags |= SneakingDisabled;
        if (mRolloverDisabled)
            flags |= RolloverDisabled;

        esm.writeHNT("CFLG", flags);
    }

}

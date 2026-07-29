#ifndef OPENMW_ESM_CONTROLSSTATE_H
#define OPENMW_ESM_CONTROLSSTATE_H

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    // format 0, saved games only

    struct ControlsState
    {
        ControlsState();

        enum Flags
        {
            ViewSwitchDisabled = 0x1,
            ControlsDisabled = 0x4,
            JumpingDisabled = 0x1000,
            LookingDisabled = 0x2000,
            VanityModeDisabled = 0x4000,
            WeaponDrawingDisabled = 0x8000,
            SpellDrawingDisabled = 0x10000,
            MovementDisabled = 0x20000,
            InterfaceDisabled = 0x40000,
            SneakingDisabled = 0x80000,
            RolloverDisabled = 0x100000
        };

        bool mViewSwitchDisabled;
        bool mControlsDisabled;
        bool mJumpingDisabled;
        bool mLookingDisabled;
        bool mVanityModeDisabled;
        bool mWeaponDrawingDisabled;
        bool mSpellDrawingDisabled;
        bool mMovementDisabled;
        bool mInterfaceDisabled;
        bool mSneakingDisabled;
        bool mRolloverDisabled;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };
}

#endif

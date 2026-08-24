#ifndef OPENMW_MWGUI_GAMECONTENT_H
#define OPENMW_MWGUI_GAMECONTENT_H

namespace MWWorld
{
    enum class ESM4Game;
}

namespace MWGui
{
    bool usesFalloutNewVegasInterface(MWWorld::ESM4Game game) noexcept;

    /// Return whether the active loaded game should use the Fallout: New Vegas
    /// interface. Returns false before a World is available.
    bool usesFalloutNewVegasInterface() noexcept;
}

#endif // OPENMW_MWGUI_GAMECONTENT_H

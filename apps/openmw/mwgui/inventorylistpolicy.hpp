#ifndef OPENMW_MWGUI_INVENTORYLISTPOLICY_H
#define OPENMW_MWGUI_INVENTORYLISTPOLICY_H

#include <algorithm>

namespace MWGui
{
    [[nodiscard]] constexpr int normalizeInventoryControllerFocus(int focus, int itemCount) noexcept
    {
        if (itemCount <= 0)
            return -1;
        return std::clamp(focus, 0, itemCount - 1);
    }
}

#endif

#include "magiceffects.hpp"

#include "esmreader.hpp"
#include "esmwriter.hpp"

<<<<<<< HEAD
#include <components/esm3/loadmgef.hpp>

=======
>>>>>>> origin/main
namespace ESM
{

    void MagicEffects::save(ESMWriter& esm) const
    {
        for (const auto& [key, params] : mEffects)
        {
<<<<<<< HEAD
            esm.writeHNRefId("EFID", key);
=======
            esm.writeHNT("EFID", key);
>>>>>>> origin/main
            esm.writeHNT("BASE", params.first);
            esm.writeHNT("MODI", params.second);
        }
    }

    void MagicEffects::load(ESMReader& esm)
    {
        while (esm.isNextSub("EFID"))
        {
<<<<<<< HEAD
            RefId effectId;
            if (esm.getFormatVersion() <= MaxSerializeEffectRefIdFormatVersion)
            {
                int32_t id;
                esm.getHT(id);
                effectId = ESM::MagicEffect::indexToRefId(id);
            }
            else
                effectId = esm.getRefId();
            std::pair<int32_t, float> params;
=======
            int32_t id;
            std::pair<int32_t, float> params;
            esm.getHT(id);
>>>>>>> origin/main
            esm.getHNT(params.first, "BASE");
            if (esm.getFormatVersion() <= MaxClearModifiersFormatVersion)
                params.second = 0.f;
            else
                esm.getHNT(params.second, "MODI");
<<<<<<< HEAD
            mEffects.emplace(effectId, params);
=======
            mEffects.emplace(id, params);
>>>>>>> origin/main
        }
    }

}

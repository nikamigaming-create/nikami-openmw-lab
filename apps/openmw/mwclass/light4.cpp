#include "light4.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwworld/ptr.hpp"

#include <components/esm4/loadligh.hpp>

namespace MWClass
{
    bool shouldPlayEsm4LightLoop(const ESM4::Light& light) noexcept
    {
        return !light.mSound.isZeroOrUnset() && (light.mData.flags & ESM4::Light::OffDefault) == 0;
    }

    ESM4Light::ESM4Light()
        : MWWorld::RegisteredClass<ESM4Light, ESM4Base<ESM4::Light>>(ESM4::Light::sRecordId)
    {
    }

    void ESM4Light::insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
        MWPhysics::PhysicsSystem& physics) const
    {
        ESM4Base<ESM4::Light>::insertObject(ptr, model, rotation, physics);

        const ESM4::Light& light = *ptr.get<ESM4::Light>()->mBase;
        if (shouldPlayEsm4LightLoop(light))
        {
            MWBase::Environment::get().getSoundManager()->playSound3D(ptr, ESM::RefId(light.mSound), 1.f, 1.f,
                MWSound::Type::Sfx, MWSound::PlayMode::Loop);
        }
    }

    void ESM4Light::insertObjectRendering(
        const MWWorld::Ptr& ptr, const std::string& model, MWRender::RenderingInterface& renderingInterface) const
    {
        MWWorld::LiveCellRef<ESM4::Light>* ref = ptr.get<ESM4::Light>();

        // Insert even if model is empty, so that the light is added
        renderingInterface.getObjects().insertModel(ptr, model, !(ref->mBase->mData.flags & ESM4::Light::OffDefault));
    }
}

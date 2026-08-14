#ifndef OPENW_MWCLASS_LIGHT4
#define OPENW_MWCLASS_LIGHT4

#include "../mwworld/registeredclass.hpp"

#include "esm4base.hpp"

namespace MWClass
{
<<<<<<< HEAD
=======
    bool shouldPlayEsm4LightLoop(const ESM4::Light& light) noexcept;

>>>>>>> origin/main
    class ESM4Light : public MWWorld::RegisteredClass<ESM4Light, ESM4Base<ESM4::Light>>
    {
        friend MWWorld::RegisteredClass<ESM4Light, ESM4Base<ESM4::Light>>;

        ESM4Light();

    public:
<<<<<<< HEAD
=======
        void insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override;
>>>>>>> origin/main
        void insertObjectRendering(const MWWorld::Ptr& ptr, const std::string& model,
            MWRender::RenderingInterface& renderingInterface) const override;
        ///< Add reference into a cell for rendering
    };
}
#endif

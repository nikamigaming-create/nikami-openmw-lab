#include "additivelayer.hpp"

#include <osg/BlendFunc>
#include <osg/StateSet>

#include "myguirendermanager.hpp"

namespace MyGUIPlatform
{

    AdditiveLayer::AdditiveLayer()
    {
        mStateSet = new osg::StateSet;
        mStateSet->setAttributeAndModes(new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA, osg::BlendFunc::ONE));
    }

    AdditiveLayer::~AdditiveLayer()
    {
        // defined in .cpp file since we can't delete incomplete types
    }

    void AdditiveLayer::renderToTarget(MyGUI::IRenderTarget* target, bool update)
    {
        auto* injectableTarget = dynamic_cast<StateInjectableRenderTarget*>(target);

        if (injectableTarget)
            injectableTarget->setInjectState(mStateSet.get());

        MyGUI::OverlappedLayer::renderToTarget(target, update);

        if (injectableTarget)
            injectableTarget->setInjectState(nullptr);
    }

}

#include "viewer.hpp"

#include <osg/BufferObject>
#include <osg/Geode>
#include <osg/GL>
#include <osg/NodeVisitor>
#include <osg/StateAttribute>
#include <osg/Texture2DArray>
#include <osg/VertexArrayState>
#include <osgViewer/Renderer>

#include <components/debug/gldebug.hpp>

#include <components/misc/callbackmanager.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/vr/layer.hpp>
#include <components/vr/session.hpp>
#include <components/vr/swapchain.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/trackingtransform.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <sys/system_properties.h>
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER GL_ARRAY_BUFFER_ARB
#endif

#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING GL_ARRAY_BUFFER_BINDING_ARB
#endif

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER GL_READ_FRAMEBUFFER_EXT
#endif

#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif

namespace VR
{
    namespace
    {
        int sAndroidVrBlitFrame = 0;
#if defined(__ANDROID__)
        GLuint sAndroidMenuTexture = 0;
        std::vector<unsigned char> sAndroidMenuPixels;
        int sAndroidMenuPixelsWidth = 0;
        int sAndroidMenuPixelsHeight = 0;

        bool androidSystemPropertyEnabled(const char* name)
        {
            char value[PROP_VALUE_MAX] = {};
            const int length = __system_property_get(name, value);
            if (length <= 0)
                return false;

            const std::string_view text(value, static_cast<size_t>(length));
            return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES";
        }
#endif

        bool shouldCaptureAndroidVrProof()
        {
#if defined(__ANDROID__)
            if (!androidSystemPropertyEnabled("debug.openmw.vr.proof"))
                return false;
#else
            return false;
#endif

            return sAndroidVrBlitFrame == 60 || sAndroidVrBlitFrame == 180 || sAndroidVrBlitFrame == 500
                || sAndroidVrBlitFrame == 1000 || sAndroidVrBlitFrame == 1800 || sAndroidVrBlitFrame == 3000
                || sAndroidVrBlitFrame == 6000 || sAndroidVrBlitFrame == 9000;
        }

        void writeAndroidVrFramebufferProof(const char* label, int eye, int width, int height)
        {
            if (width <= 0 || height <= 0)
                return;

            ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
            std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
            ::glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            const GLenum readError = ::glGetError();

            uint64_t sumR = 0;
            uint64_t sumG = 0;
            uint64_t sumB = 0;
            uint64_t nonBlack = 0;
            for (size_t offset = 0; offset + 2 < pixels.size(); offset += 3)
            {
                const auto r = pixels[offset + 0];
                const auto g = pixels[offset + 1];
                const auto b = pixels[offset + 2];
                sumR += r;
                sumG += g;
                sumB += b;
                if ((r | g | b) != 0)
                    ++nonBlack;
            }

            const auto pixelsCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
            const std::string path = "/sdcard/OpenMWVR/files/config/openmw_vr_" + std::string(label)
                + "_eye" + std::to_string(eye) + "_frame" + std::to_string(sAndroidVrBlitFrame) + ".ppm";

            bool wrote = false;
            std::ofstream stream(path, std::ios::binary);
            if (stream)
            {
                stream << "P6\n" << width << " " << height << "\n255\n";
                stream.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
                wrote = static_cast<bool>(stream);
            }

            Log(Debug::Warning) << "Android VR " << label << " framebuffer proof eye=" << eye
                                << " frame=" << sAndroidVrBlitFrame << " size=" << width << "x" << height
                                << " avg=(" << (sumR / pixelsCount) << "," << (sumG / pixelsCount) << ","
                                << (sumB / pixelsCount) << ") nonBlack=" << nonBlack << "/" << pixelsCount
                                << " glReadPixelsError=0x" << std::hex << readError << std::dec
                                << " file=" << (wrote ? "ok" : "failed") << " path=" << path;
        }

#if defined(__ANDROID__)
        class AndroidVrSceneAuditVisitor : public osg::NodeVisitor
        {
        public:
            AndroidVrSceneAuditVisitor()
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                ++mNodes;
                if (mSamples < 16)
                {
                    const auto& name = node.getName();
                    if (!name.empty())
                    {
                        mSampleText += " node=\"";
                        mSampleText += name;
                        mSampleText += "\" mask=0x";
                        char buffer[16] = {};
                        std::snprintf(buffer, sizeof(buffer), "%x", node.getNodeMask());
                        mSampleText += buffer;
                    }
                }
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                ++mNodes;
                mDrawables += geode.getNumDrawables();
                if (mSamples < 16)
                {
                    mSampleText += " geode=\"";
                    mSampleText += geode.getName();
                    mSampleText += "\" mask=0x";
                    char buffer[16] = {};
                    std::snprintf(buffer, sizeof(buffer), "%x", geode.getNodeMask());
                    mSampleText += buffer;
                    mSampleText += " drawables=";
                    mSampleText += std::to_string(geode.getNumDrawables());
                    ++mSamples;
                }
                traverse(geode);
            }

            size_t mNodes = 0;
            size_t mDrawables = 0;
            size_t mSamples = 0;
            std::string mSampleText;
        };

        void logAndroidVrSceneAudit(osgViewer::Viewer* viewer)
        {
            static int sLastAuditFrame = -1;
            if (!shouldCaptureAndroidVrProof() || sLastAuditFrame == sAndroidVrBlitFrame || !viewer
                || !viewer->getSceneData() || !viewer->getCamera())
                return;
            sLastAuditFrame = sAndroidVrBlitFrame;

            AndroidVrSceneAuditVisitor audit;
            audit.setTraversalMask(viewer->getCamera()->getCullMask());
            viewer->getSceneData()->accept(audit);
            AndroidVrSceneAuditVisitor allAudit;
            allAudit.setTraversalMask(~0u);
            viewer->getSceneData()->accept(allAudit);

            const auto bound = viewer->getSceneData()->getBound();
            const auto center = bound.center();
            Log(Debug::Warning) << "Android VR scene audit frame=" << sAndroidVrBlitFrame
                                << " cullMask=0x" << std::hex << viewer->getCamera()->getCullMask() << std::dec
                                << " nodes=" << audit.mNodes << " drawables=" << audit.mDrawables
                                << " allNodes=" << allAudit.mNodes << " allDrawables=" << allAudit.mDrawables
                                << " boundCenter=(" << center.x() << "," << center.y() << "," << center.z()
                                << ") radius=" << bound.radius() << " visibleSamples:" << audit.mSampleText
                                << " allSamples:" << allAudit.mSampleText;
        }

        using FramebufferTexture2DFn = void (*) (GLenum, GLenum, GLenum, GLuint, GLint);

        void androidFramebufferTexture2D(
            osg::GLExtensions* gl, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
        {
            static auto framebufferTexture2D
                = reinterpret_cast<FramebufferTexture2DFn>(eglGetProcAddress("glFramebufferTexture2D"));
            if (framebufferTexture2D)
                framebufferTexture2D(target, attachment, textarget, texture, level);
            else
                gl->glFramebufferTexture2D(target, attachment, textarget, texture, level);
        }

        GLuint bindAndroidXrFramebuffer(osg::GLExtensions* gl, GLuint texture, GLenum textureTarget)
        {
            static std::map<GLuint, GLuint> framebuffers;
            auto& framebuffer = framebuffers[texture];
            if (!framebuffer)
                gl->glGenFramebuffers(1, &framebuffer);

            gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
            if (textureTarget == GL_TEXTURE_2D)
            {
                androidFramebufferTexture2D(
                    gl, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textureTarget, texture, 0);
            }
            else if (textureTarget == GL_TEXTURE_2D_ARRAY)
            {
                gl->glFramebufferTextureLayer(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, texture, 0, 0);
            }

            const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            gl->glDrawBuffers(1, &drawBuffer);
            return framebuffer;
        }
#endif
    }

#if defined(__ANDROID__)
}

extern "C" void openmw_android_vr_set_menu_texture(unsigned int texture)
{
    VR::sAndroidMenuTexture = static_cast<GLuint>(texture);
}

extern "C" void openmw_android_vr_update_menu_pixels(const unsigned char* pixels, int width, int height)
{
    static GLuint sComposedMenuTexture = 0;
    if (!pixels || width <= 0 || height <= 0)
        return;
    VR::sAndroidMenuPixels.assign(
        pixels, pixels + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
    VR::sAndroidMenuPixelsWidth = width;
    VR::sAndroidMenuPixelsHeight = height;

    GLint previousTexture = 0;
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    if (!sComposedMenuTexture)
    {
        ::glGenTextures(1, &sComposedMenuTexture);
        ::glBindTexture(GL_TEXTURE_2D, sComposedMenuTexture);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
        ::glBindTexture(GL_TEXTURE_2D, sComposedMenuTexture);

    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    VR::sAndroidMenuTexture = sComposedMenuTexture;
    ::glBindTexture(GL_TEXTURE_2D, previousTexture);
}

extern "C" const unsigned char* openmw_android_vr_get_menu_pixels(int* width, int* height)
{
    if (width)
        *width = VR::sAndroidMenuPixelsWidth;
    if (height)
        *height = VR::sAndroidMenuPixelsHeight;
    return VR::sAndroidMenuPixels.empty() ? nullptr : VR::sAndroidMenuPixels.data();
}

namespace VR
{
#endif

    static bool isNumber(const std::string& in)
    {
        for (auto c : in)
        {
            if (!std::isdigit(c))
            {
                return false;
            }
        }
        return true;
    }

    int parseResolution(std::string conf, int recommended, int max)
    {
        if (isNumber(conf))
        {
            int res = std::atoi(conf.c_str());
            if (res <= 0)
                return recommended;
            if (res > max)
                return max;
            return res;
        }
        conf = Misc::StringUtils::lowerCase(conf);
        if (conf == "auto" || conf == "recommended")
        {
            return recommended;
        }
        if (conf == "max")
        {
            return max;
        }
        return recommended;
    }

    struct UpdateViewCallback : public Stereo::Manager::UpdateViewCallback
    {
        UpdateViewCallback(Viewer* viewer)
            : mViewer(viewer){}

        //! Called during the update traversal of every frame to source updated stereo values.
        virtual void updateView(Stereo::View& left, Stereo::View& right) override { mViewer->updateView(left, right); }

        Viewer* mViewer;
    };

    struct SwapBuffersCallback : public osg::GraphicsContext::SwapCallback
    {
    public:
        SwapBuffersCallback(Viewer* viewer)
            : mViewer(viewer){}
        void swapBuffersImplementation(osg::GraphicsContext* gc) override { mViewer->swapBuffersCallback(gc); }

    private:
        Viewer* mViewer;
    };

    struct InitialDrawCallback : public Misc::CallbackManager::MwDrawCallback
    {
    public:
        InitialDrawCallback(Viewer* viewer)
            : mViewer(viewer)
        {
        }

        bool operator()(osg::RenderInfo& info, Misc::CallbackManager::View view) const override
        {
            mViewer->initialDrawCallback(info, view);
            return true;
        }

    private:
        Viewer* mViewer;
    };

    struct FinaldrawCallback : public Misc::CallbackManager::MwDrawCallback
    {
    public:
        FinaldrawCallback(Viewer* viewer)
            : mViewer(viewer)
        {
        }

        bool operator()(osg::RenderInfo& info, Misc::CallbackManager::View view) const override
        {
#if defined(ANDROID)
            static uint32_t sAndroidVrFinalWrapperLogs = 0;
            if (sAndroidVrFinalWrapperLogs < 80)
            {
                ++sAndroidVrFinalWrapperLogs;
                Log(Debug::Warning) << "Android VR final draw wrapper entered"
                                    << " view=" << static_cast<int>(view);
            }
#endif
            mViewer->finalDrawCallback(info, view);
            return true;
        }

    private:
        Viewer* mViewer;
    };

    Viewer* sViewer = nullptr;

    Viewer& Viewer::instance()
    {
        assert(sViewer);
        return *sViewer;
    }

    Viewer::Viewer(std::shared_ptr<VR::Session> session, osg::ref_ptr<osgViewer::Viewer> viewer)
        : mSession(session)
        , mViewer(viewer)
        , mSwapBuffersCallback(new SwapBuffersCallback(this))
        , mInitialDraw(new InitialDrawCallback(this))
        , mFinalDraw(new FinaldrawCallback(this))
        , mUpdateViewCallback(new UpdateViewCallback(this))
    {
        if (!sViewer)
            sViewer = this;
        else
            throw std::logic_error("Duplicated VR::Viewer singleton");

        // Read swapchain configs
        std::array<std::string, 2> xConfString;
        std::array<std::string, 2> yConfString;
        auto swapchainConfigs = VR::Session::instance().getRecommendedSwapchainConfig();
        xConfString[0] = Settings::Manager::getString("left eye resolution x", "VR");
        yConfString[0] = Settings::Manager::getString("left eye resolution y", "VR");
        xConfString[1] = Settings::Manager::getString("right eye resolution x", "VR");
        yConfString[1] = Settings::Manager::getString("right eye resolution y", "VR");

        // Instantiate swapchains for each view
        std::array<const char*, 2> viewNames = { "LeftEye", "RightEye" };
        for (unsigned i = 0; i < viewNames.size(); i++)
        {
            auto width
                = parseResolution(xConfString[i], swapchainConfigs[i].recommendedWidth, swapchainConfigs[i].maxWidth);
            auto height
                = parseResolution(yConfString[i], swapchainConfigs[i].recommendedHeight, swapchainConfigs[i].maxHeight);

            Log(Debug::Verbose) << viewNames[i] << " resolution: Recommended x=" << swapchainConfigs[i].recommendedWidth
                                << ", y=" << swapchainConfigs[i].recommendedHeight;
            Log(Debug::Verbose) << viewNames[i] << " resolution: Max x=" << swapchainConfigs[i].maxWidth
                                << ", y=" << swapchainConfigs[i].maxHeight;
            Log(Debug::Verbose) << viewNames[i] << " resolution: Selected x=" << width << ", y=" << height;

            mSubImages[i].width = width;
            mSubImages[i].height = height;
            mSubImages[i].x = mSubImages[i].y = 0;
        }

        // Determine samples and dimensions of framebuffers.
        mFramebufferWidth = mSubImages[0].width;
        mFramebufferHeight = mSubImages[0].height;

        if (mSubImages[0].width != mSubImages[1].width || mSubImages[0].height != mSubImages[1].height)
            Log(Debug::Warning) << "Warning: Eyes have differing resolutions. This case is not implemented";

        mViewer->getCamera()->setViewport(0, 0, mFramebufferWidth, mFramebufferHeight);

        // The gamma resolve framebuffer will be used to write the result of gamma post-processing.
        mGammaResolveFramebuffer = new osg::FrameBufferObject();
        mGammaResolveFramebuffer->setAttachment(osg::Camera::COLOR_BUFFER,
            osg::FrameBufferAttachment(new osg::RenderBuffer(
                mFramebufferWidth, mFramebufferHeight, SceneUtil::Color::colorInternalFormat(), 0)));

        mViewer->setReleaseContextAtEndOfFrameHint(false);
        mViewer->getCamera()->getGraphicsContext()->setSwapCallback(mSwapBuffersCallback);
        mViewer->getCamera()->setViewport(0, 0, mFramebufferWidth, mFramebufferHeight);
        Stereo::Manager::instance().overrideEyeResolution(osg::Vec2i(mFramebufferWidth, mFramebufferHeight));

        setupMirrorTexture();
        setupSwapchains();

        mProjectionLayer = std::make_shared<VR::ProjectionLayer>();
        for (uint32_t i = 0; i < 2; i++)
        {
            mProjectionLayer->views[i].subImage.index = 0;
            mProjectionLayer->views[i].subImage.width = mFramebufferWidth;
            mProjectionLayer->views[i].subImage.height = mFramebufferHeight;
            mProjectionLayer->views[i].subImage.x = 0;
            mProjectionLayer->views[i].subImage.y = 0;
            mProjectionLayer->views[i].colorSwapchain = mColorSwapchain[i];
            if (mSession->appShouldShareDepthInfo())
                mProjectionLayer->views[i].depthSwapchain = mDepthSwapchain[i];
        }
    }

    Viewer::~Viewer(void)
    {
        sViewer = nullptr;
    }

    static Viewer::MirrorTextureEye mirrorTextureEyeFromString(const std::string& str)
    {
        if (Misc::StringUtils::ciEqual(str, "left"))
            return Viewer::MirrorTextureEye::Left;
        if (Misc::StringUtils::ciEqual(str, "right"))
            return Viewer::MirrorTextureEye::Right;
        if (Misc::StringUtils::ciEqual(str, "both"))
            return Viewer::MirrorTextureEye::Both;
        return Viewer::MirrorTextureEye::Both;
    }

    void Viewer::configureCallbacks()
    {
        if (mCallbacksConfigured)
            return;

        // Give the main camera an initial draw callback that disables camera setup (we don't want it)
        Stereo::Manager::instance().setUpdateViewCallback(mUpdateViewCallback);
        Misc::CallbackManager::instance().addCallback(Misc::CallbackManager::DrawStage::Initial, mInitialDraw);
        Misc::CallbackManager::instance().addCallback(Misc::CallbackManager::DrawStage::Final, mFinalDraw);

        mCallbacksConfigured = true;
    }

    void Viewer::setupMirrorTexture()
    {
        mMirrorTextureEnabled = Settings::Manager::getBool("mirror texture", "VR");
        mMirrorTextureEye = mirrorTextureEyeFromString(Settings::Manager::getString("mirror texture eye", "VR"));
        mFlipMirrorTextureOrder = Settings::Manager::getBool("flip mirror texture order", "VR");

        mMirrorTextureViews.clear();
        if (mMirrorTextureEye == MirrorTextureEye::Left || mMirrorTextureEye == MirrorTextureEye::Both)
            mMirrorTextureViews.push_back(VR::Side_Left);
        if (mMirrorTextureEye == MirrorTextureEye::Right || mMirrorTextureEye == MirrorTextureEye::Both)
            mMirrorTextureViews.push_back(VR::Side_Right);
        if (mFlipMirrorTextureOrder)
            std::reverse(mMirrorTextureViews.begin(), mMirrorTextureViews.end());
    }

    void Viewer::processChangedSettings(const std::set<std::pair<std::string, std::string>>& changed)
    {
        bool mirrorTextureChanged = false;
        for (Settings::CategorySettingVector::const_iterator it = changed.begin(); it != changed.end(); ++it)
        {
            if (it->first == "VR" && it->second == "mirror texture")
            {
                mirrorTextureChanged = true;
            }
            if (it->first == "VR" && it->second == "mirror texture eye")
            {
                mirrorTextureChanged = true;
            }
            if (it->first == "VR" && it->second == "flip mirror texture order")
            {
                mirrorTextureChanged = true;
            }
        }

        if (mirrorTextureChanged)
            setupMirrorTexture();
    }

    void Viewer::insertLayer(std::shared_ptr<Layer> layer)
    {
        if (std::find(mLayers.begin(), mLayers.end(), layer) == mLayers.end())
            mLayers.push_back(layer);
    }

    void Viewer::removeLayer(std::shared_ptr<Layer> layer)
    {
        const auto previousSize = mLayers.size();
        mLayers.erase(std::remove(mLayers.begin(), mLayers.end(), layer), mLayers.end());
        if (previousSize == mLayers.size())
            Log(Debug::Warning) << "VR::Viewer::removeLayer() called, but no such layer existed";
    }

    osg::ref_ptr<osg::FrameBufferObject> Viewer::getFboForView(Stereo::Eye view)
    {
        osg::ref_ptr<osg::FrameBufferObject> fbo = nullptr;
        auto stereoFbo = Stereo::Manager::instance().multiviewFramebuffer();
        if (Stereo::getMultiview())
        {
            fbo = stereoFbo->multiviewFbo();
        }
        else
        {
            int i = view == Stereo::Eye::Left ? 0 : 1;
            fbo = stereoFbo->layerFbo(i);
        }

        return fbo;
    }

    void Viewer::submitDepthForView(osg::State& state, osg::FrameBufferObject* depthFbo, Stereo::Eye view)
    {
        if (!mSession->appShouldShareDepthInfo())
            return;

        auto stereoFbo = Stereo::Manager::instance().multiviewFramebuffer();

        if (Stereo::getMultiview())
        {
            // TODO: Should cache this, but the pp keeps remaking the depth fbo so i need a dirty/cleanup step too.
            auto foo = std::make_unique<Stereo::MultiviewFramebufferResolve>(
                depthFbo, stereoFbo->multiviewFbo(), GL_DEPTH_BUFFER_BIT);
            foo->resolveImplementation(state);
        }
        else
        {
            stereoFbo->layerFbo(view == Stereo::Eye::Left ? 0 : 1)
                ->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            depthFbo->apply(state, osg::FrameBufferObject::READ_FRAMEBUFFER);
            osg::GLExtensions* ext = state.get<osg::GLExtensions>();
            ext->glBlitFramebuffer(0, 0, mFramebufferWidth, mFramebufferHeight, 0, 0, mFramebufferWidth,
                mFramebufferHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
    }

    const VR::Frame& Viewer::currentUpdateFrame()
    {
        if (mReadyFrames.empty())
            throw std::logic_error("VR::Viewer::currentUpdateFrame() called outside of update");
        return mReadyFrames.back();
    }

    osg::ref_ptr<osg::FrameBufferObject> Viewer::getXrFramebuffer(uint32_t view, osg::State* state)
    {
        uint64_t colorImage = mColorSwapchain[view]->image()->glImage();
        uint64_t depthImage = 0;
        uint32_t textureTarget = mColorSwapchain[view]->textureTarget();

        if (mSession->appShouldShareDepthInfo())
            depthImage = mDepthSwapchain[view]->image()->glImage();

        auto it = mSwapchainFramebuffers.find(std::pair{ colorImage, depthImage });
        if (it == mSwapchainFramebuffers.end())
        {
            osg::ref_ptr<osg::FrameBufferObject> fbo = new osg::FrameBufferObject();

            auto colorAttachment = Stereo::createLayerAttachmentFromHandle(
                state, colorImage, textureTarget, mFramebufferWidth, mFramebufferHeight, view);
            fbo->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER, colorAttachment);

            if (depthImage != 0)
            {
                auto depthAttachment = Stereo::createLayerAttachmentFromHandle(
                    state, depthImage, textureTarget, mFramebufferWidth, mFramebufferHeight, view);
                fbo->setAttachment(osg::FrameBufferObject::BufferComponent::DEPTH_BUFFER, depthAttachment);
            }

            it = mSwapchainFramebuffers.emplace(std::pair{ colorImage, depthImage }, fbo).first;
        }
        return it->second;
    }

    void Viewer::blitXrFramebuffer(osg::State* state, int i)
    {
        auto* gl = osg::GLExtensions::Get(state->getContextID(), false);
#if defined(__ANDROID__)
        const bool androidEyeMenuProof = androidSystemPropertyEnabled("debug.openmw.vr.eye_menu_proof");
        const bool captureProof = shouldCaptureAndroidVrProof();
#else
        const bool androidEyeMenuProof = false;
        const bool captureProof = i == 0 && shouldCaptureAndroidVrProof();
#endif

        auto dst = getXrFramebuffer(i, state);
#if !defined(__ANDROID__)
        dst->apply(*state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
#endif

        auto width = mSubImages[i].width;
        auto height = mSubImages[i].height;
        bool flip = mColorSwapchain[i]->mustFlipVertical();

        uint32_t srcX0 = 0;
        uint32_t srcX1 = srcX0 + width;
        uint32_t srcY0 = flip ? height : 0;
        uint32_t srcY1 = flip ? 0 : height;
        uint32_t dstX0 = mSubImages[i].x;
        uint32_t dstX1 = dstX0 + width;
        uint32_t dstY0 = mSubImages[i].y;
        uint32_t dstY1 = dstY0 + height;

        static GLuint blitProgram = 0;
        static GLuint blitVbo = 0;
        static bool blitInitialized = false;

        if (!blitInitialized)
        {
            blitInitialized = true;
            const char* vsSource =
                "#version 100\n"
                "attribute vec2 aPosition;\n"
                "attribute vec2 aTexCoord;\n"
                "varying vec2 vTexCoord;\n"
                "void main() {\n"
                "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
                "    vTexCoord = aTexCoord;\n"
                "}\n";

            const char* fsSource =
                "#version 100\n"
                "precision highp float;\n"
                "varying vec2 vTexCoord;\n"
                "uniform sampler2D uTexture;\n"
                "uniform float uFlipY;\n"
                "void main() {\n"
                "    vec2 texCoord = vTexCoord;\n"
                "    if (uFlipY > 0.5) texCoord.y = 1.0 - texCoord.y;\n"
                "    vec4 color = texture2D(uTexture, texCoord);\n"
                "    gl_FragColor = vec4(color.rgb, 1.0);\n"
                "}\n";

            auto compileShader = [&](GLenum type, const char* source) -> GLuint {
                GLuint shader = gl->glCreateShader(type);
                gl->glShaderSource(shader, 1, &source, nullptr);
                gl->glCompileShader(shader);
                return shader;
            };

            GLuint vs = compileShader(GL_VERTEX_SHADER, vsSource);
            GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSource);

            blitProgram = gl->glCreateProgram();
            gl->glAttachShader(blitProgram, vs);
            gl->glAttachShader(blitProgram, fs);

            // Explicitly bind attribute locations so we don't conflict with OSG's use of 0/1, but we MUST use 0 for the draw call to work on GLES 2.0 Adreno drivers!
            gl->glBindAttribLocation(blitProgram, 0, "aPosition");
            gl->glBindAttribLocation(blitProgram, 1, "aTexCoord");

            gl->glLinkProgram(blitProgram);

            gl->glDeleteShader(vs);
            gl->glDeleteShader(fs);

            // In GLES 2.0 we don't need VAOs. Just create the VBO.
            gl->glGenBuffers(1, &blitVbo);
            gl->glBindBuffer(GL_ARRAY_BUFFER, blitVbo);
            float quad[] = {
                -1.0f, -1.0f, 0.0f, 0.0f,
                 1.0f, -1.0f, 1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f, 1.0f,
                 1.0f,  1.0f, 1.0f, 1.0f,
            };
            gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        osg::Texture2D* colorTex = Stereo::Manager::instance().multiviewFramebuffer()->layerColorBuffer(i);
        osg::Texture::TextureObject* texObj = colorTex ? colorTex->getTextureObject(state->getContextID()) : nullptr;

        if (texObj)
        {
#if defined(__ANDROID__)
            const GLuint xrTexture = static_cast<GLuint>(mColorSwapchain[i]->image()->glImage());
            const GLenum xrTextureTarget = static_cast<GLenum>(mColorSwapchain[i]->textureTarget());
            const GLuint rawXrFramebuffer = bindAndroidXrFramebuffer(gl, xrTexture, xrTextureTarget);
            const GLenum rawXrStatus = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
            if (captureProof)
                Log(Debug::Warning) << "Android VR raw XR draw target eye=" << i
                                    << " frame=" << sAndroidVrBlitFrame << " texture=" << xrTexture
                                    << " target=" << xrTextureTarget << " fbo=" << rawXrFramebuffer
                                    << " status=0x" << std::hex << rawXrStatus << std::dec;
#else
            dst->apply(*state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
#endif
            if (captureProof)
            {
                auto src = Stereo::Manager::instance().multiviewFramebuffer()->layerFbo(i);
                src->apply(*state, osg::FrameBufferObject::READ_FRAMEBUFFER);
                const GLenum srcStatus = gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
                Log(Debug::Warning) << "Android VR blit source proof setup eye=" << i
                                    << " frame=" << sAndroidVrBlitFrame << " texObj=" << texObj->id()
                                    << " readStatus=0x" << std::hex << srcStatus << std::dec;
                writeAndroidVrFramebufferProof("source", i, width, height);
#if defined(__ANDROID__)
                gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, rawXrFramebuffer);
#else
                dst->apply(*state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
#endif
            }

            // Save state
            GLint prevProgram = 0;
            ::glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);

            GLint prevActiveTex = 0;
            ::glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);

            gl->glActiveTexture(GL_TEXTURE0);
            GLint prevTex0 = 0;
            ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);

            GLint prevVbo = 0;
            ::glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevVbo);

            GLint prevAttr0Enabled = 0;
            gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prevAttr0Enabled);
            GLint prevSize0 = 4, prevType0 = GL_FLOAT, prevNormalized0 = 0, prevStride0 = 0;
            GLvoid* prevPointer0 = nullptr;
            gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &prevSize0);
            gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &prevType0);
            gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &prevNormalized0);
            gl->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &prevStride0);
            gl->glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &prevPointer0);

            GLint prevAttr1Enabled = 0;
            gl->glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prevAttr1Enabled);
            GLint prevSize1 = 4, prevType1 = GL_FLOAT, prevNormalized1 = 0, prevStride1 = 0;
            GLvoid* prevPointer1 = nullptr;
            gl->glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_SIZE, &prevSize1);
            gl->glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_TYPE, &prevType1);
            gl->glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &prevNormalized1);
            gl->glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &prevStride1);
            gl->glGetVertexAttribPointerv(1, GL_VERTEX_ATTRIB_ARRAY_POINTER, &prevPointer1);

            GLint prevViewport[4];
            ::glGetIntegerv(GL_VIEWPORT, prevViewport);

            GLboolean depthTestEnabled = ::glIsEnabled(GL_DEPTH_TEST);
            GLboolean cullFaceEnabled = ::glIsEnabled(GL_CULL_FACE);
            GLboolean blendEnabled = ::glIsEnabled(GL_BLEND);
            GLboolean scissorTestEnabled = ::glIsEnabled(GL_SCISSOR_TEST);
            GLint prevScissorBox[4];
            ::glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);

            GLboolean colorMask[4];
            ::glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

            gl->glUseProgram(blitProgram);

            // Bind VBO and attributes
            gl->glBindBuffer(GL_ARRAY_BUFFER, blitVbo);
            gl->glEnableVertexAttribArray(0);
            gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            gl->glEnableVertexAttribArray(1);
            gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            gl->glActiveTexture(GL_TEXTURE0);
            ::glBindTexture(GL_TEXTURE_2D, texObj->id());
            gl->glUniform1i(gl->glGetUniformLocation(blitProgram, "uTexture"), 0);
            gl->glUniform1f(gl->glGetUniformLocation(blitProgram, "uFlipY"), flip ? 1.0f : 0.0f);

            ::glViewport(dstX0, dstY0, width, height);

            ::glDisable(GL_DEPTH_TEST);
            ::glDisable(GL_CULL_FACE);
            ::glDisable(GL_BLEND);
            ::glDisable(GL_SCISSOR_TEST);
            ::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            ::glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            const GLenum drawError = ::glGetError();
#if defined(__ANDROID__)
            GLenum menuDrawError = GL_NO_ERROR;
            if (androidEyeMenuProof && sAndroidMenuTexture != 0)
            {
                ::glBindTexture(GL_TEXTURE_2D, sAndroidMenuTexture);
                gl->glUniform1f(gl->glGetUniformLocation(blitProgram, "uFlipY"), 0.0f);
                ::glViewport(static_cast<GLint>(width * 0.14f), static_cast<GLint>(height * 0.10f),
                    static_cast<GLsizei>(width * 0.72f), static_cast<GLsizei>(height * 0.78f));
                ::glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                menuDrawError = ::glGetError();
                ::glViewport(dstX0, dstY0, width, height);
            }
            if (captureProof)
                Log(Debug::Warning) << "Android VR direct XR menu overlay eye=" << i
                                    << " frame=" << sAndroidVrBlitFrame << " texture=" << sAndroidMenuTexture
                                    << " enabled=" << androidEyeMenuProof
                                    << " error=0x" << std::hex << menuDrawError << std::dec;
#endif
            if (captureProof)
            {
#if defined(__ANDROID__)
                gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, rawXrFramebuffer);
                const GLenum dstStatus = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
#else
                dst->apply(*state, osg::FrameBufferObject::READ_FRAMEBUFFER);
                const GLenum dstStatus = gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
#endif
                Log(Debug::Warning) << "Android VR blit destination proof setup eye=" << i
                                    << " frame=" << sAndroidVrBlitFrame << " drawError=0x" << std::hex
                                    << drawError << " readStatus=0x" << dstStatus << std::dec;
                writeAndroidVrFramebufferProof("destination", i, width, height);
#if defined(__ANDROID__)
                gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, rawXrFramebuffer);
#else
                dst->apply(*state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
#endif
            }

            // Restore VBO first because VertexAttribPointer is tied to the bound VBO!
            gl->glBindBuffer(GL_ARRAY_BUFFER, prevVbo);

            // Clean up attributes to exactly how they were, so OSG's cache remains perfectly in sync with driver state
            if (prevAttr0Enabled) gl->glEnableVertexAttribArray(0); else gl->glDisableVertexAttribArray(0);
            gl->glVertexAttribPointer(0, prevSize0, prevType0, prevNormalized0, prevStride0, prevPointer0);

            if (prevAttr1Enabled) gl->glEnableVertexAttribArray(1); else gl->glDisableVertexAttribArray(1);
            gl->glVertexAttribPointer(1, prevSize1, prevType1, prevNormalized1, prevStride1, prevPointer1);

            if (depthTestEnabled) ::glEnable(GL_DEPTH_TEST);
            if (cullFaceEnabled) ::glEnable(GL_CULL_FACE);
            if (blendEnabled) ::glEnable(GL_BLEND);
            if (scissorTestEnabled) ::glEnable(GL_SCISSOR_TEST);
            else ::glDisable(GL_SCISSOR_TEST);
            ::glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
            ::glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);

            ::glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
            gl->glBindBuffer(GL_ARRAY_BUFFER, prevVbo);

            gl->glActiveTexture(GL_TEXTURE0);
            ::glBindTexture(GL_TEXTURE_2D, prevTex0);
            gl->glActiveTexture(prevActiveTex);

            gl->glUseProgram(prevProgram);
        }
        else
        {
            Stereo::Manager::instance().multiviewFramebuffer()->layerFbo(i)->apply(
                *state, osg::FrameBufferObject::READ_FRAMEBUFFER);
            if (captureProof)
                writeAndroidVrFramebufferProof("source_blit", i, width, height);
            gl->glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (captureProof)
            {
                dst->apply(*state, osg::FrameBufferObject::READ_FRAMEBUFFER);
                writeAndroidVrFramebufferProof("destination_blit", i, width, height);
            }
        }

        if (mSession->appShouldShareDepthInfo())
        {
            Stereo::Manager::instance().multiviewFramebuffer()->layerFbo(i)->apply(
                *state, osg::FrameBufferObject::READ_FRAMEBUFFER);
            gl->glBlitFramebuffer(
                srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }

        gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    }

    void Viewer::blitMirrorTexture(osg::State* state, int i)
    {
        auto* gl = osg::GLExtensions::Get(state->getContextID(), false);

        auto* traits = SDLUtil::GraphicsWindowSDL2::findContext(*mViewer)->getTraits();
        int screenWidth = traits->width;
        int screenHeight = traits->height;

        // Compute the dimensions of each eye on the mirror texture.
        int dstWidth = screenWidth / mMirrorTextureViews.size();

        // Blit each eye
        // Which eye is blitted left/right is determined by which order left/right was added to mMirrorTextureViews
        int dstX = 0;
        gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
        Stereo::Manager::instance().multiviewFramebuffer()->layerFbo(i)->apply(
            *state, osg::FrameBufferObject::READ_FRAMEBUFFER);
        for (auto viewId : mMirrorTextureViews)
        {
            if (viewId == static_cast<unsigned int>(i))
                gl->glBlitFramebuffer(0, 0, mFramebufferWidth, mFramebufferHeight, dstX, 0, dstX + dstWidth,
                    screenHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            dstX += dstWidth;
        }
    }

    void Viewer::setupSwapchains()
    {
        for (int i : { 0, 1 })
        {
            mColorSwapchain[i] = VR::Session::instance().createSwapchain(mFramebufferWidth, mFramebufferHeight, 1, 1,
                VR::Swapchain::Attachment::Color, i == 0 ? "LeftEye" : "RightEye");
            if (mSession->appShouldShareDepthInfo())
            {
                // Depth support is buggy or just not supported on some runtimes and has to be guarded.
                try
                {
                    mDepthSwapchain[i] = VR::Session::instance().createSwapchain(mFramebufferWidth, mFramebufferHeight,
                        1, 1, VR::Swapchain::Attachment::DepthStencil, i == 0 ? "LeftEye" : "RightEye");
                }
                catch (std::exception& e)
                {
                    Log(Debug::Warning) << "XR_KHR_composition_layer_depth was enabled, but a depth attachment "
                                           "swapchain could not be created. Depth information will not be submitted: "
                                        << e.what();
                    mSession->setAppShouldShareDepthBuffer(false);
                    mDepthSwapchain[0] = mDepthSwapchain[1] = nullptr;
                }
            }
        }
    }

    void Viewer::blit(osg::RenderInfo& info)
    {
        auto* state = info.getState();
        auto* gl = osg::GLExtensions::Get(state->getContextID(), false);
        ++sAndroidVrBlitFrame;
        if (sAndroidVrBlitFrame <= 8 || shouldCaptureAndroidVrProof())
            Log(Debug::Warning) << "Android VR blit frame=" << sAndroidVrBlitFrame
                                << " shouldCapture=" << shouldCaptureAndroidVrProof();
#if defined(__ANDROID__)
        logAndroidVrSceneAudit(mViewer);
#endif

        for (auto i = 0; i < 2; i++)
        {
            mColorSwapchain[i]->beginFrame(state->getGraphicsContext());
            if (mSession->appShouldShareDepthInfo())
                mDepthSwapchain[i]->beginFrame(state->getGraphicsContext());

            if (mMirrorTextureEnabled)
                blitMirrorTexture(state, i);
            blitXrFramebuffer(state, i);

            mColorSwapchain[i]->endFrame(state->getGraphicsContext());
            if (mSession->appShouldShareDepthInfo())
                mDepthSwapchain[i]->endFrame(state->getGraphicsContext());
        }

        // Undo all framebuffer bindings we have done.
        gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    }

    void Viewer::initialDrawCallback(osg::RenderInfo& info, Misc::CallbackManager::View view)
    {
        // Should only activate on the first callback
        if (view == Misc::CallbackManager::View::Right)
            return;

        {
            std::scoped_lock lock(mMutex);
            mDrawFrame = mReadyFrames.front();
            mReadyFrames.pop();
        }
        VR::Session::instance().frameBeginRender(mDrawFrame);
    }

    void Viewer::finalDrawCallback(osg::RenderInfo& info, Misc::CallbackManager::View view)
    {
        // Should only activate on the last callback
        if (view == Misc::CallbackManager::View::Left)
            return;
        static uint32_t sAndroidVrFinalDrawLogs = 0;
        const bool frameRenderable = mDrawFrame.shouldSyncFrameLoop && mDrawFrame.shouldRender
            && mDrawFrame.predictedDisplayTime != 0;
        if (sAndroidVrFinalDrawLogs < 80 || shouldCaptureAndroidVrProof())
        {
            ++sAndroidVrFinalDrawLogs;
            Log(Debug::Warning) << "Android VR final draw view=Right nextBlitFrame=" << (sAndroidVrBlitFrame + 1)
                                << " shouldRender=" << mDrawFrame.shouldRender
                                << " shouldSync=" << mDrawFrame.shouldSyncFrameLoop
                                << " predictedDisplayTime=" << mDrawFrame.predictedDisplayTime;
        }
        if (!frameRenderable)
            return;

        VR::Session::instance().frameEnd(info, mDrawFrame);
        if (mDrawFrame.shouldRender)
        {
            blit(info);
        }
    }

    void Viewer::swapBuffersCallback(osg::GraphicsContext* gc)
    {
        VR::Session::instance().swapBuffers(gc, mDrawFrame);
    }

    void Viewer::newFrame()
    {
        auto frame = mSession->newFrame();
        mSession->frameBeginUpdate(frame);
        std::unique_lock<std::mutex> lock(mMutex);
        mReadyFrames.push(frame);
    }

    void Viewer::updateView(Stereo::View& left, Stereo::View& right)
    {
        newFrame();

        std::unique_lock<std::mutex> lock(mMutex);
        auto& frame = mReadyFrames.back();
        if (!frame.shouldSyncFrameLoop || !frame.shouldRender || frame.predictedDisplayTime == 0)
            return;

        auto referenceSpaceLocal = mSession->getReferenceSpace(VR::ReferenceSpace::Local);
        auto referenceSpaceView = mSession->getReferenceSpace(VR::ReferenceSpace::View);
        auto localViews
            = VR::Session::instance().locateViews(frame.predictedDisplayTime, *referenceSpaceLocal);
        auto views = VR::Session::instance().locateViews(frame.predictedDisplayTime, *referenceSpaceView);

        left = views[VR::Side_Left];
        right = views[VR::Side_Right];

        // Print view once to log, useful for debugging the views of headsets i do not posess.
        static bool havePrintedView = false;
        if (!havePrintedView)
        {
            havePrintedView = true;
            Log(Debug::Verbose) << "Left View: " << left;
            Log(Debug::Verbose) << "Right View: " << right;
        }

        std::shared_ptr<VR::ProjectionLayer> projectionLayer
            = std::make_shared<VR::ProjectionLayer>(*mProjectionLayer);

        projectionLayer->space = referenceSpaceLocal;
        for (uint32_t i = 0; i < 2; i++)
        {
            projectionLayer->views[i].view = localViews[i];
        }
        frame.layers.push_back(projectionLayer);
        if (!mLayers.empty())
            frame.layers.insert(frame.layers.end(), mLayers.begin(), mLayers.end());
    }
}

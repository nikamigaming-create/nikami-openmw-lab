#include "engine.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osg/ComputeBoundsVisitor>
#include <osg/Image>
#include <osg/NodeVisitor>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

// ## VR_PATCH BEGIN
#include "mwrender/camera.hpp"
#include "mwvr/vrgui.hpp"
#include "mwvr/vrinputmanager.hpp"
#include "mwvr/vranimation.hpp"
#include <components/misc/callbackmanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vr/session.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/viewer.hpp>
#include <components/vr/vr.hpp>
#include <components/xr/instance.hpp>
#include <components/xr/interactionprofiles.hpp>
#include <components/xr/session.hpp>
// ## VR_PATCH END

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/esm/position.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadweap.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>
#include <components/misc/strings/lower.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/cellstore.hpp"
#include "mwworld/containerstore.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/esmstore.hpp"
#include "mwworld/manualref.hpp"
#include "mwworld/worldimp.hpp"
#include "mwworld/worldmodel.hpp"

#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"
#include "mwclass/esm4npc.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"
#include "mwmechanics/actorutil.hpp"
#include "mwmechanics/stat.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            std::string messageFormat
                = MWBase::Environment::get().getL10nManager()->getMessage("OMWEngine", "ScreenshotMade");

            std::string message = Misc::StringUtils::format(messageFormat, filePath);

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }

    int readProofInt(const char* name, int fallback)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return fallback;

        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value)
            return fallback;

        return static_cast<int>(parsed);
    }

    float readProofFloat(const char* name, float fallback)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return fallback;

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value)
            return fallback;

        return parsed;
    }

    bool isFalloutProofFaceNodeName(std::string_view lowerName)
    {
        if (lowerName.find("fnv part ") == std::string_view::npos)
            return false;

        return lowerName.find("characters/head/head") != std::string_view::npos
            || lowerName.find("characters/head/mouth") != std::string_view::npos
            || lowerName.find("characters/head/teeth") != std::string_view::npos
            || lowerName.find("characters/head/tongue") != std::string_view::npos
            || lowerName.find("characters/head/eye") != std::string_view::npos
            || lowerName.find("characters/hair/beard") != std::string_view::npos;
    }

    bool isFalloutProofFaceHeadNodeName(std::string_view lowerName)
    {
        return lowerName.find("fnv part ") != std::string_view::npos
            && lowerName.find("characters/head/head") != std::string_view::npos;
    }

    bool isFalloutProofFaceFeatureNodeName(std::string_view lowerName)
    {
        if (lowerName.find("fnv part ") == std::string_view::npos)
            return false;

        return lowerName.find("characters/head/mouth") != std::string_view::npos
            || lowerName.find("characters/head/teeth") != std::string_view::npos
            || lowerName.find("characters/head/tongue") != std::string_view::npos
            || lowerName.find("characters/head/eye") != std::string_view::npos
            || lowerName.find("characters/hair/beard") != std::string_view::npos;
    }

    class FalloutProofFaceBoundsVisitor : public osg::NodeVisitor
    {
    public:
        FalloutProofFaceBoundsVisitor()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        }

        void apply(osg::Node& node) override
        {
            const std::string lowerName = Misc::StringUtils::lowerCase(node.getName());
            if (lowerName == "bip01 head" || lowerName == "bip01 head_nub")
            {
                const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
                mHeadCenter += localToWorld.getTrans();
                ++mHeadMatched;
            }

            if (isFalloutProofFaceNodeName(lowerName))
            {
                const osg::BoundingSphere bound = node.getBound();
                if (bound.valid())
                {
                    const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
                    const osg::Vec3d center = bound.center() * localToWorld;
                    const double radius = bound.radius();
                    mBounds.expandBy(osg::Vec3d(center.x() - radius, center.y() - radius, center.z() - radius));
                    mBounds.expandBy(osg::Vec3d(center.x() + radius, center.y() + radius, center.z() + radius));
                    ++mMatched;

                    if (isFalloutProofFaceHeadNodeName(lowerName))
                    {
                        mHeadCenter += center;
                        ++mHeadMatched;
                    }
                    else if (isFalloutProofFaceFeatureNodeName(lowerName))
                    {
                        mFeatureCenter += center;
                        ++mFeatureMatched;
                    }
                }
            }

            traverse(node);
        }

        const osg::BoundingBox& getBounds() const { return mBounds; }
        unsigned int getMatched() const { return mMatched; }
        unsigned int getHeadMatched() const { return mHeadMatched; }
        unsigned int getFeatureMatched() const { return mFeatureMatched; }
        osg::Vec3d getHeadCenter() const
        {
            return mHeadMatched > 0 ? mHeadCenter / static_cast<double>(mHeadMatched) : osg::Vec3d();
        }
        osg::Vec3d getFeatureCenter() const
        {
            return mFeatureMatched > 0 ? mFeatureCenter / static_cast<double>(mFeatureMatched) : osg::Vec3d();
        }

    private:
        osg::BoundingBox mBounds;
        osg::Vec3d mHeadCenter;
        osg::Vec3d mFeatureCenter;
        unsigned int mMatched = 0;
        unsigned int mHeadMatched = 0;
        unsigned int mFeatureMatched = 0;
    };

    template <typename T>
    ESM::RefId findEsm4EditorId(const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& typedStore = store.get<T>();
        for (auto it = typedStore.begin(); it != typedStore.end(); ++it)
        {
            if (it->mEditorId == editorId)
                return ESM::RefId::formIdRefId(it->mId);
        }

        return ESM::RefId();
    }

    template <typename T>
    bool addFNVEditorItem(
        MWWorld::ContainerStore& inventory, const MWWorld::ESMStore& store, std::string_view editorId, int count)
    {
        const ESM::RefId id = findEsm4EditorId<T>(store, editorId);
        if (id.empty())
            return false;

        try
        {
            inventory.add(id, count, false);
            Log(Debug::Info) << "FNV/ESM4 proof: starter inventory added " << editorId << " x" << count << " id="
                             << id.toDebugString();
            return true;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: starter inventory failed " << editorId << " id="
                                << id.toDebugString() << ": " << e.what();
            return false;
        }
    }

    bool addProofInventoryItem(MWWorld::ContainerStore& inventory, std::string_view id, int count)
    {
        const ESM::RefId refId = ESM::RefId::stringRefId(id);
        try
        {
            inventory.add(refId, count, false);
            Log(Debug::Info) << "FNV/ESM4 proof: starter proof inventory added " << id << " x" << count;
            return true;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: starter proof inventory failed " << id << ": " << e.what();
            return false;
        }
    }

    void applyFNVLevelOneCourierBootstrap(
        MWBase::World& world, MWBase::Journal& journal, bool moveOutsideDoc, bool applyProfile)
    {
        const int vcg01Stage = readProofInt("OPENMW_FNV_BOOTSTRAP_VCG01_STAGE", 200);
        const ESM::RefId vcg01 = ESM::RefId::stringRefId("VCG01");
        journal.setJournalIndex(vcg01, vcg01Stage);
        const float proofHour = readProofFloat("OPENMW_FNV_BOOTSTRAP_HOUR", 12.f);
        world.setGlobalFloat(MWWorld::Globals::sGameHour, proofHour);
        world.advanceTime(0.0, false);
        Log(Debug::Info) << "FNV/ESM4 proof: set quest state VCG01 stage=" << vcg01Stage
                         << " previousLoadSafe=explicit-bootstrap hour=" << proofHour;

        if (moveOutsideDoc)
        {
            ESM::Position outside;
            outside.pos[0] = readProofFloat("OPENMW_FNV_BOOTSTRAP_POS_X", -67735.f);
            outside.pos[1] = readProofFloat("OPENMW_FNV_BOOTSTRAP_POS_Y", 3204.f);
            outside.pos[2] = readProofFloat("OPENMW_FNV_BOOTSTRAP_POS_Z", 8425.f);
            outside.rot[0] = readProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_X", 0.f);
            outside.rot[1] = readProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_Y", 0.f);
            outside.rot[2] = readProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_Z", -0.6981317f);
            ESM::Position cellProbe = outside;
            ESM::RefId cellId = world.findExteriorPosition("Goodsprings", cellProbe);
            if (cellId.empty())
            {
                cellId = ESM::RefId::formIdRefId(ESM::FormId::fromUint32(0x010daeb9));
                Log(Debug::Warning) << "FNV/ESM4 proof: Goodsprings cell lookup failed; falling back to "
                                    << cellId.toDebugString();
            }
            else
            {
                Log(Debug::Info) << "FNV/ESM4 proof: resolved Goodsprings bootstrap cell " << cellId.toDebugString()
                                 << " preserving explicit proof pos=(" << outside.pos[0] << "," << outside.pos[1]
                                 << "," << outside.pos[2] << ")";
            }
            world.changeToCell(cellId, outside, false, true);
            MWWorld::Ptr player = world.getPlayerPtr();
            player = world.moveObject(player, outside.asVec3(), true, true);
            if (player.getCell() != nullptr)
                player.getCell()->setWaterLevel(-200000.f);
            world.rotateObject(player, osg::Vec3f(outside.rot[0], outside.rot[1], outside.rot[2]));
            if (MWRender::Camera* camera = world.getCamera())
            {
                const char* cameraMode = std::getenv("OPENMW_FNV_BOOTSTRAP_CAMERA_MODE");
                const bool forceFirstPerson
                    = cameraMode != nullptr && Misc::StringUtils::ciEqual(cameraMode, "firstperson");
                const float cameraDistance = readProofFloat("OPENMW_FNV_BOOTSTRAP_CAMERA_DISTANCE", 0.f);
                camera->attachTo(player);
                camera->setMode(
                    forceFirstPerson ? MWRender::Camera::Mode::FirstPerson : MWRender::Camera::Mode::ThirdPerson,
                    true);
                camera->setPreferredCameraDistance(cameraDistance);
                camera->update(0.f, false);
                camera->setPitch(-outside.rot[0], true);
                camera->setYaw(-outside.rot[2], true);
                camera->setRoll(0.f);
                camera->updateCamera();
                const ESM::Position& actual = player.getRefData().getPosition();
                const osg::Vec3d cameraPos = camera->getPosition();
                Log(Debug::Info) << "FNV/ESM4 proof: reset player camera mode="
                                 << static_cast<int>(camera->getMode()) << " playerPos=("
                                 << actual.pos[0] << "," << actual.pos[1] << "," << actual.pos[2]
                                 << ") playerRot=(" << actual.rot[0] << "," << actual.rot[1] << ","
                                 << actual.rot[2] << ") cameraPos=(" << cameraPos.x() << "," << cameraPos.y()
                                 << "," << cameraPos.z() << ") cameraPitch=" << camera->getPitch()
                                 << " cameraYaw=" << camera->getYaw() << " cameraDistance=" << cameraDistance;
            }
            Log(Debug::Info) << "FNV/ESM4 proof: moved player outside after Doc handoff activeCell="
                             << world.getCellName(player.getCell()) << " pos=("
                             << outside.pos[0] << "," << outside.pos[1] << "," << outside.pos[2] << ") rotZ="
                             << outside.rot[2] << " rotX=" << outside.rot[0] << " rotY=" << outside.rot[1]
                             << " cell=" << cellId.toDebugString() << " waterLevel=-200000";
        }

        if (!applyProfile)
            return;

        MWWorld::Ptr player = world.getPlayerPtr();
        MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
        stats.setLevel(1);
        stats.setLevelProgress(0);
        stats.setBounty(0);
        stats.setReputation(0);
        stats.setBaseDisposition(50);

        const std::pair<ESM::RefId, float> attributes[] = {
            { ESM::Attribute::Strength, 6.f },
            { ESM::Attribute::Intelligence, 6.f },
            { ESM::Attribute::Willpower, 5.f },
            { ESM::Attribute::Agility, 6.f },
            { ESM::Attribute::Speed, 5.f },
            { ESM::Attribute::Endurance, 6.f },
            { ESM::Attribute::Personality, 5.f },
            { ESM::Attribute::Luck, 6.f },
        };
        for (const auto& [id, value] : attributes)
            stats.setAttribute(id, value * 10.f);

        for (int i = 0; i < ESM::Skill::Length; ++i)
        {
            MWMechanics::SkillValue skill;
            skill.setBase(35.f);
            skill.setProgress(0.f);
            stats.setSkill(ESM::Skill::indexToRefId(i), skill);
        }

        stats.setHealth(MWMechanics::DynamicStat<float>(125.f, 125.f, 125.f));
        stats.setMagicka(MWMechanics::DynamicStat<float>(60.f, 60.f, 60.f));
        stats.setFatigue(MWMechanics::DynamicStat<float>(220.f, 220.f, 220.f));

        MWWorld::ContainerStore& inventory = player.getClass().getContainerStore(player);
        const MWWorld::ESMStore& store = world.getStore();
        int added = 0;
        added += addFNVEditorItem<ESM4::Weapon>(inventory, store, "WeapNV9mmPistol", 1) ? 1 : 0;
        added += addFNVEditorItem<ESM4::Weapon>(inventory, store, "WeapNVVarmintRifle", 1) ? 1 : 0;
        added += addFNVEditorItem<ESM4::Ammunition>(inventory, store, "Ammo9mm", 60) ? 1 : 0;
        added += addFNVEditorItem<ESM4::Potion>(inventory, store, "Stimpak", 5) ? 1 : 0;
        added += addFNVEditorItem<ESM4::MiscItem>(inventory, store, "BobbyPin", 5) ? 1 : 0;
        added += addFNVEditorItem<ESM4::MiscItem>(inventory, store, "Caps001", 75) ? 1 : 0;
        int proofAdded = 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_9MM_PISTOL", 1) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_VARMINT_RIFLE", 1) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_9MM_AMMO", 60) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_STIMPAK", 5) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_BOBBY_PIN", 5) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_CAPS", 75) ? 1 : 0;

        const ESM::RefId proofOutfitId = findEsm4EditorId<ESM4::Armor>(store, "OutfitRepublican02");
        if (!proofOutfitId.empty())
        {
            if (const ESM4::Armor* proofOutfit = store.get<ESM4::Armor>().search(proofOutfitId))
            {
                try
                {
                    const bool equipped = MWClass::ESM4Npc::addEquippedArmor(player, proofOutfit);
                    Log(Debug::Info) << "FNV/ESM4 proof: level-1 Courier visual outfit "
                                     << proofOutfit->mEditorId << " form=" << proofOutfitId.toDebugString()
                                     << " model=" << MWClass::ESM4Npc::chooseEquipmentModel(
                                                        proofOutfit, MWClass::ESM4Npc::isFemale(player))
                                     << " added=" << equipped;
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: level-1 Courier visual outfit "
                                        << proofOutfit->mEditorId << " skipped: " << e.what();
                }
            }
        }
        else
            Log(Debug::Warning) << "FNV/ESM4 proof: level-1 Courier visual outfit OutfitRepublican02 not found";

        Log(Debug::Info) << "FNV/ESM4 proof: level-1 Courier profile applied level=" << stats.getLevel()
                         << " attributes=8 skills=" << ESM::Skill::Length << " starterItemKinds=" << added
                         << " proofStarterItemKinds=" << proofAdded;
    }
    // ## VR_PATCH BEGIN

    class InitializeVrOperation : public osg::GraphicsOperation
    {
    public:
        InitializeVrOperation(OMW::Engine* engine)
            : GraphicsOperation("InitializeVrOperation", false)
            , mEngine(engine)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override { mEngine->configureVRGraphics(graphicsContext); }

        OMW::Engine* mEngine;
    };
    // ## VR_PATCH END
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const auto getProofFrame = [](const char* name) {
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return -1;
        char* end = nullptr;
        const long value = std::strtol(env, &end, 10);
        if (end == env || value < 0)
            return -1;
        return static_cast<int>(value);
    };
    const auto getProofFrames = [](const char* name) {
        std::vector<int> frames;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return frames;

        std::string_view value(env);
        while (!value.empty())
        {
            const std::size_t comma = value.find(',');
            std::string_view token = value.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
            {
                int frame = -1;
                const auto result = std::from_chars(token.data(), token.data() + token.size(), frame);
                if (result.ec == std::errc() && result.ptr == token.data() + token.size() && frame >= 0)
                    frames.push_back(frame);
            }
            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }

        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        return frames;
    };
    const auto getProofFloats = [](const char* name) {
        std::vector<float> values;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return values;

        std::string_view value(env);
        while (!value.empty())
        {
            const std::size_t comma = value.find(',');
            std::string_view token = value.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
            {
                const std::string tokenText(token);
                char* end = nullptr;
                const float parsed = std::strtof(tokenText.c_str(), &end);
                if (end != tokenText.c_str() && *end == '\0')
                    values.push_back(parsed);
            }
            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }

        return values;
    };
    static const std::vector<int> proofScreenshotFrames = getProofFrames("OPENMW_PROOF_SCREENSHOT_FRAME");
    static const std::vector<float> proofActorViewOrbitDegrees
        = getProofFloats("OPENMW_PROOF_ACTOR_VIEW_ORBIT_DEGREES");
    static const int proofScreenshotReadyFrames = getProofFrame("OPENMW_PROOF_SCREENSHOT_READY_FRAMES");
    static const int proofInventoryFrame = getProofFrame("OPENMW_PROOF_INVENTORY_FRAME");
    static const int proofQuickSaveFrame = getProofFrame("OPENMW_PROOF_QUICKSAVE_FRAME");
    static const int proofSayFrame = getProofFrame("OPENMW_PROOF_SAY_FRAME");
    static const int proofTimedScript1Frame = getProofFrame("OPENMW_PROOF_TIMED_SCRIPT_1_FRAME");
    static const int proofTimedScript2Frame = getProofFrame("OPENMW_PROOF_TIMED_SCRIPT_2_FRAME");
    static std::size_t proofScreenshotFrameIndex = 0;
    static bool proofScreenshotReadyQueued = false;
    static bool proofInventoryOpened = false;
    static bool proofQuickSaveQueued = false;
    static bool proofSayQueued = false;
    static bool proofTimedScript1Executed = false;
    static bool proofTimedScript2Executed = false;
    static bool proofActorCameraAligned = false;
    static int proofActorCameraAlignedFrame = -1;
    static bool proofActorAlignedScreenshotQueued = false;
    static bool proofActorScreenshotWaitLogged = false;
    static int proofActorScreenshotLastResolveFrame = -1;
    static bool proofFirstPersonHidden = false;
    static bool proofGuiHidden = false;
    static bool proofDelayedStartupScriptExecuted = false;
    static bool proofFNVBootstrapApplied = false;
    static bool proofScreenshotWaitLogged = false;
    static int proofWorldReadyFrames = 0;
    const auto parseProofFormId = [](std::string_view value) -> std::optional<ESM::FormId> {
        constexpr std::string_view prefix = "FormId:0x";
        constexpr std::string_view shortPrefix = "0x";
        std::size_t offset = std::string_view::npos;
        if (value.size() > prefix.size() && value.substr(0, prefix.size()) == prefix)
            offset = prefix.size();
        else if (value.size() > shortPrefix.size() && value.substr(0, shortPrefix.size()) == shortPrefix)
            offset = shortPrefix.size();
        else
            return std::nullopt;

        std::uint32_t formId = 0;
        const auto result = std::from_chars(value.data() + offset, value.data() + value.size(), formId, 16);
        if (result.ec != std::errc() || result.ptr != value.data() + value.size())
            return std::nullopt;
        return ESM::FormId::fromUint32(formId);
    };
    const auto makeProofRefId = [&](const char* value) {
        const std::string_view ref(value);
        if (std::optional<ESM::FormId> formId = parseProofFormId(ref))
            return ESM::RefId::formIdRefId(*formId);
        return ESM::RefId::stringRefId(ref);
    };
    const auto compactProofToken = [](std::string_view value) {
        std::string compact;
        compact.reserve(value.size());
        for (char ch : Misc::StringUtils::lowerCase(value))
        {
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                compact.push_back(ch);
        }
        return compact;
    };
    const auto resolveProofActor = [&](const char* value) {
        MWWorld::Ptr actor = mWorld->searchPtr(makeProofRefId(value), false, false);
        if (!actor.isEmpty())
            return actor;

        const std::string_view target(value);
        const ESM::RefId targetRefId = makeProofRefId(value);
        const std::optional<ESM::FormId> targetFormId = parseProofFormId(target);
        const std::string targetCompact = compactProofToken(target);
        const bool logActors = std::getenv("OPENMW_PROOF_LOG_ACTORS") != nullptr;
        int scanned = 0;
        int actors = 0;
        MWWorld::Ptr found;

        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
        {
            if (cellstore == nullptr)
                continue;

            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                ++scanned;
                if (ptr.isEmpty() || !ptr.getClass().isActor())
                    return true;
                ++actors;

                const ESM::RefId baseId = ptr.getCellRef().getRefId();
                const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
                std::string name;
                try
                {
                    name = std::string(ptr.getClass().getName(ptr));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: actor name lookup failed for " << ptr.toString()
                                        << ": " << e.what();
                }

                const std::string baseDebug = baseId.toDebugString();
                const std::string refDebug = refNum.toString("FormId:");
                const std::string baseCompact = compactProofToken(baseDebug);
                const std::string refCompact = compactProofToken(refDebug);
                const std::string nameCompact = compactProofToken(name);
                const ESM::Position& pos = ptr.getRefData().getPosition();

                if (logActors)
                {
                    Log(Debug::Info) << "FNV/ESM4 proof: active actor ref=" << refDebug << " base=" << baseDebug
                                     << " name=\"" << name << "\" pos=(" << pos.pos[0] << "," << pos.pos[1]
                                     << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << "," << pos.rot[1]
                                     << "," << pos.rot[2] << ") ptr=" << ptr.toString();
                }

                const bool formIdMatchesRef = targetFormId.has_value() && refNum == *targetFormId;
                const bool refIdMatchesBase = baseId == targetRefId;
                const bool tokenMatches = !targetCompact.empty()
                    && (targetCompact == baseCompact || targetCompact == refCompact || targetCompact == nameCompact
                        || (!nameCompact.empty()
                            && (targetCompact.find(nameCompact) != std::string::npos
                                || nameCompact.find(targetCompact) != std::string::npos)));
                if (formIdMatchesRef || refIdMatchesBase || tokenMatches)
                {
                    found = ptr;
                    Log(Debug::Info) << "FNV/ESM4 proof: active-cell actor match target \"" << value
                                     << "\" ref=" << refDebug << " base=" << baseDebug << " name=\"" << name
                                     << "\" ptr=" << ptr.toString() << " pos=(" << pos.pos[0] << ","
                                     << pos.pos[1] << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << ","
                                     << pos.rot[1] << "," << pos.rot[2] << ") scanned=" << scanned
                                     << " actors=" << actors;
                    return false;
                }
                return true;
            });

            if (!found.isEmpty())
                break;
        }

        if (found.isEmpty())
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: active-cell actor lookup failed for \"" << value
                                << "\" scanned=" << scanned << " actors=" << actors;

            if (std::getenv("OPENMW_PROOF_PLACE_ACTOR_IF_MISSING") != nullptr)
            {
                try
                {
                    MWWorld::Ptr player = MWMechanics::getPlayer();
                    MWWorld::CellStore* store = nullptr;
                    ESM::RefId worldspace = ESM::Cell::sDefaultWorldspaceId;
                    ESM::Position pos;
                    if (!player.isEmpty() && player.isInCell())
                    {
                        const ESM::Position& playerPos = player.getRefData().getPosition();
                        pos.pos[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", playerPos.pos[0] + 96.f);
                        pos.pos[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", playerPos.pos[1]);
                        pos.pos[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", playerPos.pos[2]);
                        pos.rot[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", 0.f);
                        pos.rot[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", 0.f);
                        pos.rot[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", 0.f);

                        MWWorld::CellStore* playerCell = player.getCell();
                        if (playerCell != nullptr && playerCell->isExterior())
                        {
                            worldspace = playerCell->getCell()->getWorldSpace();
                            store = &mWorld->getWorldModel().getExterior(
                                ESM::positionToExteriorCellLocation(pos.pos[0], pos.pos[1], worldspace));
                        }
                        else
                            store = playerCell;
                    }
                    else
                    {
                        pos.pos[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", 0.f);
                        pos.pos[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", 0.f);
                        pos.pos[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", 0.f);
                        pos.rot[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", 0.f);
                        pos.rot[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", 0.f);
                        pos.rot[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", 0.f);
                        const auto& activeCells = mWorld->getWorldScene().getActiveCells();
                        if (!activeCells.empty())
                            store = *activeCells.begin();
                    }

                    if (store == nullptr)
                        throw std::runtime_error("no active cell available for proof placement");

                    ESM::RefId placementRefId = targetRefId;
                    if (const char* placementFormId = std::getenv("OPENMW_PROOF_PLACE_ACTOR_FORM_ID"))
                    {
                        if (std::optional<ESM::FormId> formId = parseProofFormId(placementFormId))
                        {
                            placementRefId = ESM::RefId::formIdRefId(*formId);
                            Log(Debug::Info) << "FNV/ESM4 proof: using inventory form id " << placementFormId
                                             << " for missing actor target \"" << value << "\" base="
                                             << targetRefId.toDebugString();
                        }
                        else
                            Log(Debug::Warning) << "FNV/ESM4 proof: ignored invalid inventory form id \""
                                                << placementFormId << "\" for missing actor target \"" << value << "\"";
                    }

                    MWWorld::ManualRef ref(mWorld->getStore(), placementRefId);
                    ref.getPtr().mRef->mData.mPhysicsPostponed = !ref.getPtr().getClass().isActor();
                    ref.getPtr().getCellRef().setPosition(pos);
                    found = mWorld->placeObject(ref.getPtr(), store, pos);
                    found.getClass().adjustPosition(found, true);
                    Log(Debug::Info) << "FNV/ESM4 proof: placed missing actor target \"" << value
                                     << "\" base=" << placementRefId.toDebugString() << " requestedBase="
                                     << targetRefId.toDebugString() << " pos=(" << pos.pos[0] << "," << pos.pos[1]
                                     << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << "," << pos.rot[1] << ","
                                     << pos.rot[2] << ") ptr=" << found.toString();
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: failed to place missing actor target \"" << value
                                        << "\": " << e.what();
                }
            }
        }
        return found;
    };

    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    mEnvironment.setFrameDuration(frametime);

    try
    {
        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            static bool loggedHiddenVrWindow = false;
            if (!mWindowManager->isWindowVisible() && VR::getVR() && !loggedHiddenVrWindow)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: VR mirror window hidden; keeping world simulation running";
                loggedHiddenVrWindow = true;
            }

            if (!mWindowManager->isWindowVisible() && !VR::getVR())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", mUnrefQueue->getSize());

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", mWorkQueue->getNumItems());
        stats->setAttribute(frameNumber, "WorkThread", mWorkQueue->getNumActiveThreads());

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    mViewer->eventTraversal();
    mViewer->updateTraversal();

    // update GUI by world data
    {
        ScopedProfile<UserStatsType::WindowManager> profile(frameStart, frameNumber, *timer, *stats);
        mWorld->updateWindowManager();
    }

    if (VR::getVR())
    {
        if (mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            auto playerPtr = MWMechanics::getPlayer();
            auto playerAnim = MWBase::Environment::get().getWorld()->getAnimation(playerPtr);
            if (playerAnim)
                static_cast<MWVR::VRAnimation*>(playerAnim)->updateSpace();
        }
        if (VR::getShouldRecenterZ() || VR::getShouldRecenterXY())
        {
            MWBase::Environment::get().getLuaManager()->vrRecentered(
                VR::getShouldRecenterZ(), VR::getShouldRecenterXY());
            VR::setShouldRecenterXY(false);
            VR::setShouldRecenterZ(false);
        }
        mLuaManager->onVRFrame();
        VR::Session::instance().updateSpaces();
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    if (std::getenv("OPENMW_PROOF_HIDE_FIRST_PERSON") != nullptr)
    {
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~MWRender::Mask_FirstPerson;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        if (!proofFirstPersonHidden)
        {
            proofFirstPersonHidden = true;
            Log(Debug::Info) << "FNV/ESM4 proof: hidden first-person render mask for clean proof capture";
        }
    }

    if (std::getenv("OPENMW_PROOF_HIDE_PLAYER_VISUAL") != nullptr)
    {
        static bool proofPlayerMaskHidden = false;
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~MWRender::Mask_Player;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        if (!proofPlayerMaskHidden)
        {
            proofPlayerMaskHidden = true;
            Log(Debug::Info) << "FNV/ESM4 proof: hidden player render mask for clean proof capture";
        }
    }

    if (!proofGuiHidden && std::getenv("OPENMW_PROOF_HIDE_GUI") != nullptr)
    {
        mWindowManager->setHudVisibility(false);
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~MWRender::VisMask::Mask_GUI;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        proofGuiHidden = true;
        Log(Debug::Info) << "FNV/ESM4 proof: hidden GUI/HUD for clean proof capture";
    }

    const bool proofRunning = mStateManager->getState() == MWBase::StateManager::State_Running;
    const bool proofLoadingGui = mWindowManager->containsMode(MWGui::GM_Loading)
        || mWindowManager->containsMode(MWGui::GM_LoadingWallpaper)
        || mWindowManager->containsMode(MWGui::GM_MainMenu);
    const bool proofWorldReady = proofRunning && !proofLoadingGui;
    if (proofWorldReady)
        ++proofWorldReadyFrames;
    else
        proofWorldReadyFrames = 0;

    if (!proofDelayedStartupScriptExecuted && std::getenv("OPENMW_PROOF_DELAY_STARTUP_SCRIPT") != nullptr
        && !mStartupScript.empty() && proofWorldReady)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: executing delayed startup script after world load";
        mWindowManager->executeInConsole(mStartupScript);
        proofDelayedStartupScriptExecuted = true;
    }

    const bool proofFNVBootstrapProfile = std::getenv("OPENMW_FNV_BOOTSTRAP_LEVEL1_COURIER") != nullptr;
    const bool proofFNVBootstrapOutside = std::getenv("OPENMW_FNV_BOOTSTRAP_DOC_SENT") != nullptr;
    if (!proofFNVBootstrapApplied && proofRunning && (proofFNVBootstrapProfile || proofFNVBootstrapOutside))
    {
        try
        {
            applyFNVLevelOneCourierBootstrap(
                *mWorld, *mJournal, proofFNVBootstrapOutside, proofFNVBootstrapProfile);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "FNV/ESM4 proof: level-1 Courier bootstrap failed: " << e.what();
        }
        proofFNVBootstrapApplied = true;
    }

    if (!proofInventoryOpened && proofInventoryFrame >= 0 && frameNumber >= static_cast<unsigned>(proofInventoryFrame)
        && proofRunning)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: opening native inventory screen at frame " << frameNumber;
        mWindowManager->pushGuiMode(MWGui::GM_Inventory);
        proofInventoryOpened = true;
    }

    if (!proofQuickSaveQueued && proofQuickSaveFrame >= 0 && frameNumber >= static_cast<unsigned>(proofQuickSaveFrame)
        && proofRunning)
    {
        const char* quickSaveName = std::getenv("OPENMW_PROOF_QUICKSAVE_NAME");
        const std::string name
            = quickSaveName != nullptr && *quickSaveName != '\0' ? quickSaveName : "FNV Proof Save";
        Log(Debug::Info) << "FNV/ESM4 proof: requesting quicksave \"" << name << "\" at frame " << frameNumber;
        mStateManager->quickSave(name);
        proofQuickSaveQueued = true;
    }

    const bool proofRequiresActorForScreenshot = std::getenv("OPENMW_PROOF_REQUIRE_ACTOR_FOR_SCREENSHOT") != nullptr;
    const int proofActorResolveRetryFramesEnv = getProofFrame("OPENMW_PROOF_ACTOR_RESOLVE_RETRY_FRAMES");
    const int proofActorResolveRetryFrames = proofActorResolveRetryFramesEnv >= 0 ? proofActorResolveRetryFramesEnv : 30;
    const bool proofOrbitBurstAlignReached = proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && !proofActorViewOrbitDegrees.empty()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    const bool proofActorScreenshotNeedsResolveRaw = proofRequiresActorForScreenshot && !proofActorCameraAligned
        && proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    const bool proofActorScreenshotNeedsResolve = proofActorScreenshotNeedsResolveRaw
        && (proofActorScreenshotLastResolveFrame < 0
            || frameNumber
                >= static_cast<unsigned>(proofActorScreenshotLastResolveFrame + proofActorResolveRetryFrames));
    if ((!proofSayQueued || proofOrbitBurstAlignReached || proofActorScreenshotNeedsResolve) && proofSayFrame >= 0
        && frameNumber >= static_cast<unsigned>(proofSayFrame)
        && mSoundManager != nullptr)
    {
        const char* proofSayFile = std::getenv("OPENMW_PROOF_SAY_FILE");
        const char* proofSayActor = std::getenv("OPENMW_PROOF_SAY_ACTOR");
        const char* proofSayTopic = std::getenv("OPENMW_PROOF_SAY_TOPIC");
        MWWorld::Ptr proofActor;
        if (proofSayActor != nullptr && *proofSayActor != '\0')
        {
            proofActor = resolveProofActor(proofSayActor);
            if (proofActorScreenshotNeedsResolve)
                proofActorScreenshotLastResolveFrame = static_cast<int>(frameNumber);
            Log(Debug::Info) << "FNV/ESM4 proof: resolved proof say actor \"" << proofSayActor
                             << "\" -> " << proofActor.toString();
            if (!proofActor.isEmpty() && std::getenv("OPENMW_PROOF_ALIGN_PLAYER_TO_ACTOR") != nullptr)
            {
                if (std::getenv("OPENMW_PROOF_STAGE_ACTOR") != nullptr)
                {
                    const ESM::Position& currentActorPos = proofActor.getRefData().getPosition();
                    const osg::Vec3f stagedPos(
                        readProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", currentActorPos.pos[0]),
                        readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", currentActorPos.pos[1]),
                        readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", currentActorPos.pos[2]));
                    const osg::Vec3f stagedRot(
                        readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", currentActorPos.rot[0]),
                        readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", currentActorPos.rot[1]),
                        readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", currentActorPos.rot[2]));
                    try
                    {
                        proofActor = mWorld->moveObject(proofActor, stagedPos, true, true);
                        mWorld->rotateObject(proofActor, stagedRot);
                        Log(Debug::Info) << "FNV/ESM4 proof: staged actor target=\"" << proofSayActor << "\" pos=("
                                         << stagedPos.x() << "," << stagedPos.y() << "," << stagedPos.z()
                                         << ") rot=(" << stagedRot.x() << "," << stagedRot.y() << ","
                                         << stagedRot.z() << ") ptr=" << proofActor.toString();
                    }
                    catch (const std::exception& e)
                    {
                        Log(Debug::Warning) << "FNV/ESM4 proof: failed to stage actor target=\"" << proofSayActor
                                            << "\": " << e.what();
                    }
                }

                const ESM::Position& actorPos = proofActor.getRefData().getPosition();
                float offsetX = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_X", 0.f);
                float offsetY = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y", -220.f);
                const float offsetZ = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z", 20.f);
                const float targetZ = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_TARGET_Z", 120.f);
                const float cameraDistance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_CAMERA_DISTANCE", 0.f);
                const float cameraPitch = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_PITCH", 0.f);
                const bool staticDialogueCamera = std::getenv("OPENMW_PROOF_ACTOR_VIEW_STATIC_CAMERA") != nullptr;
                bool useFaceAxisCamera = false;
                osg::Vec2f faceAxis(0.f, 0.f);
                osg::Vec3d actorAim(actorPos.pos[0], actorPos.pos[1], actorPos.pos[2] + targetZ);
                double cameraZ = actorPos.pos[2] + offsetZ;
                if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_RENDER_BOUNDS") != nullptr
                    && proofActor.getRefData().getBaseNode() != nullptr)
                {
                    osg::ComputeBoundsVisitor boundsVisitor;
                    boundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
                    proofActor.getRefData().getBaseNode()->accept(boundsVisitor);
                    const osg::BoundingBox bounds = boundsVisitor.getBoundingBox();
                    if (bounds.valid())
                    {
                        const float focusPercent = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_BOUNDS_FOCUS", 0.72f);
                        const double focusZ = bounds.zMin() + (bounds.zMax() - bounds.zMin()) * focusPercent;
                        actorAim = osg::Vec3d(bounds.center().x(), bounds.center().y(), focusZ);
                        cameraZ = focusZ + (offsetZ - targetZ);
                        Log(Debug::Info) << "FNV/ESM4 proof: actor render bounds target=\"" << proofSayActor
                                         << "\" min=(" << bounds.xMin() << "," << bounds.yMin() << ","
                                         << bounds.zMin() << ") max=(" << bounds.xMax() << "," << bounds.yMax()
                                         << "," << bounds.zMax() << ") focus=(" << actorAim.x() << ","
                                         << actorAim.y() << "," << actorAim.z() << ") cameraZ=" << cameraZ;
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor render bounds invalid target=\""
                                            << proofSayActor << "\"";
                }
                if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_BOUNDS") != nullptr
                    && proofActor.getRefData().getBaseNode() != nullptr)
                {
                    FalloutProofFaceBoundsVisitor faceBoundsVisitor;
                    proofActor.getRefData().getBaseNode()->accept(faceBoundsVisitor);
                    const osg::BoundingBox bounds = faceBoundsVisitor.getBounds();
                    if (bounds.valid())
                    {
                        actorAim = osg::Vec3d(bounds.center().x(), bounds.center().y(), bounds.center().z());
                        const osg::Vec3d headCenter = faceBoundsVisitor.getHeadCenter();
                        const osg::Vec3d featureCenter = faceBoundsVisitor.getFeatureCenter();
                        const osg::Vec3d faceVector = featureCenter - headCenter;
                        if (faceBoundsVisitor.getHeadMatched() > 0 && faceBoundsVisitor.getFeatureMatched() > 0)
                        {
                            const float featureFocus
                                = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FEATURE_FOCUS", 0.3f);
                            actorAim = headCenter + faceVector * featureFocus;
                        }
                        else if (faceBoundsVisitor.getFeatureMatched() > 0)
                            actorAim = featureCenter;
                        else if (faceBoundsVisitor.getHeadMatched() > 0)
                            actorAim = headCenter;
                        cameraZ = actorAim.z() + (offsetZ - targetZ);
                        const double faceVectorPlanar
                            = std::sqrt(faceVector.x() * faceVector.x() + faceVector.y() * faceVector.y());
                        if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_AXIS") != nullptr
                            && faceBoundsVisitor.getHeadMatched() > 0 && faceBoundsVisitor.getFeatureMatched() > 0
                            && faceVectorPlanar > 1e-3)
                        {
                            faceAxis.set(static_cast<float>(faceVector.x() / faceVectorPlanar),
                                static_cast<float>(faceVector.y() / faceVectorPlanar));
                            useFaceAxisCamera = true;
                        }
                        Log(Debug::Info) << "FNV/ESM4 proof: actor face bounds target=\"" << proofSayActor
                                         << "\" matched=" << faceBoundsVisitor.getMatched() << " min=("
                                         << bounds.xMin() << "," << bounds.yMin() << "," << bounds.zMin()
                                         << ") max=(" << bounds.xMax() << "," << bounds.yMax() << ","
                                         << bounds.zMax() << ") focus=(" << actorAim.x() << "," << actorAim.y()
                                         << "," << actorAim.z() << ") cameraZ=" << cameraZ
                                         << " headMatched=" << faceBoundsVisitor.getHeadMatched()
                                         << " featureMatched=" << faceBoundsVisitor.getFeatureMatched()
                                         << " headCenter=(" << headCenter.x() << "," << headCenter.y() << ","
                                         << headCenter.z() << ") featureCenter=(" << featureCenter.x() << ","
                                         << featureCenter.y() << "," << featureCenter.z() << ") faceAxis=("
                                         << faceAxis.x() << "," << faceAxis.y()
                                         << ") useFaceAxisCamera=" << useFaceAxisCamera;
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor face bounds invalid target=\""
                                            << proofSayActor << "\" matched="
                                            << faceBoundsVisitor.getMatched();
                }
                if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_ACTOR_FACING") != nullptr)
                {
                    float frontDistance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FRONT_DISTANCE", 0.f);
                    if (frontDistance <= 0.f)
                    {
                        frontDistance = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                        if (frontDistance <= 0.f)
                            frontDistance = 220.f;
                    }
                    const float actorYaw = actorPos.rot[2];
                    const float frontSign = std::getenv("OPENMW_PROOF_ACTOR_VIEW_FALLOUT_FRONT") != nullptr ? -1.f : 1.f;
                    if (useFaceAxisCamera)
                    {
                        offsetX = faceAxis.x() * frontDistance;
                        offsetY = faceAxis.y() * frontDistance;
                    }
                    else
                    {
                        offsetX = std::sin(actorYaw) * frontDistance * frontSign;
                        offsetY = std::cos(actorYaw) * frontDistance * frontSign;
                    }
                    float orbitDegrees = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_DEGREES", 0.f);
                    if (!proofActorViewOrbitDegrees.empty())
                    {
                        const std::size_t orbitIndex = std::min(
                            proofScreenshotFrameIndex, proofActorViewOrbitDegrees.size() - 1);
                        orbitDegrees = proofActorViewOrbitDegrees[orbitIndex];
                    }
                    if (std::abs(orbitDegrees) > 1e-3f)
                    {
                        const float orbitRadians = orbitDegrees * static_cast<float>(osg::PI) / 180.f;
                        const float cosOrbit = std::cos(orbitRadians);
                        const float sinOrbit = std::sin(orbitRadians);
                        const float rotatedX = offsetX * cosOrbit - offsetY * sinOrbit;
                        const float rotatedY = offsetX * sinOrbit + offsetY * cosOrbit;
                        offsetX = rotatedX;
                        offsetY = rotatedY;
                    }
                    const float openMwForwardX = std::sin(actorYaw);
                    const float openMwForwardY = std::cos(actorYaw);
                    const float falloutForwardX = -openMwForwardX;
                    const float falloutForwardY = -openMwForwardY;
                    const float offsetLength = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                    const float cameraDirX = offsetLength > 0.f ? offsetX / offsetLength : 0.f;
                    const float cameraDirY = offsetLength > 0.f ? offsetY / offsetLength : 0.f;
                    const float openMwFrontDot = cameraDirX * openMwForwardX + cameraDirY * openMwForwardY;
                    const float falloutFrontDot = cameraDirX * falloutForwardX + cameraDirY * falloutForwardY;
                    Log(Debug::Info) << "FNV/ESM4 proof: actor-facing camera offset target=\"" << proofSayActor
                                     << "\" actorYaw=" << actorYaw << " frontDistance=" << frontDistance
                                     << " frontSign=" << frontSign
                                     << " useFaceAxisCamera=" << useFaceAxisCamera
                                     << " orbitDegrees=" << orbitDegrees
                                     << " offset=(" << offsetX << "," << offsetY << "," << offsetZ
                                     << ") cameraDir=(" << cameraDirX << "," << cameraDirY
                                     << ") openMwFrontDot=" << openMwFrontDot
                                     << " falloutFrontDot=" << falloutFrontDot;
                }
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                const osg::Vec3f targetPos(
                    static_cast<float>(actorAim.x() + offsetX), static_cast<float>(actorAim.y() + offsetY),
                    static_cast<float>(cameraZ));
                if (!staticDialogueCamera)
                    player = mWorld->moveObject(player, targetPos, true, true);
                const ESM::Position& playerPos = player.getRefData().getPosition();
                const float yawToActor = static_cast<float>(
                    std::atan2(actorAim.x() - targetPos.x(), actorAim.y() - targetPos.y()));
                if (!staticDialogueCamera)
                    mWorld->rotateObject(player, osg::Vec3f(0.f, 0.f, -yawToActor));
                if (MWRender::Camera* camera = mWorld->getCamera())
                {
                    osg::Vec3d proofCameraPos;
                    if (staticDialogueCamera)
                    {
                        const osg::Vec3d cameraTarget(actorAim);
                        const osg::Vec3d cameraPos(targetPos.x(), targetPos.y(), targetPos.z());
                        proofCameraPos = cameraPos;
                        const float dx = static_cast<float>(cameraTarget.x() - cameraPos.x());
                        const float dy = static_cast<float>(cameraTarget.y() - cameraPos.y());
                        const float dz = static_cast<float>(cameraTarget.z() - cameraPos.z());
                        const float horizontal = std::sqrt(dx * dx + dy * dy);
                        camera->setMode(MWRender::Camera::Mode::Static, true);
                        camera->setStaticPosition(cameraPos);
                        camera->setPitch(static_cast<float>(std::atan2(dz, horizontal)), true);
                        camera->setYaw(static_cast<float>(std::atan2(dx, dy)), true);
                        camera->setRoll(0.f);
                        camera->updateCamera();
                    }
                    else
                    {
                        camera->attachTo(player);
                        camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
                        camera->setPreferredCameraDistance(cameraDistance);
                        camera->update(0.f, false);
                        const osg::Vec3d initialCameraPos = camera->getPosition();
                        const float dx = static_cast<float>(actorAim.x() - initialCameraPos.x());
                        const float dy = static_cast<float>(actorAim.y() - initialCameraPos.y());
                        const float dz = static_cast<float>(actorAim.z() - initialCameraPos.z());
                        const float horizontal = std::sqrt(dx * dx + dy * dy);
                        camera->setPitch(static_cast<float>(std::atan2(dz, horizontal)) + cameraPitch, true);
                        camera->setYaw(yawToActor, true);
                        camera->setRoll(0.f);
                        camera->updateCamera();
                        proofCameraPos = camera->getPosition();
                    }
                    const osg::Vec3d cameraPos = camera->getPosition();
                    Log(Debug::Info) << "FNV/ESM4 proof: aligned player camera to actor target=\""
                                     << proofSayActor << "\" playerPos=(" << playerPos.pos[0] << ","
                                     << playerPos.pos[1] << "," << playerPos.pos[2] << ") actorPos=("
                                     << actorPos.pos[0] << "," << actorPos.pos[1] << "," << actorPos.pos[2]
                                     << ") yawToActor=" << yawToActor << " cameraPos=(" << cameraPos.x() << ","
                                     << cameraPos.y() << "," << cameraPos.z() << ") cameraDistance="
                                     << cameraDistance << " cameraPitch=" << camera->getPitch()
                                     << " cameraYaw=" << camera->getYaw() << " staticDialogueCamera="
                                     << staticDialogueCamera;
                    proofActorCameraAligned = true;
                    proofActorCameraAlignedFrame = static_cast<int>(frameNumber);

                    if (std::getenv("OPENMW_PROOF_LOG_NEARBY_REFS") != nullptr)
                    {
                        const float radius = readProofFloat("OPENMW_PROOF_NEARBY_REF_RADIUS", 450.f);
                        const float radiusSq = radius * radius;
                        int nearbyLogged = 0;
                        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
                        {
                            if (cellstore == nullptr)
                                continue;
                            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                                if (ptr.isEmpty())
                                    return true;
                                const ESM::Position& pos = ptr.getRefData().getPosition();
                                const float dx = pos.pos[0] - static_cast<float>(proofCameraPos.x());
                                const float dy = pos.pos[1] - static_cast<float>(proofCameraPos.y());
                                const float dz = pos.pos[2] - static_cast<float>(proofCameraPos.z());
                                if (dx * dx + dy * dy + dz * dz > radiusSq)
                                    return true;

                                const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
                                const ESM::RefId baseId = ptr.getCellRef().getRefId();
                                std::string name;
                                try
                                {
                                    name = std::string(ptr.getClass().getName(ptr));
                                }
                                catch (const std::exception&)
                                {
                                }
                                VFS::Path::Normalized model;
                                try
                                {
                                    model = ptr.getClass().getCorrectedModel(ptr);
                                }
                                catch (const std::exception& e)
                                {
                                    Log(Debug::Warning) << "FNV/ESM4 proof: nearby ref model lookup failed for "
                                                        << ptr.toString() << ": " << e.what();
                                }
                                unsigned int nodeMask = 0;
                                osg::Vec3d scenePos;
                                if (ptr.getRefData().getBaseNode())
                                {
                                    nodeMask = ptr.getRefData().getBaseNode()->getNodeMask();
                                    scenePos = ptr.getRefData().getBaseNode()->getPosition();
                                }
                                Log(Debug::Info) << "FNV/ESM4 proof: nearby ref ref="
                                                 << refNum.toString("FormId:") << " base="
                                                 << baseId.toDebugString() << " type=" << ptr.getType()
                                                 << " actor=" << ptr.getClass().isActor() << " name=\"" << name
                                                 << "\" model=" << model.value() << " pos=(" << pos.pos[0]
                                                 << "," << pos.pos[1] << "," << pos.pos[2] << ") scenePos=("
                                                 << scenePos.x() << "," << scenePos.y() << "," << scenePos.z()
                                                 << ") nodeMask=0x" << std::hex << nodeMask << std::dec
                                                 << " ptr=" << ptr.toString();
                                ++nearbyLogged;
                                return nearbyLogged < 120;
                            });
                            if (nearbyLogged >= 120)
                                break;
                        }
                        Log(Debug::Info) << "FNV/ESM4 proof: nearby ref dump complete count=" << nearbyLogged
                                         << " radius=" << radius;
                    }
                }
            }
        }

        if (!proofSayQueued && !proofActor.isEmpty() && proofSayTopic != nullptr && *proofSayTopic != '\0'
            && mDialogueManager != nullptr)
        {
            const bool said = mDialogueManager->say(proofActor, ESM::RefId::stringRefId(proofSayTopic));
            Log(Debug::Info) << "FNV/ESM4 proof: actor dialogue say topic \"" << proofSayTopic << "\" result="
                             << said << " at frame " << frameNumber;
        }
        else if (!proofSayQueued && !proofActor.isEmpty() && proofSayFile != nullptr && *proofSayFile != '\0')
        {
            Log(Debug::Info) << "FNV/ESM4 proof: playing actor proof voice \"" << proofSayFile << "\" for "
                             << proofActor.toString() << " at frame " << frameNumber;
            mSoundManager->say(proofActor, VFS::Path::Normalized(proofSayFile));
        }
        else if (!proofSayQueued && proofSayFile != nullptr && *proofSayFile != '\0')
        {
            Log(Debug::Info) << "FNV/ESM4 proof: playing proof voice \"" << proofSayFile << "\" at frame "
                             << frameNumber;
            mSoundManager->say(VFS::Path::Normalized(proofSayFile));
        }
        proofSayQueued = true;
    }

    const auto executeProofTimedScript = [&](int targetFrame, const char* pathEnv, bool& executed) {
        if (executed || targetFrame < 0 || frameNumber < static_cast<unsigned>(targetFrame) || !proofRunning
            || !proofWorldReady)
            return;
        const char* scriptPath = std::getenv(pathEnv);
        if (scriptPath == nullptr || *scriptPath == '\0')
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: timed script env " << pathEnv
                                << " is empty at frame " << frameNumber;
            executed = true;
            return;
        }
        Log(Debug::Info) << "FNV/ESM4 proof: executing timed script " << pathEnv << "=\""
                         << scriptPath << "\" at frame " << frameNumber;
        mWindowManager->executeInConsole(std::filesystem::path(scriptPath));
        executed = true;
    };
    executeProofTimedScript(proofTimedScript1Frame, "OPENMW_PROOF_TIMED_SCRIPT_1", proofTimedScript1Executed);
    executeProofTimedScript(proofTimedScript2Frame, "OPENMW_PROOF_TIMED_SCRIPT_2", proofTimedScript2Executed);

    const bool proofScreenshotFrameReached = proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    const bool proofScreenshotReadyFramesReached
        = !proofScreenshotReadyQueued && proofScreenshotReadyFrames >= 0 && proofWorldReadyFrames >= proofScreenshotReadyFrames;
    const int proofActorAlignedScreenshotDelay = getProofFrame("OPENMW_PROOF_ACTOR_ALIGNED_SCREENSHOT_DELAY");
    const int proofActorAlignedScreenshotMinFrame = getProofFrame("OPENMW_PROOF_ACTOR_ALIGNED_SCREENSHOT_MIN_FRAME");
    const bool proofActorAlignedScreenshotReached = !proofActorAlignedScreenshotQueued && proofActorCameraAligned
        && proofActorAlignedScreenshotDelay >= 0 && proofActorCameraAlignedFrame >= 0
        && frameNumber >= static_cast<unsigned>(proofActorCameraAlignedFrame + proofActorAlignedScreenshotDelay)
        && (proofActorAlignedScreenshotMinFrame < 0
            || frameNumber >= static_cast<unsigned>(proofActorAlignedScreenshotMinFrame));
    if ((proofScreenshotFrameReached || proofScreenshotReadyFramesReached || proofActorAlignedScreenshotReached)
        && (mScreenCaptureHandler != nullptr || mWorld != nullptr))
    {
        if (!proofWorldReady && !proofScreenshotWaitLogged)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: waiting to capture screenshot until world is ready state="
                             << static_cast<int>(mStateManager->getState()) << " loadingGui=" << proofLoadingGui
                             << " frame=" << frameNumber;
            proofScreenshotWaitLogged = true;
        }
        if (!proofWorldReady)
        {
            mViewer->renderingTraversals();
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            return true;
        }
        if (proofRequiresActorForScreenshot && !proofActorCameraAligned)
        {
            if (!proofActorScreenshotWaitLogged)
            {
                const char* proofSayActor = std::getenv("OPENMW_PROOF_SAY_ACTOR");
                Log(Debug::Info) << "FNV/ESM4 proof: waiting to capture screenshot until actor is resolved target=\""
                                 << (proofSayActor != nullptr ? proofSayActor : "") << "\" frame=" << frameNumber
                                 << " screenshotIndex=" << proofScreenshotFrameIndex;
                proofActorScreenshotWaitLogged = true;
            }
            mViewer->renderingTraversals();
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            return true;
        }

        Log(Debug::Info) << "FNV/ESM4 proof: capturing native screenshot at frame " << frameNumber;
        osg::ref_ptr<osg::Image> screenshot = new osg::Image;
        mWorld->screenshot(screenshot.get(), 1280, 720);
        const std::filesystem::path fileName = SceneUtil::writeScreenshotToFile(
            mCfgMgr.getScreenshotPath(), Settings::general().mScreenshotFormat, *screenshot);
        if (fileName.empty())
            Log(Debug::Warning) << "FNV/ESM4 proof: native screenshot write failed at frame " << frameNumber;
        else
            Log(Debug::Info) << mCfgMgr.getScreenshotPath() / fileName << " has been saved";
        if (proofScreenshotFrameReached)
            ++proofScreenshotFrameIndex;
        if (proofScreenshotReadyFramesReached)
            proofScreenshotReadyQueued = true;
        if (proofActorAlignedScreenshotReached)
            proofActorAlignedScreenshotQueued = true;
    }

    mViewer->renderingTraversals();

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(false)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
    // ## VR_PATCH BEGIN
    , mVrGUIManager(nullptr)
    , mXrInstance(nullptr)
// ## VR_PATCH END
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
        mScreenCaptureOperation->stop();

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;
    // ## VR_PATCH BEGIN
    mVrViewer = nullptr;
    mCallbackManager = nullptr;
    mVrGUIManager = nullptr;
    mXrSession = nullptr;
    mXrInstance = nullptr;
    // ## VR_PATCH END

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    // ## VR_PATCH BEGIN
    if (VR::getVR())
        // MSAA needs to happen in offscreen buffers.
        antialiasing = 0;
    // ## VR_PATCH END


    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                        << antialiasing / 2;
                    antialiasing /= 2;
                    Settings::video().mAntialiasing.set(antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        // Since we use physical resolution internally, we have to create the window with scaled resolution,
        // but we can't get the scale before the window exists, so instead we have to resize aftewards.
        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(mWindow, &dw, &dh);
        if (dw != w || dh != h)
        {
            SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
        traits->vsync = 0;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
        if (!graphicsWindow->valid())
            throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    // ## VR_PATCH BEGIN
    if (VR::getVR())
        realizeOperations->add(new InitializeVrOperation(this));
    // ## VR_PATCH END

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = Stereo::Position::fromMWUnits(leftEyeOffset),
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = Stereo::Position::fromMWUnits(rightEyeOffset),
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(mViewer, stereoEnabled, Settings::camera().mNearClip,
        Settings::camera().mViewingDistance, static_cast<unsigned>(Settings::video().mAntialiasing));

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createWindow();

    // ## VR_PATCH BEGIN
    mCallbackManager = std::make_unique<Misc::CallbackManager>(mViewer);
    // ## VR_PATCH END

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap, Settings::general().mAnisotropy);
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();
    bool shadersSupported = exts.glslLanguageVersion >= 1.2f;

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mViewer, guiRoot, mResourceSystem.get(),
        mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
        Version::getOpenmwVersionDescription(), shadersSupported, mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        configureVRPreScene(keybinderUser, keybinderUserExists, userGameControllerdb, gameControllerdb);
    }
    else
    {
        mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
            keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    }
    // ## VR_PATCH END
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // ## VR_PATCH BEGIN
    // In VR, the MWRender::Camera object needs to be created right away to apply tracking updates even before the scene and
    // RenderingManager has been created.
    auto camera = std::make_unique<MWRender::Camera>(mViewer->getCamera());
    // ## VR_PATCH END
    //  Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue, std::move(camera));
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);

    // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        configureVRScene();
    }
    // ## VR_PATCH END

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader(
        [gmst, misses = std::set<std::string, std::less<>>()](std::string_view gmstName) mutable {
            const ESM::GameSetting* res = gmst->search(gmstName);
            if (res && res->mValue.getType() == ESM::VT_String)
                return res->mValue.getString();
            else
            {
                if (misses.count(gmstName) == 0)
                {
                    misses.emplace(gmstName);
                    Log(Debug::Error) << "GMST " << gmstName << " not found";
                }
                return std::string("GMST:") + std::string(gmstName);
            }
        });

    mWindowManager->setStore(mWorld->getStore());
    mWindowManager->initUI();

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->init();

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->setReleaseContextAtEndOfFrameHint(false);

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    Log(Debug::Info) << "FNV/ESM4 proof: entering prepareEngine skipMenu=" << mSkipMenu << " newGame=" << mNewGame
                     << " saveFile=\"" << mSaveGameFile.string() << "\"";
    try
    {
        prepareEngine();
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "FNV/ESM4 proof: prepareEngine failed: " << e.what();
        throw;
    }
    Log(Debug::Info) << "FNV/ESM4 proof: prepareEngine complete skipMenu=" << mSkipMenu << " newGame=" << mNewGame
                     << " saveFile=\"" << mSaveGameFile.string() << "\"";

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

           // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        // Mask_GUI gets re-enabled at some point.
        mViewer->getCamera()->setCullMask(mViewer->getCamera()->getCullMask() & ~(MWRender::VisMask::Mask_GUI));
    }

           // ## VR_PATCH END
    //  Start the game
    if (!mSaveGameFile.empty())
    {
        Log(Debug::Info) << "FNV/ESM4 proof: loading save from command line \"" << mSaveGameFile.string() << "\"";
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        Log(Debug::Info) << "FNV/ESM4 proof: starting command-line game bypass=" << (!mNewGame);
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, dt))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

        frameRateLimiter.limit();
    }

    mLuaWorker->join();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}

// ## VR_PATCH BEGIN
void OMW::Engine::configureVRGraphics(osg::GraphicsContext* gc)
{
    // Interaction profiles need to be configured before XR::Instance, to enable all relevant extensions
    configureVRInputProfiles();

    mXrInstance = std::make_unique<XR::Instance>(gc, mWindow);
    mXrSession = mXrInstance->createSession();
    if (mXrSession->appShouldShareDepthInfo())
        mSelectDepthFormatOperation->setSupportedFormats(mXrInstance->platform().supportedDepthFormats());
    mSelectColorFormatOperation->setSupportedFormats({ mXrInstance->platform().supportedColorFormats() });
}

void OMW::Engine::configureVRInputProfiles()
{
    const std::string xrinputuserdefault = mCfgMgr.getUserConfigPath().string() + "/openxrinteractionprofiles.xml";
    const std::string xrinputlocaldefault = mCfgMgr.getLocalPath().string() + "/openxrinteractionprofiles.xml";
    const std::string xrinputglobaldefault = mCfgMgr.getGlobalPath().string() + "/openxrinteractionprofiles.xml";

    std::string xrInteractionProfiles;
    if (std::filesystem::exists(xrinputuserdefault))
        xrInteractionProfiles = xrinputuserdefault;
    else if (std::filesystem::exists(xrinputlocaldefault))
        xrInteractionProfiles = xrinputlocaldefault;
    else if (std::filesystem::exists(xrinputglobaldefault))
        xrInteractionProfiles = xrinputglobaldefault;
    else
        xrInteractionProfiles = ""; // if it doesn't exist, pass in an empty string

    std::string defaulXrInteractionProfiles;
    if (std::filesystem::exists(xrinputlocaldefault))
        defaulXrInteractionProfiles = xrinputlocaldefault;
    else if (std::filesystem::exists(xrinputglobaldefault))
        defaulXrInteractionProfiles = xrinputglobaldefault;
    else
        defaulXrInteractionProfiles = ""; // if it doesn't exist, pass in an empty string

    Log(Debug::Verbose) << "xrinteractionprofiles user: " << xrinputuserdefault;
    Log(Debug::Verbose) << "xrinteractionprofiles local: " << xrinputlocaldefault;
    Log(Debug::Verbose) << "xrinteractionprofiles global: " << xrinputglobaldefault;

    XR::loadInteractionProfiles(xrInteractionProfiles, defaulXrInteractionProfiles);
}

void OMW::Engine::configureVRPreScene(const std::filesystem::path& userFile, bool userFileExists,
    const std::filesystem::path& userControllerBindingsFile, const std::filesystem::path& controllerBindingsFile)
{
    VR::setLeftHandedMode(Settings::vr().mLeftHandedMode);

    // Set up enough of VR to view the intro cinematic/loading screen
    mVrViewer = std::make_unique<VR::Viewer>(mXrSession, mViewer);
    mVrViewer->configureCallbacks();
    auto cullMask = ~(MWRender::VisMask::Mask_UpdateVisitor | MWRender::VisMask::Mask_SimpleWater);
    cullMask &= ~MWRender::VisMask::Mask_GUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI_NonIntersectable;
    mViewer->getCamera()->setCullMask(cullMask);
    mViewer->getCamera()->setCullMaskLeft(cullMask);
    mViewer->getCamera()->setCullMaskRight(cullMask);

    mInputManager = std::make_unique<MWVR::VRInputManager>(mWindow, mViewer, mScreenCaptureHandler, userFile,
        userFileExists, userControllerBindingsFile, controllerBindingsFile, mGrab);
    mVrGUIManager = std::make_unique<MWVR::VRGUIManager>(mViewer->getSceneData()->asGroup());

    // Before the RenderingManager and associated infrastructure is created, we need to render directly into the stereo framebuffer
    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(true);
}

void OMW::Engine::configureVRScene() 
{
    // Rendering should now be done in the post-processor FBOs
    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(false);
    // Fully initialize with integration into the rendering manager
    mVrGUIManager->initScene();
}
// ## VR_PATCH END

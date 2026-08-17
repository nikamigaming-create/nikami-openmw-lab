#include "radialmenu.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>

#include <osg/Group>
#include <osg/LightModel>
#include <osg/Material>
#include <osg/MatrixTransform>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <limits>
#include <variant>

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/itemwidget.hpp"
#include "../mwgui/quickkeysmenu.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/falloutcombat.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/vr/session.hpp>
#include <components/esm3/quickkeys.hpp>
#include <components/vfs/pathutil.hpp>

#include "openxrinput.hpp"
#include "vranimation.hpp"
#include "vrutil.hpp"

namespace MWVR
{

    namespace
    {
        class StaticWeaponPreviewVisitor : public osg::NodeVisitor
        {
        public:
            StaticWeaponPreviewVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                node.setUpdateCallback(nullptr);
                traverse(node);
            }
        };

        bool actionPressed(const std::vector<std::string>& ids)
        {
            auto& actionSet = OpenXRInput::instance().getActionSet(MWActionSet::Actions);
            for (const std::string& id : ids)
            {
                try
                {
                    const std::optional<XR::InputAction::Value> value = actionSet.getValue(id);
                    if (!value)
                        continue;
                    if (const bool* pressed = std::get_if<bool>(&*value); pressed != nullptr && *pressed)
                        return true;
                    if (const float* amount = std::get_if<float>(&*value); amount != nullptr && *amount > 0.55f)
                        return true;
                }
                catch (const std::out_of_range&)
                {
                }
            }
            return false;
        }

        std::vector<std::string> radialActionIds()
        {
            if (VR::getLeftHandedMode())
                return { "/user/hand/left/input/b/click", "/user/hand/left/input/y/click",
                    "/user/hand/right/input/trackpad/click", "/user/hand/right/input/trackpad/down" };
            return { "/user/hand/right/input/b/click", "/user/hand/left/input/trackpad/click",
                "/user/hand/left/input/trackpad/down" };
        }

        std::vector<std::string> selectingGripActionIds()
        {
            const std::string hand = VR::getLeftHandedMode() ? "/user/hand/left" : "/user/hand/right";
            return { hand + "/input/squeeze/value", hand + "/input/squeeze/click",
                hand + "/input/grip/value", hand + "/input/grip/click" };
        }

    }

    RadialMenu::RadialMenu(int w, int h, MWGui::QuickKeysMenu* qkm, osg::Group* sceneRoot,
        Resource::ResourceSystem* resourceSystem)
        : WindowBase("openmw_vr_radial_menu.layout")
        , mWidth(w)
        , mHeight(h)
        , mQkm(qkm)
        , mResourceSystem(resourceSystem)
        , mSceneRoot(sceneRoot)
        , mWeaponWheelRoot(new osg::Group)
    {
        mWeaponWheelRoot->setName("Fallout VR hand weapon wheel");
        mWeaponWheelRoot->setNodeMask(0u);
        if (mSceneRoot.valid())
            mSceneRoot->addChild(mWeaponWheelRoot);
        initMenu();
        updateMenu();
    }

    RadialMenu::~RadialMenu()
    {
        clearWeaponWheel();
        if (mSceneRoot.valid() && mWeaponWheelRoot)
            mSceneRoot->removeChild(mWeaponWheelRoot);
    }

    void RadialMenu::onResChange(int w, int h)
    {
        mWidth = w;
        mHeight = h;

        updateMenu();
    }

    void RadialMenu::setVisible(bool visible)
    {
        if (visible)
        {
            updateMenu();
            rebuildWeaponWheel();
        }
        else
        {
            mWheelVisible = false;
            mHoveredWeapon = -1;
            mOpenAmount = 0.f;
            if (mWeaponWheelRoot)
                mWeaponWheelRoot->setNodeMask(0u);
        }

        // The Fallout wheel is scene-native 3D geometry. Keeping even an empty MyGUI radial layout visible causes
        // the legacy VR GUI quad to appear as a large black trapezoid in front of the hands.
        Layout::setVisible(visible && !mWheelVisible);
    }

    void RadialMenu::onFrame(float dt)
    {
        if (mWheelVisible)
            updateWeaponWheel(dt);
    }

    void RadialMenu::close()
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_RadialMenu);
    }

    void RadialMenu::onButtonClicked(MyGUI::Widget* sender)
    {
        auto userString = sender->getUserString("QuickKey");
        close();
        mQkm->activateQuickKey(std::stoi(std::string(userString)));
    }

    bool RadialMenu::exit()
    {
        return MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Running;
    }

    void RadialMenu::clearWeaponWheel()
    {
        if (mWeaponWheelRoot)
            mWeaponWheelRoot->removeChildren(0, mWeaponWheelRoot->getNumChildren());
        mWheelWeapons.clear();
        mHoveredWeapon = -1;
    }

    void RadialMenu::rebuildWeaponWheel()
    {
        clearWeaponWheel();
        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (world == nullptr || mResourceSystem == nullptr || !mSceneRoot.valid())
            return;

        MWWorld::Ptr player = world->getPlayerPtr();
        if (player.isEmpty() || !player.getClass().hasInventoryStore(player))
            return;

        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        std::set<ESM::RefId> added;
        std::vector<MWWorld::Ptr> weapons;
        for (MWWorld::ContainerStoreIterator it = inventory.begin(); it != inventory.end(); ++it)
        {
            if (it->getType() != ESM4::Weapon::sRecordId || it->getCellRef().getCount() <= 0)
                continue;
            const ESM::RefId id = it->getCellRef().getRefId();
            if (added.insert(id).second)
                weapons.push_back(*it);
        }
        std::stable_sort(weapons.begin(), weapons.end(), [](const MWWorld::Ptr& left, const MWWorld::Ptr& right) {
            return left.getClass().getName(left) < right.getClass().getName(right);
        });

        for (const MWWorld::Ptr& weapon : weapons)
        {
            const ESM4::Weapon* const record = weapon.get<ESM4::Weapon>()->mBase;
            if (record == nullptr || record->mModel.empty())
                continue;
            try
            {
                osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
                transform->setName("Fallout VR wheel weapon " + record->mEditorId);
                const VFS::Path::Normalized modelPath
                    = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(record->mModel));
                osg::ref_ptr<osg::Node> model
                    = mResourceSystem->getSceneManager()->getInstance(modelPath, transform);
                // The wheel is a UI surface and must stay readable independently of the world cell's light list.
                // Preserve each asset's diffuse texture while supplying a small, deterministic ambient/emissive fill.
                osg::StateSet* const previewState = transform->getOrCreateStateSet();
                previewState->setMode(GL_LIGHTING, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                osg::ref_ptr<osg::LightModel> previewLightModel = new osg::LightModel;
                previewLightModel->setAmbientIntensity(osg::Vec4f(0.65f, 0.65f, 0.65f, 1.f));
                previewState->setAttributeAndModes(
                    previewLightModel, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                osg::ref_ptr<osg::Material> previewMaterial = new osg::Material;
                // Official weapon preview meshes commonly carry black vertex colors. The
                // wheel's authored full-bright material must own its color so those vertex
                // colors cannot turn otherwise textured entries into black silhouettes.
                previewMaterial->setColorMode(osg::Material::OFF);
                previewMaterial->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                previewMaterial->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                previewMaterial->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.18f, 0.18f, 0.18f, 1.f));
                previewState->setAttributeAndModes(
                    previewMaterial, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                // A wheel entry is a static preview, not another live weapon animation surface. Keep every preview
                // instance controller-free so it cannot participate in or visually masquerade as the held rig.
                StaticWeaponPreviewVisitor staticPreview;
                model->accept(staticPreview);
                // Material values are compiled into the object shader's uniforms. Rebuild from the preview root so
                // the override above is part of the shader state instead of being shadowed by cached model uniforms.
                mResourceSystem->getSceneManager()->reinstateRemovedState(transform);
                mResourceSystem->getSceneManager()->recreateShaders(transform, "objects", true);
                transform->addCullCallback(new SceneUtil::LightListCallback);
                const osg::BoundingSphere bound = model->getBound();
                if (!bound.valid() || bound.radius() <= 1e-4f)
                    continue;
                mWeaponWheelRoot->addChild(transform);
                WheelWeapon entry;
                entry.mItem = weapon;
                entry.mTransform = transform;
                entry.mModelCenter = bound.center();
                entry.mBaseScale = std::clamp(7.f / bound.radius(), 0.08f, 4.f);
                mWheelWeapons.push_back(std::move(entry));
            }
            catch (const std::exception& error)
            {
                Log(Debug::Warning) << "FNV VR weapon wheel: skipped model=" << record->mModel
                                    << " reason=" << error.what();
            }
        }

        mWheelVisible = !mWheelWeapons.empty();
        mOpenAmount = 0.f;
        mWheelTelemetryLogged = false;
        mWheelBasisValid = false;
        mRadialWasDown = actionPressed(radialActionIds());
        mGripWasDown = actionPressed(selectingGripActionIds());
        if (mWeaponWheelRoot)
            mWeaponWheelRoot->setNodeMask(mWheelVisible ? ~0u : 0u);
        if (mWheelVisible)
            Log(Debug::Info) << "FNV VR weapon wheel: visible=1 weapons=" << mWheelWeapons.size()
                             << " anchor=offhand-grip selection=button-release-or-direct-grab centered=1";
    }

    void RadialMenu::selectWheelWeapon(int index, const char* interaction)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= mWheelWeapons.size())
            return;

        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return;
        MWWorld::Ptr player = world->getPlayerPtr();
        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        const ESM::RefId selectedId = mWheelWeapons[static_cast<std::size_t>(index)].mItem.getCellRef().getRefId();
        MWWorld::ContainerStoreIterator selected = inventory.end();
        for (MWWorld::ContainerStoreIterator it = inventory.begin(); it != inventory.end(); ++it)
        {
            if (it->getType() == ESM4::Weapon::sRecordId && it->getCellRef().getRefId() == selectedId)
            {
                selected = it;
                break;
            }
        }
        if (selected == inventory.end())
            return;

        const MWWorld::ContainerStoreIterator equippedBefore
            = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (equippedBefore != inventory.end() && equippedBefore->getCellRef().getRefId() == selectedId)
        {
            const ESM4::Weapon& weapon = *selected->get<ESM4::Weapon>()->mBase;
            Log(Debug::Info) << "FNV VR weapon wheel: selected=" << selectedId
                             << " editor=" << weapon.mEditorId << " interaction=" << interaction
                             << " inventoryIdentity=1 alreadyEquipped=1 mutation=none";
            mWheelVisible = false;
            if (mWeaponWheelRoot)
                mWeaponWheelRoot->setNodeMask(0u);
            close();
            return;
        }

        const ESM4::Weapon& weapon = *selected->get<ESM4::Weapon>()->mBase;
        const MWWorld::ESMStore& store = world->getStore();
        std::vector<ESM::FormId> ammunition;
        if (store.get<ESM4::Ammunition>().search(weapon.mAmmo) != nullptr)
            ammunition.push_back(weapon.mAmmo);
        else if (const ESM4::FormIdList* list = store.get<ESM4::FormIdList>().search(weapon.mAmmo))
            ammunition = list->mObjects;
        MWWorld::ContainerStoreIterator ammunitionItem = inventory.end();
        ESM::RefId ammunitionId;
        for (ESM::FormId candidate : ammunition)
        {
            const ESM::RefId candidateId = ESM::RefId::formIdRefId(candidate);
            for (MWWorld::ContainerStoreIterator it = inventory.begin(); it != inventory.end(); ++it)
            {
                if (it->getType() == ESM4::Ammunition::sRecordId && it->getCellRef().getCount() > 0
                    && it->getCellRef().getRefId() == candidateId)
                {
                    ammunitionItem = it;
                    ammunitionId = candidateId;
                    break;
                }
            }
            if (ammunitionItem != inventory.end())
                break;
        }

        // Publish one complete equipment state. A weapon event followed by a second ammunition event destroyed and
        // rebound the held part twice during one wheel selection, resetting its rigid fixture in mid-handoff.
        const bool equipAmmunition = ammunitionItem != inventory.end();
        inventory.equip(MWWorld::InventoryStore::Slot_CarriedRight, selected, !equipAmmunition);
        if (equipAmmunition)
        {
            inventory.equip(MWWorld::InventoryStore::Slot_Ammunition, ammunitionItem);
            inventory.setFalloutAmmoSelection(selectedId, ammunitionId);
        }
        const MWWorld::ContainerStoreIterator equipped
            = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (equipped == inventory.end() || equipped->getCellRef().getRefId() != selectedId)
        {
            Log(Debug::Error) << "FNV VR weapon wheel: selection commit failed requested=" << selectedId
                              << " interaction=" << interaction;
            return;
        }
        if (!inventory.getFalloutLoadedAmmo(selectedId).has_value())
            inventory.setFalloutLoadedAmmo(selectedId, 0);
        player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);
        MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);

        Log(Debug::Info) << "FNV VR weapon wheel: selected=" << selectedId
                         << " editor=" << weapon.mEditorId << " interaction=" << interaction
                         << " inventoryIdentity=1 equipmentEvents=1 forceStateUpdates=1";
        mWheelVisible = false;
        if (mWeaponWheelRoot)
            mWeaponWheelRoot->setNodeMask(0u);
        close();
    }

    void RadialMenu::updateWeaponWheel(float dt)
    {
        const std::optional<CachedVrControllerPose> anchor
            = getCachedVrControllerPose(VR::getLeftHandedMode() ? "right" : "left");
        const std::optional<CachedVrControllerPose> selector
            = getCachedVrControllerPose(VR::getLeftHandedMode() ? "left" : "right");
        if (!anchor || !selector)
            return;

        mOpenAmount = std::min(1.f, mOpenAmount + std::max(0.f, dt) * 7.f);
        const osg::Vec3f center = anchor->mWorldPosition;
        if (!mWheelBasisValid)
        {
            mWheelNormal = selector->mWorldPosition - center;
            if (mWheelNormal.normalize() <= 1e-5f)
                mWheelNormal.set(0.f, -1.f, 0.f);
            mWheelRight = osg::Vec3f(0.f, 0.f, 1.f) ^ mWheelNormal;
            if (mWheelRight.normalize() <= 1e-5f)
                mWheelRight.set(1.f, 0.f, 0.f);
            mWheelUp = mWheelNormal ^ mWheelRight;
            mWheelUp.normalize();
            mWheelBasisValid = true;
        }
        const osg::Vec3f& normal = mWheelNormal;
        const osg::Vec3f& wheelRight = mWheelRight;
        const osg::Vec3f& wheelUp = mWheelUp;

        constexpr std::size_t itemsPerRing = 10;
        const osg::Vec3f gripPosition = selector->mWorldPosition;
        int closestReach = -1;
        float closestReachDistance = 14.f;
        int nearestWeapon = -1;
        float nearestWeaponDistance = std::numeric_limits<float>::max();
        float maxRingRadiusError = 0.f;

        for (std::size_t i = 0; i < mWheelWeapons.size(); ++i)
        {
            WheelWeapon& entry = mWheelWeapons[i];
            const std::size_t ring = i / itemsPerRing;
            const std::size_t first = ring * itemsPerRing;
            const std::size_t ringCount = std::min(itemsPerRing, mWheelWeapons.size() - first);
            const std::size_t ringIndex = i - first;
            const float angle = -osg::PI_2 + osg::PI * 2.f * static_cast<float>(ringIndex)
                / static_cast<float>(ringCount);
            const float radius = (22.f + 15.f * static_cast<float>(ring)) * mOpenAmount;
            const osg::Vec3f radial = wheelRight * std::cos(angle) + wheelUp * std::sin(angle);
            const osg::Vec3f tangent = -wheelRight * std::sin(angle) + wheelUp * std::cos(angle);
            entry.mWorldCenter = center + radial * radius;
            maxRingRadiusError = std::max(
                maxRingRadiusError, std::abs((entry.mWorldCenter - center).length() - radius));

            const ESM4::Weapon& record = *entry.mItem.get<ESM4::Weapon>()->mBase;
            const osg::Vec3f sourceAim = MWMechanics::isFalloutMeleeAnimationType(record.mData.animationType)
                ? osg::Vec3f(0.f, 1.f, 0.f) : osg::Vec3f(1.f, 0.f, 0.f);
            osg::Quat orientation;
            orientation.makeRotate(sourceAim, tangent);
            const float gripDistance = (entry.mWorldCenter - gripPosition).length();
            if (gripDistance < nearestWeaponDistance)
            {
                nearestWeaponDistance = gripDistance;
                nearestWeapon = static_cast<int>(i);
            }
            if (gripDistance < closestReachDistance)
            {
                closestReachDistance = gripDistance;
                closestReach = static_cast<int>(i);
            }
            const float displayScale = entry.mBaseScale * mOpenAmount;
            entry.mTransform->setMatrix(osg::Matrix::translate(-entry.mModelCenter)
                * osg::Matrix::scale(displayScale, displayScale, displayScale)
                * osg::Matrix::rotate(orientation) * osg::Matrix::translate(entry.mWorldCenter));
        }

        if (!mWheelTelemetryLogged && mOpenAmount >= 1.f)
        {
            mWheelTelemetryLogged = true;
            Log(Debug::Info) << "FNV VR weapon wheel fixture: centerWorld=(" << center.x() << ',' << center.y()
                             << ',' << center.z() << ") offhandWorld=(" << anchor->mWorldPosition.x()
                             << ',' << anchor->mWorldPosition.y() << ','
                             << anchor->mWorldPosition.z() << ") centerError="
                             << (center - anchor->mWorldPosition).length()
                             << " planeDotNormalRight=" << std::abs(normal * wheelRight)
                             << " planeDotNormalUp=" << std::abs(normal * wheelUp)
                             << " planeDotRightUp=" << std::abs(wheelRight * wheelUp)
                             << " maxRingRadiusError=" << maxRingRadiusError
                             << " selectorWorld=(" << gripPosition.x() << ',' << gripPosition.y() << ','
                             << gripPosition.z() << ')'
                             << " items=" << mWheelWeapons.size() << " centered=1";
            for (std::size_t i = 0; i < mWheelWeapons.size(); ++i)
            {
                const ESM4::Weapon& weapon = *mWheelWeapons[i].mItem.get<ESM4::Weapon>()->mBase;
                const osg::Vec3f delta = mWheelWeapons[i].mWorldCenter - center;
                Log(Debug::Info) << "FNV VR weapon wheel fixture item=" << i << " editor=" << weapon.mEditorId
                                 << " centerWorld=(" << mWheelWeapons[i].mWorldCenter.x() << ','
                                 << mWheelWeapons[i].mWorldCenter.y() << ','
                                 << mWheelWeapons[i].mWorldCenter.z() << ") radius=" << delta.length()
                                 << " modelScale=" << mWheelWeapons[i].mBaseScale;
            }
        }

        const int hovered = closestReach;
        if (hovered >= 0 && hovered != mHoveredWeapon)
        {
            mHoveredWeapon = hovered;
            if (hovered >= 0)
            {
                const ESM4::Weapon& weapon
                    = *mWheelWeapons[static_cast<std::size_t>(hovered)].mItem.get<ESM4::Weapon>()->mBase;
                Log(Debug::Info) << "FNV VR weapon wheel: hover=" << weapon.mEditorId
                                 << " mode=reach"
                                 << " gripDistance="
                                 << (mWheelWeapons[static_cast<std::size_t>(hovered)].mWorldCenter - gripPosition).length()
                                 << " threshold=14";
            }
        }
        if (mHoveredWeapon >= 0)
        {
            WheelWeapon& entry = mWheelWeapons[static_cast<std::size_t>(mHoveredWeapon)];
            osg::Matrix matrix = entry.mTransform->getMatrix();
            const osg::Vec3f position = matrix.getTrans();
            matrix.setTrans(osg::Vec3f());
            matrix.postMult(osg::Matrix::scale(1.22f, 1.22f, 1.22f));
            matrix.setTrans(position);
            entry.mTransform->setMatrix(matrix);
        }

        const bool gripDown = actionPressed(selectingGripActionIds());
        if (gripDown && !mGripWasDown)
        {
            Log(Debug::Info) << "FNV VR weapon wheel calibration: selectorWorld=(" << gripPosition.x() << ','
                             << gripPosition.y() << ',' << gripPosition.z() << ") nearestIndex=" << nearestWeapon
                             << " nearestDistance=" << nearestWeaponDistance << " mutation=none";
        }
        if (gripDown && !mGripWasDown && mHoveredWeapon >= 0)
        {
            Log(Debug::Info) << "FNV VR weapon wheel grab fixture: index=" << mHoveredWeapon
                             << " currentReachIndex=" << closestReach
                             << " gripCenterDistance=" << closestReachDistance << " threshold=14 pass=1";
            selectWheelWeapon(mHoveredWeapon, "direct-grab");
            return;
        }
        mGripWasDown = gripDown;

        const bool radialDown = actionPressed(radialActionIds());
        if (mRadialWasDown && !radialDown)
        {
            if (mHoveredWeapon >= 0)
                selectWheelWeapon(mHoveredWeapon, "radial-release");
            else
                close();
            return;
        }
        mRadialWasDown = mRadialWasDown || radialDown;
    }

    void RadialMenu::initMenu()
    {
        for (uint32_t i = 0; i < 10; i++)
        {
            std::string buttonId = "QuickKey" + std::to_string(i + 1);
            MyGUI::Widget* button = nullptr;
            getWidget(button, buttonId);

            auto size = button->getSize();
            float angleRadian = static_cast<float>(i) * 2.f * osg::PIf / 10.f;
            MyGUI::FloatPoint position = { std::cos(angleRadian) * 150.f, std::sin(angleRadian) * 150.f };

            MyGUI::FloatPoint offset = { static_cast<float>(size.width) / 2.f, static_cast<float>(size.height) / 2.f };

            position += MyGUI::FloatPoint{ 512.f / 2.f, 512.f / 2.f } - offset;

            button->setPosition(MyGUI::IntPoint{ static_cast<int>(position.left), static_cast<int>(position.top) });
            button->eventMouseButtonClick += MyGUI::newDelegate(this, &RadialMenu::onButtonClicked);
            button->setUserString("QuickKey", std::to_string(i + 1));
            button->setVisible(true);
        }
    }

    void RadialMenu::updateMenu()
    {
        for (uint32_t i = 0; i < 10; i++)
        {
            std::string buttonId = "QuickKey" + std::to_string(i + 1);
            MWGui::ItemWidget* button = nullptr;
            getWidget(button, buttonId);
            button->clearUserStrings();
            button->setUserString("QuickKey", std::to_string(i + 1));
            button->setItem(MWWorld::Ptr());

            while (button->getChildCount()) // Destroy number label
                MyGUI::Gui::getInstance().destroyWidget(button->getChildAt(0));

            auto* key = mQkm->keyAt(i);

            for (const auto& userString : key->button->getUserStrings())
            {
                button->setUserString(userString.first, userString.second);
            }

            if (key->type == ESM::QuickKeys::Type::HandToHand)
            {
                MyGUI::ImageBox* image = button->createWidget<MyGUI::ImageBox>(
                    "ImageBox", MyGUI::IntCoord(14, 13, 32, 32), MyGUI::Align::Default);

                image->setImageTexture("icons\\k\\stealth_handtohand.dds");
                image->setNeedMouseFocus(false);
            }
            else if (key->type == ESM::QuickKeys::Type::Unassigned)
            {
                MyGUI::TextBox* textBox = button->createWidgetReal<MyGUI::TextBox>(
                    "SandText", MyGUI::FloatCoord(0, 0, 1, 1), MyGUI::Align::Default);

                textBox->setTextAlign(MyGUI::Align::Center);
                textBox->setCaption(MyGUI::utility::toString(key->index));
                textBox->setNeedMouseFocus(false);
            }
            else
            {
                button->setIcon(key->button->getIcon());
                if (key->button->hasFrame())
                    button->setFrame(key->button->getFrame(), key->button->getFrameCoords());

                MWWorld::Ptr* item = key->button->getUserData<MWWorld::Ptr>(false);
                if (item)
                    button->setUserData(*item);
            }
        }
    }
}

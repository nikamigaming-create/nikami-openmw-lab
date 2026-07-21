#include "renderingmanager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

#include <osg/ClipControl>
#include <osg/ComputeBoundsVisitor>
#include <osg/Fog>
#include <osg/Group>
#include <osg/Light>
#include <osg/LightModel>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/PolygonMode>
#include <osg/UserDataContainer>

#include <osgUtil/LineSegmentIntersector>

#include <osgViewer/Viewer>

#include <components/nifosg/nifloader.hpp>

#include <components/debug/debuglog.hpp>

#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>

#include <components/shader/removedalphafunc.hpp>
#include <components/shader/shadermanager.hpp>

#include <components/settings/values.hpp>

#include <components/vfs/manager.hpp>

#include <components/sceneutil/cullsafeboundsvisitor.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sceneutil/writescene.hpp>

#include <components/misc/constants.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadweap.hpp>

#include <components/debug/debugdraw.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/navmeshcacheitem.hpp>

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/groundcoverstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/livecellref.hpp"
#include "../mwworld/scene.hpp"

#include "../mwgui/postprocessorhud.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwclass/esm4npc.hpp"

#include "actorspaths.hpp"
#include "playervisualpolicy.hpp"
#include "camera.hpp"
#include "esm4npcanimation.hpp"
#include "effectmanager.hpp"
#include "fogmanager.hpp"
#include "falloutweaponanimation.hpp"
#include "groundcover.hpp"
#include "navmesh.hpp"
#include "npcanimation.hpp"
#include "objectpaging.hpp"
#include "pathgrid.hpp"
#include "postprocessor.hpp"
#include "recastmesh.hpp"
#include "screenshotmanager.hpp"
#include "sky.hpp"
#include "terrainstorage.hpp"
#include "util.hpp"
#include "vismask.hpp"
#include "water.hpp"

//## VR_PATCH BEGIN
#include <osg/ViewportIndexed>
#include <components/vr/vr.hpp>
#include "../mwvr/vranimation.hpp"
#include "../mwvr/vrgui.hpp"
#include "../mwvr/vrpointer.hpp"

//## VR_PATCH END
namespace MWRender
{
    namespace
    {
        bool envFlagEnabled(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0' && std::string(value) != "0";
        }

        float envFloatOr(const char* name, float fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || *value == '\0')
                return fallback;
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            return end != value && std::isfinite(parsed) ? parsed : fallback;
        }

        const ESM4::Npc* findEsm4PlayerVisualRecord()
        {
            if (envFlagEnabled("OPENMW_ESM4_DISABLE_PLAYER_VISUAL_PROXY")
                || envFlagEnabled("OPENMW_FNV_DISABLE_PLAYER_VISUAL_PROXY"))
            {
                Log(Debug::Info)
                    << "ESM4 player visual proxy disabled by runtime environment";
                return nullptr;
            }
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return nullptr;

            const char* env = std::getenv("OPENMW_ESM4_PLAYER_NPC");
            if (env == nullptr || *env == '\0')
                env = std::getenv("OPENMW_FNV_PLAYER_NPC");
            const std::string_view wanted = env != nullptr && *env != '\0' ? std::string_view(env) : "Player";
            const ESM4::Npc* fallback = nullptr;

            for (const ESM4::Npc& npc : store->get<ESM4::Npc>())
            {
                if (Misc::StringUtils::ciEqual(npc.mEditorId, wanted))
                    return &npc;
                if (fallback == nullptr && Misc::StringUtils::ciEqual(npc.mEditorId, "Player"))
                    fallback = &npc;
            }

            return fallback;
        }

        const ESM4::Npc* findFalloutPlayerVisualRecord()
        {
            const ESM4::Npc* player = findEsm4PlayerVisualRecord();
            return player != nullptr && player->mIsFONV ? player : nullptr;
        }

        bool hasFalloutNvContentLoaded()
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return false;

            for (const ESM4::Npc& npc : store->get<ESM4::Npc>())
            {
                if (npc.mIsFONV)
                    return true;
            }

            return false;
        }

        const ESM4::Armor* findFalloutArmorByEditorId(const std::string_view editorId)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || editorId.empty())
                return nullptr;

            for (const ESM4::Armor& armor : store->get<ESM4::Armor>())
            {
                if (Misc::StringUtils::ciEqual(armor.mEditorId, editorId))
                    return &armor;
            }

            return nullptr;
        }

        const ESM4::Weapon* findFalloutWeaponByEditorId(const std::string_view editorId)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || editorId.empty())
                return nullptr;

            for (const ESM4::Weapon& weapon : store->get<ESM4::Weapon>())
            {
                if (Misc::StringUtils::ciEqual(weapon.mEditorId, editorId))
                    return &weapon;
            }

            return nullptr;
        }

        void applyFalloutPlayerProxyConfiguredEquipment(const MWWorld::Ptr& visualPtr, const char* context)
        {
            const ESM4PlayerVisualEquipmentPolicy policy = resolveESM4PlayerVisualEquipmentPolicy(
                std::getenv("OPENMW_ESM4_PLAYER_OUTFIT"), std::getenv("OPENMW_FNV_PLAYER_OUTFIT"),
                std::getenv("OPENMW_ESM4_PLAYER_HEADGEAR"), std::getenv("OPENMW_FNV_PLAYER_HEADGEAR"),
                std::getenv("OPENMW_ESM4_PLAYER_WEAPON"), std::getenv("OPENMW_FNV_PLAYER_WEAPON"),
                std::getenv("OPENMW_FNV_BOOTSTRAP_LEVEL1_COURIER") != nullptr);
            if (policy.mOutfit.empty() && policy.mHeadgear.empty() && policy.mWeapon.empty())
                return;

            const auto addArmor = [&](std::string_view editorId, std::string_view role) {
                if (editorId.empty())
                    return;
                const ESM4::Armor* armor = findFalloutArmorByEditorId(editorId);
                if (armor == nullptr)
                {
                    Log(Debug::Warning) << "ESM4 proof: " << context << " player proxy " << role << " "
                                        << editorId << " not found";
                    return;
                }

                const std::size_t armorBefore = MWClass::ESM4Npc::getEquippedArmor(visualPtr).size();
                const std::size_t clothingBefore = MWClass::ESM4Npc::getEquippedClothing(visualPtr).size();
                const bool added = MWClass::ESM4Npc::addEquippedArmorReplacingSlots(visualPtr, armor);
                const std::size_t armorAfter = MWClass::ESM4Npc::getEquippedArmor(visualPtr).size();
                const std::size_t clothingAfter = MWClass::ESM4Npc::getEquippedClothing(visualPtr).size();
                Log(Debug::Info) << "ESM4 proof: " << context << " player proxy " << role << " "
                                 << armor->mEditorId << " model="
                                 << MWClass::ESM4Npc::chooseEquipmentModel(
                                        armor, MWClass::ESM4Npc::isFemale(visualPtr))
                                 << " added=" << added << " replacedArmor="
                                 << (armorBefore + (added ? 1 : 0) - armorAfter) << " replacedClothing="
                                 << (clothingBefore - clothingAfter);
            };

            addArmor(policy.mOutfit, "outfit");
            addArmor(policy.mHeadgear, "headgear");
            if (!policy.mWeapon.empty())
            {
                const ESM4::Weapon* weapon = findFalloutWeaponByEditorId(policy.mWeapon);
                if (weapon == nullptr)
                {
                    Log(Debug::Warning) << "ESM4 proof: " << context << " player proxy weapon "
                                        << policy.mWeapon << " not found";
                }
                else
                {
                    const bool changed = MWClass::ESM4Npc::setEquippedWeapon(visualPtr, weapon);
                    visualPtr.getClass().getCreatureStats(visualPtr).setDrawState(MWMechanics::DrawState::Weapon);
                    Log(Debug::Info) << "ESM4 proof: " << context << " player proxy weapon "
                                     << weapon->mEditorId << " model=" << weapon->mModel
                                     << " changed=" << changed << " drawn=1";
                }
            }
        }

        std::vector<ESM::FormId> makeFalloutWornVisualSignature(
            std::span<const ESM4::Armor* const> equippedArmor)
        {
            std::vector<ESM::FormId> signature;
            signature.reserve(equippedArmor.size());
            for (const ESM4::Armor* armor : equippedArmor)
            {
                if (armor != nullptr)
                    signature.push_back(armor->mId);
            }
            return canonicalizeFalloutWornVisualSignature(std::move(signature));
        }

        std::vector<const ESM4::Armor*> collectLiveFalloutArmor(const MWWorld::InventoryStore& inventory)
        {
            std::vector<const ESM4::Armor*> result;
            result.reserve(MWWorld::InventoryStore::Slots);
            for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
            {
                const MWWorld::ConstContainerStoreIterator item = inventory.getSlot(slot);
                if (item == inventory.end() || item->getType() != ESM4::Armor::sRecordId)
                    continue;
                if (const ESM4::Armor* armor = item->get<ESM4::Armor>()->mBase)
                    result.push_back(armor);
            }
            std::ranges::sort(result,
                [](const ESM4::Armor* left, const ESM4::Armor* right) { return left->mId < right->mId; });
            result.erase(std::unique(result.begin(), result.end(),
                             [](const ESM4::Armor* left, const ESM4::Armor* right) {
                                 return left->mId == right->mId;
                             }),
                result.end());
            return result;
        }

        std::optional<ESM4NpcAnimation::FirstPersonState> makeFalloutFirstPersonState(
            std::span<const ESM4::Armor* const> equippedArmor, const MWWorld::Ptr& visualPtr,
            float firstPersonFieldOfView, Resource::ResourceSystem* resourceSystem)
        {
            ESM4NpcAnimation::FirstPersonState state;
            state.mFieldOfView = firstPersonFieldOfView;
            const bool female = MWClass::ESM4Npc::isFemale(visualPtr);
            bool unresolvedWornLeftHand = false;
            for (const ESM4::Armor* armor : equippedArmor)
            {
                if (armor == nullptr)
                    continue;
                state.mPipBoy = state.mPipBoy || (armor->mArmorFlags & ESM4::Armor::FO3_PipBoy) != 0;
                if ((armor->mArmorFlags & ESM4::Armor::FO3_LeftHand) != 0)
                {
                    std::string candidate = normalizeFalloutFirstPersonBipedModel(
                        MWClass::ESM4Npc::chooseEquipmentModel(armor, female));
                    const VFS::Manager* vfs = resourceSystem != nullptr ? resourceSystem->getVFS() : nullptr;
                    const bool exists = vfs != nullptr && !candidate.empty()
                        && vfs->exists(VFS::Path::toNormalized(candidate));
                    Log(exists ? Debug::Info : Debug::Error)
                        << "FNV first-person equipped left-hand model: form=" << ESM::RefId(armor->mId)
                        << " editor=" << armor->mEditorId << " biped="
                        << MWClass::ESM4Npc::chooseEquipmentModel(armor, female) << " selected=" << candidate
                        << " exists=" << exists << " source=ARMO-biped-1st-convention";
                    if (exists)
                        state.mSaveWornLeftHandModel = std::move(candidate);
                    else
                        unresolvedWornLeftHand = true;
                }
                if ((armor->mArmorFlags & ESM4::Armor::FO3_UpperBody) == 0)
                    continue;
                const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(armor, female);
                if (!model.empty())
                    state.mSaveWornArmorModels.emplace_back(model);
                Log(!model.empty() ? Debug::Info : Debug::Error)
                    << "FNV first-person equipped armor model: form=" << ESM::RefId(armor->mId)
                    << " editor=" << armor->mEditorId << " flags=0x" << std::hex << armor->mArmorFlags
                    << std::dec << " selected=" << model << " source=ARMO-biped-MODL/MOD3";
            }
            if (unresolvedWornLeftHand)
            {
                Log(Debug::Error) << "FNV first-person equipped profile: worn left-hand armor has no authored 1st "
                                     "model; profile=disabled";
                return std::nullopt;
            }
            state.mPipBoyGlove
                = isFalloutPipBoyGloveFirstPersonModel(state.mPipBoy, state.mSaveWornLeftHandModel);
            const ESM4::Weapon* equippedWeapon = MWClass::ESM4Npc::getEquippedWeapon(visualPtr);
            Log(Debug::Info) << "FNV first-person equipped profile: armor=" << equippedArmor.size()
                             << " unarmed=" << (equippedWeapon == nullptr) << " equippedWeapon="
                             << (equippedWeapon != nullptr ? equippedWeapon->mEditorId : std::string("none"))
                             << " pipBoy=" << state.mPipBoy << " pipBoyGlove=" << state.mPipBoyGlove
                             << " armorModels=" << state.mSaveWornArmorModels.size()
                             << " fov=" << state.mFieldOfView << " profile=flat-first-person";
            return state;
        }

        std::optional<ESM4NpcAnimation::FirstPersonState> applyFalloutSaveWornPlayerVisuals(
            std::span<const ESM::FormId> wornVisualItems, const MWWorld::Ptr& visualPtr, float firstPersonFieldOfView,
            Resource::ResourceSystem* resourceSystem)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
            {
                Log(Debug::Error) << "FNV first-person saveWorn: no ESM store";
                return std::nullopt;
            }

            std::size_t saveWorn = 0;
            std::size_t savedArmor = 0;
            std::size_t savedWeapon = 0;
            for (const ESM::FormId id : wornVisualItems)
            {
                ++saveWorn;
                const ESM::RefId record(id);
                if (const ESM4::Armor* armor = store->get<ESM4::Armor>().search(record))
                {
                    const bool added = MWClass::ESM4Npc::addEquippedArmorReplacingSlots(visualPtr, armor);
                    ++savedArmor;
                    Log(added ? Debug::Info : Debug::Error)
                        << "FNV first-person saveWorn: ordinal=" << saveWorn << " type=ARMO form="
                        << record << " editor=" << armor->mEditorId << " flags=0x" << std::hex
                        << armor->mArmorFlags << std::dec << " added=" << added;
                }
                else if (const ESM4::Weapon* weapon = store->get<ESM4::Weapon>().search(record))
                {
                    ++savedWeapon;
                    const bool changed = MWClass::ESM4Npc::setEquippedWeapon(visualPtr, weapon);
                    Log(Debug::Info) << "FNV first-person saveWorn: ordinal=" << saveWorn
                                     << " type=WEAP form=" << record << " editor=" << weapon->mEditorId
                                     << " worldModel=" << weapon->mModel
                                     << " firstPersonModel=" << weapon->mFirstPersonModel
                                     << " equipped=1 changed=" << changed << " selectedProfile=weapon";
                }
                else
                {
                    Log(Debug::Error) << "FNV first-person saveWorn: ordinal=" << saveWorn
                                      << " form=" << record << " recordType=unresolved profile=disabled";
                    return std::nullopt;
                }
            }

            Log(Debug::Info) << "FNV first-person saveWorn: total=" << saveWorn << " armor=" << savedArmor
                             << " weapon=" << savedWeapon << " source=native-save";
            return makeFalloutFirstPersonState(
                MWClass::ESM4Npc::getEquippedArmor(visualPtr), visualPtr, firstPersonFieldOfView, resourceSystem);
        }

        uint32_t getFalloutActorCoveredBodySlots(const MWWorld::Ptr& ptr)
        {
            uint32_t covered = 0;
            for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(ptr))
                covered |= armor->mArmorFlags;
            for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(ptr))
                covered |= clothing->mClothingFlags;
            return covered;
        }

        std::string formatHex32(uint32_t value)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << value;
            return stream.str();
        }

        bool falloutModelMentionsHand(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("lefthand") != std::string::npos || lowered.find("righthand") != std::string::npos
                || lowered.find("hand") != std::string::npos || lowered.find("pipboy") != std::string::npos
                || lowered.find("glove") != std::string::npos;
        }

        void logFalloutVrHandSourceCandidates(const MWWorld::Ptr& actorPtr, std::string_view label)
        {
            if (actorPtr.isEmpty() || actorPtr.getType() != ESM4::Npc::sRecordId)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly source " << label
                                 << " is not ESM4 NPC; ptr=" << actorPtr.toString();
                return;
            }

            const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(actorPtr);
            const ESM4::Npc* modelRecord = MWClass::ESM4Npc::getModelRecord(actorPtr);
            const ESM4::Race* race = MWClass::ESM4Npc::getRace(actorPtr);
            const bool isFemale = MWClass::ESM4Npc::isFemale(actorPtr);
            const uint32_t coveredSlots = getFalloutActorCoveredBodySlots(actorPtr);
            const bool coversLeft = (coveredSlots & ESM4::Armor::FO3_LeftHand) != 0;
            const bool coversRight = (coveredSlots & ESM4::Armor::FO3_RightHand) != 0;
            const bool coversPipBoy = (coveredSlots & ESM4::Armor::FO3_PipBoy) != 0;

            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly source " << label
                             << " ptr=" << actorPtr.toString()
                             << " traits=" << (traits != nullptr ? traits->mEditorId : std::string_view{})
                             << " traitsForm=" << (traits != nullptr ? ESM::RefId(traits->mId) : ESM::RefId())
                             << " modelRecord=" << (modelRecord != nullptr ? modelRecord->mEditorId : std::string_view{})
                             << " race=" << (race != nullptr ? race->mEditorId : std::string_view{})
                             << " female=" << isFemale
                             << " coveredSlots=" << formatHex32(coveredSlots)
                             << " coversLeftHand=" << coversLeft
                             << " coversRightHand=" << coversRight
                             << " coversPipBoy=" << coversPipBoy;

            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly intended attach leftBone=Bip01 L Hand rightBone=Bip01 R Hand";

            if (race != nullptr)
            {
                const std::vector<ESM4::Race::BodyPart>& bodyParts
                    = isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale;
                for (const ESM4::Race::BodyPart& bodyPart : bodyParts)
                {
                    if (!falloutModelMentionsHand(bodyPart.mesh))
                        continue;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly race hand candidate source=" << label
                                     << " mesh=" << bodyPart.mesh
                                     << " texture=" << bodyPart.texture
                                     << " skippedByLeftCoverage="
                                     << (coversLeft && Misc::StringUtils::lowerCase(bodyPart.mesh).find("lefthand")
                                             != std::string::npos)
                                     << " skippedByRightCoverage="
                                     << (coversRight && Misc::StringUtils::lowerCase(bodyPart.mesh).find("righthand")
                                             != std::string::npos);
                }
            }

            for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(actorPtr))
            {
                const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(armor, isFemale);
                const bool handSlots = armor != nullptr
                    && ((armor->mArmorFlags & (ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_RightHand
                                                | ESM4::Armor::FO3_PipBoy))
                        != 0);
                if (handSlots || falloutModelMentionsHand(model))
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly equipped armor candidate source=" << label
                                     << " editor=" << (armor != nullptr ? armor->mEditorId : std::string_view{})
                                     << " form=" << (armor != nullptr ? ESM::RefId(armor->mId) : ESM::RefId())
                                     << " flags="
                                     << (armor != nullptr ? formatHex32(armor->mArmorFlags) : std::string("0x0"))
                                     << " model=" << model
                                     << " handSlots=" << handSlots;
            }

            for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(actorPtr))
            {
                const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(clothing, isFemale);
                const bool handSlots = clothing != nullptr
                    && ((clothing->mClothingFlags & (ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_RightHand
                                                     | ESM4::Armor::FO3_PipBoy))
                        != 0);
                if (handSlots || falloutModelMentionsHand(model))
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly equipped clothing candidate source=" << label
                                     << " editor=" << (clothing != nullptr ? clothing->mEditorId : std::string_view{})
                                     << " form=" << (clothing != nullptr ? ESM::RefId(clothing->mId) : ESM::RefId())
                                     << " flags="
                                     << (clothing != nullptr ? formatHex32(clothing->mClothingFlags) : std::string("0x0"))
                                     << " model=" << model
                                     << " handSlots=" << handSlots;
            }

            if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(actorPtr))
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly equipped weapon candidate source=" << label
                                 << " editor=" << weapon->mEditorId
                                 << " form=" << ESM::RefId(weapon->mId)
                                 << " model=" << weapon->mModel
                                 << " worldModel=" << ESM::RefId(weapon->mWorldModel)
                                 << " type=" << weapon->mData.type
                                 << " intendedAttach=Weapon/Bip01 R Hand";
            }
            else
                Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly equipped weapon candidate source=" << label
                                 << " editor=<none>";
        }

        bool appendFalloutVrWeaponSurface(
            std::vector<MWVR::VRAnimation::FalloutVrHandSurface>& surfaces, const MWWorld::Ptr& actorPtr,
            std::string_view label)
        {
            if (actorPtr.isEmpty())
                return false;

            const auto addWeaponSurface = [&](const ESM4::Weapon* weapon, std::string source) {
                if (weapon == nullptr || weapon->mModel.empty())
                    return false;
                surfaces.push_back(MWVR::VRAnimation::FalloutVrHandSurface{
                    weapon->mModel, {}, std::move(source), false });
                Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly appended right-hand weapon source="
                                 << surfaces.back().source << " editor=" << weapon->mEditorId
                                 << " model=" << weapon->mModel;
                return true;
            };

            const MWWorld::Class& actorClass = actorPtr.getClass();
            if (actorClass.hasInventoryStore(actorPtr))
            {
                const MWWorld::InventoryStore& inventory = actorClass.getInventoryStore(actorPtr);
                const MWWorld::ConstContainerStoreIterator weaponSlot
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                if (weaponSlot != inventory.end() && weaponSlot->getType() == ESM4::Weapon::sRecordId)
                {
                    const ESM4::Weapon* weapon = weaponSlot->get<ESM4::Weapon>()->mBase;
                    if (addWeaponSurface(weapon, std::string(label) + ":live-weapon:" + weapon->mEditorId))
                        return true;
                }
            }

            if (actorPtr.getType() == ESM4::Npc::sRecordId)
            {
                const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(actorPtr);
                if (weapon != nullptr)
                    return addWeaponSurface(weapon, std::string(label) + ":npc-weapon:" + weapon->mEditorId);
            }

            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly no right-hand weapon surface source=" << label;
            return false;
        }

        std::vector<MWVR::VRAnimation::FalloutVrHandSurface> collectFalloutVrHandSurfaces(
            const MWWorld::Ptr& actorPtr, std::string_view label, bool includeWeapon = true)
        {
            std::vector<MWVR::VRAnimation::FalloutVrHandSurface> surfaces;
            if (actorPtr.isEmpty() || actorPtr.getType() != ESM4::Npc::sRecordId)
                return surfaces;

            const ESM4::Race* race = MWClass::ESM4Npc::getRace(actorPtr);
            const bool isFemale = MWClass::ESM4Npc::isFemale(actorPtr);
            const uint32_t coveredSlots = getFalloutActorCoveredBodySlots(actorPtr);
            const bool coversLeft = (coveredSlots & ESM4::Armor::FO3_LeftHand) != 0;
            const bool coversRight = (coveredSlots & ESM4::Armor::FO3_RightHand) != 0;

            const auto addSurface = [&](std::string_view model, std::string_view texture, std::string source, bool left) {
                if (model.empty())
                    return;
                surfaces.push_back(MWVR::VRAnimation::FalloutVrHandSurface{
                    std::string(model), std::string(texture), std::move(source), left });
            };

            if (race != nullptr)
            {
                const std::vector<ESM4::Race::BodyPart>& bodyParts
                    = isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale;
                for (const ESM4::Race::BodyPart& bodyPart : bodyParts)
                {
                    if (!falloutModelMentionsHand(bodyPart.mesh))
                        continue;
                    const std::string lowered = Misc::StringUtils::lowerCase(bodyPart.mesh);
                    const bool left = lowered.find("lefthand") != std::string::npos;
                    const bool right = lowered.find("righthand") != std::string::npos;
                    if ((left && coversLeft) || (right && coversRight))
                        continue;
                    addSurface(bodyPart.mesh, bodyPart.texture, std::string(label) + ":race", left && !right);
                }
            }

            for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(actorPtr))
            {
                if (armor == nullptr)
                    continue;
                const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(armor, isFemale);
                const uint32_t handFlags = armor->mArmorFlags
                    & (ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_RightHand | ESM4::Armor::FO3_PipBoy);
                if (handFlags == 0 && !falloutModelMentionsHand(model))
                    continue;
                const std::string lowered = Misc::StringUtils::lowerCase(model);
                const bool left = (handFlags & (ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_PipBoy)) != 0
                    || lowered.find("left") != std::string::npos || lowered.find("pipboy") != std::string::npos;
                addSurface(model, {}, std::string(label) + ":armor:" + armor->mEditorId, left);
            }

            for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(actorPtr))
            {
                if (clothing == nullptr)
                    continue;
                const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(clothing, isFemale);
                const uint32_t handFlags = clothing->mClothingFlags
                    & (ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_RightHand | ESM4::Armor::FO3_PipBoy);
                if (handFlags == 0 && !falloutModelMentionsHand(model))
                    continue;
                const std::string lowered = Misc::StringUtils::lowerCase(model);
                const bool left = (handFlags & (ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_PipBoy)) != 0
                    || lowered.find("left") != std::string::npos || lowered.find("pipboy") != std::string::npos;
                addSurface(model, {}, std::string(label) + ":clothing:" + clothing->mEditorId, left);
            }

            if (includeWeapon)
                appendFalloutVrWeaponSurface(surfaces, actorPtr, label);
            return surfaces;
        }

        std::vector<MWVR::VRAnimation::FalloutVrHandSurface> collectFalloutVrHandSurfacesForVisualRecord(
            const MWWorld::Ptr& player, const ESM4::Npc* visualRecord)
        {
            if (visualRecord == nullptr)
                return {};

            ESM::CellRef visualRef;
            visualRef.blank();
            visualRef.mRefID = ESM::RefId(visualRecord->mId);
            MWWorld::LiveCellRef<ESM4::Npc> liveVisualRef(visualRef, visualRecord);
            liveVisualRef.mData.setPosition(player.getRefData().getPosition());
            MWWorld::Ptr visualPtr(&liveVisualRef, player.getCell());
            applyFalloutPlayerProxyConfiguredEquipment(visualPtr, "vr-hands-attach");
            std::vector<MWVR::VRAnimation::FalloutVrHandSurface> surfaces
                = collectFalloutVrHandSurfaces(visualPtr, "fallout-visual-record", false);
            const bool rightPipBoyCalibration = [] {
                if (const char* value = std::getenv("OPENMW_FNV_RIGHT_PIPBOY_CALIBRATION"))
                    return *value != '\0' && std::string_view(value) != "0";
                return false;
            }();
            if (rightPipBoyCalibration)
            {
                std::optional<MWVR::VRAnimation::FalloutVrHandSurface> rightPipBoySurface;
                for (const MWVR::VRAnimation::FalloutVrHandSurface& surface : surfaces)
                {
                    const std::string lowered = Misc::StringUtils::lowerCase(surface.model);
                    if (lowered.find("pipboyarm") == std::string::npos)
                        continue;
                    rightPipBoySurface = MWVR::VRAnimation::FalloutVrHandSurface{
                        surface.model, surface.diffuseTexture, "right-pipboy-calibration:" + surface.source, false };
                    break;
                }
                if (rightPipBoySurface)
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly appended right PipBoy calibration model="
                                     << rightPipBoySurface->model;
                    surfaces.push_back(std::move(*rightPipBoySurface));
                }
            }
            appendFalloutVrWeaponSurface(surfaces, player, "save-loaded-vr-player-ptr");
            return surfaces;
        }

        void logFalloutVrHandSelectionDiagnostic(const MWWorld::Ptr& player, const ESM4::Npc* visualRecord)
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly diagnostic begin playerPtr=" << player.toString()
                             << " playerType=" << player.getTypeDescription()
                             << " visualRecord="
                             << (visualRecord != nullptr ? visualRecord->mEditorId : std::string_view{})
                             << " visualForm="
                             << (visualRecord != nullptr ? ESM::RefId(visualRecord->mId) : ESM::RefId());

            logFalloutVrHandSourceCandidates(player, "vr-player-ptr");

            if (visualRecord == nullptr)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly diagnostic no Fallout visual record available";
                return;
            }

            ESM::CellRef visualRef;
            visualRef.blank();
            visualRef.mRefID = ESM::RefId(visualRecord->mId);
            MWWorld::LiveCellRef<ESM4::Npc> liveVisualRef(visualRef, visualRecord);
            liveVisualRef.mData.setPosition(player.getRefData().getPosition());
            MWWorld::Ptr visualPtr(&liveVisualRef, player.getCell());
            applyFalloutPlayerProxyConfiguredEquipment(visualPtr, "vr-hands-diagnostic");
            logFalloutVrHandSourceCandidates(visualPtr, "fallout-visual-record");
        }
    }

    class PerViewUniformStateUpdater final : public SceneUtil::StateSetUpdater
    {
    public:
        PerViewUniformStateUpdater(Resource::SceneManager* sceneManager)
            : mSceneManager(sceneManager)
        {
            mOpaqueTextureUnit = mSceneManager->getShaderManager().reserveGlobalTextureUnits(
                Shader::ShaderManager::Slot::OpaqueDepthTexture);
        }

        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf{}));
            if (mSkyRTT)
                stateset->addUniform(new osg::Uniform("sky", mSkyTextureUnit));
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            stateset->getUniform("projectionMatrix")->set(mProjectionMatrix);
            if (mSkyRTT && nv->getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
            {
                osg::Texture* skyTexture = mSkyRTT->getColorTexture(static_cast<osgUtil::CullVisitor*>(nv));
                stateset->setTextureAttribute(
                    mSkyTextureUnit, skyTexture, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }

            stateset->setTextureAttribute(mOpaqueTextureUnit,
                mSceneManager->getOpaqueDepthTex(nv->getTraversalNumber()), osg::StateAttribute::ON);
        }

        void applyLeft(osg::StateSet* stateset, osgUtil::CullVisitor* nv) override
        {
            stateset->getUniform("projectionMatrix")->set(getEyeProjectionMatrix(0));
        }

        void applyRight(osg::StateSet* stateset, osgUtil::CullVisitor* nv) override
        {
            stateset->getUniform("projectionMatrix")->set(getEyeProjectionMatrix(1));
        }

        void setProjectionMatrix(const osg::Matrixf& projectionMatrix) { mProjectionMatrix = projectionMatrix; }

        const osg::Matrixf& getProjectionMatrix() const { return mProjectionMatrix; }

        void enableSkyRTT(int skyTextureUnit, SceneUtil::RTTNode* skyRTT)
        {
            mSkyTextureUnit = skyTextureUnit;
            mSkyRTT = skyRTT;
        }

    private:
        osg::Matrixf getEyeProjectionMatrix(int view)
        {
            return Stereo::Manager::instance().computeEyeProjection(view, SceneUtil::AutoDepth::isReversed());
        }

        osg::Matrixf mProjectionMatrix;
        int mSkyTextureUnit = -1;
        SceneUtil::RTTNode* mSkyRTT = nullptr;

        Resource::SceneManager* mSceneManager;
        int mOpaqueTextureUnit = -1;
    };

    class SharedUniformStateUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        SharedUniformStateUpdater()
            : mNear(0.f)
            , mFar(0.f)
            , mWindSpeed(0.f)
            , mSkyBlendingStartCoef(Settings::fog().mSkyBlendingStart)
        {
        }

        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->addUniform(new osg::Uniform("near", 0.f));
            stateset->addUniform(new osg::Uniform("far", 0.f));
            stateset->addUniform(new osg::Uniform("skyBlendingStart", 0.f));
            stateset->addUniform(new osg::Uniform("screenRes", osg::Vec2f{}));
            stateset->addUniform(new osg::Uniform("isReflection", false));
            stateset->addUniform(new osg::Uniform("windSpeed", 0.0f));
            stateset->addUniform(new osg::Uniform("playerPos", osg::Vec3f(0.f, 0.f, 0.f)));
            stateset->addUniform(new osg::Uniform("useTreeAnim", false));
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            stateset->getUniform("near")->set(mNear);
            stateset->getUniform("far")->set(mFar);
            stateset->getUniform("skyBlendingStart")->set(mFar * mSkyBlendingStartCoef);
            stateset->getUniform("screenRes")->set(mScreenRes);
            stateset->getUniform("windSpeed")->set(mWindSpeed);
            stateset->getUniform("playerPos")->set(mPlayerPos);
        }

        void setNear(float near) { mNear = near; }

        void setFar(float far) { mFar = far; }

        void setScreenRes(float width, float height) { mScreenRes = osg::Vec2f(width, height); }

        void setWindSpeed(float windSpeed) { mWindSpeed = windSpeed; }

        void setPlayerPos(osg::Vec3f playerPos) { mPlayerPos = playerPos; }

    private:
        float mNear;
        float mFar;
        float mWindSpeed;
        float mSkyBlendingStartCoef;
        osg::Vec3f mPlayerPos;
        osg::Vec2f mScreenRes;
    };

    class StateUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        StateUpdater()
            : mFogStart(0.f)
            , mFogEnd(0.f)
            , mWireframe(false)
        {
        }

        void setDefaults(osg::StateSet* stateset) override
        {
            osg::LightModel* lightModel = new osg::LightModel;
            stateset->setAttribute(lightModel, osg::StateAttribute::ON);
            osg::Fog* fog = new osg::Fog;
            fog->setMode(osg::Fog::LINEAR);
            stateset->setAttributeAndModes(fog, osg::StateAttribute::ON);
            if (mWireframe)
            {
                osg::PolygonMode* polygonmode = new osg::PolygonMode;
                polygonmode->setMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE);
                stateset->setAttributeAndModes(polygonmode, osg::StateAttribute::ON);
            }
            else
                stateset->removeAttribute(osg::StateAttribute::POLYGONMODE);
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
        {
            osg::LightModel* lightModel
                = static_cast<osg::LightModel*>(stateset->getAttribute(osg::StateAttribute::LIGHTMODEL));
            lightModel->setAmbientIntensity(mAmbientColor);
            osg::Fog* fog = static_cast<osg::Fog*>(stateset->getAttribute(osg::StateAttribute::FOG));
            fog->setColor(mFogColor);
            fog->setStart(mFogStart);
            fog->setEnd(mFogEnd);
        }

        void setAmbientColor(const osg::Vec4f& col) { mAmbientColor = col; }

        void setFogColor(const osg::Vec4f& col) { mFogColor = col; }

        void setFogStart(float start) { mFogStart = start; }

        void setFogEnd(float end) { mFogEnd = end; }

        void setWireframe(bool wireframe)
        {
            if (mWireframe != wireframe)
            {
                mWireframe = wireframe;
                reset();
            }
        }

        bool getWireframe() const { return mWireframe; }

    private:
        osg::Vec4f mAmbientColor;
        osg::Vec4f mFogColor;
        float mFogStart;
        float mFogEnd;
        bool mWireframe;
    };

    class PreloadCommonAssetsWorkItem : public SceneUtil::WorkItem
    {
    public:
        PreloadCommonAssetsWorkItem(Resource::ResourceSystem* resourceSystem)
            : mResourceSystem(resourceSystem)
        {
        }

        void doWork() override
        {
            try
            {
                for (const VFS::Path::Normalized& v : mModels)
                    mResourceSystem->getSceneManager()->getTemplate(v);
                for (const VFS::Path::Normalized& v : mTextures)
                    mResourceSystem->getImageManager()->getImage(v);
                for (const VFS::Path::Normalized& v : mKeyframes)
                    mResourceSystem->getKeyframeManager()->get(v);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to preload common assets: " << e.what();
            }
        }

        std::vector<VFS::Path::Normalized> mModels;
        std::vector<VFS::Path::Normalized> mTextures;
        std::vector<VFS::Path::Normalized> mKeyframes;

    private:
        Resource::ResourceSystem* mResourceSystem;
    };

    RenderingManager::RenderingManager(osgViewer::Viewer* viewer, osg::ref_ptr<osg::Group> rootNode,
        Resource::ResourceSystem* resourceSystem, SceneUtil::WorkQueue* workQueue,
        DetourNavigator::Navigator& navigator, const MWWorld::GroundcoverStore& groundcoverStore,
//## VR_PATCH BEGIN
// Add camera to signature
        SceneUtil::UnrefQueue& unrefQueue, std::unique_ptr<Camera> camera)
//## VR_PATCH END
        : mSkyBlending(Settings::fog().mSkyBlending)
        , mViewer(viewer)
        , mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mWorkQueue(workQueue)
        , mNavigator(navigator)
        , mNightEyeFactor(0.f)
        // TODO: Near clip should not need to be bounded like this, but too small values break OSG shadow calculations
        // CPU-side. See issue: #6072
        , mNearClip(Settings::camera().mNearClip)
        , mViewDistance(Settings::camera().mViewingDistance)
        , mFieldOfViewOverridden(false)
        , mFieldOfViewOverride(0.f)
        , mProjectionMatrixOverridden(false)
        , mFieldOfView(Settings::camera().mFieldOfView)
        , mFirstPersonFieldOfView(Settings::camera().mFirstPersonFieldOfView)
        , mGroundCoverStore(groundcoverStore)
    {
        bool reverseZ = SceneUtil::AutoDepth::isReversed();
        const SceneUtil::LightingMethod lightingMethod = Settings::shaders().mLightingMethod;

        resourceSystem->getSceneManager()->setParticleSystemMask(MWRender::Mask_ParticleSystem);

        // Figure out which pipeline must be used by default and inform the user
        bool forceShaders = Settings::shaders().mForceShaders;
        {
            std::vector<std::string> requesters;
            if (!forceShaders)
            {
                if (Settings::fog().mRadialFog)
                    requesters.push_back("radial fog");
                if (Settings::fog().mExponentialFog)
                    requesters.push_back("exponential fog");
                if (mSkyBlending)
                    requesters.push_back("sky blending");
                if (Settings::shaders().mSoftParticles)
                    requesters.push_back("soft particles");
                if (Settings::shadows().mEnableShadows)
                    requesters.push_back("shadows");
                if (lightingMethod != SceneUtil::LightingMethod::FFP)
                    requesters.push_back("lighting method");
                if (reverseZ)
                    requesters.push_back("reverse-Z depth buffer");
                if (Stereo::getMultiview())
                    requesters.push_back("stereo multiview");

                if (!requesters.empty())
                    forceShaders = true;
            }

            if (forceShaders)
            {
                std::string message = "Using rendering with shaders by default";
                if (requesters.empty())
                {
                    message += " (forced)";
                }
                else
                {
                    message += ", requested by:";
                    for (size_t i = 0; i < requesters.size(); i++)
                        message += "\n - " + requesters[i];
                }
                Log(Debug::Info) << message;
            }
            else
            {
                Log(Debug::Info) << "Using fixed-function rendering by default";
            }
        }

        resourceSystem->getSceneManager()->setForceShaders(forceShaders);
        // FIXME: calling dummy method because terrain needs to know whether lighting is clamped
        resourceSystem->getSceneManager()->setClampLighting(Settings::shaders().mClampLighting);
        resourceSystem->getSceneManager()->setAutoUseNormalMaps(Settings::shaders().mAutoUseObjectNormalMaps);
        resourceSystem->getSceneManager()->setNormalMapPattern(Settings::shaders().mNormalMapPattern);
        resourceSystem->getSceneManager()->setNormalHeightMapPattern(Settings::shaders().mNormalHeightMapPattern);
        resourceSystem->getSceneManager()->setAutoUseSpecularMaps(Settings::shaders().mAutoUseObjectSpecularMaps);
        resourceSystem->getSceneManager()->setSpecularMapPattern(Settings::shaders().mSpecularMapPattern);
        resourceSystem->getSceneManager()->setApplyLightingToEnvMaps(
            Settings::shaders().mApplyLightingToEnvironmentMaps);
        resourceSystem->getSceneManager()->setConvertAlphaTestToAlphaToCoverage(shouldAddMSAAIntermediateTarget());
        resourceSystem->getSceneManager()->setAdjustCoverageForAlphaTest(
            Settings::shaders().mAdjustCoverageForAlphaTest);

        // Let LightManager choose which backend to use based on our hint. For methods besides legacy lighting, this
        // depends on support for various OpenGL extensions.
        osg::ref_ptr<SceneUtil::LightManager> sceneRoot = new SceneUtil::LightManager(SceneUtil::LightSettings{
            .mLightingMethod = lightingMethod,
            .mMaxLights = Settings::shaders().mMaxLights,
            .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
            .mLightFadeStart = Settings::shaders().mLightFadeStart,
            .mLightBoundsMultiplier = Settings::shaders().mLightBoundsMultiplier,
        });
        resourceSystem->getSceneManager()->setLightingMethod(sceneRoot->getLightingMethod());
        resourceSystem->getSceneManager()->setSupportedLightingMethods(sceneRoot->getSupportedLightingMethods());

        sceneRoot->setLightingMask(Mask_Lighting);
        mSceneRoot = sceneRoot;
        sceneRoot->setStartLight(1);
        sceneRoot->setNodeMask(Mask_Scene);
        sceneRoot->setName("Scene Root");

        int shadowCastingTraversalMask = Mask_Scene;
        if (Settings::shadows().mActorShadows)
            shadowCastingTraversalMask |= Mask_Actor;
        if (Settings::shadows().mPlayerShadows)
            shadowCastingTraversalMask |= Mask_Player;

        int indoorShadowCastingTraversalMask = shadowCastingTraversalMask;
        if (Settings::shadows().mObjectShadows)
            shadowCastingTraversalMask |= (Mask_Object | Mask_Static);
        if (Settings::shadows().mTerrainShadows)
            shadowCastingTraversalMask |= Mask_Terrain;

        mShadowManager = std::make_unique<SceneUtil::ShadowManager>(sceneRoot, mRootNode, shadowCastingTraversalMask,
            indoorShadowCastingTraversalMask, Mask_Terrain | Mask_Object | Mask_Static, Settings::shadows(),
            mResourceSystem->getSceneManager()->getShaderManager());

        Shader::ShaderManager::DefineMap shadowDefines = mShadowManager->getShadowDefines(Settings::shadows());
        Shader::ShaderManager::DefineMap lightDefines = sceneRoot->getLightDefines();
        Shader::ShaderManager::DefineMap globalDefines
            = mResourceSystem->getSceneManager()->getShaderManager().getGlobalDefines();

        for (auto itr = shadowDefines.begin(); itr != shadowDefines.end(); itr++)
            globalDefines[itr->first] = itr->second;

        globalDefines["forcePPL"] = Settings::shaders().mForcePerPixelLighting ? "1" : "0";
        globalDefines["clamp"] = Settings::shaders().mClampLighting ? "1" : "0";
        globalDefines["preLightEnv"] = Settings::shaders().mApplyLightingToEnvironmentMaps ? "1" : "0";
        globalDefines["classicFalloff"] = Settings::shaders().mClassicFalloff ? "1" : "0";
        const bool exponentialFog = Settings::fog().mExponentialFog;
        globalDefines["radialFog"] = (exponentialFog || Settings::fog().mRadialFog) ? "1" : "0";
        globalDefines["exponentialFog"] = exponentialFog ? "1" : "0";
        globalDefines["skyBlending"] = mSkyBlending ? "1" : "0";
        globalDefines["waterRefraction"] = "0";
        globalDefines["useGPUShader4"] = "0";
        globalDefines["useOVR_multiview"] = "0";
        globalDefines["numViews"] = "1";
        globalDefines["disableNormals"] = "1";
        globalDefines["softParticles"] = "0";

        for (auto itr = lightDefines.begin(); itr != lightDefines.end(); itr++)
            globalDefines[itr->first] = itr->second;

        // Refactor this at some point - most shaders don't care about these defines
        const float groundcoverDistance = Settings::groundcover().mRenderingDistance;
        globalDefines["groundcoverFadeStart"] = std::to_string(groundcoverDistance * 0.9f);
        globalDefines["groundcoverFadeEnd"] = std::to_string(groundcoverDistance);
        globalDefines["groundcoverStompMode"] = std::to_string(Settings::groundcover().mStompMode);
        globalDefines["groundcoverStompIntensity"] = std::to_string(Settings::groundcover().mStompIntensity);

        globalDefines["reverseZ"] = reverseZ ? "1" : "0";

        // It is unnecessary to stop/start the viewer as no frames are being rendered yet.
        mResourceSystem->getSceneManager()->getShaderManager().setGlobalDefines(globalDefines);

        mNavMesh = std::make_unique<NavMesh>(mRootNode, mWorkQueue, Settings::navigator().mEnableNavMeshRender,
            Settings::navigator().mNavMeshRenderMode);
        mActorsPaths = std::make_unique<ActorsPaths>(mRootNode, Settings::navigator().mEnableAgentsPathsRender);
        mRecastMesh = std::make_unique<RecastMesh>(mRootNode, Settings::navigator().mEnableRecastMeshRender);
        mPathgrid = std::make_unique<Pathgrid>(mRootNode);

        mObjects = std::make_unique<Objects>(mResourceSystem, sceneRoot, unrefQueue);

        if (getenv("OPENMW_DONT_PRECOMPILE") == nullptr)
        {
            mViewer->setIncrementalCompileOperation(new osgUtil::IncrementalCompileOperation);
            mViewer->getIncrementalCompileOperation()->setTargetFrameRate(Settings::cells().mTargetFramerate);
        }

        mDebugDraw = new Debug::DebugDrawer(mResourceSystem->getSceneManager()->getShaderManager());
        mDebugDraw->setNodeMask(Mask_Debug);
        sceneRoot->addChild(mDebugDraw);

        mResourceSystem->getSceneManager()->setIncrementalCompileOperation(mViewer->getIncrementalCompileOperation());

        mEffectManager = std::make_unique<EffectManager>(sceneRoot, mResourceSystem);

        const std::string& normalMapPattern = Settings::shaders().mNormalMapPattern;
        const std::string& heightMapPattern = Settings::shaders().mNormalHeightMapPattern;
        const std::string& specularMapPattern = Settings::shaders().mTerrainSpecularMapPattern;
        const bool useTerrainNormalMaps = Settings::shaders().mAutoUseTerrainNormalMaps;
        const bool useTerrainSpecularMaps = Settings::shaders().mAutoUseTerrainSpecularMaps;

        mTerrainStorage = std::make_unique<TerrainStorage>(mResourceSystem, normalMapPattern, heightMapPattern,
            useTerrainNormalMaps, specularMapPattern, useTerrainSpecularMaps);

        WorldspaceChunkMgr& chunkMgr = getWorldspaceChunkMgr(ESM::Cell::sDefaultWorldspaceId);
        mTerrain = chunkMgr.mTerrain.get();
        mGroundcover = chunkMgr.mGroundcover.get();
        mObjectPaging = chunkMgr.mObjectPaging.get();

        mStateUpdater = new StateUpdater;
        sceneRoot->addUpdateCallback(mStateUpdater);

        mSharedUniformStateUpdater = new SharedUniformStateUpdater();
        rootNode->addUpdateCallback(mSharedUniformStateUpdater);

        mPerViewUniformStateUpdater = new PerViewUniformStateUpdater(mResourceSystem->getSceneManager());
        rootNode->addCullCallback(mPerViewUniformStateUpdater);

        mPostProcessor = new PostProcessor(*this, viewer, mRootNode, resourceSystem->getVFS());
        resourceSystem->getSceneManager()->setOpaqueDepthTex(
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 0),
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 1));
        resourceSystem->getSceneManager()->setSupportsNormalsRT(mPostProcessor->getSupportsNormalsRT());
        resourceSystem->getSceneManager()->setWeatherParticleOcclusion(Settings::shaders().mWeatherParticleOcclusion);

        // water goes after terrain for correct waterculling order
        mWater = std::make_unique<Water>(
            sceneRoot->getParent(0), sceneRoot, mResourceSystem, mViewer->getIncrementalCompileOperation());

//## VR_PATCH BEGIN
        mCamera = std::move(camera);
//## VR_PATCH END

        mScreenshotManager = std::make_unique<ScreenshotManager>(viewer);

        mViewer->setLightingMode(osgViewer::View::NO_LIGHT);

        osg::ref_ptr<osg::LightSource> source = new osg::LightSource;
        source->setNodeMask(Mask_Lighting);
        mSunLight = new osg::Light;
        source->setLight(mSunLight);
        mSunLight->setDiffuse(osg::Vec4f(0, 0, 0, 1));
        mSunLight->setAmbient(osg::Vec4f(0, 0, 0, 1));
        mSunLight->setSpecular(osg::Vec4f(0, 0, 0, 0));
        mSunLight->setConstantAttenuation(1.f);
        sceneRoot->setSunlight(mSunLight);
        sceneRoot->addChild(source);

        sceneRoot->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
        sceneRoot->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::ON);
        sceneRoot->getOrCreateStateSet()->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Material> defaultMat(new osg::Material);
        defaultMat->setColorMode(osg::Material::OFF);
        defaultMat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        sceneRoot->getOrCreateStateSet()->setAttribute(defaultMat);
        sceneRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("emissiveMult", 1.f));
        sceneRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("specStrength", 1.f));
        sceneRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("distortionStrength", 0.f));

        resourceSystem->getSceneManager()->setUpNormalsRTForStateSet(sceneRoot->getOrCreateStateSet(), true);

        mFog = std::make_unique<FogManager>();

        mSky = std::make_unique<SkyManager>(
            sceneRoot, mRootNode, mViewer->getCamera(), resourceSystem->getSceneManager(), mSkyBlending);
        if (mSkyBlending)
        {
            int skyTextureUnit = mResourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                Shader::ShaderManager::Slot::SkyTexture);
            mPerViewUniformStateUpdater->enableSkyRTT(skyTextureUnit, mSky->getSkyRTT());
        }

        source->setStateSetModes(*mRootNode->getOrCreateStateSet(), osg::StateAttribute::ON);

        osg::Camera::CullingMode cullingMode = osg::Camera::DEFAULT_CULLING | osg::Camera::FAR_PLANE_CULLING;

        if (!Settings::camera().mSmallFeatureCulling)
            cullingMode &= ~(osg::CullStack::SMALL_FEATURE_CULLING);
        else
        {
            mViewer->getCamera()->setSmallFeatureCullingPixelSize(Settings::camera().mSmallFeatureCullingPixelSize);
            cullingMode |= osg::CullStack::SMALL_FEATURE_CULLING;
        }

        mViewer->getCamera()->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        mViewer->getCamera()->setCullingMode(cullingMode);
        mViewer->getCamera()->setName(Constants::SceneCamera);

        auto mask = ~(Mask_UpdateVisitor | Mask_SimpleWater);
        MWBase::Environment::get().getWindowManager()->setCullMask(mask);
        NifOsg::Loader::setHiddenNodeMask(Mask_UpdateVisitor);
        NifOsg::Loader::setIntersectionDisabledNodeMask(Mask_Effect);
        NifOsg::Loader::setSoftEffectEnabled(Settings::shaders().mSoftParticles);
        Nif::Reader::setLoadUnsupportedFiles(Settings::models().mLoadUnsupportedNifFiles);

        mStateUpdater->setFogEnd(mViewDistance);

        // Hopefully, anything genuinely requiring the default alpha func of GL_ALWAYS explicitly sets it
        mRootNode->getOrCreateStateSet()->setAttribute(Shader::RemovedAlphaFunc::getInstance(GL_ALWAYS));
        // The transparent renderbin sets alpha testing on because that was faster on old GPUs. It's now slower and
        // breaks things.
        mRootNode->getOrCreateStateSet()->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF);

        if (reverseZ)
        {
            osg::ref_ptr<osg::ClipControl> clipcontrol
                = new osg::ClipControl(osg::ClipControl::LOWER_LEFT, osg::ClipControl::ZERO_TO_ONE);
            mRootNode->getOrCreateStateSet()->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
            mRootNode->getOrCreateStateSet()->setAttributeAndModes(clipcontrol, osg::StateAttribute::ON);
        }

        SceneUtil::setCameraClearDepth(mViewer->getCamera());

        updateProjectionMatrix();

        mViewer->getCamera()->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    RenderingManager::~RenderingManager()
    {
        clearLiveObjectsForShutdown();

        // let background loading thread finish before we delete anything else
        mWorkQueue = nullptr;
    }

    osgUtil::IncrementalCompileOperation* RenderingManager::getIncrementalCompileOperation()
    {
        return mViewer->getIncrementalCompileOperation();
    }

    MWRender::Objects& RenderingManager::getObjects()
    {
        return *mObjects.get();
    }

    Resource::ResourceSystem* RenderingManager::getResourceSystem()
    {
        return mResourceSystem;
    }

    SceneUtil::WorkQueue* RenderingManager::getWorkQueue()
    {
        return mWorkQueue.get();
    }

    Terrain::World* RenderingManager::getTerrain()
    {
        return mTerrain;
    }

    void RenderingManager::preloadCommonAssets()
    {
        osg::ref_ptr<PreloadCommonAssetsWorkItem> workItem(new PreloadCommonAssetsWorkItem(mResourceSystem));
        mSky->listAssetsToPreload(workItem->mModels, workItem->mTextures);
        mWater->listAssetsToPreload(workItem->mTextures);

        const VFS::Manager* vfs = mResourceSystem != nullptr ? mResourceSystem->getVFS() : nullptr;
        const bool hasMorrowindCommonActors = vfs != nullptr && vfs->exists(Settings::models().mXbaseanim)
            && vfs->exists(Settings::models().mXbaseanimkf);

        if (hasMorrowindCommonActors)
        {
            workItem->mModels.push_back(Settings::models().mXbaseanim);
            workItem->mModels.push_back(Settings::models().mXbaseanim1st);
            workItem->mModels.push_back(Settings::models().mXbaseanimfemale);
            workItem->mModels.push_back(Settings::models().mXargonianswimkna);

            workItem->mKeyframes.push_back(Settings::models().mXbaseanimkf);
            workItem->mKeyframes.push_back(Settings::models().mXbaseanim1stkf);
            workItem->mKeyframes.push_back(Settings::models().mXbaseanimfemalekf);
            workItem->mKeyframes.push_back(Settings::models().mXargonianswimknakf);
        }
        else
            Log(Debug::Info) << "World viewer: skipped Morrowind common actor preloads because xbase assets are absent";

        workItem->mTextures.emplace_back("textures/_land_default.dds");

        mWorkQueue->addWorkItem(std::move(workItem));
    }

    double RenderingManager::getReferenceTime() const
    {
        return mViewer->getFrameStamp()->getReferenceTime();
    }

    SceneUtil::LightManager* RenderingManager::getLightRoot()
    {
        return mSceneRoot.get();
    }

    void RenderingManager::setNightEyeFactor(float factor)
    {
        if (factor != mNightEyeFactor)
        {
            mNightEyeFactor = factor;
            updateAmbient();
        }
    }

    void RenderingManager::setAmbientColour(const osg::Vec4f& colour)
    {
        mAmbientColor = colour;
        updateAmbient();
    }

    int RenderingManager::skyGetMasserPhase() const
    {
        return mSky->getMasserPhase();
    }

    int RenderingManager::skyGetSecundaPhase() const
    {
        return mSky->getSecundaPhase();
    }

    void RenderingManager::skySetMoonColour(bool red)
    {
        mSky->setMoonColour(red);
    }

    void RenderingManager::configureAmbient(const MWWorld::Cell& cell)
    {
        bool isInterior = !cell.isExterior() && !cell.isQuasiExterior();
        bool needsAdjusting = false;
        if (mResourceSystem->getSceneManager()->getLightingMethod() != SceneUtil::LightingMethod::FFP)
            needsAdjusting = isInterior && !Settings::shaders().mClassicFalloff;

        osg::Vec4f ambient = SceneUtil::colourFromRGB(cell.getMood().mAmbiantColor);

        if (needsAdjusting)
        {
            constexpr float pR = 0.2126;
            constexpr float pG = 0.7152;
            constexpr float pB = 0.0722;

            // we already work in linear RGB so no conversions are needed for the luminosity function
            float relativeLuminance = pR * ambient.r() + pG * ambient.g() + pB * ambient.b();
            const float minimumAmbientLuminance = Settings::shaders().mMinimumInteriorBrightness;
            if (relativeLuminance < minimumAmbientLuminance)
            {
                // brighten ambient so it reaches the minimum threshold but no more, we want to mess with content data
                // as least we can
                if (ambient.r() == 0.f && ambient.g() == 0.f && ambient.b() == 0.f)
                    ambient = osg::Vec4(
                        minimumAmbientLuminance, minimumAmbientLuminance, minimumAmbientLuminance, ambient.a());
                else
                    ambient *= minimumAmbientLuminance / relativeLuminance;
            }
        }

        setAmbientColour(ambient);

        osg::Vec4f diffuse = SceneUtil::colourFromRGB(cell.getMood().mDirectionalColor);

        setSunColour(diffuse, diffuse, 1.f);
        // This is total nonsense but it's what Morrowind uses
        static const osg::Vec4f interiorSunPos
            = osg::Vec4f(-1.f, osg::DegreesToRadians(45.f), osg::DegreesToRadians(45.f), 0.f);
        mPostProcessor->getStateUpdater()->setSunPos(interiorSunPos, false);
        mSunLight->setPosition(interiorSunPos);
    }

    void RenderingManager::setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis)
    {
        // need to wrap this in a StateUpdater?
        mSunLight->setDiffuse(diffuse);
        mSunLight->setSpecular(osg::Vec4f(specular.x(), specular.y(), specular.z(), specular.w() * sunVis));

        mPostProcessor->getStateUpdater()->setSunColor(diffuse);
        mPostProcessor->getStateUpdater()->setSunVis(sunVis);
    }

    void RenderingManager::setSunDirection(const osg::Vec3f& direction)
    {
        osg::Vec3f position = -direction;

        // The sun is not synchronized with the sunlight because reasons
        // This is based on exterior sun orbit and won't make sense for interiors, see WeatherManager::update
        position.z() = 400.f - std::abs(position.x());

        // need to wrap this in a StateUpdater?
        if (Settings::shaders().mMatchSunlightToSun)
            mSunLight->setPosition(osg::Vec4f(position, 0.f));
        else
            mSunLight->setPosition(osg::Vec4f(-direction, 0.f));

        mSky->setSunDirection(position);

        mPostProcessor->getStateUpdater()->setSunPos(osg::Vec4f(position, 0.f), mNight);
    }

    void RenderingManager::setSunPosition(const osg::Vec3f& position)
    {
        setSunPosition(position, position);
    }

    void RenderingManager::setSunPosition(const osg::Vec3f& skyPosition, const osg::Vec3f& lightingPosition)
    {
        mSunLight->setPosition(osg::Vec4f(lightingPosition, 0.f));
        mSky->setSunDirection(skyPosition);
        mPostProcessor->getStateUpdater()->setSunPos(osg::Vec4f(skyPosition, 0.f), mNight);
    }

    void RenderingManager::addCell(const MWWorld::CellStore* store)
    {
        mPathgrid->addCell(store);

        mWater->changeCell(store);

        if (store->getCell()->isExterior())
        {
            enableTerrain(true, store->getCell()->getWorldSpace());
            mTerrain->loadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }
    }
    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mPathgrid->removeCell(store);
        mActorsPaths->removeCell(store);
        mObjects->removeCell(store);

        if (store->getCell()->isExterior())
        {
            getWorldspaceChunkMgr(store->getCell()->getWorldSpace())
                .mTerrain->unloadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }

        mWater->removeCell(store);
    }

    void RenderingManager::enableTerrain(bool enable, ESM::RefId worldspace)
    {
        if (!enable)
            mWater->setCullCallback(nullptr);
        else
        {
            WorldspaceChunkMgr& newChunks = getWorldspaceChunkMgr(worldspace);
            if (newChunks.mTerrain.get() != mTerrain)
            {
                mTerrain->enable(false);
                mTerrain = newChunks.mTerrain.get();
                mGroundcover = newChunks.mGroundcover.get();
                mObjectPaging = newChunks.mObjectPaging.get();
            }
        }
        mTerrain->enable(enable);
    }

    void RenderingManager::setSkyEnabled(bool enabled)
    {
        mSky->setEnabled(enabled);
        if (enabled)
            mShadowManager->enableOutdoorMode();
        else
            mShadowManager->enableIndoorMode(Settings::shadows());
        mPostProcessor->getStateUpdater()->setIsInterior(!enabled);
    }

    bool RenderingManager::toggleBorders()
    {
        bool borders = !mTerrain->getBordersVisible();
        mTerrain->setBordersVisible(borders);
        return borders;
    }

    bool RenderingManager::toggleRenderMode(RenderMode mode)
    {
        if (mode == Render_CollisionDebug || mode == Render_Pathgrid)
            return mPathgrid->toggleRenderMode(mode);
        else if (mode == Render_Wireframe)
        {
            bool wireframe = !mStateUpdater->getWireframe();
            mStateUpdater->setWireframe(wireframe);
            return wireframe;
        }
        else if (mode == Render_Water)
        {
            return mWater->toggle();
        }
        else if (mode == Render_Scene)
        {
            const auto wm = MWBase::Environment::get().getWindowManager();
            unsigned int mask = wm->getCullMask();
            bool enabled = !(mask & sToggleWorldMask);
            if (enabled)
                mask |= sToggleWorldMask;
            else
                mask &= ~sToggleWorldMask;
            mWater->showWorld(enabled);
            wm->setCullMask(mask);
//## VR_PATCH BEGIN
            mViewer->getCamera()->setCullMaskLeft(mask);
            mViewer->getCamera()->setCullMaskRight(mask);
//## VR_PATCH END
            return enabled;
        }
        else if (mode == Render_NavMesh)
        {
            return mNavMesh->toggle();
        }
        else if (mode == Render_ActorsPaths)
        {
            return mActorsPaths->toggle();
        }
        else if (mode == Render_RecastMesh)
        {
            return mRecastMesh->toggle();
        }
        return false;
    }

    void RenderingManager::configureFog(const MWWorld::Cell& cell)
    {
        mFog->configure(mViewDistance, cell);
    }

    void RenderingManager::configureFog(
        float fogDepth, float underwaterFog, float dlFactor, float dlOffset, const osg::Vec4f& color)
    {
        mFog->configure(mViewDistance, fogDepth, underwaterFog, dlFactor, dlOffset, color);
    }

    void RenderingManager::configureFog(float fogNear, float fogFar, float underwaterFog, const osg::Vec4f& color)
    {
        mFog->configureExplicit(fogNear, fogFar, underwaterFog, color);
    }

    SkyManager* RenderingManager::getSkyManager()
    {
        return mSky.get();
    }

    void RenderingManager::update(float dt, bool paused)
    {
        reportStats();

        mResourceSystem->getSceneManager()->getShaderManager().update(*mViewer);

        float rainIntensity = mSky->getPrecipitationAlpha();
        mWater->setRainIntensity(rainIntensity);
        mWater->setRainRipplesEnabled(mSky->getRainRipplesEnabled());

        mWater->update(dt, paused);
        if (!paused)
        {
            mEffectManager->update(dt);
            mSky->update(dt);

            const MWWorld::Ptr& player = mPlayerAnimation->getPtr();
            osg::Vec3f playerPos(player.getRefData().getPosition().asVec3());

            float windSpeed = mSky->getBaseWindSpeed();
            mSharedUniformStateUpdater->setWindSpeed(windSpeed);
            mSharedUniformStateUpdater->setPlayerPos(playerPos);

            if (mFalloutPlayerVisualAnimation)
            {
                const ESM4::Weapon* liveWeapon = nullptr;
                std::vector<const ESM4::Armor*> liveArmor;
                const bool hasLiveInventory = player.getClass().hasInventoryStore(player);
                if (hasLiveInventory)
                {
                    const MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
                    liveArmor = collectLiveFalloutArmor(inventory);
                    const MWWorld::ConstContainerStoreIterator weapon
                        = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                    if (weapon != inventory.end() && weapon->getType() == ESM4::Weapon::sRecordId)
                        liveWeapon = weapon->get<ESM4::Weapon>()->mBase;
                }

                const MWWorld::Ptr visualPtr = mFalloutPlayerVisualAnimation->getPtr();
                const bool weaponChanged
                    = MWClass::ESM4Npc::setEquippedWeapon(visualPtr, liveWeapon);
                const MWMechanics::DrawState liveDrawState
                    = player.getClass().getCreatureStats(player).getDrawState();
                MWMechanics::CreatureStats& visualStats = visualPtr.getClass().getCreatureStats(visualPtr);
                const bool drawStateChanged = visualStats.getDrawState() != liveDrawState;
                if (drawStateChanged)
                    visualStats.setDrawState(liveDrawState);

                if (weaponChanged)
                {
                    const std::uint8_t animationType = liveWeapon != nullptr ? liveWeapon->mData.animationType : 0;
                    const std::uint8_t reloadAnimation = liveWeapon != nullptr ? liveWeapon->mData.reloadAnim : 0;
                    const bool thirdPersonPrepared = mFalloutPlayerVisualAnimation->prepareFalloutWeaponAnimation(
                        animationType, reloadAnimation, FonvWeaponAction::Equip);
                    const bool firstPersonPrepared = mFalloutPlayerFirstPersonAnimation == nullptr
                        || mFalloutPlayerFirstPersonAnimation->prepareFalloutWeaponAnimation(
                            animationType, reloadAnimation, FonvWeaponAction::Equip);
                    Log(thirdPersonPrepared && firstPersonPrepared ? Debug::Info : Debug::Error)
                        << "FNV player equipment bridge: weapon="
                        << (liveWeapon != nullptr ? liveWeapon->mEditorId : std::string("none"))
                        << " animationType=" << static_cast<unsigned int>(animationType)
                        << " thirdPersonPrepared=" << thirdPersonPrepared
                        << " firstPersonPrepared=" << firstPersonPrepared << " source=live-inventory-slot";
                }
                if (weaponChanged || drawStateChanged)
                {
                    const bool shown = liveDrawState == MWMechanics::DrawState::Weapon && liveWeapon != nullptr;
                    mFalloutPlayerVisualAnimation->showWeapons(shown);
                    if (mFalloutPlayerFirstPersonAnimation)
                        mFalloutPlayerFirstPersonAnimation->showWeapons(shown);
                }

                const std::vector<ESM::FormId> wornSignature = makeFalloutWornVisualSignature(liveArmor);
                if (hasLiveInventory
                    && (!mFalloutPlayerFirstPersonWornSignatureObserved
                        || wornSignature != mFalloutPlayerFirstPersonWornSignature))
                {
                    mFalloutPlayerFirstPersonWornSignature = wornSignature;
                    mFalloutPlayerFirstPersonWornSignatureObserved = true;
                    const std::optional<ESM4NpcAnimation::FirstPersonState> profile
                        = makeFalloutFirstPersonState(liveArmor, visualPtr, mFirstPersonFieldOfView, mResourceSystem);
                    if (profile)
                    {
                        if (!mFalloutPlayerFirstPersonBasis)
                        {
                            mFalloutPlayerFirstPersonBasis = new osg::MatrixTransform;
                            mFalloutPlayerFirstPersonBasis->setName("FNV First Person Camera1st Alignment");
                            mFalloutPlayerFirstPersonBasis->setMatrix(osg::Matrix::identity());
                            mSceneRoot->addChild(mFalloutPlayerFirstPersonBasis);
                        }
                        try
                        {
                            osg::ref_ptr<ESM4NpcAnimation> replacement = new ESM4NpcAnimation(visualPtr,
                                osg::ref_ptr<osg::Group>(mFalloutPlayerFirstPersonBasis), mResourceSystem, *profile);
                            const bool shown
                                = liveDrawState == MWMechanics::DrawState::Weapon && liveWeapon != nullptr;
                            replacement->showWeapons(shown);
                            if (osg::Group* root = replacement->getObjectRoot())
                            {
                                const bool visible = mCamera->getMode() == Camera::Mode::FirstPerson;
                                root->setNodeMask(visible ? Mask_FirstPerson : 0);
                            }
                            mFalloutPlayerFirstPersonAnimation = std::move(replacement);
                            mFalloutPlayerFirstPersonAlignmentLogged = false;
                            Log(Debug::Info) << "FNV first-person equipment bridge: rebuilt=1 armor="
                                             << liveArmor.size() << " signature=" << wornSignature.size()
                                             << " pipBoy=" << profile->mPipBoy
                                             << " pipBoyGlove=" << profile->mPipBoyGlove;
                        }
                        catch (const std::exception& error)
                        {
                            Log(Debug::Error) << "FNV first-person equipment bridge: rebuilt=0 armor="
                                              << liveArmor.size() << " reason=" << error.what();
                        }
                    }
                }

                const MWMechanics::Movement& movement = player.getClass().getMovementSettings(player);
                std::string requestedGroup = "idle";
                const std::string driverGroup(mPlayerAnimation->getActiveGroup(BoneGroup_LowerBody));
                const auto mapDriverLocomotionGroup = [](std::string_view group) -> std::string_view {
                    if (group.find("forward") != std::string_view::npos)
                        return "walkforward";
                    if (group.find("back") != std::string_view::npos)
                        return "walkback";
                    if (group.find("left") != std::string_view::npos)
                        return "walkleft";
                    if (group.find("right") != std::string_view::npos)
                        return "walkright";
                    return {};
                };
                const std::string_view driverLocomotionGroup = mapDriverLocomotionGroup(driverGroup);
                if (!driverLocomotionGroup.empty())
                    requestedGroup = driverLocomotionGroup;
                else if (std::abs(movement.mPosition[0]) > 0.01f || std::abs(movement.mPosition[1]) > 0.01f)
                {
                    if (std::abs(movement.mPosition[1]) >= std::abs(movement.mPosition[0]))
                        requestedGroup = movement.mPosition[1] >= 0.f ? "walkforward" : "walkback";
                    else
                        requestedGroup = movement.mPosition[0] >= 0.f ? "walkright" : "walkleft";
                }

                osg::Vec2f horizontalDelta;
                if (mFalloutPlayerVisualPreviousPositionValid)
                {
                    horizontalDelta.set(playerPos.x() - mFalloutPlayerVisualPreviousPosition.x(),
                        playerPos.y() - mFalloutPlayerVisualPreviousPosition.y());
                    if (requestedGroup == "idle" && horizontalDelta.length2() > 0.0001f)
                        requestedGroup = "walkforward";
                }
                mFalloutPlayerVisualPreviousPosition = playerPos;
                mFalloutPlayerVisualPreviousPositionValid = true;
                const std::string movementRequestedGroup = requestedGroup;
                const bool authoredGroupAvailable = mFalloutPlayerVisualAnimation->hasAnimation(requestedGroup);
                ESM4NpcAnimation* esm4PlayerVisual
                    = dynamic_cast<ESM4NpcAnimation*>(mFalloutPlayerVisualAnimation.get());
                const bool useProcedural = !authoredGroupAvailable && esm4PlayerVisual != nullptr
                    && esm4PlayerVisual->supportsProceduralHumanoidLocomotion();
                if (!authoredGroupAvailable && !useProcedural)
                    requestedGroup = "idle";

                const bool groupChanged = requestedGroup != mFalloutPlayerVisualGroup;
                if (groupChanged)
                {
                    if (!mFalloutPlayerVisualGroup.empty()
                        && mFalloutPlayerVisualAnimation->hasAnimation(mFalloutPlayerVisualGroup))
                        mFalloutPlayerVisualAnimation->disable(mFalloutPlayerVisualGroup);
                    mFalloutPlayerVisualGroup = std::move(requestedGroup);
                    if (!useProcedural && mFalloutPlayerVisualAnimation->hasAnimation(mFalloutPlayerVisualGroup))
                    {
                        mFalloutPlayerVisualAnimation->play(mFalloutPlayerVisualGroup, Animation::AnimPriority(1),
                            BlendMask_All, false, 1.f, "start", "stop", 0.f,
                            std::numeric_limits<std::uint32_t>::max(), true);
                    }
                    mFalloutPlayerVisualGroupElapsed = 0.f;
                    mFalloutPlayerVisualCycleLogged = false;
                }
                mFalloutPlayerVisualAnimation->runAnimation(dt);
                bool proceduralApplied = false;
                if (useProcedural)
                {
                    proceduralApplied = esm4PlayerVisual->applyProceduralHumanoidLocomotion(
                        mFalloutPlayerVisualGroup, mFalloutPlayerVisualGroupElapsed + dt);
                }
                const bool selectedGroupAvailable = authoredGroupAvailable || proceduralApplied;
                if (groupChanged)
                {
                    Log(Debug::Info) << "ESM4 player visual locomotion: phase=selected requested=\""
                                     << movementRequestedGroup << "\" selected=\"" << mFalloutPlayerVisualGroup
                                     << "\" available=" << selectedGroupAvailable
                                     << " authoredAvailable=" << authoredGroupAvailable
                                     << " driver=\"" << (useProcedural ? "procedural-humanoid-ik" : driverGroup)
                                     << "\" side=" << movement.mPosition[0]
                                     << " forward=" << movement.mPosition[1] << " horizontalDelta=("
                                     << horizontalDelta.x() << "," << horizontalDelta.y() << ")";
                }
                mFalloutPlayerVisualGroupElapsed += dt;
                if (!mFalloutPlayerVisualCycleLogged && mFalloutPlayerVisualGroupElapsed >= 0.25f)
                {
                    mFalloutPlayerVisualCycleLogged = true;
                    const float animationTime = useProcedural
                        ? mFalloutPlayerVisualGroupElapsed
                        : mFalloutPlayerVisualAnimation->getCurrentTime(mFalloutPlayerVisualGroup);
                    Log(Debug::Info) << "ESM4 player visual locomotion: phase=advanced selected=\""
                                     << mFalloutPlayerVisualGroup << "\" elapsed="
                                     << mFalloutPlayerVisualGroupElapsed << " animationTime=" << animationTime
                                     << " driver=\"" << (useProcedural ? "procedural-humanoid-ik" : driverGroup)
                                     << "\" available=" << selectedGroupAvailable;
                }
            }
            if (mFalloutPlayerFirstPersonAnimation)
                mFalloutPlayerFirstPersonAnimation->runAnimation(dt);
        }

        updateNavMesh();
        updateRecastMesh();

        if (mUpdateProjectionMatrix)
        {
            mUpdateProjectionMatrix = false;
            updateProjectionMatrix();
        }
        mCamera->update(dt, paused);

        if (mFalloutPlayerFirstPersonAnimation && mFalloutPlayerFirstPersonBasis
            && mCamera->getMode() == Camera::Mode::FirstPerson)
        {
            // The Fallout first-person skeleton is authored around Camera1st.  Solve
            // its world transform from that anchor instead of tuning offsets against
            // one screenshot: Camera1st * basis == the live gameplay camera frame.
            const osg::Node* authoredCamera = mFalloutPlayerFirstPersonAnimation->getNode("Camera1st");
            bool aligned = false;
            if (authoredCamera != nullptr)
            {
                for (const osg::NodePath& path : authoredCamera->getParentalNodePaths())
                {
                    const auto basisIt = std::find(path.begin(), path.end(), mFalloutPlayerFirstPersonBasis.get());
                    if (basisIt == path.end() || std::next(basisIt) == path.end())
                        continue;

                    const osg::NodePath authoredPath(std::next(basisIt), path.end());
                    const osg::Matrixd authoredCameraInBasis = osg::computeLocalToWorld(authoredPath);
                    const osg::Matrixd gameplayCameraFrame
                        = osg::Matrixd::rotate(mCamera->getOrient())
                        * osg::Matrixd::translate(mCamera->getPosition());
                    const osg::Matrixd solvedBasis
                        = osg::Matrixd::inverse(authoredCameraInBasis) * gameplayCameraFrame;
                    mFalloutPlayerFirstPersonBasis->setMatrix(solvedBasis);

                    if (!mFalloutPlayerFirstPersonAlignmentLogged)
                    {
                        const osg::Matrixd alignedCamera = authoredCameraInBasis * solvedBasis;
                        const double positionResidual
                            = (alignedCamera.getTrans() - gameplayCameraFrame.getTrans()).length();
                        osg::Vec3d alignedForward
                            = alignedCamera.getRotate() * osg::Vec3d(0.0, 1.0, 0.0);
                        osg::Vec3d gameplayForward
                            = gameplayCameraFrame.getRotate() * osg::Vec3d(0.0, 1.0, 0.0);
                        alignedForward.normalize();
                        gameplayForward.normalize();
                        const double forwardResidualRadians = std::acos(std::clamp(
                            alignedForward * gameplayForward, -1.0, 1.0));
                        const bool exact = positionResidual <= 1e-4 && forwardResidualRadians <= 1e-5;
                        Log(exact ? Debug::Info : Debug::Error)
                            << "FNV first-person camera alignment: anchor=Camera1st positionResidual="
                            << positionResidual << " forwardResidualRadians=" << forwardResidualRadians
                            << " exact=" << exact;
                        mFalloutPlayerFirstPersonAlignmentLogged = true;
                    }
                    aligned = true;
                    break;
                }
            }
            if (!aligned && !mFalloutPlayerFirstPersonAlignmentLogged)
            {
                Log(Debug::Error) << "FNV first-person camera alignment: Camera1st path is unavailable";
                mFalloutPlayerFirstPersonAlignmentLogged = true;
            }
        }

        if (mFalloutPlayerVisualAnimation)
        {
            const bool proofHidePlayerVisual = envFlagEnabled("OPENMW_PROOF_HIDE_PLAYER_VISUAL")
                || envFlagEnabled("OPENMW_FNV_HIDE_PLAYER_PROOF_PARTS");
            const bool showThirdPersonPlayer = !VR::getVR() && !proofHidePlayerVisual
                && mCamera->getMode() != Camera::Mode::FirstPerson;
            if (osg::Group* playerVisualRoot = mFalloutPlayerVisualAnimation->getObjectRoot())
                playerVisualRoot->setNodeMask(showThirdPersonPlayer ? Mask_Player : 0);
        }
        if (mFalloutPlayerFirstPersonAnimation)
        {
            const bool showFirstPersonPlayer
                = !VR::getVR() && mCamera->getMode() == Camera::Mode::FirstPerson;
            if (osg::Group* firstPersonRoot = mFalloutPlayerFirstPersonAnimation->getObjectRoot())
                firstPersonRoot->setNodeMask(showFirstPersonPlayer ? Mask_FirstPerson : 0);
        }

        bool isUnderwater = mWater->isUnderwater(mCamera->getPosition());

        float fogStart = mFog->getFogStart(isUnderwater);
        float fogEnd = mFog->getFogEnd(isUnderwater);
        osg::Vec4f fogColor = mFog->getFogColor(isUnderwater);

        mStateUpdater->setFogStart(fogStart);
        mStateUpdater->setFogEnd(fogEnd);
        setFogColor(fogColor);

        auto world = MWBase::Environment::get().getWorld();
        const auto& stateUpdater = mPostProcessor->getStateUpdater();

        stateUpdater->setFogRange(fogStart, fogEnd);
        stateUpdater->setNearFar(mNearClip, mViewDistance);
        stateUpdater->setIsUnderwater(isUnderwater);
        stateUpdater->setFogColor(fogColor);
        stateUpdater->setGameHour(world->getTimeStamp().getHour());
        stateUpdater->setWeatherId(world->getCurrentWeatherScriptId());
        stateUpdater->setNextWeatherId(world->getNextWeatherScriptId());
        stateUpdater->setWeatherTransition(world->getWeatherTransition());
        stateUpdater->setWindSpeed(world->getWindSpeed());
        stateUpdater->setSkyColor(mSky->getSkyColor());
        mPostProcessor->setUnderwaterFlag(isUnderwater);
//## VR_PATCH BEGIN

        mPlayerAnimation->updateCrosshairs();
//## VR_PATCH END
    }

    void RenderingManager::updatePlayerPtr(const MWWorld::Ptr& ptr)
    {
        if (mPlayerAnimation.get())
        {
            setupPlayer(ptr);
            mPlayerAnimation->updatePtr(ptr);
            if (VR::getVR())
            {
                if (auto* vrAnimation = dynamic_cast<MWVR::VRAnimation*>(mPlayerAnimation.get()))
                {
                    if (ptr.getType() == ESM4::Npc::sRecordId)
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly save-loaded live player surface refresh";
                        logFalloutVrHandSourceCandidates(ptr, "save-loaded-vr-player-ptr");
                        vrAnimation->setViewMode(NpcAnimation::VM_VRFirstPerson);
                        vrAnimation->setFalloutVrHandSurfaces(
                            collectFalloutVrHandSurfaces(ptr, "save-loaded-vr-player-ptr"));
                    }
                    else if (const ESM4::Npc* falloutPlayerVisualRecord = findFalloutPlayerVisualRecord())
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly save-loaded visual-record surface refresh";
                        logFalloutVrHandSelectionDiagnostic(ptr, falloutPlayerVisualRecord);
                        vrAnimation->setViewMode(NpcAnimation::VM_VRFirstPerson);
                        vrAnimation->setFalloutVrHandSurfaces(
                            collectFalloutVrHandSurfacesForVisualRecord(ptr, falloutPlayerVisualRecord));
                    }
                }
            }
        }
        mCamera->attachTo(ptr);
    }

    void RenderingManager::removePlayer(const MWWorld::Ptr& player)
    {
        mWater->removeEmitter(player);
    }

    void RenderingManager::rotateObject(const MWWorld::Ptr& ptr, const osg::Quat& rot)
    {
        if (ptr == mCamera->getTrackingPtr() && !mCamera->isVanityOrPreviewModeEnabled())
        {
            mCamera->rotateCameraToTrackingPtr();
        }

        ptr.getRefData().getBaseNode()->setAttitude(rot);
    }

    void RenderingManager::moveObject(const MWWorld::Ptr& ptr, const osg::Vec3f& pos)
    {
        ptr.getRefData().getBaseNode()->setPosition(pos);
    }

    void RenderingManager::scaleObject(const MWWorld::Ptr& ptr, const osg::Vec3f& scale)
    {
        ptr.getRefData().getBaseNode()->setScale(scale);

        if (ptr == mCamera->getTrackingPtr()) // update height of camera
            mCamera->processViewChange();
    }

    void RenderingManager::removeObject(const MWWorld::Ptr& ptr)
    {
        mActorsPaths->remove(ptr);
        mObjects->removeObject(ptr);
        mWater->removeEmitter(ptr);
    }

    void RenderingManager::setWaterEnabled(bool enabled)
    {
        mWater->setEnabled(enabled);
        mSky->setWaterEnabled(enabled);

        mPostProcessor->getStateUpdater()->setIsWaterEnabled(enabled);
    }

    void RenderingManager::setWaterHeight(float height)
    {
        mWater->setCullCallback(mTerrain->getHeightCullCallback(height, Mask_Water));
        mWater->setHeight(height);
        mSky->setWaterHeight(height);

        mPostProcessor->getStateUpdater()->setWaterHeight(height);
    }

    void RenderingManager::screenshot(osg::Image* image, int w, int h)
    {
        mScreenshotManager->screenshot(image, w, h);
    }

    osg::Vec2f RenderingManager::getScreenCoords(const osg::BoundingBox& bb)
    {
        if (bb.valid())
        {
            const osg::Matrix viewProj
                = mViewer->getCamera()->getViewMatrix() * mViewer->getCamera()->getProjectionMatrix();
            const osg::Vec3f worldPoint((bb.xMin() + bb.xMax()) * 0.5f, (bb.yMin() + bb.yMax()) * 0.5f, bb.zMax());
            const osg::Vec4f clipPoint = osg::Vec4f(worldPoint, 1.0f) * viewProj;
            if (clipPoint.w() > 0.f)
            {
                const float screenPointX = (clipPoint.x() / clipPoint.w() + 1.f) * 0.5f;
                const float screenPointY = (clipPoint.y() / clipPoint.w() - 1.f) * (-0.5f);
                if (screenPointX >= 0.f && screenPointX <= 1.f && screenPointY >= 0.f && screenPointY <= 1.f)
                    return osg::Vec2f(screenPointX, screenPointY);
            }
        }

        return osg::Vec2f(0.5f, 0.f);
    }

    RayResult getIntersectionResult(osgUtil::LineSegmentIntersector* intersector,
        const osg::ref_ptr<osgUtil::IntersectionVisitor>& visitor, std::span<const MWWorld::Ptr> ignoreList = {})
    {
//## VR_PATCH BEGIN
// VR needs the actual node hit, because the 3dgui does not exist as an object in the world.
        RayResult result;
        constexpr auto nonObjectWorldMask = Mask_Terrain | Mask_Water;
        result.mHit = false;
        result.mRatio = 0;
        result.mHitNode = nullptr;

        if (!intersector->containsIntersections())
            return result;

        auto test = [&](const osgUtil::LineSegmentIntersector::Intersection& intersection) {
//## VR_PATCH END
            PtrHolder* ptrHolder = nullptr;
            std::vector<RefnumMarker*> refnumMarkers;
            bool hitNonObjectWorld = false;
            for (osg::NodePath::const_iterator it = intersection.nodePath.begin(); it != intersection.nodePath.end();
                 ++it)
            {
                const auto& nodeMask = (*it)->getNodeMask();
                if (!hitNonObjectWorld)
                    hitNonObjectWorld = nodeMask & nonObjectWorldMask;

                osg::UserDataContainer* userDataContainer = (*it)->getUserDataContainer();
                if (!userDataContainer)
                    continue;
                for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
                {
                    if (PtrHolder* p = dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                    {
                        if (std::find(ignoreList.begin(), ignoreList.end(), p->mPtr) == ignoreList.end())
                        {
                            ptrHolder = p;
                        }
                    }
                    if (RefnumMarker* r = dynamic_cast<RefnumMarker*>(userDataContainer->getUserObject(i)))
                    {
                        refnumMarkers.push_back(r);
                    }
                }
            }

            if (ptrHolder)
                result.mHitObject = ptrHolder->mPtr;

            unsigned int vertexCounter = 0;
            for (unsigned int i = 0; i < refnumMarkers.size(); ++i)
            {
                unsigned int intersectionIndex = intersection.indexList.empty() ? 0 : intersection.indexList[0];
                if (!refnumMarkers[i]->mNumVertices
                    || (intersectionIndex >= vertexCounter
                        && intersectionIndex < vertexCounter + refnumMarkers[i]->mNumVertices))
                {
                    auto it = std::find_if(
                        ignoreList.begin(), ignoreList.end(), [target = refnumMarkers[i]->mRefnum](const auto& ptr) {
                            return target == ptr.getCellRef().getRefNum();
                        });

                    if (it == ignoreList.end())
                    {
                        result.mHitRefnum = refnumMarkers[i]->mRefnum;
                    }

                    break;
                }
                vertexCounter += refnumMarkers[i]->mNumVertices;
            }

            if (!result.mHitObject.isEmpty() || result.mHitRefnum.isSet() || hitNonObjectWorld)
            {
                result.mHit = true;
                result.mHitNode = intersection.nodePath.empty() ? nullptr : intersection.nodePath.back();
                result.mHitNodePath.clear();
                result.mHitNodePath.reserve(intersection.nodePath.size());
                for (const osg::Node* node : intersection.nodePath)
                {
                    if (node != nullptr && !node->getName().empty())
                        result.mHitNodePath.push_back(node->getName());
                }
                result.mHitPointWorld = intersection.getWorldIntersectPoint();
                result.mHitNormalWorld = intersection.getWorldIntersectNormal();
                result.mHitPointLocal = intersection.getLocalIntersectPoint();
                result.mRatio = intersection.ratio;
            }
        };

        if (ignoreList.empty() || intersector->getIntersectionLimit() != osgUtil::LineSegmentIntersector::NO_LIMIT)
        {
            test(intersector->getFirstIntersection());
        }
        else
        {
            for (const auto& intersection : intersector->getIntersections())
            {
                test(intersection);

                if (result.mHit)
                {
                    break;
                }
            }
        }

        return result;
    }

    class IntersectionVisitorWithIgnoreList : public osgUtil::IntersectionVisitor
    {
    public:
        bool skipTransform(osg::Transform& transform)
        {
            if (mContainsPagedRefs)
                return false;

            osg::UserDataContainer* userDataContainer = transform.getUserDataContainer();
            if (!userDataContainer)
                return false;

            for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
            {
                if (PtrHolder* p = dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                {
                    if (std::find(mIgnoreList.begin(), mIgnoreList.end(), p->mPtr) != mIgnoreList.end())
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        void apply(osg::Transform& transform) override
        {
            if (skipTransform(transform))
            {
                return;
            }
            osgUtil::IntersectionVisitor::apply(transform);
        }

        void setIgnoreList(std::span<const MWWorld::Ptr> ignoreList) { mIgnoreList = ignoreList; }
        void setContainsPagedRefs(bool contains) { mContainsPagedRefs = contains; }

    private:
        std::span<const MWWorld::Ptr> mIgnoreList;
        bool mContainsPagedRefs = false;
    };

    osg::ref_ptr<osgUtil::IntersectionVisitor> RenderingManager::getIntersectionVisitor(
//## VR_PATCH BEGIN
        osgUtil::Intersector* intersector, bool ignorePlayer, bool ignoreActors, uint32_t ignoreMask,
//## VR_PATCH END
        std::span<const MWWorld::Ptr> ignoreList)
    {
        if (!mIntersectionVisitor)
            mIntersectionVisitor = new IntersectionVisitorWithIgnoreList;

        mIntersectionVisitor->setIgnoreList(ignoreList);
        mIntersectionVisitor->setContainsPagedRefs(false);

        MWWorld::Scene* worldScene = MWBase::Environment::get().getWorldScene();
        for (const auto& ptr : ignoreList)
        {
            if (worldScene->isPagedRef(ptr))
            {
                mIntersectionVisitor->setContainsPagedRefs(true);
                intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::NO_LIMIT);
                break;
            }
        }

        mIntersectionVisitor->setTraversalNumber(mViewer->getFrameStamp()->getFrameNumber());
        mIntersectionVisitor->setFrameStamp(mViewer->getFrameStamp());
        mIntersectionVisitor->setIntersector(intersector);

        unsigned int mask = ~0u;
        mask &= ~(Mask_RenderToTexture | Mask_Sky | Mask_Debug | Mask_Effect | Mask_Water | Mask_SimpleWater
            | Mask_Groundcover | Mask_Pointer);
        if (ignorePlayer)
//## VR_PATCH BEGIN
// Ignore the 3d pointer, but include the 3d gui
            mask &= ~(Mask_Player);
        if (ignoreActors)
            mask &= ~(Mask_Actor | Mask_Player);
        mask &= ~ignoreMask;

        mIntersectionVisitor->setTraversalMask(mask);
        return mIntersectionVisitor;
    }

    RayResult RenderingManager::castRay(const osg::Vec3f& origin, const osg::Vec3f& dest,
        bool ignorePlayer, bool ignoreActors, uint32_t ignoreMask, std::span<const MWWorld::Ptr> ignoreList)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::LineSegmentIntersector::MODEL, origin, dest));
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        mRootNode->accept(*getIntersectionVisitor(intersector, ignorePlayer, ignoreActors, ignoreMask, ignoreList));

        return getIntersectionResult(intersector, mIntersectionVisitor, ignoreList);
    }

    RayResult RenderingManager::castRay(
        const osg::Transform* source, float maxDistance, bool ignorePlayer, bool ignoreActors, uint32_t ignoreMask)
    {

        if (source)
        {
            osg::Matrix worldMatrix = osg::computeLocalToWorld(source->getParentalNodePaths()[0]);

            osg::Vec3f direction = worldMatrix.getRotate() * osg::Vec3f(0, 1, 0);
            direction.normalize();

            osg::Vec3f raySource = worldMatrix.getTrans();
            osg::Vec3f rayTarget = worldMatrix.getTrans() + direction * maxDistance;

            return castRay(raySource, rayTarget, ignorePlayer, ignoreActors, ignoreMask);
        }
        return RayResult();
    }
//## VR_PATCH END

//## VR_PATCH BEGIN
    RayResult RenderingManager::castCameraToViewportRay(
        const float nX, const float nY, float maxDistance, bool ignorePlayer, bool ignoreActors, uint32_t ignoreMask)
//## VR_PATCH END
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(new osgUtil::LineSegmentIntersector(
            osgUtil::LineSegmentIntersector::PROJECTION, nX * 2.f - 1.f, nY * (-2.f) + 1.f));

        osg::Vec3d dist(0.f, 0.f, -maxDistance);

        dist = dist * mViewer->getCamera()->getProjectionMatrix();

        osg::Vec3d end = intersector->getEnd();
        end.z() = dist.z();
        intersector->setEnd(end);
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

//## VR_PATCH BEGIN
        mViewer->getCamera()->accept(*getIntersectionVisitor(intersector, ignorePlayer, ignoreActors, ignoreMask));
//## VR_PATCH END

        return getIntersectionResult(intersector, mIntersectionVisitor);
    }

    void RenderingManager::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated)
    {
        mObjects->updatePtr(old, updated);
        mActorsPaths->updatePtr(old, updated);
    }

    void RenderingManager::spawnEffect(VFS::Path::NormalizedView model, std::string_view texture,
        const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight)
    {
        mEffectManager->addEffect(model, texture, worldPosition, scale, isMagicVFX, useAmbientLight);
    }

    void RenderingManager::notifyWorldSpaceChanged()
    {
        mEffectManager->clear();
        mWater->clearRipples();
    }

    void RenderingManager::clear()
    {
        mSky->setMoonColour(false);
        mFalloutSaveWornVisualItems.clear();
        mFalloutPlayerFirstPersonWornSignature.clear();
        mFalloutPlayerFirstPersonWornSignatureObserved = false;

        notifyWorldSpaceChanged();
        if (mObjectPaging)
            mObjectPaging->clear();
    }

    void RenderingManager::clearLiveObjectsForShutdown()
    {
        if (mCamera)
        {
            mCamera->setAnimation(nullptr);
            mCamera->attachTo(MWWorld::Ptr());
        }

        if (mFalloutPlayerFirstPersonAnimation)
        {
            mFalloutPlayerFirstPersonAnimation->removeFromScene();
            mFalloutPlayerFirstPersonAnimation = nullptr;
        }
        if (mFalloutPlayerVisualAnimation)
        {
            mFalloutPlayerVisualAnimation->removeFromScene();
            mFalloutPlayerVisualAnimation = nullptr;
        }
        if (mFalloutPlayerVisualBasis)
        {
            while (mFalloutPlayerVisualBasis->getNumParents() > 0)
                mFalloutPlayerVisualBasis->getParent(0)->removeChild(mFalloutPlayerVisualBasis);
            mFalloutPlayerVisualBasis = nullptr;
        }
        if (mFalloutPlayerFirstPersonBasis)
        {
            while (mFalloutPlayerFirstPersonBasis->getNumParents() > 0)
                mFalloutPlayerFirstPersonBasis->getParent(0)->removeChild(mFalloutPlayerFirstPersonBasis);
            mFalloutPlayerFirstPersonBasis = nullptr;
        }
        mFalloutPlayerVisualGroup.clear();
        mFalloutPlayerVisualGroupElapsed = 0.f;
        mFalloutPlayerVisualCycleLogged = false;
        mFalloutPlayerFirstPersonAlignmentLogged = false;
        mFalloutPlayerVisualPreviousPositionValid = false;
        mFalloutPlayerFirstPersonWornSignature.clear();
        mFalloutPlayerFirstPersonWornSignatureObserved = false;
        mFalloutPlayerVisualRef.reset();

        if (mPlayerAnimation)
        {
            mPlayerAnimation->removeFromScene();
            mPlayerAnimation = nullptr;
        }

        if (mPlayerNode)
        {
            if (mPlayerNode->getNumParents() > 0)
                mPlayerNode->getParent(0)->removeChild(mPlayerNode);
            mPlayerNode = nullptr;
        }

        if (mObjects)
            mObjects->clear();

        if (mWater)
            mWater->clearRipples();
    }

    MWRender::Animation* RenderingManager::getAnimation(const MWWorld::Ptr& ptr)
    {
        if (mPlayerAnimation.get() && ptr == mPlayerAnimation->getPtr())
            return mPlayerAnimation.get();

        return mObjects->getAnimation(ptr);
    }

    const MWRender::Animation* RenderingManager::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        if (mPlayerAnimation.get() && ptr == mPlayerAnimation->getPtr())
            return mPlayerAnimation.get();

        return mObjects->getAnimation(ptr);
    }

    PostProcessor* RenderingManager::getPostProcessor()
    {
        return mPostProcessor;
    }

    void RenderingManager::setupPlayer(const MWWorld::Ptr& player)
    {
        if (!mPlayerNode)
        {
            mPlayerNode = new SceneUtil::PositionAttitudeTransform;
            mPlayerNode->setNodeMask(Mask_Player);
            mPlayerNode->setName("Player Root");
            mSceneRoot->addChild(mPlayerNode);
        }

        mPlayerNode->setUserDataContainer(new osg::DefaultUserDataContainer);
        mPlayerNode->getUserDataContainer()->addUserObject(new PtrHolder(player));

        player.getRefData().setBaseNode(mPlayerNode);

        mWater->removeEmitter(player);
        mWater->addEmitter(player);
    }

    void RenderingManager::renderPlayer(const MWWorld::Ptr& player)
    {
        if (mFalloutPlayerFirstPersonAnimation)
            mFalloutPlayerFirstPersonAnimation->removeFromScene();
        if (mFalloutPlayerVisualAnimation)
            mFalloutPlayerVisualAnimation->removeFromScene();
        if (mFalloutPlayerVisualBasis)
        {
            while (mFalloutPlayerVisualBasis->getNumParents() > 0)
                mFalloutPlayerVisualBasis->getParent(0)->removeChild(mFalloutPlayerVisualBasis);
        }
        if (mFalloutPlayerFirstPersonBasis)
        {
            while (mFalloutPlayerFirstPersonBasis->getNumParents() > 0)
                mFalloutPlayerFirstPersonBasis->getParent(0)->removeChild(mFalloutPlayerFirstPersonBasis);
        }
        mFalloutPlayerFirstPersonAnimation = nullptr;
        mFalloutPlayerVisualAnimation = nullptr;
        mFalloutPlayerVisualBasis = nullptr;
        mFalloutPlayerFirstPersonBasis = nullptr;
        mFalloutPlayerVisualRef.reset();
        mFalloutPlayerVisualGroup.clear();
        mFalloutPlayerVisualGroupElapsed = 0.f;
        mFalloutPlayerVisualCycleLogged = false;
        mFalloutPlayerFirstPersonAlignmentLogged = false;
        mFalloutPlayerVisualPreviousPositionValid = false;
        mFalloutPlayerFirstPersonWornSignature.clear();
        mFalloutPlayerFirstPersonWornSignatureObserved = false;
        const ESM4::Npc* falloutPlayerVisualRecord
            = VR::getVR() ? findFalloutPlayerVisualRecord() : findEsm4PlayerVisualRecord();
        const bool falloutFlatProfile = !VR::getVR() && falloutPlayerVisualRecord != nullptr;
        const bool falloutVrProfile = VR::getVR() && falloutPlayerVisualRecord != nullptr;

//## VR_PATCH BEGIN
        if(VR::getVR())
        {
            mPlayerAnimation
                = new MWVR::VRAnimation(player, player.getRefData().getBaseNode(), mResourceSystem, false, mSceneRoot);
            auto* vrAnimation = static_cast<MWVR::VRAnimation*>(mPlayerAnimation.get());
            if (falloutVrProfile)
            {
                vrAnimation->setViewMode(NpcAnimation::VM_VRFirstPerson);
                vrAnimation->setEnableCrosshairs(Settings::Manager::getBool("show 3D crosshairs", "VR"));
                logFalloutVrHandSelectionDiagnostic(player, falloutPlayerVisualRecord);
                vrAnimation->setFalloutVrHandSurfaces(
                    collectFalloutVrHandSurfacesForVisualRecord(player, falloutPlayerVisualRecord));
            }
            else
                vrAnimation->setEnableCrosshairs(Settings::Manager::getBool("show 3D crosshairs", "VR"));
        }
        else
        {
            mPlayerAnimation = new NpcAnimation(player, player.getRefData().getBaseNode(), mResourceSystem, 0,
                NpcAnimation::VM_Normal, mFirstPersonFieldOfView);
        }
//## VR_PATCH END

        const bool hideLocalPlayerVisual = VR::getVR();
        const bool proofHidePlayerVisual = envFlagEnabled("OPENMW_PROOF_HIDE_PLAYER_VISUAL")
            || envFlagEnabled("OPENMW_FNV_HIDE_PLAYER_PROOF_PARTS");
        const bool suppressFalloutPlayerProxy = hideLocalPlayerVisual || proofHidePlayerVisual;
        if (proofHidePlayerVisual)
            Log(Debug::Info) << "FNV/ESM4: skipped Fallout NPC player visual proxy for hidden player capture";
        if (const ESM4::Npc* falloutPlayerVisual
            = suppressFalloutPlayerProxy ? nullptr : falloutPlayerVisualRecord)
        {
            ESM::CellRef proxyRef;
            proxyRef.blank();
            proxyRef.mRefID = ESM::RefId::stringRefId("Player");
            mFalloutPlayerVisualRef = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, falloutPlayerVisual);
            mFalloutPlayerVisualRef->mData.setPosition(player.getRefData().getPosition());
            MWWorld::Ptr visualPtr(mFalloutPlayerVisualRef.get(), player.getCell());
            applyFalloutPlayerProxyConfiguredEquipment(visualPtr, "world");
            const std::optional<ESM4NpcAnimation::FirstPersonState> firstPersonProfile
                = falloutFlatProfile
                ? applyFalloutSaveWornPlayerVisuals(
                    mFalloutSaveWornVisualItems, visualPtr, mFirstPersonFieldOfView, mResourceSystem)
                : std::nullopt;
            if (falloutFlatProfile)
            {
                mFalloutPlayerFirstPersonWornSignature
                    = makeFalloutWornVisualSignature(MWClass::ESM4Npc::getEquippedArmor(visualPtr));
                mFalloutPlayerFirstPersonWornSignatureObserved = true;
            }
            visualPtr.getClass().getCreatureStats(visualPtr).setDrawState(
                player.getClass().getCreatureStats(player).getDrawState());

            Log(Debug::Info) << "ESM4 diag: using native player visual proxy "
                             << falloutPlayerVisual->mEditorId << " (" << ESM::RefId(falloutPlayerVisual->mId)
                             << ") on player root; hiding legacy ESM3 body";

            mFalloutPlayerVisualBasis = new osg::MatrixTransform;
            mFalloutPlayerVisualBasis->setName("FNV Player Visual Basis Conversion");
            const float playerVisualYawOffset = getFalloutFlatPlayerVisualYawOffset();
            mFalloutPlayerVisualBasis->setMatrix(
                osg::Matrix::rotate(playerVisualYawOffset, osg::Vec3f(0.f, 0.f, -1.f)));
            player.getRefData().getBaseNode()->addChild(mFalloutPlayerVisualBasis);
            Log(Debug::Info) << "FNV player visual basis: angleDegrees="
                             << osg::RadiansToDegrees(playerVisualYawOffset) << " axis=(0,0,-1)"
                             << " parent=" << player.getRefData().getBaseNode()->getName();

            mFalloutPlayerVisualAnimation = new ESM4NpcAnimation(
                visualPtr, osg::ref_ptr<osg::Group>(mFalloutPlayerVisualBasis), mResourceSystem);
            if (firstPersonProfile)
            {
                mFalloutPlayerFirstPersonBasis = new osg::MatrixTransform;
                mFalloutPlayerFirstPersonBasis->setName("FNV First Person Camera1st Alignment");
                mFalloutPlayerFirstPersonBasis->setMatrix(osg::Matrix::identity());
                // First-person geometry is camera-space, not player-root space.  Keeping
                // this basis on the scene root lets the per-frame Camera1st solve follow
                // pitch, yaw, roll and eye translation exactly.
                mSceneRoot->addChild(mFalloutPlayerFirstPersonBasis);
                mFalloutPlayerFirstPersonAnimation = new ESM4NpcAnimation(visualPtr,
                    osg::ref_ptr<osg::Group>(mFalloutPlayerFirstPersonBasis), mResourceSystem,
                    *firstPersonProfile);
                Log(Debug::Info) << "FNV first-person profile created: attachedNodeCount="
                                 << mFalloutPlayerFirstPersonAnimation->getFirstPersonAttachedPartCount()
                                 << " fov=" << firstPersonProfile->mFieldOfView
                                 << " anchor=Camera1st profile=flat-first-person";
            }
            if (osg::Group* legacyPlayerRoot = mPlayerAnimation->getObjectRoot())
                legacyPlayerRoot->setNodeMask(0);
            if (osg::Group* falloutRoot = mFalloutPlayerVisualAnimation->getObjectRoot())
                falloutRoot->setNodeMask(Mask_Player);
        }

        if (falloutFlatProfile || proofHidePlayerVisual)
        {
            if (osg::Group* legacyPlayerRoot = mPlayerAnimation->getObjectRoot())
                legacyPlayerRoot->setNodeMask(0);
            if (proofHidePlayerVisual && mFalloutPlayerVisualAnimation)
            {
                if (osg::Group* falloutRoot = mFalloutPlayerVisualAnimation->getObjectRoot())
                    falloutRoot->setNodeMask(0);
            }
            Log(Debug::Info) << "ESM4: hidden legacy player render root";
        }

        mCamera->setAnimation(mPlayerAnimation.get());
        mCamera->attachTo(player);

        if (falloutFlatProfile)
        {
            // Keep the hidden tracking rig's authored eye height.  Lowering it by 40
            // units places the camera inside the native torso, exposing the collar,
            // legs and the underside of first-person arms when looking up or down.
            const float profileEyeOffsetZ
                = envFloatOr("OPENMW_ESM4_FIRST_PERSON_EYE_OFFSET_Z", 0.f);
            mCamera->setFirstPersonProfileOffset(osg::Vec3f(0.f, 0.f, profileEyeOffsetZ));
            Log(Debug::Info) << "ESM4: persistent first-person eye offset z=" << profileEyeOffsetZ;
        }

        if (falloutVrProfile)
        {
            auto* vrAnimation = static_cast<MWVR::VRAnimation*>(mPlayerAnimation.get());
            vrAnimation->setViewMode(NpcAnimation::VM_VRFirstPerson);
            Log(Debug::Info) << "FNV/ESM4: VR player render uses first-person hand-only rig";
        }

        if (falloutFlatProfile || proofHidePlayerVisual)
        {
            if (osg::Group* legacyPlayerRoot = mPlayerAnimation->getObjectRoot())
                legacyPlayerRoot->setNodeMask(0);
            if (proofHidePlayerVisual && mFalloutPlayerVisualAnimation)
            {
                if (osg::Group* falloutRoot = mFalloutPlayerVisualAnimation->getObjectRoot())
                    falloutRoot->setNodeMask(0);
            }
            Log(Debug::Info) << "ESM4: hidden legacy player render root after camera attachment";
        }
    }

    void RenderingManager::rebuildPtr(const MWWorld::Ptr& ptr)
    {
        NpcAnimation* anim = nullptr;
        if (ptr == mPlayerAnimation->getPtr())
            anim = mPlayerAnimation.get();
        else
            anim = dynamic_cast<NpcAnimation*>(mObjects->getAnimation(ptr));
        if (anim)
        {
            anim->rebuild();
            if (mCamera->getTrackingPtr() == ptr)
            {
                mCamera->attachTo(ptr);
                mCamera->setAnimation(anim);
            }
        }
    }

    void RenderingManager::addWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWater->addEmitter(ptr);
    }

    void RenderingManager::removeWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWater->removeEmitter(ptr);
    }

    void RenderingManager::emitWaterRipple(const osg::Vec3f& pos)
    {
        mWater->emitRipple(pos);
    }

    void RenderingManager::updateProjectionMatrix()
    {
        if (mNearClip < 0.0f)
            throw std::runtime_error("Near clip is less than zero");
        if (mViewDistance < mNearClip)
            throw std::runtime_error("Viewing distance is less than near clip");

        const double width = Settings::video().mResolutionX;
        const double height = Settings::video().mResolutionY;

        double aspect = (height == 0.0) ? 1.0 : width / height;
        float fov = mFieldOfView;
        if (mFieldOfViewOverridden)
            fov = mFieldOfViewOverride;

        if (mProjectionMatrixOverridden)
        {
            mViewer->getCamera()->setProjectionMatrix(mProjectionMatrixOverride);
            mPerViewUniformStateUpdater->setProjectionMatrix(mProjectionMatrixOverride);
        }
        else
        {
            mViewer->getCamera()->setProjectionMatrixAsPerspective(fov, aspect, mNearClip, mViewDistance);

            if (SceneUtil::AutoDepth::isReversed())
            {
                mPerViewUniformStateUpdater->setProjectionMatrix(
                    SceneUtil::getReversedZProjectionMatrixAsPerspective(fov, aspect, mNearClip, mViewDistance));
            }
            else
                mPerViewUniformStateUpdater->setProjectionMatrix(mViewer->getCamera()->getProjectionMatrix());
        }

        mSharedUniformStateUpdater->setNear(mNearClip);
        mSharedUniformStateUpdater->setFar(mViewDistance);
        if (Stereo::getStereo())
        {
            auto res = Stereo::Manager::instance().eyeResolution();
            mSharedUniformStateUpdater->setScreenRes(res.x(), res.y());
            Stereo::Manager::instance().setMasterProjectionMatrix(mPerViewUniformStateUpdater->getProjectionMatrix());
        }
        else
        {
            mSharedUniformStateUpdater->setScreenRes(width, height);
        }

        // Since our fog is not radial yet, we should take FOV in account, otherwise terrain near viewing distance may
        // disappear. Limit FOV here just for sure, otherwise viewing distance can be too high.
        float distanceMult = std::cos(osg::DegreesToRadians(std::min(fov, 140.f)) / 2.f);
        mTerrain->setViewDistance(mViewDistance * (distanceMult ? 1.f / distanceMult : 1.f));

        if (mPostProcessor)
        {
            mPostProcessor->getStateUpdater()->setProjectionMatrix(mPerViewUniformStateUpdater->getProjectionMatrix());
            mPostProcessor->getStateUpdater()->setFov(fov);
        }
    }

    void RenderingManager::setScreenRes(int width, int height)
    {
        mSharedUniformStateUpdater->setScreenRes(width, height);
    }

//## VR_PATCH BEGIN
    void RenderingManager::enableVRPointer(bool left, bool right)
    {
        if (mPlayerAnimation)
            static_cast<MWVR::VRAnimation*>(mPlayerAnimation.get())->enablePointers(left, right);
    }

//## VR_PATCH END
    void RenderingManager::updateTextureFiltering()
    {
        mViewer->stopThreading();

        mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
            Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap, Settings::general().mAnisotropy);

        mTerrain->updateTextureFiltering();
        mWater->processChangedSettings({});

        mViewer->startThreading();
    }

    void RenderingManager::updateAmbient()
    {
        osg::Vec4f color = mAmbientColor;

        if (mNightEyeFactor > 0.f)
            color += osg::Vec4f(0.7, 0.7, 0.7, 0.0) * mNightEyeFactor;

        mPostProcessor->getStateUpdater()->setAmbientColor(color);
        mStateUpdater->setAmbientColor(color);
    }

    void RenderingManager::setFogColor(const osg::Vec4f& color)
    {
        mViewer->getCamera()->setClearColor(color);
//## VR_PATCH BEGIN
        for (unsigned int i = 0; i < mViewer->getNumSlaves(); i++)
        {
            const auto& slave = mViewer->getSlave(i);
            if (slave._camera)
                slave._camera->setClearColor(color);
        }
//## VR_PATCH END

        mStateUpdater->setFogColor(color);
    }

    RenderingManager::WorldspaceChunkMgr& RenderingManager::getWorldspaceChunkMgr(ESM::RefId worldspace)
    {
        auto existingChunkMgr = mWorldspaceChunks.find(worldspace);
        if (existingChunkMgr != mWorldspaceChunks.end())
            return existingChunkMgr->second;
        RenderingManager::WorldspaceChunkMgr newChunkMgr;

        const float lodFactor = Settings::terrain().mLodFactor;
        const bool groundcover = Settings::groundcover().mEnabled && worldspace == ESM::Cell::sDefaultWorldspaceId;
        const bool distantTerrain = Settings::terrain().mDistantTerrain;
        const double expiryDelay = Settings::cells().mCacheExpiryDelay;
        if (distantTerrain || groundcover)
        {
            const int compMapResolution = Settings::terrain().mCompositeMapResolution;
            const int compMapPower = Settings::terrain().mCompositeMapLevel;
            const float compMapLevel = std::pow(2, compMapPower);
            const int vertexLodMod = Settings::terrain().mVertexLodMod;
            const float maxCompGeometrySize = Settings::terrain().mMaxCompositeGeometrySize;
            const bool debugChunks = Settings::terrain().mDebugChunks;
            auto quadTreeWorld = std::make_unique<Terrain::QuadTreeWorld>(mSceneRoot, mRootNode, mResourceSystem,
                mTerrainStorage.get(), Mask_Terrain, Mask_PreCompile, Mask_Debug, compMapResolution, compMapLevel,
                lodFactor, vertexLodMod, maxCompGeometrySize, debugChunks, worldspace, expiryDelay);
            if (Settings::terrain().mObjectPaging)
            {
                newChunkMgr.mObjectPaging
                    = std::make_unique<ObjectPaging>(mResourceSystem->getSceneManager(), worldspace);
                quadTreeWorld->addChunkManager(newChunkMgr.mObjectPaging.get());
                mResourceSystem->addResourceManager(newChunkMgr.mObjectPaging.get());
            }
            if (groundcover)
            {
                const float groundcoverDistance = Settings::groundcover().mRenderingDistance;
                const float density = Settings::groundcover().mDensity;

                newChunkMgr.mGroundcover = std::make_unique<Groundcover>(
                    mResourceSystem->getSceneManager(), density, groundcoverDistance, mGroundCoverStore);
                quadTreeWorld->addChunkManager(newChunkMgr.mGroundcover.get());
                mResourceSystem->addResourceManager(newChunkMgr.mGroundcover.get());
            }
            newChunkMgr.mTerrain = std::move(quadTreeWorld);
        }
        else
            newChunkMgr.mTerrain = std::make_unique<Terrain::TerrainGrid>(mSceneRoot, mRootNode, mResourceSystem,
                mTerrainStorage.get(), Mask_Terrain, worldspace, expiryDelay, Mask_PreCompile, Mask_Debug);

        newChunkMgr.mTerrain->setTargetFrameRate(Settings::cells().mTargetFramerate);
        float distanceMult = std::cos(osg::DegreesToRadians(std::min(mFieldOfView, 140.f)) / 2.f);
        newChunkMgr.mTerrain->setViewDistance(mViewDistance * (distanceMult ? 1.f / distanceMult : 1.f));
        newChunkMgr.mTerrain->enableHeightCullCallback(Settings::terrain().mWaterCulling);

        return mWorldspaceChunks.emplace(worldspace, std::move(newChunkMgr)).first->second;
    }

    void RenderingManager::reportStats() const
    {
        osg::Stats* stats = mViewer->getViewerStats();
        unsigned int frameNumber = mViewer->getFrameStamp()->getFrameNumber();
        if (stats->collectStats("resource"))
        {
            mTerrain->reportStats(frameNumber, stats);
        }
    }

    void RenderingManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        // Only perform a projection matrix update once if a relevant setting is changed.
        bool updateProjection = false;

        for (Settings::CategorySettingVector::const_iterator it = changed.begin(); it != changed.end(); ++it)
        {
            if (it->first == "Camera" && it->second == "field of view")
            {
                mFieldOfView = Settings::camera().mFieldOfView;
                updateProjection = true;
            }
            else if (it->first == "Video" && (it->second == "resolution x" || it->second == "resolution y"))
            {
                updateProjection = true;
            }
            else if (it->first == "Camera" && it->second == "viewing distance")
            {
                setViewDistance(Settings::camera().mViewingDistance);
            }
            else if (it->first == "General"
                && (it->second == "texture filter" || it->second == "texture mipmap" || it->second == "anisotropy"))
            {
                updateTextureFiltering();
            }
            else if (it->first == "Water")
            {
                mWater->processChangedSettings(changed);
            }
            else if (it->first == "Shaders" && it->second == "minimum interior brightness")
            {
                if (MWMechanics::getPlayer().isInCell())
                    configureAmbient(*MWMechanics::getPlayer().getCell()->getCell());
            }
            else if (it->first == "Shaders"
                && (it->second == "force per pixel lighting" || it->second == "classic falloff"))
            {
                mViewer->stopThreading();

                auto defines = mResourceSystem->getSceneManager()->getShaderManager().getGlobalDefines();
                defines["forcePPL"] = Settings::shaders().mForcePerPixelLighting ? "1" : "0";
                defines["classicFalloff"] = Settings::shaders().mClassicFalloff ? "1" : "0";
                mResourceSystem->getSceneManager()->getShaderManager().setGlobalDefines(defines);

                if (MWMechanics::getPlayer().isInCell() && it->second == "classic falloff")
                    configureAmbient(*MWMechanics::getPlayer().getCell()->getCell());

                mViewer->startThreading();
            }
            else if (it->first == "Shaders"
                && (it->second == "light bounds multiplier" || it->second == "maximum light distance"
                    || it->second == "light fade start" || it->second == "max lights"))
            {
                auto* lightManager = getLightRoot();

                lightManager->processChangedSettings(Settings::shaders().mLightBoundsMultiplier,
                    Settings::shaders().mMaximumLightDistance, Settings::shaders().mLightFadeStart);

                if (it->second == "max lights" && !lightManager->usingFFP())
                {
                    mViewer->stopThreading();

                    lightManager->updateMaxLights(Settings::shaders().mMaxLights);

                    auto defines = mResourceSystem->getSceneManager()->getShaderManager().getGlobalDefines();
                    for (const auto& [name, key] : lightManager->getLightDefines())
                        defines[name] = key;
                    mResourceSystem->getSceneManager()->getShaderManager().setGlobalDefines(defines);

                    mStateUpdater->reset();

                    mViewer->startThreading();
                }
            }
            else if (it->first == "Post Processing" && it->second == "enabled")
            {
                if (Settings::postProcessing().mEnabled)
                    mPostProcessor->enable();
                else
                {
                    mPostProcessor->disable();
                    if (auto* hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())
                        hud->setVisible(false);
                }
            }
//## VR_PATCH BEGIN
            else if (it->first == "VR")
            {
                if (it->second == "show 3D crosshairs")
                {
                    if(VR::getVR())
                        static_cast<MWVR::VRAnimation*>(mPlayerAnimation.get())
                            ->setEnableCrosshairs(Settings::Manager::getBool("show 3D crosshairs", "VR"));
                }
            }
//## VR_PATCH END
        }

        if (updateProjection)
        {
            updateProjectionMatrix();
        }
    }

    void RenderingManager::setViewDistance(float distance, bool delay)
    {
        mViewDistance = distance;

        if (delay)
        {
            mUpdateProjectionMatrix = true;
            return;
        }

        updateProjectionMatrix();
    }

    float RenderingManager::getTerrainHeightAt(const osg::Vec3f& pos, ESM::RefId worldspace)
    {
        return getWorldspaceChunkMgr(worldspace).mTerrain->getHeightAt(pos);
    }

    void RenderingManager::overrideFieldOfView(float val)
    {
        if (mFieldOfViewOverridden != true || mFieldOfViewOverride != val)
        {
            mFieldOfViewOverridden = true;
            mFieldOfViewOverride = val;
            updateProjectionMatrix();
        }
    }

    void RenderingManager::overrideProjectionMatrix(
        const osg::Matrixf& matrix, float fieldOfView, float nearClip, float farClip)
    {
        if (!(fieldOfView > 0.f) || !(nearClip > 0.f) || !(farClip > nearClip))
            throw std::runtime_error("Invalid explicit projection override contract");

        const bool changed = !mProjectionMatrixOverridden || mProjectionMatrixOverride != matrix
            || !mFieldOfViewOverridden || mFieldOfViewOverride != fieldOfView || mNearClip != nearClip
            || mViewDistance != farClip;
        mProjectionMatrixOverridden = true;
        mProjectionMatrixOverride = matrix;
        mFieldOfViewOverridden = true;
        mFieldOfViewOverride = fieldOfView;
        mNearClip = nearClip;
        mViewDistance = farClip;
        if (changed)
            updateProjectionMatrix();
    }

    void RenderingManager::resetProjectionMatrixOverride()
    {
        if (!mProjectionMatrixOverridden)
            return;
        mProjectionMatrixOverridden = false;
        updateProjectionMatrix();
    }

    void RenderingManager::setFieldOfView(float val)
    {
        mFieldOfView = val;
        mUpdateProjectionMatrix = true;
    }

    float RenderingManager::getFieldOfView() const
    {
        return mFieldOfViewOverridden ? mFieldOfViewOverride : mFieldOfView;
    }

    osg::Vec3f RenderingManager::getHalfExtents(const MWWorld::ConstPtr& object) const
    {
        osg::Vec3f halfExtents(0, 0, 0);
        VFS::Path::Normalized modelName(object.getClass().getCorrectedModel(object));
        if (modelName.empty())
            return halfExtents;

        osg::ref_ptr<const osg::Node> node = mResourceSystem->getSceneManager()->getTemplate(modelName);
        osg::ComputeBoundsVisitor computeBoundsVisitor;
        computeBoundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        const_cast<osg::Node*>(node.get())->accept(computeBoundsVisitor);
        osg::BoundingBox bounds = computeBoundsVisitor.getBoundingBox();

        if (bounds.valid())
        {
            halfExtents[0] = std::abs(bounds.xMax() - bounds.xMin()) / 2.f;
            halfExtents[1] = std::abs(bounds.yMax() - bounds.yMin()) / 2.f;
            halfExtents[2] = std::abs(bounds.zMax() - bounds.zMin()) / 2.f;
        }

        return halfExtents;
    }

    osg::BoundingBox RenderingManager::getCullSafeBoundingBox(const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return {};

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> rootNode = ptr.getRefData().getBaseNode();

        // Recalculate bounds on the ptr's template when the object is not loaded or is loaded but paged
        MWWorld::Scene* worldScene = MWBase::Environment::get().getWorldScene();
        if (!rootNode || worldScene->isPagedRef(ptr))
        {
            const VFS::Path::Normalized model(ptr.getClass().getCorrectedModel(ptr));

            if (model.empty())
                return {};

            rootNode = new SceneUtil::PositionAttitudeTransform;
            // Hack even used by osg internally, osg's NodeVisitor won't accept const qualified nodes
            rootNode->addChild(const_cast<osg::Node*>(mResourceSystem->getSceneManager()->getTemplate(model).get()));

            const float refScale = ptr.getCellRef().getScale();
            rootNode->setScale({ refScale, refScale, refScale });
            const auto& rotation = ptr.getCellRef().getPosition().rot;
            if (!ptr.getClass().isActor())
                rootNode->setAttitude(osg::Quat(rotation[0], osg::Vec3(-1, 0, 0))
                    * osg::Quat(rotation[1], osg::Vec3(0, -1, 0)) * osg::Quat(rotation[2], osg::Vec3(0, 0, -1)));
            rootNode->setPosition(ptr.getCellRef().getPosition().asVec3());

            osg::ref_ptr<Animation> animation = nullptr;

            if (ptr.getClass().isNpc())
            {
                rootNode->setNodeMask(Mask_Actor);
                animation = new NpcAnimation(ptr, osg::ref_ptr<osg::Group>(rootNode), mResourceSystem);
            }
        }

        SceneUtil::CullSafeBoundsVisitor computeBounds;
        computeBounds.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        rootNode->accept(computeBounds);

        return computeBounds.mBoundingBox;
    }

    void RenderingManager::resetFieldOfView()
    {
        if (mFieldOfViewOverridden == true)
        {
            mFieldOfViewOverridden = false;

            updateProjectionMatrix();
        }
    }
    void RenderingManager::exportSceneGraph(
        const MWWorld::Ptr& ptr, const std::filesystem::path& filename, const std::string& format)
    {
        osg::Node* node = mViewer->getSceneData();
        if (!ptr.isEmpty())
            node = ptr.getRefData().getBaseNode();

        SceneUtil::writeScene(node, filename, format);
    }

    LandManager* RenderingManager::getLandManager() const
    {
        return mTerrainStorage->getLandManager();
    }

    void RenderingManager::updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path,
        const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end) const
    {
        mActorsPaths->update(actor, path, agentBounds, start, end, mNavigator.getSettings());
    }

    void RenderingManager::removeActorPath(const MWWorld::ConstPtr& actor) const
    {
        mActorsPaths->remove(actor);
    }

    void RenderingManager::setNavMeshNumber(const std::size_t value)
    {
        mNavMeshNumber = value;
    }

    void RenderingManager::updateNavMesh()
    {
        if (!mNavMesh->isEnabled())
            return;

        const auto navMeshes = mNavigator.getNavMeshes();

        auto it = navMeshes.begin();
        for (std::size_t i = 0; it != navMeshes.end() && i < mNavMeshNumber; ++i)
            ++it;
        if (it == navMeshes.end())
        {
            mNavMesh->reset();
        }
        else
        {
            try
            {
                mNavMesh->update(it->second, mNavMeshNumber, mNavigator.getSettings());
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "NavMesh render update exception: " << e.what();
            }
        }
    }

    void RenderingManager::updateRecastMesh()
    {
        if (!mRecastMesh->isEnabled())
            return;

        mRecastMesh->update(mNavigator.getRecastMeshTiles(), mNavigator.getSettings());
    }

    void RenderingManager::setActiveGrid(const osg::Vec4i& grid)
    {
        mTerrain->setActiveGrid(grid);
    }
    bool RenderingManager::pagingEnableObject(int type, const MWWorld::ConstPtr& ptr, bool enabled)
    {
        if (!ptr.isInCell() || !ptr.getCell()->isExterior() || !mObjectPaging)
            return false;
        if (mObjectPaging->enableObject(type, ptr.getCellRef().getRefNum(), ptr.getCellRef().getPosition().asVec3(),
                osg::Vec2i(ptr.getCell()->getCell()->getGridX(), ptr.getCell()->getCell()->getGridY()), enabled))
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }
    void RenderingManager::pagingBlacklistObject(int type, const MWWorld::ConstPtr& ptr)
    {
        if (!ptr.isInCell() || !ptr.getCell()->isExterior() || !mObjectPaging)
            return;
        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (!refnum.hasContentFile())
            return;
        if (mObjectPaging->blacklistObject(type, refnum, ptr.getCellRef().getPosition().asVec3(),
                osg::Vec2i(ptr.getCell()->getCell()->getGridX(), ptr.getCell()->getCell()->getGridY())))
            mTerrain->rebuildViews();
    }
    bool RenderingManager::pagingUnlockCache()
    {
        if (mObjectPaging && mObjectPaging->unlockCache())
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }
    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)
    {
        if (mObjectPaging)
            mObjectPaging->getPagedRefnums(activeGrid, out);
    }

    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)
    {
        mNavMesh->setMode(value);
    }
}

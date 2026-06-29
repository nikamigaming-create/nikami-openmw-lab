#include "characterpreview.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Fog>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/Material>
#include <osg/PositionAttitudeTransform>
#include <osg/PolygonMode>
#include <osg/Quat>
#include <osg/Texture2D>
#include <osg/ValueObject>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/multiview.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/esm4npc.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "animation.hpp"
#include "creatureanimation.hpp"
#include "esm4npcanimation.hpp"
#include "npcanimation.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        const ESM4::Npc* findFalloutInventoryPlayerVisualRecord()
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return nullptr;

            const char* env = std::getenv("OPENMW_FNV_PLAYER_NPC");
            const std::string_view wanted = env != nullptr && *env != '\0' ? std::string_view(env) : "Player";
            const ESM4::Npc* fallback = nullptr;

            for (const ESM4::Npc& npc : store->get<ESM4::Npc>())
            {
                if (!npc.mIsFONV)
                    continue;
                if (Misc::StringUtils::ciEqual(npc.mEditorId, wanted))
                    return &npc;
                if (fallback == nullptr && Misc::StringUtils::ciEqual(npc.mEditorId, "Player"))
                    fallback = &npc;
            }

            return fallback;
        }

        const ESM4::Armor* findFalloutInventoryArmorByEditorId(std::string_view editorId)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || editorId.empty())
                return nullptr;

            for (const ESM4::Armor& armor : store->get<ESM4::Armor>())
                if (Misc::StringUtils::ciEqual(armor.mEditorId, editorId))
                    return &armor;

            return nullptr;
        }

        bool isFalloutContentLoaded()
        {
            const MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return false;

            for (const std::string& file : world->getContentFiles())
                if (Misc::StringUtils::ciEndsWith(file, "FalloutNV.esm"))
                    return true;

            return false;
        }

        bool shouldUseFalloutInventoryPlayerVisual()
        {
            if (std::getenv("OPENMW_FNV_DISABLE_INVENTORY_PREVIEW") != nullptr)
                return false;

            return std::getenv("OPENMW_FNV_INVENTORY_PLAYER_PROXY") != nullptr;
        }

        void applyFalloutInventoryPlayerProxyProofOutfit(const MWWorld::Ptr& visualPtr)
        {
            const char* outfitEnv = std::getenv("OPENMW_FNV_PLAYER_OUTFIT");
            const std::string_view outfitEditorId
                = outfitEnv != nullptr && *outfitEnv != '\0' ? std::string_view(outfitEnv) : "OutfitRepublican02";
            const ESM4::Armor* armor = findFalloutInventoryArmorByEditorId(outfitEditorId);
            if (armor == nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: inventory player proxy outfit " << outfitEditorId
                                    << " not found";
                return;
            }

            visualPtr.getRefData().setCustomData(std::unique_ptr<MWWorld::CustomData>());
            const bool added = MWClass::ESM4Npc::addEquippedArmor(visualPtr, armor);
            Log(Debug::Info) << "FNV/ESM4 proof: inventory player proxy outfit " << armor->mEditorId << " model="
                             << MWClass::ESM4Npc::chooseEquipmentModel(
                                    armor, MWClass::ESM4Npc::isFemale(visualPtr))
                             << " added=" << added;
        }

        std::string getFalloutPreviewAnimationGroup()
        {
            const char* value = std::getenv("OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP");
            if (value == nullptr || value[0] == '\0')
                return "idle";

            std::string group(value);
            while (!group.empty()
                && (group.front() == ' ' || group.front() == '\t' || group.front() == '\r'
                    || group.front() == '\n' || group.front() == '"' || group.front() == '\''))
                group.erase(group.begin());
            while (!group.empty()
                && (group.back() == ' ' || group.back() == '\t' || group.back() == '\r'
                    || group.back() == '\n' || group.back() == '"' || group.back() == '\''))
                group.pop_back();
            Misc::StringUtils::lowerCaseInPlace(group);
            return group.empty() ? std::string("idle") : group;
        }

        float getFalloutPreviewAnimationStartPoint()
        {
            const char* value = std::getenv("OPENMW_FNV_ACTOR_KIT_ANIMATION_STARTPOINT");
            if (value == nullptr || value[0] == '\0')
                return 0.35f;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return 0.35f;
            return std::clamp(parsed, 0.f, 0.999f);
        }

        bool shouldUseFalloutNeutralActorBindPose()
        {
            if (std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_BIND_POSE") != nullptr)
                return true;

            return std::getenv("OPENMW_FNV_ASSET_STUDIO") != nullptr
                && std::getenv("OPENMW_FNV_ASSET_STUDIO_ANIMATE") == nullptr;
        }

        std::string_view getFalloutNeutralActorPreviewProfile()
        {
            const char* value = std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_PROFILE");
            if (value == nullptr || value[0] == '\0')
                return "upper";
            return value;
        }

        float getFalloutNeutralActorPreviewFloat(const char* name, float fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0')
                return fallback;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return fallback;
            return parsed;
        }

        bool getFalloutNeutralActorPreviewBool(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
        }

        std::string getFalloutNeutralActorPreviewString(const char* name)
        {
            const char* value = std::getenv(name);
            if (value == nullptr)
                return {};

            std::string result(value);
            while (!result.empty() && std::isspace(static_cast<unsigned char>(result.front())))
                result.erase(result.begin());
            while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())))
                result.pop_back();
            return result;
        }

        osg::Vec4f mixFalloutDebugColor(const osg::Vec4f& low, const osg::Vec4f& high, float amount)
        {
            const float clamped = std::clamp(amount, 0.f, 1.f);
            return low * (1.f - clamped) + high * clamped;
        }

        osg::Vec4f getFalloutBoneWeightHeatColor(float weight)
        {
            if (weight <= 0.001f)
                return osg::Vec4f(0.035f, 0.035f, 0.04f, 1.f);

            const osg::Vec4f low(0.05f, 0.16f, 0.75f, 1.f);
            const osg::Vec4f high(1.f, 0.92f, 0.12f, 1.f);
            return mixFalloutDebugColor(low, high, std::sqrt(std::clamp(weight, 0.f, 1.f)));
        }

        osg::Vec4f getFalloutBonePaletteColor(std::size_t index, float weight)
        {
            if (weight <= 0.001f)
                return osg::Vec4f(0.035f, 0.035f, 0.04f, 1.f);

            static const std::array<osg::Vec4f, 12> palette = {
                osg::Vec4f(0.95f, 0.18f, 0.12f, 1.f), osg::Vec4f(0.12f, 0.62f, 1.f, 1.f),
                osg::Vec4f(0.10f, 0.88f, 0.35f, 1.f), osg::Vec4f(0.95f, 0.78f, 0.12f, 1.f),
                osg::Vec4f(0.78f, 0.30f, 1.f, 1.f), osg::Vec4f(0.05f, 0.88f, 0.88f, 1.f),
                osg::Vec4f(1.f, 0.38f, 0.70f, 1.f), osg::Vec4f(0.98f, 0.50f, 0.10f, 1.f),
                osg::Vec4f(0.48f, 0.72f, 0.12f, 1.f), osg::Vec4f(0.58f, 0.48f, 1.f, 1.f),
                osg::Vec4f(0.92f, 0.12f, 0.46f, 1.f), osg::Vec4f(0.72f, 0.90f, 1.f, 1.f),
            };

            const osg::Vec4f base = palette[index % palette.size()];
            const osg::Vec4f dark(0.03f, 0.03f, 0.035f, 1.f);
            return mixFalloutDebugColor(dark, base, 0.25f + 0.75f * std::sqrt(std::clamp(weight, 0.f, 1.f)));
        }

        osg::Vec4f getFalloutFingerBoneDebugColor(std::size_t slot, float weight)
        {
            static const std::array<osg::Vec4f, 5> fingerColors = {
                osg::Vec4f(1.f, 0.38f, 0.08f, 1.f), osg::Vec4f(0.10f, 0.68f, 1.f, 1.f),
                osg::Vec4f(0.15f, 0.95f, 0.32f, 1.f), osg::Vec4f(0.78f, 0.34f, 1.f, 1.f),
                osg::Vec4f(1.f, 0.30f, 0.68f, 1.f),
            };

            const std::size_t finger = std::min<std::size_t>(fingerColors.size() - 1, slot / 3);
            const float segmentBoost = 0.72f + 0.14f * static_cast<float>(slot % 3);
            const osg::Vec4f base(fingerColors[finger].r() * segmentBoost, fingerColors[finger].g() * segmentBoost,
                fingerColors[finger].b() * segmentBoost, 1.f);
            const osg::Vec4f dark(0.03f, 0.03f, 0.035f, 1.f);
            return mixFalloutDebugColor(dark, base, 0.25f + 0.75f * std::sqrt(std::clamp(weight, 0.f, 1.f)));
        }

        bool applyFalloutWeightDebugColorArray(SceneUtil::RigGeometry& rig, osg::Vec4Array* colors)
        {
            if (colors == nullptr || colors->empty())
                return false;

            bool applied = false;
            if (osg::Geometry* source = rig.getSourceGeometry().get())
            {
                source->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
                source->dirtyGLObjects();
                applied = true;
            }

            for (unsigned int i = 0; i < 2; ++i)
            {
                osg::Geometry* renderGeometry = rig.getRenderGeometry(i);
                if (renderGeometry == nullptr)
                    continue;

                osg::ref_ptr<osg::Vec4Array> renderColors
                    = static_cast<osg::Vec4Array*>(colors->clone(osg::CopyOp::DEEP_COPY_ALL));
                renderGeometry->setColorArray(renderColors.get(), osg::Array::BIND_PER_VERTEX);
                renderGeometry->dirtyGLObjects();
                applied = true;
            }

            return applied;
        }

        struct FalloutWeightDebugResult
        {
            bool mApplied = false;
            unsigned int mMatchedBones = 0;
            unsigned int mWeightedVertices = 0;
            float mMaxWeight = 0.f;
        };

        osg::Vec4f getFalloutFingerWeightDebugColor(float thumb, float index, float grip)
        {
            const float strongest = std::max({ thumb, index, grip });
            if (strongest <= 0.001f)
                return osg::Vec4f(0.08f, 0.08f, 0.08f, 1.f);

            const float alpha = 0.35f + 0.65f * std::min(1.f, strongest);
            if (thumb >= index && thumb >= grip)
                return osg::Vec4f(1.f, 0.25f, 0.05f, alpha);
            if (index >= thumb && index >= grip)
                return osg::Vec4f(0.1f, 0.75f, 1.f, alpha);
            return osg::Vec4f(0.1f, 1.f, 0.25f, alpha);
        }

        FalloutWeightDebugResult applyFalloutFingerWeightDebugColors(SceneUtil::RigGeometry& rig)
        {
            FalloutWeightDebugResult result;
            std::vector<float> thumbWeights;
            std::vector<float> indexWeights;
            std::vector<float> gripWeights;
            if (!rig.getFalloutFingerVertexWeights(thumbWeights, indexWeights, gripWeights))
                return result;

            const std::size_t vertexCount = thumbWeights.size();
            if (vertexCount == 0 || indexWeights.size() != vertexCount || gripWeights.size() != vertexCount)
                return result;

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->reserve(vertexCount);
            std::array<bool, 3> usedGroups{};
            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                const float strongest = std::max({ thumbWeights[i], indexWeights[i], gripWeights[i] });
                if (strongest > 0.001f)
                {
                    ++result.mWeightedVertices;
                    if (thumbWeights[i] >= indexWeights[i] && thumbWeights[i] >= gripWeights[i])
                        usedGroups[0] = true;
                    else if (indexWeights[i] >= thumbWeights[i] && indexWeights[i] >= gripWeights[i])
                        usedGroups[1] = true;
                    else
                        usedGroups[2] = true;
                }
                result.mMaxWeight = std::max(result.mMaxWeight, strongest);
                colors->push_back(getFalloutFingerWeightDebugColor(thumbWeights[i], indexWeights[i], gripWeights[i]));
            }

            for (bool used : usedGroups)
                if (used)
                    ++result.mMatchedBones;
            result.mApplied = applyFalloutWeightDebugColorArray(rig, colors.get());
            return result;
        }

        FalloutWeightDebugResult applyFalloutAllBoneWeightDebugColors(SceneUtil::RigGeometry& rig)
        {
            std::vector<SceneUtil::RigGeometry::BoneInfo> bones;
            std::vector<SceneUtil::RigGeometry::BoneWeights> vertexInfluences;
            std::vector<osg::Matrixf> localBoneMatrices;
            std::vector<osg::Matrixf> skeletonBoneMatrices;
            osg::Matrixf transform;
            osg::Matrixf skinToSkelMatrix;
            FalloutWeightDebugResult result;
            if (!rig.getSkinningDebugData(
                    bones, vertexInfluences, localBoneMatrices, skeletonBoneMatrices, transform, skinToSkelMatrix))
                return result;

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->reserve(vertexInfluences.size());
            for (const SceneUtil::RigGeometry::BoneWeights& influences : vertexInfluences)
            {
                std::size_t strongestBone = 0;
                float strongestWeight = 0.f;
                for (const auto& [boneIndex, weight] : influences)
                {
                    if (boneIndex < bones.size() && weight > strongestWeight)
                    {
                        strongestBone = boneIndex;
                        strongestWeight = weight;
                    }
                }

                if (strongestWeight > 0.001f)
                    ++result.mWeightedVertices;
                result.mMaxWeight = std::max(result.mMaxWeight, strongestWeight);
                colors->push_back(getFalloutBonePaletteColor(strongestBone, strongestWeight));
            }

            result.mMatchedBones = static_cast<unsigned int>(bones.size());
            result.mApplied = applyFalloutWeightDebugColorArray(rig, colors.get());
            return result;
        }

        FalloutWeightDebugResult applyFalloutFingerBoneWeightDebugColors(SceneUtil::RigGeometry& rig)
        {
            std::array<std::vector<float>, 15> fingerBones;
            FalloutWeightDebugResult result;
            if (!rig.getFalloutFingerBoneVertexWeights(fingerBones))
                return result;

            const std::size_t vertexCount = fingerBones.front().size();
            if (vertexCount == 0)
                return result;

            std::array<bool, 15> usedSlots{};
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->reserve(vertexCount);
            for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
            {
                std::size_t strongestSlot = 0;
                float strongestWeight = 0.f;
                for (std::size_t slot = 0; slot < fingerBones.size(); ++slot)
                {
                    if (fingerBones[slot].size() != vertexCount)
                        return {};

                    if (fingerBones[slot][vertex] > strongestWeight)
                    {
                        strongestSlot = slot;
                        strongestWeight = fingerBones[slot][vertex];
                    }
                }

                if (strongestWeight > 0.001f)
                {
                    ++result.mWeightedVertices;
                    usedSlots[strongestSlot] = true;
                }
                result.mMaxWeight = std::max(result.mMaxWeight, strongestWeight);
                colors->push_back(getFalloutFingerBoneDebugColor(strongestSlot, strongestWeight));
            }

            for (bool used : usedSlots)
                if (used)
                    ++result.mMatchedBones;
            result.mApplied = applyFalloutWeightDebugColorArray(rig, colors.get());
            return result;
        }

        bool parseFalloutWeightBoneIndex(std::string_view selector, std::size_t& index)
        {
            if (selector.empty())
                return false;

            if (selector.front() == '#')
                selector.remove_prefix(1);
            if (selector.empty())
                return false;

            std::size_t parsed = 0;
            for (char c : selector)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    return false;
                parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
            }

            index = parsed;
            return true;
        }

        FalloutWeightDebugResult applyFalloutSelectedBoneWeightDebugColors(
            SceneUtil::RigGeometry& rig, std::string_view selector)
        {
            std::vector<SceneUtil::RigGeometry::BoneInfo> bones;
            std::vector<SceneUtil::RigGeometry::BoneWeights> vertexInfluences;
            std::vector<osg::Matrixf> localBoneMatrices;
            std::vector<osg::Matrixf> skeletonBoneMatrices;
            osg::Matrixf transform;
            osg::Matrixf skinToSkelMatrix;
            FalloutWeightDebugResult result;
            if (!rig.getSkinningDebugData(
                    bones, vertexInfluences, localBoneMatrices, skeletonBoneMatrices, transform, skinToSkelMatrix))
                return result;

            std::vector<bool> matchedBones(bones.size(), false);
            std::size_t selectedIndex = 0;
            if (parseFalloutWeightBoneIndex(selector, selectedIndex))
            {
                if (selectedIndex < matchedBones.size())
                    matchedBones[selectedIndex] = true;
            }
            else
            {
                const std::string loweredSelector = Misc::StringUtils::lowerCase(std::string(selector));
                for (std::size_t i = 0; i < bones.size(); ++i)
                {
                    const std::string loweredBone = Misc::StringUtils::lowerCase(bones[i].mName);
                    if (loweredBone.find(loweredSelector) != std::string::npos)
                        matchedBones[i] = true;
                }
            }

            for (bool matched : matchedBones)
                if (matched)
                    ++result.mMatchedBones;
            if (result.mMatchedBones == 0)
                return result;

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->reserve(vertexInfluences.size());
            for (const SceneUtil::RigGeometry::BoneWeights& influences : vertexInfluences)
            {
                float selectedWeight = 0.f;
                for (const auto& [boneIndex, weight] : influences)
                    if (boneIndex < matchedBones.size() && matchedBones[boneIndex])
                        selectedWeight += weight;

                selectedWeight = std::clamp(selectedWeight, 0.f, 1.f);
                if (selectedWeight > 0.001f)
                    ++result.mWeightedVertices;
                result.mMaxWeight = std::max(result.mMaxWeight, selectedWeight);
                colors->push_back(getFalloutBoneWeightHeatColor(selectedWeight));
            }

            result.mApplied = applyFalloutWeightDebugColorArray(rig, colors.get());
            return result;
        }

        void applyFalloutPreviewWireframeState(osg::StateSet& stateSet, float lineWidth)
        {
            stateSet.setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet.setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet.setAttributeAndModes(
                new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet.setAttributeAndModes(
                new osg::LineWidth(lineWidth), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        osg::ref_ptr<osg::Geode> makeFalloutPreviewAxisGeode(std::string_view name, float size, bool secondaryColors)
        {
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->setName(std::string(name));
            geode->setCullingActive(false);

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);

            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            const osg::Vec4f xColor = secondaryColors ? osg::Vec4f(1.f, 0.f, 1.f, 1.f) : osg::Vec4f(1.f, 0.f, 0.f, 1.f);
            const osg::Vec4f yColor = secondaryColors ? osg::Vec4f(0.f, 1.f, 1.f, 1.f) : osg::Vec4f(0.f, 1.f, 0.f, 1.f);
            const osg::Vec4f zColor = secondaryColors ? osg::Vec4f(1.f, 1.f, 0.f, 1.f) : osg::Vec4f(0.1f, 0.35f, 1.f, 1.f);
            const auto addAxis = [&](const osg::Vec3f& axis, const osg::Vec4f& color) {
                vertices->push_back(osg::Vec3f());
                vertices->push_back(axis * size);
                colors->push_back(color);
                colors->push_back(color);
            };
            addAxis(osg::Vec3f(1.f, 0.f, 0.f), xColor);
            addAxis(osg::Vec3f(0.f, 1.f, 0.f), yColor);
            addAxis(osg::Vec3f(0.f, 0.f, 1.f), zColor);

            geometry->setVertexArray(vertices);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, vertices->size()));
            geode->addDrawable(geometry);

            osg::StateSet* stateSet = geode->getOrCreateStateSet();
            stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setAttributeAndModes(new osg::LineWidth(6.f), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            return geode;
        }

        class FalloutActorPreviewRigDebugVisitor : public osg::NodeVisitor
        {
        public:
            FalloutActorPreviewRigDebugVisitor(bool wireframe, bool weights, std::string weightSelector)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mWireframe(wireframe)
                , mWeights(weights)
                , mWeightSelector(std::move(weightSelector))
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    osg::Drawable* drawable = geode.getDrawable(i);
                    if (drawable == nullptr)
                        continue;

                    if (mWireframe)
                    {
                        applyFalloutPreviewWireframeState(*drawable->getOrCreateStateSet(),
                            getFalloutNeutralActorPreviewFloat("OPENMW_FNV_ACTOR_PREVIEW_DEBUG_LINE_WIDTH", 4.f));
                        ++mWireframeDrawables;
                    }

                    SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable);
                    if (rig == nullptr)
                        continue;

                    ++mRigs;
                    mBones += rig->getBoneCount();
                    if (mWeights)
                    {
                        FalloutWeightDebugResult result;
                        if (mWeightSelector.empty() || Misc::StringUtils::ciEqual(mWeightSelector, "aggregate"))
                            result = applyFalloutFingerWeightDebugColors(*rig);
                        else if (Misc::StringUtils::ciEqual(mWeightSelector, "all")
                            || Misc::StringUtils::ciEqual(mWeightSelector, "*")
                            || Misc::StringUtils::ciEqual(mWeightSelector, "strongest"))
                            result = applyFalloutAllBoneWeightDebugColors(*rig);
                        else if (Misc::StringUtils::ciEqual(mWeightSelector, "fingers")
                            || Misc::StringUtils::ciEqual(mWeightSelector, "finger-bones"))
                            result = applyFalloutFingerBoneWeightDebugColors(*rig);
                        else
                            result = applyFalloutSelectedBoneWeightDebugColors(*rig, mWeightSelector);

                        if (result.mApplied)
                        {
                            ++mWeightRigs;
                            mWeightMatchedBones += result.mMatchedBones;
                            mWeightVertices += result.mWeightedVertices;
                            mMaxWeight = std::max(mMaxWeight, result.mMaxWeight);
                        }
                        else
                            ++mWeightMisses;
                    }
                }

                traverse(geode);
            }

            unsigned int mRigs = 0;
            unsigned int mBones = 0;
            unsigned int mWeightRigs = 0;
            unsigned int mWeightMisses = 0;
            unsigned int mWeightMatchedBones = 0;
            unsigned int mWeightVertices = 0;
            unsigned int mWireframeDrawables = 0;
            float mMaxWeight = 0.f;

        private:
            bool mWireframe;
            bool mWeights;
            std::string mWeightSelector;
        };

        void applyFalloutNeutralActorOrbitCamera(FalloutActorPreview::ViewMode viewMode, float distance, float cameraZ,
            float lookAtZ, osg::Vec3f& position, osg::Vec3f& lookAt, const char*& viewName)
        {
            const float diagonal = distance * 0.70710678118f;
            position = osg::Vec3f(0.f, distance, cameraZ);
            lookAt = osg::Vec3f(0.f, 0.f, lookAtZ);
            viewName = "front";
            switch (viewMode)
            {
                case FalloutActorPreview::ViewMode::Front:
                    break;
                case FalloutActorPreview::ViewMode::FrontLeft:
                    position = osg::Vec3f(-diagonal, diagonal, cameraZ);
                    viewName = "front-left";
                    break;
                case FalloutActorPreview::ViewMode::FrontRight:
                    position = osg::Vec3f(diagonal, diagonal, cameraZ);
                    viewName = "front-right";
                    break;
                case FalloutActorPreview::ViewMode::Left:
                    position = osg::Vec3f(-distance, 0.f, cameraZ);
                    viewName = "left";
                    break;
                case FalloutActorPreview::ViewMode::Right:
                    position = osg::Vec3f(distance, 0.f, cameraZ);
                    viewName = "right";
                    break;
                case FalloutActorPreview::ViewMode::Top:
                    position = osg::Vec3f(0.f, 24.f, cameraZ + 210.f);
                    lookAt = osg::Vec3f(0.f, 0.f, lookAtZ - 8.f);
                    viewName = "top";
                    break;
                case FalloutActorPreview::ViewMode::Bottom:
                    position = osg::Vec3f(0.f, 24.f, lookAtZ - distance);
                    lookAt = osg::Vec3f(0.f, 0.f, lookAtZ);
                    viewName = "bottom";
                    break;
                case FalloutActorPreview::ViewMode::Back:
                    position = osg::Vec3f(0.f, -distance, cameraZ);
                    viewName = "back";
                    break;
                case FalloutActorPreview::ViewMode::IsoNW:
                    position = osg::Vec3f(-diagonal, diagonal, cameraZ + distance * 0.35f);
                    viewName = "iso-nw";
                    break;
                case FalloutActorPreview::ViewMode::IsoSW:
                    position = osg::Vec3f(-diagonal, -diagonal, cameraZ + distance * 0.35f);
                    viewName = "iso-sw";
                    break;
            }
        }

        void applyFalloutNeutralActorPreviewYawOffset(osg::Vec3f& position, const osg::Vec3f& lookAt, float degrees)
        {
            if (!std::isfinite(degrees) || std::abs(degrees) < 0.001f)
                return;

            const float radians = degrees * 0.017453292519943295f;
            const float sinYaw = std::sin(radians);
            const float cosYaw = std::cos(radians);
            const osg::Vec3f delta = position - lookAt;
            position = lookAt
                + osg::Vec3f(delta.x() * cosYaw - delta.y() * sinYaw,
                    delta.x() * sinYaw + delta.y() * cosYaw, delta.z());
        }

        osg::Vec3f getWorldPosition(const osg::Node* node)
        {
            if (node == nullptr)
                return osg::Vec3f();

            osg::NodePathList paths = node->getParentalNodePaths();
            if (paths.empty())
                return osg::Vec3f();

            return osg::computeLocalToWorld(paths.front()).getTrans();
        }

        std::string formatVec3(const osg::Vec3f& value)
        {
            std::ostringstream stream;
            stream << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
            return stream.str();
        }
    }

    class DrawOnceCallback : public SceneUtil::NodeCallback<DrawOnceCallback>
    {
    public:
        DrawOnceCallback(osg::Node* subgraph)
            : mRendered(false)
            , mLastRenderedFrame(0)
            , mSubgraph(subgraph)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mRendered)
            {
                mRendered = true;

                mLastRenderedFrame = nv->getTraversalNumber();

                osg::ref_ptr<osg::FrameStamp> previousFramestamp = const_cast<osg::FrameStamp*>(nv->getFrameStamp());
                osg::FrameStamp* fs = new osg::FrameStamp(*previousFramestamp);
                fs->setSimulationTime(mSimulationTime);

                nv->setFrameStamp(fs);

                // Update keyframe controllers in the scene graph first...
                // RTTNode does not continue update traversal, so manually continue the update traversal since we need
                // it.
                mSubgraph->accept(*nv);
                traverse(node, nv);

                nv->setFrameStamp(previousFramestamp);
            }
            else
            {
                node->setNodeMask(0);
            }
        }

        void redrawNextFrame() { mRendered = false; }
        void setSimulationTime(double simulationTime) { mSimulationTime = simulationTime; }

        unsigned int getLastRenderedFrame() const { return mLastRenderedFrame; }

    private:
        bool mRendered;
        unsigned int mLastRenderedFrame;
        double mSimulationTime = 0.0;
        osg::ref_ptr<osg::Node> mSubgraph;
    };

    // Set up alpha blending mode to avoid issues caused by transparent objects writing onto the alpha value of the FBO
    // This makes the RTT have premultiplied alpha, though, so the source blend factor must be GL_ONE when it's applied
    class SetUpBlendVisitor : public osg::NodeVisitor
    {
    public:
        SetUpBlendVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (osg::ref_ptr<osg::StateSet> stateset = node.getStateSet())
            {
                osg::ref_ptr<osg::StateSet> newStateSet;
                if (stateset->getAttribute(osg::StateAttribute::BLENDFUNC)
                    || stateset->getBinNumber() == osg::StateSet::TRANSPARENT_BIN)
                {
                    osg::BlendFunc* blendFunc
                        = static_cast<osg::BlendFunc*>(stateset->getAttribute(osg::StateAttribute::BLENDFUNC));

                    if (blendFunc)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                        osg::ref_ptr<osg::BlendFunc> newBlendFunc = new osg::BlendFunc(*blendFunc);
                        newStateSet->setAttribute(newBlendFunc, osg::StateAttribute::ON);
                        // I *think* (based on some by-hand maths) that the RGB and dest alpha factors are unchanged,
                        // and only dest determines source alpha factor This has the benefit of being idempotent if we
                        // assume nothing used glBlendFuncSeparate before we touched it
                        if (blendFunc->getDestination() == osg::BlendFunc::ONE_MINUS_SRC_ALPHA)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ONE);
                        else if (blendFunc->getDestination() == osg::BlendFunc::ONE)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ZERO);
                        // Other setups barely exist in the wild and aren't worth supporting as they're not equippable
                        // gear
                        else
                            Log(Debug::Info) << "Unable to adjust blend mode for character preview. Source factor 0x"
                                             << std::hex << blendFunc->getSource() << ", destination factor 0x"
                                             << blendFunc->getDestination() << std::dec;
                    }
                }
                if (stateset->getMode(GL_BLEND) & osg::StateAttribute::ON)
                {
                    if (!newStateSet)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                    }
                    // Disable noBlendAlphaEnv
                    newStateSet->setTextureMode(7, GL_TEXTURE_2D, osg::StateAttribute::OFF);
                    newStateSet->setDefine("FORCE_OPAQUE", "0", osg::StateAttribute::ON);
                }
            }
            traverse(node);
        }
    };

    class FalloutActorPreviewPartMaskVisitor : public osg::NodeVisitor
    {
    public:
        enum class Mode
        {
            FaceHeadgear,
            RightHandWeapon,
        };

        FalloutActorPreviewPartMaskVisitor(Mode mode)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mMode(mode)
        {
        }

        void apply(osg::Node& node) override
        {
            const std::string name = Misc::StringUtils::lowerCase(node.getName());
            if (name.rfind("fnv part ", 0) == 0)
            {
                const bool keep = mMode == Mode::FaceHeadgear ? keepFaceHeadgearPart(name) : keepRightHandWeaponPart(name);
                if (!keep)
                {
                    node.setNodeMask(0);
                    ++mMasked;
                    return;
                }
                ++mKept;
            }

            traverse(node);
        }

        unsigned int getKept() const { return mKept; }
        unsigned int getMasked() const { return mMasked; }

    private:
        static bool hasAny(std::string_view value, std::initializer_list<std::string_view> needles)
        {
            for (std::string_view needle : needles)
            {
                if (value.find(needle) != std::string_view::npos)
                    return true;
            }
            return false;
        }

        static bool keepFaceHeadgearPart(std::string_view name)
        {
            return hasAny(name,
                { "characters/head/", "characters\\head\\", "characters/hair/", "characters\\hair\\", "headhuman",
                    "headold", "mouth", "teeth", "tongue", "eye", "eyebrow", "beard", "hair", "headgear", "hat",
                    "cowboy", "bandana" });
        }

        static bool keepRightHandWeaponPart(std::string_view name)
        {
            return hasAny(name,
                { "righthand", "right hand", "forearm", "foretwist", "finger", "thumb", "arms", "weapon",
                    "weap", "1hand", "pistol", "revolver", "rifle", "gun" });
        }

        Mode mMode;
        unsigned int mKept = 0;
        unsigned int mMasked = 0;
    };

    class FalloutActorPreviewHideBloodVisitor : public osg::NodeVisitor
    {
    public:
        FalloutActorPreviewHideBloodVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (shouldHide(node.getName()))
            {
                node.setNodeMask(0);
                ++mNodes;
                return;
            }

            traverse(node);
        }

        void apply(osg::Geode& geode) override
        {
            if (shouldHide(geode.getName()))
            {
                geode.setNodeMask(0);
                ++mNodes;
                return;
            }

            for (unsigned int index = 0; index < geode.getNumDrawables(); ++index)
            {
                osg::Drawable* drawable = geode.getDrawable(index);
                if (drawable == nullptr)
                    continue;

                if (shouldHide(*drawable))
                {
                    drawable->setNodeMask(0);
                    ++mDrawables;
                }
            }

            traverse(geode);
        }

        unsigned int getHiddenNodes() const { return mNodes; }
        unsigned int getHiddenDrawables() const { return mDrawables; }

    private:
        static bool containsAny(std::string_view value, std::initializer_list<std::string_view> needles)
        {
            for (std::string_view needle : needles)
            {
                if (value.find(needle) != std::string_view::npos)
                    return true;
            }
            return false;
        }

        static bool shouldHide(std::string_view name)
        {
            const std::string lowered = Misc::StringUtils::lowerCase(std::string(name));
            return containsAny(lowered,
                { "meatcap", "gorecap", "bodymeat", "headmeat", "blood", "gore", "dismember", "dismemberment" });
        }

        static bool shouldHide(osg::Drawable& drawable)
        {
            if (shouldHide(drawable.getName()))
                return true;

            if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
            {
                if (osg::Geometry* source = rig->getSourceGeometry(); source != nullptr && shouldHide(source->getName()))
                    return true;
                for (unsigned int index = 0; index < 2; ++index)
                    if (osg::Geometry* render = rig->getRenderGeometry(index);
                        render != nullptr && shouldHide(render->getName()))
                        return true;
            }

            return false;
        }

        unsigned int mNodes = 0;
        unsigned int mDrawables = 0;
    };

    class FalloutActorPreviewDrawableAuditVisitor : public osg::NodeVisitor
    {
    public:
        FalloutActorPreviewDrawableAuditVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (node.getNodeMask() == 0)
                return;
            traverse(node);
        }

        void apply(osg::Geode& geode) override
        {
            if (geode.getNodeMask() == 0)
                return;

            ++mGeodes;
            for (unsigned int index = 0; index < geode.getNumDrawables(); ++index)
            {
                osg::Drawable* drawable = geode.getDrawable(index);
                if (drawable == nullptr)
                    continue;

                ++mDrawables;
                const osg::BoundingBox bound = drawable->getBoundingBox();
                if (bound.valid())
                    mBounds.expandBy(bound);

                osg::Geometry* geometry = drawable->asGeometry();
                if (geometry == nullptr)
                    continue;

                ++mGeometry;
                if (const osg::Array* vertices = geometry->getVertexArray())
                    mVertices += vertices->getNumElements();
            }

            traverse(geode);
        }

        unsigned int getGeodes() const { return mGeodes; }
        unsigned int getDrawables() const { return mDrawables; }
        unsigned int getGeometry() const { return mGeometry; }
        unsigned int getVertices() const { return mVertices; }
        const osg::BoundingBox& getBounds() const { return mBounds; }

    private:
        unsigned int mGeodes = 0;
        unsigned int mDrawables = 0;
        unsigned int mGeometry = 0;
        unsigned int mVertices = 0;
        osg::BoundingBox mBounds;
    };

    class CharacterPreviewRTTNode : public SceneUtil::RTTNode
    {
        static constexpr float fovYDegrees = 12.3f;
        static constexpr float znear = 4.0f;
        static constexpr float zfar = 10000.f;

    public:
        CharacterPreviewRTTNode(uint32_t sizeX, uint32_t sizeY)
            : RTTNode(sizeX, sizeY, Settings::video().mAntialiasing, false, 0,
                StereoAwareness::Unaware_MultiViewShaders, shouldAddMSAAIntermediateTarget())
            , mAspectRatio(static_cast<float>(sizeX) / static_cast<float>(sizeY))
        {
            if (SceneUtil::AutoDepth::isReversed())
                mPerspectiveMatrix = static_cast<osg::Matrixf>(
                    SceneUtil::getReversedZProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar));
            else
                mPerspectiveMatrix = osg::Matrixf::perspective(fovYDegrees, mAspectRatio, znear, zfar);
            mGroup->getOrCreateStateSet()->addUniform(new osg::Uniform("projectionMatrix", mPerspectiveMatrix));
            mViewMatrix = osg::Matrixf::identity();
            setColorBufferInternalFormat(GL_RGBA);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("CharacterPreview");
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
            camera->setClearColor(osg::Vec4(0.f, 0.f, 0.f, 0.f));
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            camera->setProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar);
            camera->setViewport(0, 0, width(), height());
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setCullMask(~(Mask_UpdateVisitor));
            camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            SceneUtil::setCameraClearDepth(camera);

            camera->setNodeMask(Mask_RenderToTexture);
            camera->addChild(mGroup);
        }

        void apply(osg::Camera* camera) override
        {
            if (mCameraStateset)
                camera->setStateSet(mCameraStateset);
            camera->setViewMatrix(mViewMatrix);

            if (shouldDoTextureArray())
                Stereo::setMultiviewMatrices(mGroup->getOrCreateStateSet(), { mPerspectiveMatrix, mPerspectiveMatrix });
        }

        void addChild(osg::Node* node) { mGroup->addChild(node); }

        void setCameraStateset(osg::StateSet* stateset) { mCameraStateset = stateset; }

        void setViewMatrix(const osg::Matrixf& viewMatrix) { mViewMatrix = viewMatrix; }

        osg::ref_ptr<osg::Group> mGroup = new osg::Group;
        osg::Matrixf mPerspectiveMatrix;
        osg::Matrixf mViewMatrix;
        osg::ref_ptr<osg::StateSet> mCameraStateset;
        float mAspectRatio;
    };

    CharacterPreview::CharacterPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        const MWWorld::Ptr& character, int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt)
        : mParent(parent)
        , mResourceSystem(resourceSystem)
        , mPosition(position)
        , mLookAt(lookAt)
        , mCharacter(character)
        , mAnimation(nullptr)
        , mSizeX(sizeX)
        , mSizeY(sizeY)
    {
        mTextureStateSet = new osg::StateSet;
        mTextureStateSet->setAttribute(new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));

        mRTTNode = new CharacterPreviewRTTNode(sizeX, sizeY);
        mRTTNode->setNodeMask(Mask_RenderToTexture);

        osg::ref_ptr<SceneUtil::LightManager> lightManager = new SceneUtil::LightManager(
            SceneUtil::LightSettings{
                .mClusteredLighting = Settings::shaders().mClusteredLighting,
                .mMaxLights = Settings::shaders().mMaxLights,
                .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
                .mLightFadeStart = Settings::shaders().mLightFadeStart,
                .mLightRadiusMultiplier = Settings::shaders().mLightRadiusMultiplier,
            },
            resourceSystem);
        osg::ref_ptr<osg::StateSet> stateset = lightManager->getOrCreateStateSet();
        stateset->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON);
        stateset->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Material> defaultMat(new osg::Material);
        defaultMat->setColorMode(isFalloutContentLoaded() ? osg::Material::AMBIENT_AND_DIFFUSE : osg::Material::OFF);
        defaultMat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        stateset->setAttribute(defaultMat);

        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

        // assign large value to effectively turn off fog
        // shaders don't respect glDisable(GL_FOG)
        osg::ref_ptr<osg::Fog> fog(new osg::Fog);
        fog->setStart(10000000);
        fog->setEnd(10000000);
        stateset->setAttributeAndModes(fog, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        // TODO: Clean up this mess of loose uniforms that shaders depend on.
        // turn off sky blending
        stateset->addUniform(new osg::Uniform("far", 10000000.0f));
        stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.0f));
        stateset->addUniform(new osg::Uniform("screenRes", osg::Vec2f{ 1, 1 }));

        stateset->addUniform(new osg::Uniform("emissiveMult", 1.f));

        osg::ref_ptr<osg::Texture2D> dummyTexture = new osg::Texture2D();
        dummyTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setInternalFormat(GL_DEPTH_COMPONENT);
        dummyTexture->setTextureSize(1, 1);
        // This might clash with a shadow map, so make sure it doesn't cast shadows
        dummyTexture->setShadowComparison(true);
        dummyTexture->setShadowCompareFunc(osg::Texture::ShadowCompareFunc::ALWAYS);
        stateset->setTextureAttributeAndModes(7, dummyTexture, osg::StateAttribute::ON);

        osg::ref_ptr<osg::Light> light = new osg::Light;
        float diffuseR = Fallback::Map::getFloat("Inventory_DirectionalDiffuseR");
        float diffuseG = Fallback::Map::getFloat("Inventory_DirectionalDiffuseG");
        float diffuseB = Fallback::Map::getFloat("Inventory_DirectionalDiffuseB");
        float ambientR = Fallback::Map::getFloat("Inventory_DirectionalAmbientR");
        float ambientG = Fallback::Map::getFloat("Inventory_DirectionalAmbientG");
        float ambientB = Fallback::Map::getFloat("Inventory_DirectionalAmbientB");
        float azimuth = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationX"));
        float altitude = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationY"));
        float positionX = -std::cos(azimuth) * std::sin(altitude);
        float positionY = std::sin(azimuth) * std::sin(altitude);
        float positionZ = std::cos(altitude);
        light->setPosition(osg::Vec4(positionX, positionY, positionZ, 0.0));
        if (isFalloutContentLoaded())
        {
            diffuseR = std::max(diffuseR, 0.95f);
            diffuseG = std::max(diffuseG, 0.95f);
            diffuseB = std::max(diffuseB, 0.95f);
            ambientR = std::max(ambientR, 0.45f);
            ambientG = std::max(ambientG, 0.45f);
            ambientB = std::max(ambientB, 0.45f);
        }
        light->setDiffuse(osg::Vec4(diffuseR, diffuseG, diffuseB, 1));
        light->setAmbient(osg::Vec4(ambientR, ambientG, ambientB, 1));
        light->setSpecular(osg::Vec4(0, 0, 0, 0));
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);
        lightManager->setSunlight(light);

        mRTTNode->addChild(lightManager);

        mNode = new osg::PositionAttitudeTransform;
        lightManager->addChild(mNode);

        mDrawOnceCallback = new DrawOnceCallback(mRTTNode->mGroup);
        mRTTNode->addUpdateCallback(mDrawOnceCallback);

        mParent->addChild(mRTTNode);

        mCharacter.mCell = nullptr;
    }

    CharacterPreview::~CharacterPreview()
    {
        mParent->removeChild(mRTTNode);
    }

    int CharacterPreview::getTextureWidth() const
    {
        return mSizeX;
    }

    int CharacterPreview::getTextureHeight() const
    {
        return mSizeY;
    }

    void CharacterPreview::setBlendMode()
    {
        SetUpBlendVisitor visitor;
        mNode->accept(visitor);
    }

    void CharacterPreview::setRedrawSimulationTime(double simulationTime)
    {
        mDrawOnceCallback->setSimulationTime(simulationTime);
    }

    void CharacterPreview::onSetup()
    {
        setBlendMode();
    }

    osg::ref_ptr<Animation> CharacterPreview::createAnimation()
    {
        return new NpcAnimation(mCharacter, mNode, mResourceSystem, true,
            (renderHeadOnly() ? NpcAnimation::VM_HeadOnly : NpcAnimation::VM_Normal));
    }

    osg::ref_ptr<osg::Texture2D> CharacterPreview::getTexture()
    {
        return static_cast<osg::Texture2D*>(mRTTNode->getColorTexture(nullptr));
    }

    void CharacterPreview::rebuild()
    {
        mAnimation = nullptr;

        mAnimation = createAnimation();

        onSetup();

        redraw();
    }

    void CharacterPreview::redraw()
    {
        if (std::getenv("OPENMW_FNV_DISABLE_INVENTORY_PREVIEW") != nullptr && mNode != nullptr
            && mNode->getName() == "FNV Inventory Paper Doll Preview")
        {
            mRTTNode->setNodeMask(0);
            Log(Debug::Info) << "FNV/ESM4 proof: disabled inventory paper doll RTT for Android headset startup";
            return;
        }

        mRTTNode->setNodeMask(Mask_RenderToTexture);
        mDrawOnceCallback->redrawNextFrame();
    }

    void CharacterPreview::updateLive(double simulationTime)
    {
        if (!mAnimation)
            return;

        mAnimation->runAnimation(static_cast<float>(simulationTime));
        setRedrawSimulationTime(simulationTime);
        setBlendMode();
        redraw();
    }

    // --------------------------------------------------------------------------------------------------

    InventoryPreview::InventoryPreview(
        osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character, ViewMode viewMode)
        : CharacterPreview(parent, resourceSystem, character, 512, 1024, osg::Vec3f(0, 700, 71), osg::Vec3f(0, 0, 71))
        , mViewMode(viewMode)
    {
        mNode->setName("FNV Inventory Paper Doll Preview");
    }

    osg::ref_ptr<Animation> InventoryPreview::createAnimation()
    {
        if (shouldUseFalloutInventoryPlayerVisual())
        {
            if (const ESM4::Npc* falloutPlayerVisual = findFalloutInventoryPlayerVisualRecord())
            {
                ESM::CellRef proxyRef;
                proxyRef.blank();
                proxyRef.mRefID = ESM::RefId::stringRefId("Player");
                mFalloutPreviewRef = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, falloutPlayerVisual);
                MWWorld::Ptr visualPtr(mFalloutPreviewRef.get(), nullptr);
                visualPtr.getRefData().setCustomData(std::unique_ptr<MWWorld::CustomData>());
                applyFalloutInventoryPlayerProxyProofOutfit(visualPtr);

                Log(Debug::Info) << "FNV/ESM4 proof: using Fallout inventory player visual proxy "
                                 << falloutPlayerVisual->mEditorId << " (" << ESM::RefId(falloutPlayerVisual->mId)
                                 << ")";
                return new ESM4NpcAnimation(visualPtr, mNode, mResourceSystem);
            }

            Log(Debug::Warning)
                << "FNV/ESM4 proof: requested Fallout inventory player visual proxy, but no FONV Player NPC was found";
        }

        mFalloutPreviewRef.reset();
        if (mCharacter.getType() == ESM::REC_NPC_4)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: using live Fallout inventory player visual " << mCharacter.toString();
            return new ESM4NpcAnimation(mCharacter, mNode, mResourceSystem);
        }

        return CharacterPreview::createAnimation();
    }

    void InventoryPreview::setViewport(int sizeX, int sizeY)
    {
        sizeX = std::max(sizeX, 0);
        sizeY = std::max(sizeY, 0);

        // NB Camera::setViewport has threading issues
        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        // This expects Y-down convention; historically the origin was (0, mSizeY - sizeY)
        mViewport = new osg::Viewport(0, 0, std::min(mSizeX, sizeX), std::min(mSizeY, sizeY));
        stateset->setAttributeAndModes(mViewport);
        mRTTNode->setCameraStateset(stateset);

        redraw();
    }

    void InventoryPreview::update()
    {
        if (!mAnimation.get())
            return;

        NpcAnimation* npcAnimation = dynamic_cast<NpcAnimation*>(mAnimation.get());
        if (npcAnimation == nullptr)
        {
            const float previewStart = mFalloutPreviewRef != nullptr ? 0.35f : 0.0f;
            mDrawOnceCallback->setSimulationTime(previewStart);
            if (mAnimation->hasAnimation("idle"))
                mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", previewStart, 0);
            mAnimation->runAnimation(previewStart);
            setBlendMode();
            redraw();
            return;
        }

        npcAnimation->showWeapons(true);
        npcAnimation->updateParts();

        MWWorld::InventoryStore& inv = mCharacter.getClass().getInventoryStore(mCharacter);
        MWWorld::ContainerStoreIterator iter = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        std::string groupname = "inventoryhandtohand";
        bool showCarriedLeft = true;
        if (iter != inv.end())
        {
            groupname = "inventoryweapononehand";
            if (iter->getType() == ESM::Weapon::sRecordId)
            {
                MWWorld::LiveCellRef<ESM::Weapon>* ref = iter->get<ESM::Weapon>();
                int type = ref->mBase->mData.mType;
                const ESM::WeaponType* weaponInfo = MWMechanics::getWeaponType(type);
                showCarriedLeft = !(weaponInfo->mFlags & ESM::WeaponType::TwoHanded);

                std::string inventoryGroup = weaponInfo->mLongGroup;
                inventoryGroup = "inventory" + inventoryGroup;

                // We still should use one-handed animation as fallback
                if (npcAnimation->hasAnimation(inventoryGroup))
                    groupname = std::move(inventoryGroup);
                else
                {
                    static const std::string oneHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::Weapon::LongBladeOneHand)->mLongGroup;
                    static const std::string twoHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::Weapon::LongBladeTwoHand)->mLongGroup;

                    // For real two-handed melee weapons use 2h swords animations as fallback, otherwise use the 1h ones
                    if (weaponInfo->mFlags & ESM::WeaponType::TwoHanded
                        && weaponInfo->mWeaponClass == ESM::WeaponType::Melee)
                        groupname = twoHandFallback;
                    else
                        groupname = oneHandFallback;
                }
            }
        }

        npcAnimation->showCarriedLeft(showCarriedLeft);

        const bool falloutPreview = mFalloutPreviewRef != nullptr || mCharacter.getType() == ESM::REC_NPC_4;
        if (falloutPreview && !npcAnimation->hasAnimation(groupname) && npcAnimation->hasAnimation("idle"))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: inventory paper doll using Fallout idle pose instead of missing "
                             << groupname;
            groupname = "idle";
        }

        mCurrentAnimGroup = std::move(groupname);
        npcAnimation->play(mCurrentAnimGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);

        MWWorld::ConstContainerStoreIterator torch = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if (torch != inv.end() && torch->getType() == ESM::Light::sRecordId && showCarriedLeft)
        {
            if (!npcAnimation->getInfo("torch"))
                npcAnimation->play("torch", 2, BlendMask::BlendMask_LeftArm, false, 1.0f, "start", "stop", 0.0f,
                    std::numeric_limits<uint32_t>::max(), true);
        }
        else if (npcAnimation->getInfo("torch"))
            npcAnimation->disable("torch");

        npcAnimation->runAnimation(0.0f);
        mDrawOnceCallback->setSimulationTime(0.0f);

        setBlendMode();

        redraw();
    }

    int InventoryPreview::getSlotSelected(int posX, int posY)
    {
        if (!mViewport)
            return -1;
        NpcAnimation* npcAnimation = dynamic_cast<NpcAnimation*>(mAnimation.get());
        if (npcAnimation == nullptr)
            return -1;

        double projX = (posX / mViewport->width()) * 2 - 1;
        double projY = (posY / mViewport->height()) * 2 - 1;
        // With Intersector::WINDOW, the intersection ratios are slightly inaccurate. Seems to be a
        // precision issue - compiling with OSG_USE_FLOAT_MATRIX=0, Intersector::WINDOW works ok.
        // Using Intersector::PROJECTION results in better precision because the start/end points and the model matrices
        // don't go through as many transformations.
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::Intersector::PROJECTION, projX, projY));

        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
        // Set the traversal number from the last draw, so that the frame switch used for RigGeometry double buffering
        // works correctly
        visitor.setTraversalNumber(mDrawOnceCallback->getLastRenderedFrame());

        auto* camera = mRTTNode->getCamera(nullptr);
        osg::Node::NodeMask nodeMask = camera->getNodeMask();
        camera->setNodeMask(~0u);
        camera->accept(visitor);
        camera->setNodeMask(nodeMask);

        if (intersector->containsIntersections())
        {
            osgUtil::LineSegmentIntersector::Intersection intersection = intersector->getFirstIntersection();
            return npcAnimation->getSlot(intersection.nodePath);
        }
        return -1;
    }

    void InventoryPreview::updatePtr(const MWWorld::Ptr& ptr)
    {
        mCharacter = MWWorld::Ptr(ptr.getBase(), nullptr);
    }

    void InventoryPreview::onSetup()
    {
        CharacterPreview::onSetup();
        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);

        mNode->setScale(scale);

        osg::Vec3f position = mPosition;
        osg::Vec3f lookAt = mLookAt;
        if (mFalloutPreviewRef != nullptr)
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0, 700, 76);
                    lookAt = osg::Vec3f(0, 0, 76);
                    break;
                case ViewMode::Profile:
                    position = osg::Vec3f(320, 0, 104);
                    lookAt = osg::Vec3f(0, 0, 104);
                    break;
                case ViewMode::Top:
                    position = osg::Vec3f(0, -230, 310);
                    lookAt = osg::Vec3f(0, 0, 112);
                    break;
            }
            Log(Debug::Info) << "FNV/ESM4 proof: inventory paper doll camera "
                             << (mViewMode == ViewMode::Front ? "front"
                                     : (mViewMode == ViewMode::Profile ? "profile" : "top"))
                             << " position=(" << position.x() << "," << position.y() << "," << position.z()
                             << ") lookAt=(" << lookAt.x() << "," << lookAt.y() << "," << lookAt.z() << ")";
        }

        auto viewMatrix = osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1));
        mRTTNode->setViewMatrix(viewMatrix);
        if (mFalloutPreviewRef != nullptr)
            update();
    }

    // --------------------------------------------------------------------------------------------------

    RaceSelectionPreview::RaceSelectionPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem)
        : CharacterPreview(
            parent, resourceSystem, MWMechanics::getPlayer(), 512, 512, osg::Vec3f(0, 125, 8), osg::Vec3f(0, 0, 8))
        , mBase(*mCharacter.get<ESM::NPC>()->mBase)
        , mRef(ESM::makeBlankCellRef(), &mBase)
        , mPitchRadians(osg::DegreesToRadians(6.f))
    {
        mCharacter = MWWorld::Ptr(&mRef, nullptr);
    }

    RaceSelectionPreview::~RaceSelectionPreview() {}

    void RaceSelectionPreview::setAngle(float angleRadians)
    {
        mNode->setAttitude(osg::Quat(mPitchRadians, osg::Vec3(1, 0, 0)) * osg::Quat(angleRadians, osg::Vec3(0, 0, 1)));
        redraw();
    }

    void RaceSelectionPreview::setPrototype(const ESM::NPC& proto)
    {
        mBase = proto;
        mBase.mId = ESM::RefId::stringRefId("Player");
        rebuild();
    }

    class UpdateCameraCallback : public SceneUtil::NodeCallback<UpdateCameraCallback, CharacterPreviewRTTNode*>
    {
    public:
        UpdateCameraCallback(
            osg::ref_ptr<const osg::Node> nodeToFollow, const osg::Vec3& posOffset, const osg::Vec3& lookAtOffset)
            : mNodeToFollow(std::move(nodeToFollow))
            , mPosOffset(posOffset)
            , mLookAtOffset(lookAtOffset)
        {
        }

        void operator()(CharacterPreviewRTTNode* node, osg::NodeVisitor* nv)
        {
            // Update keyframe controllers in the scene graph first...
            traverse(node, nv);

            // Now update camera utilizing the updated head position
            osg::NodePathList nodepaths = mNodeToFollow->getParentalNodePaths();
            if (nodepaths.empty())
                return;
            osg::Matrix worldMat = osg::computeLocalToWorld(nodepaths[0]);
            osg::Vec3 headOffset = worldMat.getTrans();

            auto viewMatrix
                = osg::Matrixf::lookAt(headOffset + mPosOffset, headOffset + mLookAtOffset, osg::Vec3(0, 0, 1));
            node->setViewMatrix(viewMatrix);
        }

    private:
        osg::ref_ptr<const osg::Node> mNodeToFollow;
        osg::Vec3 mPosOffset;
        osg::Vec3 mLookAtOffset;
    };

    void RaceSelectionPreview::onSetup()
    {
        CharacterPreview::onSetup();
        mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);
        mAnimation->runAnimation(0.f);

        // attach camera to follow the head node
        if (mUpdateCameraCallback)
            mRTTNode->removeUpdateCallback(mUpdateCameraCallback);

        const osg::Node* head = mAnimation->getNode("Bip01 Head");
        if (head)
        {
            mUpdateCameraCallback = new UpdateCameraCallback(head, mPosition, mLookAt);
            mRTTNode->addUpdateCallback(mUpdateCameraCallback);
        }
        else
            Log(Debug::Error) << "Error: Bip01 Head node not found";
    }

    // --------------------------------------------------------------------------------------------------

    FalloutActorPreview::FalloutActorPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        const MWWorld::Ptr& character, ViewMode viewMode, float cameraDistanceMultiplier, std::string profileOverride,
        osg::Vec3f editorRotation, float editorScale)
        : CharacterPreview(parent, resourceSystem, MWWorld::Ptr(character.getBase(), nullptr), 720, 720,
            osg::Vec3f(0, 420, 112), osg::Vec3f(0, 0, 112))
        , mViewMode(viewMode)
        , mCameraDistanceMultiplier(cameraDistanceMultiplier)
        , mProfileOverride(std::move(profileOverride))
        , mEditorRotation(editorRotation)
        , mEditorScale(editorScale)
    {
        mNode->setName("FNV Neutral Actor Preview");
    }

    osg::ref_ptr<Animation> FalloutActorPreview::createAnimation()
    {
        if (mCharacter.getType() == ESM::REC_NPC_4)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using ESM4NpcAnimation for "
                             << mCharacter.toString();
            osg::ref_ptr<ESM4NpcAnimation> animation = new ESM4NpcAnimation(mCharacter, mNode, mResourceSystem);
            animation->setProofPreviewAnimation(true);
            animation->setProofPreviewGameplayAudit(mViewMode == ViewMode::Front);
            const std::string previewProfile = !mProfileOverride.empty()
                ? mProfileOverride
                : std::string(getFalloutNeutralActorPreviewProfile());
            const bool ikOverlayRequested = getFalloutNeutralActorPreviewBool("OPENMW_FNV_SHOW_IK_BONES")
                || getFalloutNeutralActorPreviewBool("OPENMW_FNV_BONE_IK_DEBUG")
                || getFalloutNeutralActorPreviewBool("OPENMW_FNV_SHOW_ALL_BONES")
                || getFalloutNeutralActorPreviewBool("OPENMW_FNV_ALL_BONE_DEBUG");
            animation->setFONVBoneIkDebugInProofPreview(
                ikOverlayRequested || mViewMode == ViewMode::FrontRight
                || Misc::StringUtils::ciEqual(previewProfile, "weapon-arms")
                || Misc::StringUtils::ciEqual(previewProfile, "arms"));
            return animation;
        }
        if (mCharacter.getType() == ESM::REC_CREA4)
        {
            const std::string rawModel(mCharacter.getClass().getModel(mCharacter));
            std::string animationModel;
            if (!rawModel.empty())
                animationModel = Misc::ResourceHelpers::correctActorModelPath(
                    VFS::Path::toNormalized(rawModel), mResourceSystem->getVFS());
            const bool animated = !animationModel.empty()
                && !(animationModel == rawModel && Misc::StringUtils::ciEndsWith(animationModel, ".nif"));

            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using CreatureAnimation for "
                             << mCharacter.toString() << " model=\"" << rawModel << "\" correctedModel=\""
                             << animationModel << "\" animated=" << animated
                             << " classification=visual-preview-supported source=neutral-preview";
            osg::ref_ptr<Animation> animation
                = new CreatureAnimation(mCharacter, animationModel, mResourceSystem, animated, mNode);
            animation->setProofPreviewAnimation(true);
            animation->setProofPreviewGameplayAudit(mViewMode == ViewMode::Front);
            return animation;
        }

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using base CharacterPreview animation for "
                         << mCharacter.toString();
        osg::ref_ptr<Animation> animation = CharacterPreview::createAnimation();
        if (animation != nullptr)
        {
            animation->setProofPreviewAnimation(true);
            animation->setProofPreviewGameplayAudit(mViewMode == ViewMode::Front);
        }
        return animation;
    }

    void FalloutActorPreview::onSetup()
    {
        CharacterPreview::onSetup();

        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);
        const float editorScale = std::clamp(mEditorScale, 0.05f, 10.f);
        mNode->setScale(scale * editorScale);
        mNode->setAttitude(osg::Quat(osg::DegreesToRadians(mEditorRotation.x()), osg::Vec3f(1.f, 0.f, 0.f),
            osg::DegreesToRadians(mEditorRotation.y()), osg::Vec3f(0.f, 1.f, 0.f),
            osg::DegreesToRadians(mEditorRotation.z()), osg::Vec3f(0.f, 0.f, 1.f)));

        osg::Vec3f position(0.f, 420.f, 112.f);
        osg::Vec3f lookAt(0.f, 0.f, 112.f);
        const char* viewName = "front";
        const std::string envProfile(getFalloutNeutralActorPreviewProfile());
        const std::string profile = !mProfileOverride.empty() ? mProfileOverride : envProfile;
        const bool rightHandCloseProfile = Misc::StringUtils::ciEqual(profile, "right-hand-close");
        const bool leftHandCloseProfile = Misc::StringUtils::ciEqual(profile, "left-hand-close");
        const bool handsCloseProfile = Misc::StringUtils::ciEqual(profile, "hands-close");
        if (Misc::StringUtils::ciEqual(profile, "full-body") || Misc::StringUtils::ciEqual(profile, "fullbody"))
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 760.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 78.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 78.f), position, lookAt,
                viewName);
        }
        else if (Misc::StringUtils::ciEqual(profile, "face"))
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 190.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 116.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 116.f), position, lookAt,
                viewName);
        }
        else if (Misc::StringUtils::ciEqual(profile, "hands") || Misc::StringUtils::ciEqual(profile, "weapon-arms")
            || Misc::StringUtils::ciEqual(profile, "arms"))
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0.f, 520.f, 82.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 82.f);
                    viewName = "arm-elbow-hands-wide";
                    break;
                case ViewMode::FrontLeft:
                    position = osg::Vec3f(-42.f, 500.f, 82.f);
                    lookAt = osg::Vec3f(-28.f, 0.f, 82.f);
                    viewName = "left-elbow-hand";
                    break;
                case ViewMode::FrontRight:
                    position = osg::Vec3f(42.f, 500.f, 82.f);
                    lookAt = osg::Vec3f(28.f, 0.f, 82.f);
                    viewName = "right-elbow-hand-weapon";
                    break;
                case ViewMode::Left:
                case ViewMode::Right:
                case ViewMode::Top:
                case ViewMode::Bottom:
                case ViewMode::Back:
                case ViewMode::IsoNW:
                case ViewMode::IsoSW:
                    applyFalloutNeutralActorOrbitCamera(mViewMode,
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 420.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 112.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 112.f),
                        position, lookAt, viewName);
                    break;
            }
        }
        else if (rightHandCloseProfile || leftHandCloseProfile || handsCloseProfile)
        {
            const bool left = leftHandCloseProfile;
            const float handX = left ? -24.f : 24.f;
            position = osg::Vec3f(
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_X", handX),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Y", 120.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Z", 70.f));
            lookAt = osg::Vec3f(
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_X", handX),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_Y", 0.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_Z", 70.f));
            viewName = left ? "left-hand-close" : "right-hand-close";
            if (handsCloseProfile)
                viewName = "hands-close";
        }
        else if (Misc::StringUtils::ciEqual(profile, "audit") || Misc::StringUtils::ciEqual(profile, "bot-audit"))
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0.f, 760.f, 78.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 78.f);
                    viewName = "full-body";
                    break;
                case ViewMode::FrontLeft:
                    position = osg::Vec3f(0.f, 260.f, 118.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 118.f);
                    viewName = "face-hat";
                    break;
                case ViewMode::FrontRight:
                    position = osg::Vec3f(
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_X", 20.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Y", 190.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Z", 70.f));
                    lookAt = osg::Vec3f(
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_X", 20.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_Y", 0.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_Z", 70.f));
                    viewName = "right-hand-weapon";
                    break;
                case ViewMode::Left:
                case ViewMode::Right:
                case ViewMode::Top:
                case ViewMode::Bottom:
                case ViewMode::Back:
                case ViewMode::IsoNW:
                case ViewMode::IsoSW:
                    applyFalloutNeutralActorOrbitCamera(mViewMode,
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 190.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 116.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 116.f),
                        position, lookAt, viewName);
                    break;
            }
        }
        else
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 420.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 112.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 112.f), position, lookAt,
                viewName);
        }

        const float yawOffsetDeg
            = getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_YAW_OFFSET_DEG", 0.f);
        applyFalloutNeutralActorPreviewYawOffset(position, lookAt, yawOffsetDeg);
        const float cameraDistanceMultiplier = std::clamp(mCameraDistanceMultiplier, 0.15f, 12.f);
        position = lookAt + (position - lookAt) * cameraDistanceMultiplier;
        const float cameraTiltDeg
            = getFalloutNeutralActorPreviewFloat("OPENMW_FNV_ASSET_STUDIO_CAMERA_TILT_DEG", 0.f);
        if (std::isfinite(cameraTiltDeg) && std::abs(cameraTiltDeg) > 0.001f)
        {
            osg::Vec3f direction = position - lookAt;
            osg::Vec3f right = osg::Vec3f(0.f, 0.f, 1.f) ^ direction;
            if (right.normalize() == 0.f)
                right = osg::Vec3f(1.f, 0.f, 0.f);
            direction = osg::Quat(osg::DegreesToRadians(cameraTiltDeg), right) * direction;
            position = lookAt + direction;
        }
        const osg::Vec3f cameraPan(
            getFalloutNeutralActorPreviewFloat("OPENMW_FNV_ASSET_STUDIO_CAMERA_PAN_X", 0.f), 0.f,
            getFalloutNeutralActorPreviewFloat("OPENMW_FNV_ASSET_STUDIO_CAMERA_PAN_Z", 0.f));
        position += cameraPan;
        lookAt += cameraPan;

        mRTTNode->setViewMatrix(osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1)));

        const std::string animationGroup = getFalloutPreviewAnimationGroup();
        const float previewStart = getFalloutPreviewAnimationStartPoint();
        const bool bindPose = shouldUseFalloutNeutralActorBindPose();
        if (mAnimation)
        {
            if (!bindPose && mAnimation->hasAnimation(animationGroup))
                mAnimation->play(animationGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", previewStart,
                    std::numeric_limits<uint32_t>::max(), true);
            mAnimation->runAnimation(0.0f);
        }
        if (mAnimation && (rightHandCloseProfile || leftHandCloseProfile || handsCloseProfile))
        {
            const osg::Vec3f rightHand = getWorldPosition(mAnimation->getNode("Bip01 R Hand"));
            const osg::Vec3f leftHand = getWorldPosition(mAnimation->getNode("Bip01 L Hand"));
            const bool rightValid = rightHand.length2() > 0.001f;
            const bool leftValid = leftHand.length2() > 0.001f;
            bool resolved = false;
            osg::Vec3f handTarget;
            if (rightHandCloseProfile && rightValid)
            {
                handTarget = rightHand;
                resolved = true;
            }
            else if (leftHandCloseProfile && leftValid)
            {
                handTarget = leftHand;
                resolved = true;
            }
            else if (handsCloseProfile && rightValid && leftValid)
            {
                handTarget = (rightHand + leftHand) * 0.5f;
                resolved = true;
            }

            if (resolved)
            {
                handTarget.x() += getFalloutNeutralActorPreviewFloat(
                    "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_OFFSET_X", 0.f);
                handTarget.y() += getFalloutNeutralActorPreviewFloat(
                    "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_OFFSET_Y", 0.f);
                handTarget.z() += getFalloutNeutralActorPreviewFloat(
                    "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_OFFSET_Z", 0.f);
                const float distance = getFalloutNeutralActorPreviewFloat(
                    "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Y", handsCloseProfile ? 190.f : 120.f);
                const float cameraZOffset = getFalloutNeutralActorPreviewFloat(
                    "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Z_OFFSET", 8.f);
                lookAt = handTarget;
                position = osg::Vec3f(handTarget.x(), handTarget.y() + distance, handTarget.z() + cameraZOffset);
                applyFalloutNeutralActorPreviewYawOffset(position, lookAt, yawOffsetDeg);
                position = lookAt + (position - lookAt) * cameraDistanceMultiplier;
                mRTTNode->setViewMatrix(
                    osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1)));
                Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview auto hand-close camera view=" << viewName
                                 << " target=" << formatVec3(handTarget)
                                 << " rightHand=" << formatVec3(rightHand)
                                 << " leftHand=" << formatVec3(leftHand)
                                 << " distance=" << distance
                                 << " cameraZOffset=" << cameraZOffset
                                 << " runtime=runtime-supported gate=runtime-neutral-actor-preview-hand-close";
            }
        }
        setRedrawSimulationTime(bindPose ? 0.0 : previewStart);

        if (getFalloutNeutralActorPreviewBool("OPENMW_FNV_DRAW_PART_AXES"))
        {
            mNode->addChild(makeFalloutPreviewAxisGeode("FNV Studio Axis Actor Root", 30.f, false));
            osg::Group* headNode = mAnimation
                ? dynamic_cast<osg::Group*>(const_cast<osg::Node*>(mAnimation->getNode("Bip01 Head")))
                : nullptr;
            if (headNode != nullptr)
                headNode->addChild(makeFalloutPreviewAxisGeode("FNV Studio Axis Head", 18.f, true));
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview studio axes view=" << viewName
                             << " actor=" << mCharacter.getCellRef().getRefId()
                             << " actorRoot=1 head=" << (headNode != nullptr)
                             << " runtime=runtime-supported gate=runtime-neutral-actor-preview-studio-axes";
        }

        if (getFalloutNeutralActorPreviewBool("OPENMW_FNV_ACTOR_PREVIEW_HIDE_BLOOD"))
        {
            FalloutActorPreviewHideBloodVisitor hideBlood;
            mNode->accept(hideBlood);
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview blood visibility view=" << viewName
                             << " actor=" << mCharacter.getCellRef().getRefId()
                             << " visible=0 hiddenNodes=" << hideBlood.getHiddenNodes()
                             << " hiddenDrawables=" << hideBlood.getHiddenDrawables()
                             << " runtime=runtime-supported gate=runtime-neutral-actor-preview-blood-toggle";
        }

        const bool rigWireframe = getFalloutNeutralActorPreviewBool("OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME");
        const bool rigWeights = getFalloutNeutralActorPreviewBool("OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS")
            || getFalloutNeutralActorPreviewBool("OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS");
        const std::string rigWeightSelector
            = getFalloutNeutralActorPreviewString("OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE");
        if (rigWireframe || rigWeights)
        {
            if (rigWireframe)
                applyFalloutPreviewWireframeState(*mNode->getOrCreateStateSet(),
                    getFalloutNeutralActorPreviewFloat("OPENMW_FNV_ACTOR_PREVIEW_DEBUG_LINE_WIDTH", 4.f));
            FalloutActorPreviewRigDebugVisitor rigDebug(rigWireframe, rigWeights, rigWeightSelector);
            mNode->accept(rigDebug);
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview rig debug view=" << viewName
                             << " actor=" << mCharacter.getCellRef().getRefId()
                             << " wireframe=" << rigWireframe
                             << " fingerWeights=" << rigWeights
                             << " weightSelector="
                             << (rigWeightSelector.empty() ? std::string("aggregate") : rigWeightSelector)
                             << " rigDrawables=" << rigDebug.mRigs
                             << " bones=" << rigDebug.mBones
                             << " weightColoredRigs=" << rigDebug.mWeightRigs
                             << " weightMisses=" << rigDebug.mWeightMisses
                             << " weightMatchedBones=" << rigDebug.mWeightMatchedBones
                             << " weightVertices=" << rigDebug.mWeightVertices
                             << " maxWeight=" << rigDebug.mMaxWeight
                             << " wireframeDrawables=" << rigDebug.mWireframeDrawables
                             << " runtime=runtime-supported gate=runtime-neutral-actor-preview-rig-debug";
        }

        osg::ComputeBoundsVisitor boundsVisitor;
        boundsVisitor.setTraversalMask(~(Mask_ParticleSystem | Mask_Effect));
        mNode->accept(boundsVisitor);
        const osg::BoundingBox& bounds = boundsVisitor.getBoundingBox();
        const bool boundsValid = bounds.valid();
        osg::Vec3f boundsSize;
        osg::Vec3f boundsCenter;
        if (boundsValid)
        {
            boundsCenter = bounds.center();
            boundsSize = osg::Vec3f(
                bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin(), bounds.zMax() - bounds.zMin());
        }

        const osg::Vec3f head = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 Head") : nullptr);
        const osg::Vec3f pelvis = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 Pelvis") : nullptr);
        const osg::Vec3f leftFoot = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L Foot") : nullptr);
        const osg::Vec3f rightFoot = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R Foot") : nullptr);
        const float feetZ = (leftFoot.z() + rightFoot.z()) * 0.5f;
        const float headAboveFeet = head.z() - feetZ;
        const float headAbovePelvis = head.z() - pelvis.z();
        const bool upright = boundsValid && headAboveFeet > 80.f && headAbovePelvis > 30.f && boundsSize.z() > 80.f;

        Log(upright ? Debug::Info : Debug::Warning)
            << "FNV/ESM4 proof: neutral actor preview bounds view=" << viewName
            << " actor=" << mCharacter.getCellRef().getRefId()
            << " boundsValid=" << boundsValid
            << " center=" << formatVec3(boundsCenter)
            << " size=" << formatVec3(boundsSize)
            << " head=" << formatVec3(head)
            << " pelvis=" << formatVec3(pelvis)
            << " leftFoot=" << formatVec3(leftFoot)
            << " rightFoot=" << formatVec3(rightFoot)
            << " headAboveFeet=" << headAboveFeet
            << " headAbovePelvis=" << headAbovePelvis
            << " verdict=" << (upright ? "OK" : "BAD")
            << " runtime=" << (upright ? "runtime-supported" : "loaded-pending-runtime")
            << " gate=runtime-neutral-actor-preview-bounds";

        const bool auditProfile = Misc::StringUtils::ciEqual(profile, "audit")
            || Misc::StringUtils::ciEqual(profile, "bot-audit");
        const bool weaponArmsProfile = Misc::StringUtils::ciEqual(profile, "weapon-arms")
            || Misc::StringUtils::ciEqual(profile, "arms");
        if (auditProfile || weaponArmsProfile)
        {
            std::unique_ptr<FalloutActorPreviewPartMaskVisitor> partMask;
            if (auditProfile && mViewMode == ViewMode::FrontLeft)
                partMask = std::make_unique<FalloutActorPreviewPartMaskVisitor>(
                    FalloutActorPreviewPartMaskVisitor::Mode::FaceHeadgear);

            if (partMask != nullptr)
            {
                mNode->accept(*partMask);
                Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview part mask view=" << viewName
                                 << " kept=" << partMask->getKept() << " masked=" << partMask->getMasked()
                                 << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
            }

            FalloutActorPreviewDrawableAuditVisitor drawableAudit;
            mNode->accept(drawableAudit);
            const osg::BoundingBox& drawableBounds = drawableAudit.getBounds();
            osg::ComputeBoundsVisitor visibleBoundsVisitor;
            mNode->accept(visibleBoundsVisitor);
            const osg::BoundingBox visibleBounds = visibleBoundsVisitor.getBoundingBox();
            const bool handCameraOverride = std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_X") != nullptr
                || std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Z") != nullptr
                || std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_X") != nullptr
                || std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_Y") != nullptr
                || std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_LOOK_Z") != nullptr;
            if (auditProfile && mViewMode == ViewMode::FrontRight && visibleBounds.valid() && !handCameraOverride)
            {
                const osg::Vec3f center = visibleBounds.center();
                const float distance = getFalloutNeutralActorPreviewFloat(
                    "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_HAND_CAMERA_Y", 760.f);
                lookAt = center;
                position = osg::Vec3f(center.x(), center.y() + distance, center.z());
                mRTTNode->setViewMatrix(
                    osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1)));
                Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview auto hand camera view=" << viewName
                                 << " target=" << formatVec3(center)
                                 << " targetSource=visible-combat-pose-bounds"
                                 << " distance=" << distance
                                 << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
            }
            Log(drawableAudit.getDrawables() > 0 ? Debug::Info : Debug::Warning)
                << "FNV/ESM4 proof: neutral actor preview drawable audit view=" << viewName
                << " geodes=" << drawableAudit.getGeodes()
                << " drawables=" << drawableAudit.getDrawables()
                << " geometry=" << drawableAudit.getGeometry()
                << " vertices=" << drawableAudit.getVertices()
                << " boundsValid=" << drawableBounds.valid()
                << " boundsMin=(" << drawableBounds.xMin() << "," << drawableBounds.yMin() << ","
                << drawableBounds.zMin() << ")"
                << " boundsMax=(" << drawableBounds.xMax() << "," << drawableBounds.yMax() << ","
                << drawableBounds.zMax() << ")"
                << " visibleBoundsValid=" << visibleBounds.valid()
                << " visibleBoundsCenter=(" << visibleBounds.center().x() << "," << visibleBounds.center().y() << ","
                << visibleBounds.center().z() << ")"
                << " visibleBoundsSize=(" << (visibleBounds.xMax() - visibleBounds.xMin()) << ","
                << (visibleBounds.yMax() - visibleBounds.yMin()) << ","
                << (visibleBounds.zMax() - visibleBounds.zMin()) << ")"
                << " runtime=" << (drawableAudit.getDrawables() > 0 ? "runtime-supported" : "loaded-pending-runtime")
                << " gate=runtime-neutral-actor-preview-drawables";
        }

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview camera view=" << viewName << " position=("
                         << position.x() << "," << position.y() << "," << position.z() << ") lookAt=("
                         << lookAt.x() << "," << lookAt.y() << "," << lookAt.z() << ") profile=" << profile
                         << " yawOffsetDeg=" << yawOffsetDeg << " zoom=" << cameraDistanceMultiplier
                         << " animationGroup=" << animationGroup << " startPoint=" << previewStart
                         << " bindPose=" << bindPose
                         << " simulationTime=" << (bindPose ? 0.0f : previewStart)
                         << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
    }

}

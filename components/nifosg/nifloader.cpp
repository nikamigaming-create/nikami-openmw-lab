Warning: truncated output (original token count: 99326)
Total output lines: 7400

#include "nifloader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <fstream>
#include <istream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/Matrixf>
#include <osg/Program>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/TexGen>
#include <osg/TexMat>
#include <osg/ValueObject>

#include <yaml-cpp/yaml.h>

// resource
#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/osguservalues.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nif/parent.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/serialization/osgyaml.hpp>
#include <components/vfs/manager.hpp>

// particle
#include <osgParticle/BoxPlacer>
#include <osgParticle/ConstantRateCounter>
#include <osgParticle/ModularProgram>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/Material>
#include <osg/PolygonMode>
#include <osg/PolygonOffset>
#include <osg/Stencil>
#include <osg/TexEnv>
#include <osg/TexEnvCombine>
#include <osg/Texture2D>

#include <components/bgsm/file.hpp>
#include <components/nif/effect.hpp>
#include <components/nif/exception.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/particle.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/extradata.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/texturetype.hpp>

#include "fog.hpp"
#include "falloutkf.hpp"
#include "matrixtransform.hpp"
#include "particle.hpp"

namespace
{
    bool isTransformInterpolatorRecordType(Nif::RecordType type)
    {
        return type == Nif::RC_NiTransformInterpolator || type == Nif::RC_BSRotAccumTransfInterpolator;
    }

    struct DisableOptimizer : osg::NodeVisitor
    {
        DisableOptimizer(osg::NodeVisitor::TraversalMode mode = TRAVERSE_ALL_CHILDREN)
            : osg::NodeVisitor(mode)
        {
        }

        void apply(osg::Node& node) override
        {
            node.setDataVariance(osg::Object::DYNAMIC);
            traverse(node);
        }

        void apply(osg::Drawable& node) override { traverse(node); }
    };

    void getAllNiNodes(const Nif::NiAVObject* node, std::vector<int>& outIndices)
    {
        if (const Nif::NiNode* ninode = dynamic_cast<const Nif::NiNode*>(node))
        {
            outIndices.push_back(ninode->recIndex);
            for (const auto& child : ninode->mChildren)
                if (!child.empty())
                    getAllNiNodes(child.getPtr(), outIndices);
        }
    }

    bool isTypeNiGeometry(int type)
    {
        switch (type)
        {
            case Nif::RC_NiTriShape:
            case Nif::RC_NiTriStrips:
            case Nif::RC_NiLines:
            case Nif::RC_BSLODTriShape:
            case Nif::RC_BSSegmentedTriShape:
                return true;
        }
        return false;
    }

    bool isFalloutDismemberCapShape(std::string_view shapeName)
    {
        // Fallout 3/New Vegas stores severed-limb cap meshes as MeatCap/GoreCap shapes.
        // Some creature meshes use names like "neckmeatcap" instead of a MeatCap prefix.
        const std::string name = Misc::StringUtils::lowerCase(shapeName);
        return name.find("meatcap") != std::string::npos || name.find("gorecap") != std::string::npos
            || name.find("bodycap") != std::string::npos || name.find("limbcap") != std::string::npos
            || name.find("meatneck") != std::string::npos || name.find("meathead") != std::string::npos;
    }

    bool isFalloutHiddenMorphShape(std::string_view shapeName)
    {
        // Fallout skin parts can carry hidden TRI morph target geometry next to the real skinned surface.
        return Misc::StringUtils::ciStartsWith(shapeName, "Tri ");
    }

    bool isFalloutConditionalDismemberCapPartition(std::uint32_t bodyPart)
    {
        // Fallout 3/New Vegas BSDismember partitions reserve 101..113 for section caps and 201..213 for
        // torso caps. They are conditional severed-limb surfaces and must not be drawn on an intact actor.
        // Intact body sections use ids such as 1000..13000, so a broad high-id test corrupts normal clothing.
        return (bodyPart >= 101 && bodyPart <= 113) || (bodyPart >= 201 && bodyPart <= 213);
    }

    bool containsAny(std::string_view value, std::initializer_list<std::string_view> needles)
    {
        for (std::string_view needle : needles)
        {
            if (value.find(needle) != std::string_view::npos)
                return true;
        }
        return false;
    }

    bool worldViewerEnvEnabled(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' && value[0] != '0';
    }

    bool isWorldViewerActorMeshPath(std::string_view filename)
    {
        return containsAny(filename,
            { "meshes/actors/", "meshes\\actors\\", "meshes/characters/", "meshes\\characters\\",
                "meshes/armor/", "meshes\\armor\\", "meshes/clothes/", "meshes\\clothes\\" });
    }

    bool isWorldViewerActorTelemetryMeshPath(std::string_view filename)
    {
        return isWorldViewerActorMeshPath(filename)
            || containsAny(filename, { "meshes/creatures/", "meshes\\creatures\\" });
    }

    bool worldViewerSkinPartitionFallbackEnabled()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ENABLE_SKIN_PARTITION_FALLBACK");
    }

    bool worldViewerMeshLoadTelemetryEnabled()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_MESH_LOAD_TELEMETRY")
            || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY")
            || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_TELEMETRY");
    }

    bool worldViewerMaterialTelemetryEnabled()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_MATERIAL_TELEMETRY")
            || worldViewerMeshLoadTelemetryEnabled();
    }

    bool worldViewerGenerateMissingBSNormalsEnabled()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_GENERATE_MISSING_BS_NORMALS");
    }

    bool worldViewerQuarantineFo4ActorSubIndexTriShapeEnabled()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_QUARANTINE_FO4_ACTOR_BSSUBINDEXTRISHAPE");
    }

    bool worldViewerForceFlatNifMaterials()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_NIF_MATERIALS")
            || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_WORLD_MATERIALS");
    }

    bool worldViewerForceFullbrightNifMaterials()
    {
        return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_NIF_MATERIALS")
            || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_WORLD_MATERIALS");
    }

    osg::Vec4f getWorldViewerFlatNifColor(
        std::string_view filename, std::string_view shapeName = {}, std::string_view shaderMaterialName = {})
    {
        if (isWorldViewerActorMeshPath(filename))
            return osg::Vec4f(0.86f, 0.82f, 0.74f, 1.f);

        std::string key(filename);
        key += ' ';
        key += std::string(shapeName);
        key += ' ';
        key += std::string(shaderMaterialName);
        key = Misc::StringUtils::lowerCase(key);
        const auto has = [&](std::string_view needle) { return key.find(needle) != std::string::npos; };

        if (has("glass") || has("skylight"))
            return osg::Vec4f(0.58f, 0.72f, 0.82f, 1.f);
        if (has("grass") || has("plant") || has("shrub") || has("landscape/trees")
            || has("landscape\\trees") || has("treemesa") || has("treeroot") || has("leaf") || has("vine")
            || has("groundcover") || has("canopy"))
            return osg::Vec4f(0.42f, 0.62f, 0.37f, 1.f);
        if (has("carpet") || has("rug"))
            return osg::Vec4f(0.58f, 0.33f, 0.28f, 1.f);
        if (has("metal") || has("brass") || has("gold") || has("beam") || has("trim") || has("bolt")
            || has("wire") || has("crate") || has("luggybot") || has("rail"))
            return osg::Vec4f(0.58f, 0.55f, 0.50f, 1.f);
        if (has("stone") || has("rock") || has("bark") || has("root"))
            return osg::Vec4f(0.52f, 0.53f, 0.47f, 1.f);
        if (has("plastic") || has("rubber") || has("matte"))
            return osg::Vec4f(0.42f, 0.46f, 0.50f, 1.f);
        if (has("floor") || has("tile") || has("tarmac") || has("landingpad") || has("deck")
            || has("road") || has("street") || has("sidewalk") || has("concrete"))
            return osg::Vec4f(0.62f, 0.64f, 0.58f, 1.f);
        if (has("label") || has("letter") || has("sign") || has("warning") || has("terminal"))
            return osg::Vec4f(0.70f, 0.62f, 0.42f, 1.f);
        if (has("screen") || has("glow") || has("light") || has("neon"))
            return osg::Vec4f(0.48f, 0.57f, 0.62f, 1.f);

        return osg::Vec4f(0.78f, 0.83f, 0.76f, 1.f);
    }

    void applyWorldViewerFlatStateSet(osg::StateSet* stateSet, const osg::Vec4f& color)
    {
        if (stateSet == nullptr)
            return;

        osg::ref_ptr<osg::Material> material = new osg::Material;
        material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        material->setDiffuse(osg::Material::FRONT_AND_BACK, color);
        material->setAmbient(osg::Material::FRONT_AND_BACK, color);
        material->setEmission(osg::Material::FRONT_AND_BACK, color);
        material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);

        stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        stateSet->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        for (unsigned int unit = 0; unit < 8; ++unit)
            stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setRenderingHint(osg::StateSet::DEFAULT_BIN);
    }

    void applyWorldViewerFlatGeometry(osg::Geometry* geometry, const osg::Vec4f& color)
    {
        if (geometry == nullptr)
            return;

        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back(color);
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->dirtyDisplayList();
        geometry->dirtyBound();
    }

    void applyWorldViewerFullbrightStateSet(osg::StateSet* stateSet)
    {
        if (stateSet == nullptr)
            return;

        osg::ref_ptr<osg::Material> material = new osg::Material;
        material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
        material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
        material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
        material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);

        stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        stateSet->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        for (unsigned int unit = 0; unit < 8; ++unit)
        {
            if (stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE) == nullptr)
                continue;

            const osg::StateAttribute::GLModeValue mode = unit == 0 ? osg::StateAttribute::ON : osg::StateAttribute::OFF;
            stateSet->setTextureMode(unit, GL_TEXTURE_2D, mode | osg::StateAttribute::OVERRIDE);
        }
    }

    void applyWorldViewerFullbrightGeometry(osg::Geometry* geometry)
    {
        if (geometry == nullptr)
            return;

        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->dirtyDisplayList();
        geometry->dirtyBound();
    }

    void applyWorldViewerFlatDrawable(osg::Drawable& drawable, std::string_view filename,
        std::string_view shapeName, std::string_view shaderMaterialName = {})
    {
        const bool actorPath = isWorldViewerActorMeshPath(filename);
        const bool flatMaterials = worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_NIF_MATERIALS")
            || (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_WORLD_MATERIALS") && !actorPath);
        const bool fullbrightMaterials = !flatMaterials
            && (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_NIF_MATERIALS")
                || (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_WORLD_MATERIALS") && !actorPath));
        if (!flatMaterials && !fullbrightMaterials)
            return;

        const osg::Vec4f color = getWorldViewerFlatNifColor(filename, shapeName, shaderMaterialName);
        if (flatMaterials)
        {
            applyWorldViewerFlatStateSet(drawable.getOrCreateStateSet(), color);
            applyWorldViewerFlatGeometry(drawable.asGeometry(), color);
        }
        else
        {
            applyWorldViewerFullbrightStateSet(drawable.getOrCreateStateSet());
            applyWorldViewerFullbrightGeometry(drawable.asGeometry());
        }
        if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
        {
            if (flatMaterials)
                applyWorldViewerFlatGeometry(rig->getSourceGeometry(), color);
            else
                applyWorldViewerFullbrightGeometry(rig->getSourceGeometry());
            if (osg::Geometry* source = rig->getSourceGeometry())
            {
                if (flatMaterials)
                    applyWorldViewerFlatStateSet(source->getOrCreateStateSet(), color);
                else
                    applyWorldViewerFullbrightStateSet(source->getOrCreateStateSet());
            }
            for (unsigned int i = 0; i < 2; ++i)
            {
                osg::Geometry* geometry = rig->getRenderGeometry(i);
                if (flatMaterials)
                    applyWorldViewerFlatGeometry(geometry, color);
                else
                    applyWorldViewerFullbrightGeometry(geometry);
                if (geometry != nullptr)
                {
                    if (flatMaterials)
                        applyWorldViewerFlatStateSet(geometry->getOrCreateStateSet(), color);
                    else
                        applyWorldViewerFullbrightStateSet(geometry->getOrCreateStateSet());
                }
            }
        }

        static std::atomic<int> logCount{ 0 };
        const int logIndex = logCount.fetch_add(1);
        if (logIndex < 120)
            Log(Debug::Info) << "World viewer nif proof material: mode="
                             << (flatMaterials ? "flat" : "fullbright")
                             << " file=\"" << filename << "\""
                             << " shape=\"" << shapeName << "\""
                             << " drawableClass=\"" << drawable.className() << "\""
                             << " actorPath=" << actorPath;
        else if (logIndex == 120)
            Log(Debug::Info) << "World viewer nif proof material: further logs suppressed";
    }

    bool isAmbientEmbeddedAnimationPath(std::string_view filename)
    {
        return containsAny(filename,
            { "meshes/effects/", "meshes\\effects\\", "windmill", "spinningwindmill", "fx", "smoke", "steam",
                "sanddust", "dust", "vulture", "bird", "flyswarm", "flag", "saloon-sign", "open_24hours_sign",
                "open-24-hours_sign" });
    }

    bool isFalloutFlagPath(std::string_view filename)
    {
        return containsAny(filename, { "meshes/clutter/flags/", "meshes\\clutter\\flags\\" });
    }

    bool isFalloutFlagHelperGeometry(std::string_view filename, const Nif::NiGeometry& geometry)
    {
        if (!containsAny(filename, { "meshes/clutter/flags/", "meshes\\clutter\\flags\\" }))
            return false;
        if (!geometry.mSkin.empty() || geometry.mData.empty() || geometry.mData->mVertices.size() != 24)
            return false;

        const std::string name = Misc::StringUtils::lowerCase(geometry.mName);
        return name.find("tail") != std::string::npos || name.find("rootbone") != std::string::npos
            || name.find("bip01 root") != std::string::npos;
    }

    bool isFalloutActorAddonHelperGeometry(std::string_view filename, const Nif::NiGeometry& geometry,
        const std::vector<unsigned int>& boundTextures)
    {
        if (!isWorldViewerActorTelemetryMeshPath(filename) || !geometry.mSkin.empty() || geometry.mData.empty())
            return false;

        // FO3/FNV actor add-ons sometimes retain an exported Root:0 transform helper as ordinary NiTriStrips.
        // These are untextured editor gizmos (typically a small pyramid/cube) rather than drawable actor parts.
        // Gamebryo excludes them from the render pass, but treating their lone NiMaterialProperty as a complete
        // surface produces an opaque block over nearby layered geometry.  Identify the authored helper signature
        // from render metadata instead of creature, form, or asset names so the rule applies to every actor add-on.
        const std::string name = Misc::StringUtils::lowerCase(geometry.mName);
        if (!name.ends_with("root:0") || !boundTextures.empty() || !geometry.mShaderProperty.empty()
            || !geometry.mAlphaProperty.empty() || !geometry.mData->mUVList.empty()
            || !geometry.mData->mColors.empty())
            return false;

        bool hasMaterial = false;
        for (const auto& property : geometry.mProperties)
        {
            if (property.empty())
                continue;
            if (property->recType != Nif::RC_NiMaterialProperty)
                return false;
            hasMaterial = true;
        }
        return hasMaterial;
    }

    bool enableExperimentalSkyShaderProperties()
    {
        return std::getenv("OPENMW_FNV_ENABLE_SKY_SHADER_PROPERTIES") != nullptr;
    }

    std::string remapFalloutSkyTexture(std::string_view texture)
    {
        const std::string lower = Misc::StringUtils::lowerCase(texture);
        if (lower == "textures/sky/cloudcloudy.dds" || lower == "textures\\sky\\cloudcloudy.dds")
            return "textures/sky/wastelandcloudcloudyupper01.dds";
        if (lower == "textures/sky/cloudclear.dds" || lower == "textures\\sky\\cloudclear.dds")
            return "textures/sky/nv_wastelandoverheadcloud.dds";
        return std::string(texture);
    }

    bool isActivationOnlyAnimationPath(std::string_view filename)
    {
        return containsAny(filename,
            { "door", "gate", "mailbox", "dropbox", "toolbox", "ammobox", "trash", "dumpster", "container", "flora",
                "plant", "flower", "fruit", "harvest", "cactus", "yucca", "creosote", "tree" });
    }

    bool isNiPSysControllerRecord(Nif::RecordType type)
    {
        switch (type)
        {
            case Nif::RC_NiPSysAirFieldAirFrictionCtlr:
            case Nif::RC_NiPSysAirFieldInheritVelocityCtlr:
            case Nif::RC_NiPSysAirFieldSpreadCtlr:
            case Nif::RC_NiPSysEmitterCtlr:
            case Nif::RC_NiPSysEmitterDeclinationCtlr:
            case Nif::RC_NiPSysEmitterDeclinationVarCtlr:
            case Nif::RC_NiPSysEmitterInitialRadiusCtlr:
            case Nif::RC_NiPSysEmitterLifeSpanCtlr:
            case Nif::RC_NiPSysEmitterPlanarAngleCtlr:
            case Nif::RC_NiPSysEmitterPlanarAngleVarCtlr:
            case Nif::RC_NiPSysEmitterSpeedCtlr:
            case Nif::RC_NiPSysFieldAttenuationCtlr:
            case Nif::RC_NiPSysFieldMagnitudeCtlr:
            case Nif::RC_NiPSysFieldMaxDistanceCtlr:
            case Nif::RC_NiPSysGravityStrengthCtlr:
            case Nif::RC_NiPSysInitialRotSpeedCtlr:
            case Nif::RC_NiPSysInitialRotSpeedVarCtlr:
            case Nif::RC_NiPSysInitialRotAngleCtlr:
            case Nif::RC_NiPSysInitialRotAngleVarCtlr:
            case Nif::RC_NiPSysModifierActiveCtlr:
            case Nif::RC_NiPSysResetOnLoopCtlr:
            case Nif::RC_NiPSysRotDampeningCtlr:
            case Nif::RC_NiPSysUpdateCtlr:
            case Nif::RC_BSPSysMultiTargetEmitterCtlr:
                return true;
            default:
                return false;
        }
    }

    bool isNiPSysEmitterRecord(Nif::RecordType type)
    {
        switch (type)
        {
            case Nif::RC_NiPSysBoxEmitter:
            case Nif::RC_NiPSysCylinderEmitter:
            case Nif::RC_NiPSysMeshEmitter:
            case Nif::RC_NiPSysSphereEmitter:
            case Nif::RC_BSPSysArrayEmitter:
                return true;
            default:
                return false;
        }
    }

    std::string getStringPaletteValue(const Nif::NiStringPalettePtr& palette, uint32_t offset)
    {
        if (palette.empty() || offset == std::numeric_limits<uint32_t>::max())
            return {};

        const std::string& text = palette->mPalette;
        if (offset >= text.size())
            return {};

        const std::size_t end = text.find('\0', offset);
        if (end == std::string::npos)
            return text.substr(offset);

        return text.substr(offset, end - offset);
    }

    std::string resolveControlledBlockString(const Nif::NiControllerSequence* sequence,
        const Nif::ControlledBlock& block, const std::string& directValue,
        uint32_t Nif::ControlledBlock::* offsetMember)
    {
        if (!directValue.empty())
            return directValue;

        // ControlledBlock's palette offsets are not serialized by older NIF versions.  Do not evaluate the member
        // until a palette proves this is one of the versions that actually uses offsets.
        if (!block.mStringPalette.empty())
        {
            std::string value = getStringPaletteValue(block.mStringPalette, block.*offsetMember);
            if (!value.empty())
                return value;
        }
        if (!sequence->mStringPalette.empty())
            return getStringPaletteValue(sequence->mStringPalette, block.*offsetMember);
        return {};
    }

    std::string resolveControlledBlockTargetName(
        const Nif::NiControllerSequence* sequence, const Nif::ControlledBlock& block)
    {
        std::string targetName = resolveControlledBlockString(
            sequence, block, block.mNodeName, &Nif::ControlledBlock::mNodeNameOffset);
        if (!targetName.empty())
            return targetName;
        if (!block.mTargetName.empty())
            return block.mTargetName;
        return {};
    }

    std::optional<unsigned int> parseControlledBlockUnsigned(std::string_view value)
    {
        if (value.empty())
            return std::nullopt;
        unsigned int result = 0;
        for (const char ch : value)
        {
            if (ch < '0' || ch > '9')
                return std::nullopt;
            const unsigned int digit = static_cast<unsigned int>(ch - '0');
            if (result > (std::numeric_limits<unsigned int>::max() - digit) / 10)
                return std::nullopt;
            result = result * 10 + digit;
        }
        return result;
    }

    struct ExternalTextureTransformRoute
    {
        bool mShaderMap = false;
        unsigned int mTextureSlot = 0;
        unsigned int mTransformMember = 0;
    };

    std::optional<ExternalTextureTransformRoute> parseExternalTextureTransformControllerId(
        std::string_view controllerId)
    {
        const std::size_t firstDash = controllerId.find('-');
        const std::size_t secondDash
            = firstDash == std::string_view::npos ? std::string_view::npos : controllerId.find('-', firstDash + 1);
        if (firstDash == std::string_view::npos || secondDash == std::string_view::npos)
            return std::nullopt;

        const auto shaderMap = parseControlledBlockUnsigned(controllerId.substr(0, firstDash));
        const auto textureSlot
            = parseControlledBlockUnsigned(controllerId.substr(firstDash + 1, secondDash - firstDash - 1));
        if (!shaderMap || *shaderMap > 1 || !textureSlot)
            return std::nullopt;

        const std::string member = Misc::StringUtils::lowerCase(controllerId.substr(secondDash + 1));
        unsigned int transformMember = 0;
        if (member == "tt_translate_u")
            transformMember = 0;
        else if (member == "tt_translate_v")
            transformMember = 1;
        else if (member == "tt_rotate")
            transformMember = 2;
        else if (member == "tt_scale_u")
            transformMember = 3;
        else if (member == "tt_scale_v")
            transformMember = 4;
        else
            return std::nullopt;

        return ExternalTextureTransformRoute{ *shaderMap != 0, *textureSlot, transformMember };
    }

    std::optional<Nif::NiMaterialColorController::TargetColor> parseExternalMaterialColorControllerId(
        std::string_view controllerId)
    {
        const std::string lower = Misc::StringUtils::lowerCase(controllerId);
        using TargetColor = Nif::NiMaterialColorController::TargetColor;
        if (lower == "ambient")
            return TargetColor::Ambient;
        if (lower == "diffuse")
            return TargetColor::Diffuse;
        if (lower == "specular")
            return TargetColor::Specular;
        if (lower == "self_illum" || lower == "emissive" || lower == "emission")
            return TargetColor::Emissive;
        return std::nullopt;
    }

    bool shouldAutoplayEmbeddedSequence(const Nif::NiControllerSequence& sequence, std::string_view filename)
    {
        const std::string sequenceName = Misc::StringUtils::lowerCase(sequence.mName);
        const bool ambientPath = isAmbientEmbeddedAnimationPath(filename);
        const bool falloutFlagPath = isFalloutFlagPath(filename);

        if (ambientPath && !falloutFlagPath && (sequenceName == "specialidle" || sequenceName == "idle"))
            return true;

        std::string key = sequenceName;
        for (const Nif::ControlledBlock& block : sequence.mControlledBlocks)
        {
            key += ' ';
            key += Misc::StringUtils::lowerCase(block.mTargetName);
            key += ' ';
            key += Misc::StringUtils::lowerCase(block.mNodeName);
            key += ' ';
            key += Misc::StringUtils::lowerCase(block.mControllerId);
            key += ' ';
            key += Misc::StringUtils::lowerCase(block.mInterpolatorId);
        }

        if (containsAny(key,
                { "open", "close", "activate", "deactivate", "trigger", "harvest", "pick", "container", "lid",
                    "door", "gate", "mailbox", "dropbox", "toolbox", "ammobox", "trash", "dumpster", "plant",
                    "flower", "fruit", "grow", "bloom" }))
            return false;

        if (falloutFlagPath)
        {
            if (containsAny(key, { "forward", "backward", "backwards", "left", "right", "up", "down" }))
                return false;
            if (sequence.mExtrapolationMode != Nif::NiTimeController::Cycle && sequenceName != "specialidle"
                && sequenceName != "idle")
                return false;
            return containsAny(key, { "idle", "loop", "ambient", "wind", "flag", "wave", "flutter", "sway", "tail" });
        }

        if (sequenceName == "specialidle" || sequenceName == "idle")
            return true;

        if (ambientPath && containsAny(key, { "forward", "backward", "backwards", "left", "right", "up", "down" }))
            return true;

        if (sequence.mExtrapolationMode != Nif::NiTimeController::Cycle)
            return false;

        if (ambientPath)
            return true;

        return containsAny(key,
            { "idle", "loop", "ambient", "wind", "spin", "rotate", "fan", "flag", "wave", "flutter", "sway",
                "steam", "smoke", "dust", "fx", "bird", "vulture", "fly", "swarm", "flicker", "pulse" });
    }

    bool shouldAutoplayFltAnimationNode(const Nif::NiFltAnimationNode& node, std::string_view filename)
    {
        if (isAmbientEmbeddedAnimationPath(filename))
            return true;

        const std::string name = Misc::StringUtils::lowerCase(node.mName);
        if (isActivationOnlyAnimationPath(filename)
            || containsAny(name,
                { "open", "close", "activate", "deactivate", "trigger", "harvest", "pick", "container", "lid",
                    "door", "gate", "mailbox", "dropbox", "toolbox", "ammobox", "trash", "dumpster", "plant",
                    "flower", "fruit", "grow", "bloom" }))
            return false;

        return true;
    }

    osg::Vec4f colorFromString(std::string_view value)
    {
        uint32_t hash = 2166136261u;
        for (char c : value)
        {
            hash ^= static_cast<unsigned char>(c);
            hash *= 16777619u;
        }

        static const std::array<osg::Vec4f, 10> palette = {
            osg::Vec4f(0.36f, 0.62f, 0.54f, 1.f),
            osg::Vec4f(0.66f, 0.54f, 0.31f, 1.f),
            osg::Vec4f(0.42f, 0.56f, 0.70f, 1.f),
            osg::Vec4f(0.64f, 0.43f, 0.36f, 1.f),
            osg::Vec4f(0.52f, 0.62f, 0.34f, 1.f),
            osg::Vec4f(0.33f, 0.58f, 0.65f, 1.f),
            osg::Vec4f(0.70f, 0.66f, 0.44f, 1.f),
            osg::Vec4f(0.45f, 0.49f, 0.54f, 1.f),
            osg::Vec4f(0.63f, 0.58f, 0.48f, 1.f),
            osg::Vec4f(0.38f, 0.66f, 0.42f, 1.f),
        };

        return palette[hash % palette.size()];
    }

    struct StarfieldExternalMeshData
    {
        std::vector<osg::Vec3f> mVertices;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mUv1;
        std::vector<unsigned int> mIndices;
        std::vector<std::uint32_t> mWeights;
        float mScale = 0.f;
        std::uint32_t mVersion = 0;
        std::uint32_t mWeightCountPerVertex = 0;
        std::uint32_t mUv1Count = 0;
        std::uint32_t mUv2Count = 0;
        std::uint32_t mColorCount = 0;
        std::uint32_t mNormalCount = 0;
        std::uint32_t mTangentCount = 0;
        std::uint32_t mLodCount = 0;
        std::uint32_t mMeshletCount = 0;
        std::uint32_t mCullDataCount = 0;
    };

    template <class T>
    bool readStarfieldMeshPod(std::istream& stream, T& value)
    {
        stream.read(reinterpret_cast<char*>(&value), sizeof(T));
        return static_cast<bool>(stream);
    }

    bool skipStarfieldMeshBytes(std::istream& stream, std::uint64_t bytes)
    {
        std::array<char, 4096> buffer{};
        while (bytes > 0)
        {
            const std::streamsize chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(bytes, buffer.size()));
            stream.read(buffer.data(), chunk);
            if (!stream)
                return false;
            bytes -= static_cast<std::uint64_t>(chunk);
        }
        return true;
    }

    bool skipStarfieldMeshArray(std::istream& stream, std::uint32_t count, std::uint32_t elementSize)
    {
        constexpr std::uint64_t maxSkipBytes = 512ull * 1024ull * 1024ull;
        const std::uint64_t bytes = static_cast<std::uint64_t>(count) * elementSize;
        if (bytes > maxSkipBytes)
            return false;
        return skipStarfieldMeshBytes(stream, bytes);
    }

    float getStarfieldMeshPositionScale()
    {
        static const float scale = [] {
            const char* value = std::getenv("OPENMW_STARFIELD_MESH_POSITION_SCALE");
            if (!value)
                return 32.f;
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed) || parsed <= 0.f)
                return 32.f;
            return parsed;
        }();
        return scale;
    }

    float unpackStarfieldMeshPosition(std::int16_t value, float scale)
    {
        const float positionScale = getStarfieldMeshPositionScale();
        if (value < 0)
            return static_cast<float>((value / 32768.0) * scale * positionScale);
        return static_cast<float>((value / 32767.0) * scale * positionScale);
    }

    osg::Vec3f unpackStarfieldUdec3Normal(std::uint32_t data)
    {
        return osg::Vec3f(
            static_cast<float>(((data & 1023) / 511.5) - 1.0),
            static_cast<float>((((data >> 10) & 1023) / 511.5) - 1.0),
            static_cast<float>((((data >> 20) & 1023) / 511.5) - 1.0));
    }

    float unpackStarfieldMeshHalf(std::uint16_t value)
    {
        std::uint32_t bits = static_cast<std::uint32_t>(value & 0x8000) << 16;

        const std::uint32_t exp16 = (value & 0x7c00) >> 10;
        std::uint32_t frac16 = value & 0x3ff;
        if (exp16)
            bits |= (exp16 + 0x70) << 23;
        else if (frac16)
        {
            std::uint8_t offset = 0;
            do
            {
                ++offset;
                frac16 <<= 1;
            } while ((frac16 & 0x400) != 0x400);
            frac16 &= 0x3ff;
            bits |= (0x71 - offset) << 23;
        }
        bits |= frac16 << 13;

        float result;
        std::memcpy(&result, &bits, sizeof(float));
        return result;
    }

    const Nif::SkinAttach* findStarfieldSkinAttach(const Nif::NiObjectNET* object)
    {
        if (object == nullptr)
            return nullptr;

        for (const Nif::ExtraPtr& extra : object->getExtraList())
        {
            if (!extra.empty() && extra->recType == Nif::RC_SkinAttach)
                return static_cast<const Nif::SkinAttach*>(extra.getPtr());
        }
        return nullptr;
    }

    enum class StarfieldWeightLayout
    {
        LowIndexHighHalf,
        HighIndexLowHalf,
        LowIndexHighUnorm,
        HighIndexLowUnorm,
    };

    const char* getStarfieldWeightLayoutName(StarfieldWeightLayout layout)
    {
        switch (layout)
        {
            case StarfieldWeightLayout::LowIndexHighHalf:
                return "low-index/high-half";
            case StarfieldWeightLayout::HighIndexLowHalf:
                return "high-index/low-half";
            case StarfieldWeightLayout::LowIndexHighUnorm:
                return "low-index/high-unorm";
            case StarfieldWeightLayout::HighIndexLowUnorm:
                return "high-index/low-unorm";
        }
        return "unknown";
    }

    std::pair<std::size_t, float> decodeStarfieldWeight(std::uint32_t raw, StarfieldWeightLayout layout)
    {
        const std::uint16_t low = static_cast<std::uint16_t>(raw & 0xffffu);
        const std::uint16_t high = static_cast<std::uint16_t>(raw >> 16);
        switch (layout)
        {
            case StarfieldWeightLayout::LowIndexHighHalf:
                return { low, unpackStarfieldMeshHalf(high) };
            case StarfieldWeightLayout::HighIndexLowHalf:
                return { high, unpackStarfieldMeshHalf(low) };
            case StarfieldWeightLayout::LowIndexHighUnorm:
                return { low, static_cast<float>(high) / 65535.f };
            case StarfieldWeightLayout::HighIndexLowUnorm:
                return { high, static_cast<float>(low) / 65535.f };
        }
        return { 0, 0.f };
    }

    StarfieldWeightLayout chooseStarfieldWeightLayout(
        const StarfieldExternalMeshData& mesh, std::size_t boneCount)
    {
        constexpr std::array<StarfieldWeightLayout, 4> layouts = {
            StarfieldWeightLayout::LowIndexHighHalf,
            StarfieldWeightLayout::HighIndexLowHalf,
            StarfieldWeightLayout::LowIndexHighUnorm,
            StarfieldWeightLayout::HighIndexLowUnorm,
        };

        StarfieldWeightLayout best = layouts.front();
        double bestScore = -std::numeric_limits<double>::infinity();
        for (StarfieldWeightLayout layout : layouts)
        {
            double score = 0.0;
            std::size_t offset = 0;
            const std::size_t sampleVertices = std::min<std::size_t>(mesh.mVertices.size(), 4096);
            for (std::size_t vertex = 0; vertex < sampleVertices; ++vertex)
            {
                float sum = 0.f;
                bool validVertex = true;
                for (std::uint32_t slot = 0; slot < mesh.mWeightCountPerVertex; ++slot)
                {
                    if (offset >= mesh.mWeights.size())
                    {
                        validVertex = false;
                        break;
                    }
                    const auto [bone, weight] = decodeStarfieldWeight(mesh.mWeights[offset++], layout);
                    if (bone >= boneCount || !std::isfinite(weight) || weight < 0.f || weight > 1.001f)
                    {
                        validVertex = false;
                        continue;
                    }
                    sum += weight;
                    score += weight > 0.f ? 2.0 : 0.1;
                }
                if (validVertex)
                    score += 8.0 - std::min(8.0, std::abs(static_cast<double>(sum) - 1.0) * 8.0);
                else
                    score -= 32.0;
            }
            if (score > bestScore)
            {
                bestScore = score;
                best = layout;
            }
        }
        return best;
    }

    std::string getStarfieldActorProofTexturePath(std::string_view ddsPath)
    {
        std::string path(ddsPath);
        if (!worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_STARFIELD_ACTOR_PNG_TEXTURES"))
            return path;

        const std::string lowered = Misc::StringUtils::lowerCase(path);
        if (lowered.size() >= 4 && lowered.compare(lowered.size() - 4, 4, ".dds") == 0)
            path.replace(path.size() - 4, 4, ".png");
        return path;
    }

    bool isStarfieldActorProofCutout(std::string_view filename)
    {
        const std::string path = Misc::StringUtils::lowerCase(filename);
        return path.find("actors/human/mesh/hairs/") != std::string::npos
            || path.find("actors/human/mesh/beards/") != std::string::npos
            || path.find("actors/human/hair/") != std::string::npos
            || path.find("actors/human/faces/beards/") != std::string::npos
            || path.find("actors/human/eyebrows/") != std::string::npos
            || path.find("actors/human/eyelashes/") != std::string::npos
            || path.find("actors/human/faces/eye_tears") != std::string::npos
            || path.find("human_male_hair_") != std::string::npos
            || path.find("human_female_hair_") != std::string::npos
            || path.find("human_male_beard_") != std::string::npos
            || path.find("human_female_beard_") != std::string::npos
            || path.find("human_male_eyebrow") != std::string::npos
            || path.find("human_female_eyebrow") != std::string::npos
            || path.find("human_male_eyelashes") != std::string::npos
            || path.find("human_female_eyelashes") != std::string::npos
            || path.find("human_male_eye_tears") != std::string::npos
            || path.find("human_female_eye_tears") != std::string::npos
            || path.find("actors/human/characterassets/male/eyebrow") != std::string::npos
            || path.find("actors/human/characterassets/female/eyebrow") != std::string::npos
            || path.find("actors/human/characterassets/male/eyelashes") != std::string::npos
            || path.find("actors/human/characterassets/female/eyelashes") != std::string::npos
            || path.find("actors/human/characterassets/male/eyes_tears") != std::string::npos
            || path.find("actors/human/characterassets/female/eyes_tears") != std::string::npos;
    }

    bool isStarfieldActorProofEyeSurface(std::string_view filename)
    {
        const std::string path = Misc::StringUtils::lowerCase(filename);
        return path.find("actors/human/characterassets/male/lefteye") != std::string::npos
            || path.find("actors/human/characterassets/male/righteye") != std::string::npos
            || path.find("actors/human/characterassets/female/lefteye") != std::string::npos
            || path.find("actors/human/characterassets/female/righteye") != std::string::npos
            || path.find("actors/human/faces/left_eye.mat") != std::string::npos
            || path.find("actors/human/faces/right_eye.mat") != std::string::npos
            || path.find("human_male_lefteye") != std::string::npos
            || path.find("human_male_righteye") != std::string::npos
            || path.find("human_female_lefteye") != std::string::npos
            || path.find("human_female_righteye") != std::string::npos;
    }

    osg::Vec4f getStarfieldActorProofBaseColor(std::string_view filename)
    {
        const std::string path = Misc::StringUtils::lowerCase(filename);
        if (isStarfieldActorProofEyeSurface(path))
            return osg::Vec4f(0.09f, 0.065f, 0.035f, 1.f);
        if (path.find("actors/human/characterassets/male/eyelashes") != std::string::npos
            || path.find("actors/human/characterassets/female/eyelashes") != std::string::npos)
            return osg::Vec4f(0.055f, 0.04f, 0.025f, 1.f);
        if (path.find("actors/human/characterassets/male/eyes_tears") != std::string::npos
            || path.find("actors/human/characterassets/female/eyes_tears") != std::string::npos
            || path.find("actors/human/faces/eye_tears") != std::string::npos
            || path.find("_eye_tears") != std::string::npos)
            return osg::Vec4f(0.9f, 0.95f, 1.f, 0.08f);
        if (path.find("_tongue") != std::string::npos
            || path.find("actors/human/faces/teeth/mouth.mat") != std::string::npos)
            return osg::Vec4f(0.48f, 0.13f, 0.11f, 1.f);
        if (path.find("actors/human/characterassets/male/teeth") != std::string::npos
            || path.find("actors/human/characterassets/female/teeth") != std::string::npos
            || path.find("actors/human/faces/teeth/nnteeth.mat") != std::string::npos
            || path.find("_teeth") != std::string::npos)
            return osg::Vec4f(0.88f, 0.82f, 0.68f, 1.f);
        if (path.find("actors/human/characterassets/male/tongue") != std::string::npos
            || path.find("actors/human/characterassets/female/tongue") != std::string::npos)
            return osg::Vec4f(0.48f, 0.13f, 0.11f, 1.f);
        return osg::Vec4f(0.84f, 0.86f, 0.82f, 1.f);
    }

    std::string getStarfieldActorProofDiffuse(std::string_view filename)
    {
        const std::string path = Misc::StringUtils::lowerCase(filename);
        if (path.find("actors/human/characterassets/male/malehead.nif") != std::string::npos
            || path.find("actors/human/faces/male_default.mat") != std::string::npos
            || path.find("human_male_head") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/chargen/male_default_sk3_color.dds");
        if (path.find("actors/human/characterassets/female/femalehead.nif") != std::string::npos
            || path.find("actors/human/faces/female_default.mat") != std::string::npos
            || path.find("human_female_head") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/chargen/female_default_sk3_color.dds");
        if (path.find("actors/human/mesh/naked_body/naked_m.nif") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/naked_body/nakedbodym_sk3_color.dds");
        if (path.find("actors/human/mesh/naked_body/naked_f.nif") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/naked_body/nakedbodyf_sk3_color.dds");
        if (path.find("actors/human/mesh/nakedhands/") != std::string::npos)
        {
            if (path.find("_f.") != std::string::npos || path.find("_f_") != std::string::npos
                || path.find("hands_3rd_f") != std::string::npos)
                return getStarfieldActorProofTexturePath("textures/actors/human/hands/defaulthandsf_sk3_color.dds");
            return getStarfieldActorProofTexturePath("textures/actors/human/hands/defaulthandsm_sk3_color.dds");
        }
        if (path.find("actors/human/mesh/beards/") != std::string::npos
            || path.find("actors/human/faces/beards/") != std::string::npos
            || path.find("human_male_beard_") != std::string::npos
            || path.find("human_female_beard_") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/beards/beard_shared_brown_color.dds");
        if (path.find("actors/human/mesh/hairs/faded_afro/") != std::string::npos
            || path.find("actors/human/hair/afro_hair") != std::string::npos
            || path.find("hair_faded_afro") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/hair/afro_hair_shared_brown_color.dds");
        if (path.find("actors/human/mesh/hairs/") != std::string::npos
            || path.find("actors/human/hair/") != std::string::npos
            || path.find("human_male_hair_") != std::string::npos
            || path.find("human_female_hair_") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/hair/short_hair_shared_brown_color.dds");
        if (path.find("actors/human/characterassets/male/eyebrow") != std::string::npos
            || path.find("actors/human/eyebrows/male_") != std::string::npos
            || path.find("human_male_eyebrow") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/eyebrows/eyebrows_fluffy_brown_color.dds");
        if (path.find("actors/human/characterassets/female/eyebrow") != std::string::npos
            || path.find("actors/human/eyebrows/female_") != std::string::npos
            || path.find("human_female_eyebrow") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/eyebrows/femaleeyebrows01_color.dds");
        if (path.find("actors/human/characterassets/male/eyelashes") != std::string::npos
            || path.find("actors/human/eyelashes/male_") != std::string::npos
            || path.find("human_male_eyelashes") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/eyelashes/malelashes01_color.dds");
        if (path.find("actors/human/characterassets/female/eyelashes") != std::string::npos
            || path.find("actors/human/eyelashes/female_") != std::string::npos
            || path.find("human_female_eyelashes") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/eyelashes/femalelashes01_color.dds");
        if (path.find("actors/human/characterassets/male/eyes_tears") != std::string::npos
            || path.find("actors/human/characterassets/female/eyes_tears") != std::string::npos
            || path.find("actors/human/faces/eye_tears") != std::string::npos
            || path.find("_eye_tears") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/eyes/eye_tear_color.dds");
        if (path.find("actors/human/characterassets/male/teeth") != std::string::npos
            || path.find("actors/human/characterassets/female/teeth") != std::string::npos
            || path.find("actors/human/faces/teeth/nnteeth.mat") != std::string::npos
            || path.find("_teeth") != std::string::npos)
            return getStarfieldActorProofTexturePath("textures/actors/human/faces/teeth/nnteeth_color.dds");
        if (path.find("clothes/outfit_miner_utilitysuit/") != std::string::npos)
        {
            // The installed archive really spells the texture directory "utililtysuit". The NIF material
            // contracts are Upperbody (shirt and bits), Sleeves, and LowerBody (pants).
            if (path.find("pants") != std::string::npos || path.find("lowerbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_miner_utililtysuit/outfit_miner_utilitysuit_m/outfit_miner_utilitysuit_pants_m_color.dds");
            if (path.find("sleeves") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_miner_utililtysuit/outfit_miner_utilitysuit_m/outfit_miner_utilitysuit_sleeves_lod0_m_color.dds");
            if (path.find("shirt") != std::string::npos || path.find("bits") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_miner_utililtysuit/outfit_miner_utilitysuit_m/outfit_miner_utilitysuit_shirt_materials_color.dds");
        }
        if (path.find("clothes/outfit_service_uniform_01/") != std::string::npos)
        {
            if (path.find("lowerbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_service_uniform_01/outfit_service_uniform_lowerbody_01_color.dds");
            if (path.find("sleeves") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_service_uniform_01/outfit_service_uniform_sleeves_01_color.dds");
            if (path.find("upperbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_service_uniform_01/outfit_service_uniform_upperbody_01_color.dds");
        }
        if (path.find("clothes/outfit_employee_uniform_formal_01/") != std::string::npos)
        {
            if (path.find("lowerbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_employee_uniform_formal_01/outfit_employee_uniform_formal_lowerbody_01_color.dds");
            if (path.find("sleeves") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_employee_uniform_formal_01/outfit_employee_uniform_formal_sleeves_01_color.dds");
            if (path.find("upperbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_employee_uniform_formal_01/outfit_employee_uniform_formal_upperbody_01_color.dds");
        }
        if (path.find("clothes/outfit_ucpolice/") != std::string::npos)
        {
            if (path.find("helmet") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_ucpolice/outfit_ucsecurity_helmet_mat_color.dds");
            if (path.find("visor") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_ucpolice/outfit_ucsecurity_visor_mat_color.dds");
            if (path.find("arms") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_ucpolice/outfit_ucsecurity_arms_mat_color.dds");
            if (path.find("torso") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_ucpolice/outfit_ucsecurity_torso_mat_color.dds");
            if (path.find("legsandacc") != std::string::npos || path.find("lowerbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_ucpolice/outfit_ucsecurity_legsandacc_mat_color.dds");
            return getStarfieldActorProofTexturePath(
                "textures/clothes/outfit_ucpolice/outfit_ucsecurity_legsandacc_mat_color.dds");
        }
        if (path.find("clothes/spacesuit_flightcap_01/") != std::string::npos)
            return getStarfieldActorProofTexturePath(
                "textures/clothes/spacesuit_ecliptic/spacesuit_ecliptic_flightcap_color.dds");
        if (path.find("clothes/outfit_colonist_quarterpaddedvest_01/") != std::string::npos)
        {
            if (path.find("hat") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_colonist_quarterpaddedvest_01/outfit_colonist_quarterpaddedvest_01_hat_color.dds");
            if (path.find("sleeves") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_colonist_quarterpaddedvest_01/outfit_colonist_quarterpaddedvest_01_sleeves_color.dds");
            if (path.find("pants") != std::string::npos || path.find("lowerbody") != std::string::npos)
            {
                if (path.find("_f.") != std::string::npos || path.find("_f_") != std::string::npos)
                    return getStarfieldActorProofTexturePath(
                        "textures/clothes/outfit_colonist_quarterpaddedvest_01/outfit_colonist_quarterpaddedvest_01_f/outfit_colonist_quarterpaddedvest_01_lowerbody_f_color.dds");
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_colonist_quarterpaddedvest_01/outfit_colonist_quarterpaddedvest_01_m/outfit_colonist_quarterpaddedvest_01_lowerbody_m_color.dds");
            }
            if (path.find("upperbody") != std::string::npos || path.find("quarterpaddedvest_01_m.nif") != std::string::npos
                || path.find("quarterpaddedvest_01_f.nif") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_colonist_quarterpaddedvest_01/outfit_colonist_quarterpaddedvest_01_upperbody_color.dds");
        }
        if (path.find("clothes/outfit_utilityoveralls_01/") != std::string::npos)
        {
            if (path.find("sso_hat") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/headwear_ssohat_01_color.dds");
            if (path.find("sso_jacket_01_sleeves") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/outfit_utilityoveralls_sso_jacket_sleeves_01_color.dds");
            if (path.find("sso_jacket_01_upperbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/outfit_utilityoveralls_sso_jacket_upperbody_01_color.dds");
            if (path.find("sso_jacket_cooling_01_upperbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/outfit_utilityoveralls_sso_jacket_cooling_upperbody_01_color.dds");
            if (path.find("sleeves") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/outfit_utilityoveralls_mechanic_sleeves_01_color.dds");
            if (path.find("lowerbody") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/outfit_utilityoveralls_mechanic_lowerbody_01_color.dds");
            if (path.find("upperbody") != std::string::npos || path.find("tanktop") != std::string::npos)
                return getStarfieldActorProofTexturePath(
                    "textures/clothes/outfit_utilityoveralls_01/outfit_utilityoveralls_mechanic_upperbody_01_color.dds");
        }
        return {};
    }

    std::string getStarfieldShaderMaterialName(const Nif::BSTriShape* bsTriShape)
    {
        if (bsTriShape == nullptr || bsTriShape->mShaderProperty.empty())
            return {};

        const Nif::BSShaderProperty* shader = bsTriShape->mShaderProperty.getPtr();
        if (shader == nullptr)
            return {};

        return shader->mName;
    }

    struct StarfieldMaterialBridgeEntry
    {
        std::string mDiffuse;
        std::string mEvidence;
    };

    std::string normalizeStarfieldMaterialBridgePath(std::string_view value)
    {
        std::string normalized(value);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        normalized = Misc::StringUtils::lowerCase(normalized);
        while (normalized.starts_with("data/"))
            normalized.erase(0, 5);
        return normalized;
    }

    const std::unordered_map<std::string, StarfieldMaterialBridgeEntry>& getStarfieldMaterialBridge()
    {
        static const std::unordered_map<std::string, StarfieldMaterialBridgeEntry> bridge = [] {
            std::unordered_map<std::string, StarfieldMaterialBridgeEntry> result;
            const char* path = std::getenv("OPENMW_WORLD_VIEWER_STARFIELD_MATERIAL_MAP");
            if (path == nullptr || *path == '\0')
                return result;

            std::ifstream stream(path);
            if (!stream)
            {
                Log(Debug::Warning) << "World viewer: Starfield material bridge unavailable path=\"" << path << "\"";
                return result;
            }

            std::string line;
            while (std::getline(stream, line))
            {
                if (line.empty() || line.front() == '#')
       â€¦69326 tokens truncatedâ€¦arams", bgem->mFalloffParams));
            }

            if (material->mTwoSided)
                stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            handleDepthFlags(stateset, material->mDepthTest, material->mDepthWrite);
        }

        void handleDecal(bool enabled, bool hasSortAlpha, osg::Node& node) const
        {
            if (!enabled)
                return;
            osg::ref_ptr<osg::StateSet> stateset = node.getOrCreateStateSet();
            osg::ref_ptr<osg::PolygonOffset> polygonOffset(new osg::PolygonOffset);
            polygonOffset->setUnits(SceneUtil::AutoDepth::isReversed() ? 1.f : -1.f);
            polygonOffset->setFactor(SceneUtil::AutoDepth::isReversed() ? 0.65f : -0.65f);
            polygonOffset = shareAttribute(polygonOffset);
            stateset->setAttributeAndModes(polygonOffset, osg::StateAttribute::ON);
            if (!mPushedSorter && !hasSortAlpha)
                stateset->setRenderBinDetails(1, "SORT_BACK_TO_FRONT");
        }

        static void handleAlphaTesting(
            bool enabled, osg::AlphaFunc::ComparisonFunction function, int threshold, osg::Node& node)
        {
            if (enabled)
            {
                osg::ref_ptr<osg::AlphaFunc> alphaFunc(new osg::AlphaFunc(function, threshold / 255.f));
                alphaFunc = shareAttribute(alphaFunc);
                node.getOrCreateStateSet()->setAttributeAndModes(alphaFunc, osg::StateAttribute::ON);
            }
            else if (osg::StateSet* stateset = node.getStateSet())
            {
                stateset->removeAttribute(osg::StateAttribute::ALPHAFUNC);
                stateset->removeMode(GL_ALPHA_TEST);
            }
        }

        void handleAlphaBlending(bool enabled, int sourceMode, int destMode, bool sort, bool& hasSortAlpha,
            osg::Node& node, bool protectNonstandardBlend = false) const
        {
            if (enabled)
            {
                osg::ref_ptr<osg::StateSet> stateset = node.getOrCreateStateSet();
                osg::ref_ptr<osg::BlendFunc> blendFunc(
                    new osg::BlendFunc(getBlendMode(sourceMode), getBlendMode(destMode)));
                // on AMD hardware, alpha still seems to be stored with an RGBA framebuffer with OpenGL.
                // This might be mandated by the OpenGL 2.1 specification section 2.14.9, or might be a bug.
                // Either way, D3D8.1 doesn't do that, so adapt the destination factor.
                if (blendFunc->getDestination() == GL_DST_ALPHA)
                    blendFunc->setDestination(GL_ONE);
                blendFunc = shareAttribute(blendFunc);
                osg::StateAttribute::GLModeValue blendMode = osg::StateAttribute::ON;
                if (protectNonstandardBlend
                    && (sourceMode != 6 || destMode != 7)) // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
                {
                    // A NIF NiAlphaProperty is authored at drawable scope. Actor roots can carry a standard-alpha
                    // OVERRIDE for whole-actor fading; without PROTECTED that unrelated ancestor silently replaces
                    // additive and multiplicative child composition. Preserve nonstandard local blend contracts.
                    blendMode |= osg::StateAttribute::PROTECTED;
                }
                stateset->setAttributeAndModes(blendFunc, blendMode);

                if (sort)
                {
                    hasSortAlpha = true;
                    if (!mPushedSorter)
                        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                }
                else if (!mPushedSorter)
                {
                    stateset->setRenderBinToInherit();
                }
            }
            else if (osg::ref_ptr<osg::StateSet> stateset = node.getStateSet())
            {
                stateset->removeAttribute(osg::StateAttribute::BLENDFUNC);
                stateset->removeMode(GL_BLEND);
                if (!mPushedSorter)
                    stateset->setRenderBinToInherit();
            }
        }

        void handleShaderMaterialDrawableProperties(const Bgsm::MaterialFile* shaderMat,
            osg::ref_ptr<osg::Material> mat, osg::Node& node, bool& hasSortAlpha) const
        {
            mat->setAlpha(osg::Material::FRONT_AND_BACK, shaderMat->mTransparency);
            handleAlphaTesting(shaderMat->mAlphaTest, osg::AlphaFunc::GREATER, shaderMat->mAlphaTestThreshold, node);
            handleAlphaBlending(shaderMat->mAlphaBlend, shaderMat->mSourceBlendMode, shaderMat->mDestinationBlendMode,
                true, hasSortAlpha, node);
            handleDecal(shaderMat->mDecal, hasSortAlpha, node);
            if (shaderMat->mShaderType == Bgsm::ShaderType::Lighting)
            {
                auto bgsm = static_cast<const Bgsm::BGSMFile*>(shaderMat);
                mat->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(bgsm->mEmittanceColor, 1.f));
                mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(bgsm->mSpecularColor, 1.f));
            }
            else if (shaderMat->mShaderType == Bgsm::ShaderType::Effect)
            {
                auto bgem = static_cast<const Bgsm::BGEMFile*>(shaderMat);
                mat->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(bgem->mEmittanceColor, 1.f));
                if (bgem->mSoft && Loader::getSoftEffectEnabled())
                    SceneUtil::setupSoftEffect(
                        node, { .mSize = bgem->mSoftDepth, .mFalloffDepth = bgem->mSoftDepth, .mFalloff = true });
            }
        }

        void handleTextureSet(const Nif::BSShaderTextureSet* textureSet, bool wrapS, bool wrapT, float envMapScale,
            const std::string& nodeName, osg::StateSet* stateset, std::vector<unsigned int>& boundTextures,
            bool skinShader) const
        {
            const unsigned int uvSet = 0;
            const bool worldViewerActorMesh
                = worldViewerMeshLoadTelemetryEnabled()
                && isWorldViewerActorMeshPath(Misc::StringUtils::lowerCase(mFilename.generic_string()));

            for (size_t i = 0; i < textureSet->mTextures.size(); ++i)
            {
                if (textureSet->mTextures[i].empty())
                    continue;
                switch (static_cast<Nif::BSShaderTextureSet::TextureType>(i))
                {
                    case Nif::BSShaderTextureSet::TextureType::Base:
                        attachExternalTexture(
                            "diffuseMap", textureSet->mTextures[i], wrapS, wrapT, uvSet, stateset, boundTextures);
                        break;
                    case Nif::BSShaderTextureSet::TextureType::Normal:
                        attachExternalTexture(
                            "normalMap", textureSet->mTextures[i], wrapS, wrapT, uvSet, stateset, boundTextures);
                        break;
                    case Nif::BSShaderTextureSet::TextureType::Glow:
                        if (skinShader && isSkinAuxTexture(textureSet->mTextures[i]))
                        {
                            attachExternalTexture("skinAuxMap", textureSet->mTextures[i], wrapS, wrapT, uvSet,
                                stateset, boundTextures);
                            if (worldViewerActorMesh)
                                Log(Debug::Info) << "World viewer texture ledger: file=\""
                                                 << mFilename.generic_string() << "\" role=\"skinAuxMap\""
                                                 << " path=\"" << textureSet->mTextures[i] << "\""
                                                 << " skippedAsEmissive=1";
                            break;
                        }
                        attachExternalTexture(
                            "emissiveMap", textureSet->mTextures[i], wrapS, wrapT, uvSet, stateset, boundTextures);
                        break;
                    case Nif::BSShaderTextureSet::TextureType::Environment:
                        attachExternalTexture(
                            "envMap", textureSet->mTextures[i], wrapS, wrapT, uvSet, stateset, boundTextures);
                        if (envMapScale <= 0.f)
                            envMapScale = 1.f;
                        stateset->addUniform(new osg::Uniform(
                            "envMapColor", osg::Vec4f(envMapScale, envMapScale, envMapScale, 1.f)));
                        break;
                    case Nif::BSShaderTextureSet::TextureType::EnvironmentMask:
                        attachExternalTexture(
                            "glossMap", textureSet->mTextures[i], wrapS, wrapT, uvSet, stateset, boundTextures);
                        break;
                    default:
                    {
                        Log(Debug::Info) << "Unhandled texture stage " << i << " on shape \"" << nodeName << "\" in "
                                         << mFilename;
                        continue;
                    }
                }
            }

            if (skinShader)
            {
                // FNV face textures are actor-instance inputs applied after the shared NIF template has already
                // passed through ShaderVisitor. Keep both retail FaceGen sampler branches compiled on the template
                // and bind mathematically neutral float texels until the NPC-specific textures replace them.
                // Without these typed slots the late textures exist in OSG state but SKIN2002 is compiled without
                // either sampling instruction, which is the pale/gold "layer held up but never applied" failure.
                attachTextureAtUnit(
                    "faceGenMap0", getNeutralFaceGenImage(false), 4, uvSet, stateset, boundTextures);
                attachTextureAtUnit(
                    "faceGenMap1", getNeutralFaceGenImage(true), 5, uvSet, stateset, boundTextures);
            }
        }

        std::string_view getBSShaderPrefix(unsigned int type) const
        {
            switch (static_cast<Nif::BSShaderType>(type))
            {
                case Nif::BSShaderType::ShaderType_Default:
                case Nif::BSShaderType::ShaderType_TallGrass:
                case Nif::BSShaderType::ShaderType_Sky:
                case Nif::BSShaderType::ShaderType_Water:
                case Nif::BSShaderType::ShaderType_Lighting30:
                case Nif::BSShaderType::ShaderType_Tile:
                    return "bs/default";
                case Nif::BSShaderType::ShaderType_Skin:
                    return "bs/skin";
                case Nif::BSShaderType::ShaderType_NoLighting:
                    return "bs/nolighting";
            }
            Log(Debug::Warning) << "Unknown BSShaderType " << type << " in " << mFilename;
            return "bs/default";
        }

        std::string_view getBSLightingShaderPrefix(unsigned int type) const
        {
            switch (static_cast<Nif::BSLightingShaderType>(type))
            {
                case Nif::BSLightingShaderType::ShaderType_Default:
                    return "bs/default";
                case Nif::BSLightingShaderType::ShaderType_EnvMap:
                case Nif::BSLightingShaderType::ShaderType_Glow:
                case Nif::BSLightingShaderType::ShaderType_Parallax:
                case Nif::BSLightingShaderType::ShaderType_FaceTint:
                case Nif::BSLightingShaderType::ShaderType_SkinTint:
                case Nif::BSLightingShaderType::ShaderType_HairTint:
                case Nif::BSLightingShaderType::ShaderType_ParallaxOcc:
                case Nif::BSLightingShaderType::ShaderType_MultitexLand:
                case Nif::BSLightingShaderType::ShaderType_LODLand:
                case Nif::BSLightingShaderType::ShaderType_Snow:
                case Nif::BSLightingShaderType::ShaderType_MultiLayerParallax:
                case Nif::BSLightingShaderType::ShaderType_TreeAnim:
                case Nif::BSLightingShaderType::ShaderType_LODObjects:
                case Nif::BSLightingShaderType::ShaderType_SparkleSnow:
                case Nif::BSLightingShaderType::ShaderType_LODObjectsHD:
                case Nif::BSLightingShaderType::ShaderType_EyeEnvmap:
                case Nif::BSLightingShaderType::ShaderType_Cloud:
                case Nif::BSLightingShaderType::ShaderType_LODNoise:
                case Nif::BSLightingShaderType::ShaderType_MultitexLandLODBlend:
                case Nif::BSLightingShaderType::ShaderType_Dismemberment:
                case Nif::BSLightingShaderType::ShaderType_Terrain:
                    Log(Debug::Warning) << "Unhandled BSLightingShaderType " << type << " in " << mFilename;
                    return "bs/default";
            }
            Log(Debug::Warning) << "Unknown BSLightingShaderType " << type << " in " << mFilename;
            return "bs/default";
        }

        void handleProperty(const Nif::NiProperty* property, osg::Node* node,
            SceneUtil::CompositeStateSetUpdater* composite, std::vector<unsigned int>& boundTextures, int animflags,
            bool hasStencilProperty)
        {
            switch (property->recType)
            {
                case Nif::RC_NiStencilProperty:
                {
                    const Nif::NiStencilProperty* stencilprop = static_cast<const Nif::NiStencilProperty*>(property);

                    osg::ref_ptr<osg::FrontFace> frontFace = new osg::FrontFace;
                    using DrawMode = Nif::NiStencilProperty::DrawMode;
                    switch (stencilprop->mDrawMode)
                    {
                        case DrawMode::Clockwise:
                            frontFace->setMode(osg::FrontFace::CLOCKWISE);
                            break;
                        case DrawMode::Default:
                        case DrawMode::CounterClockwise:
                        case DrawMode::Both:
                        default:
                            frontFace->setMode(osg::FrontFace::COUNTER_CLOCKWISE);
                            break;
                    }
                    frontFace = shareAttribute(frontFace);

                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    stateset->setAttribute(frontFace, osg::StateAttribute::ON);
                    if (stencilprop->mDrawMode == DrawMode::Both)
                        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                    else
                        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::ON);

                    if (stencilprop->mEnabled)
                    {
                        mHasStencilProperty = true;
                        osg::ref_ptr<osg::Stencil> stencil = new osg::Stencil;
                        stencil->setFunction(getStencilFunction(stencilprop->mTestFunction), stencilprop->mStencilRef,
                            stencilprop->mStencilMask);
                        stencil->setStencilFailOperation(getStencilOperation(stencilprop->mFailAction));
                        stencil->setStencilPassAndDepthFailOperation(getStencilOperation(stencilprop->mZFailAction));
                        stencil->setStencilPassAndDepthPassOperation(getStencilOperation(stencilprop->mPassAction));
                        stencil = shareAttribute(stencil);

                        stateset->setAttributeAndModes(stencil, osg::StateAttribute::ON);
                    }
                    break;
                }
                case Nif::RC_NiWireframeProperty:
                {
                    const Nif::NiWireframeProperty* wireprop = static_cast<const Nif::NiWireframeProperty*>(property);
                    osg::ref_ptr<osg::PolygonMode> mode = new osg::PolygonMode;
                    mode->setMode(osg::PolygonMode::FRONT_AND_BACK,
                        wireprop->mEnable ? osg::PolygonMode::LINE : osg::PolygonMode::FILL);
                    mode = shareAttribute(mode);
                    node->getOrCreateStateSet()->setAttributeAndModes(mode, osg::StateAttribute::ON);
                    break;
                }
                case Nif::RC_NiZBufferProperty:
                {
                    const Nif::NiZBufferProperty* zprop = static_cast<const Nif::NiZBufferProperty*>(property);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    // The test function from this property seems to be ignored.
                    handleDepthFlags(stateset, zprop->depthTest(), zprop->depthWrite());
                    break;
                }
                // OSG groups the material properties that NIFs have separate, so we have to parse them all again when
                // one changed
                case Nif::RC_NiMaterialProperty:
                case Nif::RC_NiVertexColorProperty:
                case Nif::RC_NiSpecularProperty:
                {
                    // Handled on drawable level so we know whether vertex colors are available
                    break;
                }
                case Nif::RC_NiAlphaProperty:
                {
                    // Handled on drawable level to prevent RenderBin nesting issues
                    break;
                }
                case Nif::RC_NiTexturingProperty:
                {
                    const Nif::NiTexturingProperty* texprop = static_cast<const Nif::NiTexturingProperty*>(property);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    handleTextureProperty(texprop, node->getName(), stateset, composite, boundTextures, animflags);
                    node->setUserValue("applyMode", static_cast<int>(texprop->mApplyMode));
                    break;
                }
                case Nif::RC_BSShaderPPLightingProperty:
                {
                    auto texprop = static_cast<const Nif::BSShaderPPLightingProperty*>(property);
                    bool shaderRequired = true;
                    node->setUserValue("shaderPrefix", std::string(getBSShaderPrefix(texprop->mType)));
                    node->setUserValue("shaderRequired", shaderRequired);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    clearBoundTextures(stateset, boundTextures);
                    if (!texprop->mTextureSet.empty())
                        handleTextureSet(texprop->mTextureSet.getPtr(), texprop->wrapS(), texprop->wrapT(),
                            texprop->mEnvMapScale,
                            node->getName(), stateset, boundTextures,
                            texprop->mType == static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Skin));
                    handleTextureControllers(texprop, composite, stateset, animflags);
                    // BSShaderPPLightingProperty carries the same authored depth-test/depth-write bits as the other
                    // Bethesda shader properties. Omitting them makes transparent overlays write depth by default,
                    // so a coplanar glare/static pass can reject the opaque surface it is meant to decorate.
                    handleDepthFlags(stateset, texprop->depthTest(), texprop->depthWrite());
                    if (texprop->refraction())
                        SceneUtil::setupDistortion(*node, { .mStrength = texprop->mRefraction.mStrength });
                    break;
                }
                case Nif::RC_BSShaderNoLightingProperty:
                {
                    auto texprop = static_cast<const Nif::BSShaderNoLightingProperty*>(property);
                    bool shaderRequired = true;
                    bool useFalloff = false;
                    node->setUserValue("shaderPrefix", std::string(getBSShaderPrefix(texprop->mType)));
                    node->setUserValue("shaderRequired", shaderRequired);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    clearBoundTextures(stateset, boundTextures);
                    if (!texprop->mFilename.empty())
                    {
                        const unsigned int uvSet = 0;
                        attachExternalTexture("diffuseMap", texprop->mFilename, texprop->wrapS(), texprop->wrapT(),
                            uvSet, stateset, boundTextures);
                    }
                    if (mBethVersion >= 27)
                    {
                        useFalloff = true;
                        stateset->addUniform(new osg::Uniform("falloffParams", texprop->mFalloffParams));
                    }
                    stateset->addUniform(new osg::Uniform("useFalloff", useFalloff));
                    handleTextureControllers(texprop, composite, stateset, animflags);
                    handleDepthFlags(stateset, texprop->depthTest(), texprop->depthWrite());
                    break;
                }
                case Nif::RC_SkyShaderProperty:
                {
                    if (!enableExperimentalSkyShaderProperties())
                        break;

                    auto texprop = static_cast<const Nif::SkyShaderProperty*>(property);
                    node->setUserValue("shaderPrefix", std::string("bs/nolighting"));
                    node->setUserValue("shaderRequired", true);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    clearBoundTextures(stateset, boundTextures);
                    if (!texprop->mFilename.empty())
                    {
                        const unsigned int uvSet = 0;
                        const std::string texture = remapFalloutSkyTexture(texprop->mFilename);
                        attachExternalTexture("diffuseMap", texture, texprop->wrapS(), texprop->wrapT(), uvSet,
                            stateset, boundTextures);
                    }
                    handleTextureControllers(texprop, composite, stateset, animflags);
                    stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
                    osg::ref_ptr<osg::BlendFunc> blendFunc(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                    blendFunc = shareAttribute(blendFunc);
                    stateset->setAttributeAndModes(blendFunc, osg::StateAttribute::ON);
                    stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                    break;
                }
                case Nif::RC_BSSkyShaderProperty:
                {
                    if (!enableExperimentalSkyShaderProperties())
                        break;

                    auto texprop = static_cast<const Nif::BSSkyShaderProperty*>(property);
                    node->setUserValue("shaderPrefix", std::string("bs/nolighting"));
                    node->setUserValue("shaderRequired", true);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    clearBoundTextures(stateset, boundTextures);
                    if (!texprop->mFilename.empty())
                    {
                        const unsigned int uvSet = 0;
                        const std::string texture = remapFalloutSkyTexture(texprop->mFilename);
                        attachExternalTexture("diffuseMap", texture, true, true, uvSet, stateset, boundTextures);
                    }
                    handleTextureControllers(texprop, composite, stateset, animflags);
                    stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
                    osg::ref_ptr<osg::BlendFunc> blendFunc(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                    blendFunc = shareAttribute(blendFunc);
                    stateset->setAttributeAndModes(blendFunc, osg::StateAttribute::ON);
                    stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                    break;
                }
                case Nif::RC_BSLightingShaderProperty:
                {
                    auto texprop = static_cast<const Nif::BSLightingShaderProperty*>(property);
                    bool shaderRequired = true;
                    node->setUserValue("shaderPrefix", std::string(getBSLightingShaderPrefix(texprop->mType)));
                    node->setUserValue("shaderRequired", shaderRequired);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    clearBoundTextures(stateset, boundTextures);
                    if (Bgsm::MaterialFilePtr material = getShaderMaterial(texprop->mName, mMaterialManager))
                    {
                        handleShaderMaterialNodeProperties(material.get(), stateset, boundTextures);
                        break;
                    }
                    if (!texprop->mTextureSet.empty())
                        handleTextureSet(texprop->mTextureSet.getPtr(), texprop->wrapS(), texprop->wrapT(),
                            texprop->mEnvMapScale,
                            node->getName(), stateset, boundTextures, false);
                    handleTextureControllers(texprop, composite, stateset, animflags);
                    if (texprop->doubleSided())
                        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                    if (texprop->treeAnim())
                        stateset->addUniform(new osg::Uniform("useTreeAnim", true));
                    handleDepthFlags(stateset, texprop->depthTest(), texprop->depthWrite());
                    if (texprop->refraction())
                        SceneUtil::setupDistortion(*node, { .mStrength = texprop->mRefractionStrength });
                    break;
                }
                case Nif::RC_BSEffectShaderProperty:
                {
                    auto texprop = static_cast<const Nif::BSEffectShaderProperty*>(property);
                    bool shaderRequired = true;
                    // TODO: implement BSEffectShader as a shader
                    node->setUserValue("shaderPrefix", std::string("bs/nolighting"));
                    node->setUserValue("shaderRequired", shaderRequired);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    clearBoundTextures(stateset, boundTextures);
                    if (Bgsm::MaterialFilePtr material = getShaderMaterial(texprop->mName, mMaterialManager))
                    {
                        handleShaderMaterialNodeProperties(material.get(), stateset, boundTextures);
                        break;
                    }
                    if (!texprop->mSourceTexture.empty())
                    {
                        const unsigned int uvSet = 0;
                        unsigned int texUnit = boundTextures.size();
                        attachExternalTexture("diffuseMap", texprop->mSourceTexture, texprop->wrapS(), texprop->wrapT(),
                            uvSet, stateset, boundTextures);
                        {
                            osg::ref_ptr<osg::TexMat> texMat(new osg::TexMat);
                            // This handles 20.2.0.7 UV settings like 4.0.0.2 UV settings (see NifOsg::UVController)
                            // TODO: verify
                            osg::Vec3f uvOrigin(0.5f, 0.5f, 0.f);
                            osg::Vec3f uvScale(texprop->mUVScale.x(), texprop->mUVScale.y(), 1.f);
                            osg::Vec3f uvTrans(-texprop->mUVOffset.x(), -texprop->mUVOffset.y(), 0.f);

                            osg::Matrixf mat = osg::Matrixf::translate(uvOrigin);
                            mat.preMultScale(uvScale);
                            mat.preMultTranslate(-uvOrigin);
                            mat.setTrans(mat.getTrans() + uvTrans);

                            texMat->setMatrix(mat);
                            stateset->setTextureAttributeAndModes(texUnit, texMat, osg::StateAttribute::ON);
                        }
                    }
                    bool useFalloff = texprop->useFalloff();
                    stateset->addUniform(new osg::Uniform("useFalloff", useFalloff));
                    if (useFalloff)
                        stateset->addUniform(new osg::Uniform("falloffParams", texprop->mFalloffParams));
                    handleTextureControllers(texprop, composite, stateset, animflags);
                    if (texprop->doubleSided())
                        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                    handleDepthFlags(stateset, texprop->depthTest(), texprop->depthWrite());
                    break;
                }
                case Nif::RC_NiFogProperty:
                {
                    const Nif::NiFogProperty* fogprop = static_cast<const Nif::NiFogProperty*>(property);
                    osg::StateSet* stateset = node->getOrCreateStateSet();
                    // Vertex alpha mode appears to be broken
                    if (!fogprop->vertexAlpha() && fogprop->enabled())
                    {
                        osg::ref_ptr<NifOsg::Fog> fog = new NifOsg::Fog;
                        fog->setMode(osg::Fog::LINEAR);
                        fog->setColor(osg::Vec4f(fogprop->mColour, 1.f));
                        fog->setDepth(fogprop->mFogDepth);
                        fog = shareAttribute(fog);
                        stateset->setAttributeAndModes(fog, osg::StateAttribute::ON);
                        // Intentionally ignoring radial fog flag
                        // We don't really want to override the global setting
                    }
                    else
                    {
                        osg::ref_ptr<osg::Fog> fog = new osg::Fog;
                        // Shaders don't respect glDisable(GL_FOG)
                        fog->setMode(osg::Fog::LINEAR);
                        fog->setStart(10000000);
                        fog->setEnd(10000000);
                        fog = shareAttribute(fog);
                        stateset->setAttributeAndModes(fog, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                    }
                    break;
                }
                // unused by mw
                case Nif::RC_NiShadeProperty:
                case Nif::RC_NiDitherProperty:
                {
                    break;
                }
                default:
                    Log(Debug::Info) << "Unhandled " << property->recName << " in " << mFilename;
                    break;
            }
        }

        struct CompareStateAttribute
        {
            bool operator()(
                const osg::ref_ptr<osg::StateAttribute>& left, const osg::ref_ptr<osg::StateAttribute>& right) const
            {
                return left->compare(*right) < 0;
            }
        };

        // global sharing of State Attributes will reduce the number of GL calls as the osg::State will check by pointer
        // to see if state is the same
        template <class Attribute>
        static Attribute* shareAttribute(const osg::ref_ptr<Attribute>& attr)
        {
            using Cache = std::set<osg::ref_ptr<Attribute>, CompareStateAttribute>;
            static Cache sCache;
            static std::mutex sMutex;
            std::lock_guard<std::mutex> lock(sMutex);
            typename Cache::iterator found = sCache.find(attr);
            if (found == sCache.end())
                found = sCache.insert(attr).first;
            return *found;
        }

        void applyDrawableProperties(osg::Node* node, const std::vector<const Nif::NiProperty*>& properties,
            SceneUtil::CompositeStateSetUpdater* composite, bool hasVertexColors, int animflags,
            const std::vector<unsigned int>* boundTextures = nullptr)
        {
            // Specular lighting is enabled by default, but there's a quirk...
            bool specEnabled = true;
            osg::ref_ptr<osg::Material> mat(new osg::Material);
            mat->setColorMode(hasVertexColors ? osg::Material::AMBIENT_AND_DIFFUSE : osg::Material::OFF);

            // NIF material defaults don't match OpenGL defaults
            mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
            mat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));

            bool hasMatCtrl = false;
            bool hasSortAlpha = false;

            auto setBinBackToFront = [](osg::StateSet* ss) { ss->setRenderBinDetails(0, "SORT_BACK_TO_FRONT"); };
            auto setBinTraversal = [](osg::StateSet* ss) { ss->setRenderBinDetails(2, "TraversalOrderBin"); };

            auto lightmode = Nif::NiVertexColorProperty::LightMode::LightMode_EmiAmbDif;
            float emissiveMult = 1.f;
            float specStrength = 1.f;
            int niMaterialProperties = 0;
            int niVertexColorProperties = 0;
            int niAlphaProperties = 0;
            int bsPPLightingProperties = 0;
            int bsLightingProperties = 0;
            int bsEffectProperties = 0;
            int bsLightingType = -1;
            int bsShaderType = -1;
            bool hasNoLightingShader = false;
            bool falloutVertexAlphaOnly = false;
            std::string shaderMaterialName;
            int shaderMaterialType = -1;

            for (const Nif::NiProperty* property : properties)
            {
                switch (property->recType)
                {
                    case Nif::RC_NiSpecularProperty:
                    {
                        // Specular property can turn specular lighting off.
                        // FIXME: NiMaterialColorController doesn't care about this.
                        auto specprop = static_cast<const Nif::NiSpecularProperty*>(property);
                        specEnabled = specprop->mEnable;
                        break;
                    }
                    case Nif::RC_NiMaterialProperty:
                    {
                        ++niMaterialProperties;
                        const Nif::NiMaterialProperty* matprop = static_cast<const Nif::NiMaterialProperty*>(property);

                        mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(matprop->mDiffuse, matprop->mAlpha));
                        mat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(matprop->mAmbient, 1.f));
                        mat->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(matprop->mEmissive, 1.f));
                        emissiveMult = matprop->mEmissiveMult;

                        mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(matprop->mSpecular, 1.f));
                        // NIFs may provide specular exponents way above OpenGL's limit.
                        // They can't be used properly, but we don't need OSG to constantly harass us about it.
                        float glossiness = std::clamp(matprop->mGlossiness, 0.f, 128.f);
                        mat->setShininess(osg::Material::FRONT_AND_BACK, glossiness);

                        if (!matprop->mController.empty())
                        {
                            hasMatCtrl = true;
                            handleMaterialControllers(matprop, composite, animflags, mat);
                        }

                        break;
                    }
                    case Nif::RC_NiVertexColorProperty:
                    {
                        ++niVertexColorProperties;
                        const Nif::NiVertexColorProperty* vertprop
                            = static_cast<const Nif::NiVertexColorProperty*>(property);

                        using VertexMode = Nif::NiVertexColorProperty::VertexMode;
                        switch (vertprop->mVertexMode)
                        {
                            case VertexMode::VertMode_SrcIgnore:
                            {
                                mat->setColorMode(osg::Material::OFF);
                                break;
                            }
                            case VertexMode::VertMode_SrcEmissive:
                            {
                                mat->setColorMode(osg::Material::EMISSION);
                                break;
                            }
                            case VertexMode::VertMode_SrcAmbDif:
                            {
                                lightmode = vertprop->mLightingMode;
                                using LightMode = Nif::NiVertexColorProperty::LightMode;
                                switch (lightmode)
                                {
                                    case LightMode::LightMode_Emissive:
                                    {
                                        mat->setColorMode(osg::Material::OFF);
                                        break;
                                    }
                                    case LightMode::LightMode_EmiAmbDif:
                                    default:
                                    {
                                        mat->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
                                        break;
                                    }
                                }
                                break;
                            }
                        }

                        break;
                    }
                    case Nif::RC_NiAlphaProperty:
                    {
                        ++niAlphaProperties;
                        const Nif::NiAlphaProperty* alphaprop = static_cast<const Nif::NiAlphaProperty*>(property);
                        hasAlphaTestWithoutBlending = hasAlphaTestWithoutBlending
                            || (alphaprop->useAlphaTesting() && !alphaprop->useAlphaBlending());
                        handleAlphaBlending(alphaprop->useAlphaBlending(), alphaprop->sourceBlendMode(),
                            alphaprop->destinationBlendMode(), !alphaprop->noSorter(), hasSortAlpha, *node, true);
                        handleAlphaTesting(alphaprop->useAlphaTesting(), getTestMode(alphaprop->alphaTestMode()),
                            alphaprop->mThreshold, *node);
                        break;
                    }
                    case Nif::RC_BSShaderPPLightingProperty:
                    {
                        ++bsPPLightingProperties;
                        auto shaderprop = static_cast<const Nif::BSShaderPPLightingProperty*>(property);
                        bsShaderType = static_cast<int>(shaderprop->mType);
                        specEnabled = shaderprop->specular();
                        falloutVertexAlphaOnly = shaderprop->vertexAlpha() && hasVertexColors;
                        break;
                    }
                    case Nif::RC_BSShaderNoLightingProperty:
                    {
                        // FO3/FNV no-lighting surfaces can carry a black legacy diffuse colour while their
                        // actual, animated screen intensity is authored through NiMaterial emission. Keep the
                        // old diffuse modulation for ordinary no-lighting assets, but route surfaces that
                        // explicitly author emission through that layer instead of multiplying their texture by
                        // (0,0,0). This is record-driven and applies to every screen/effect using this contract.
                        hasNoLightingShader = true;
                        bsShaderType = static_cast<int>(Nif::BSShaderType::ShaderType_NoLighting);
                        break;
                    }
                    case Nif::RC_BSLightingShaderProperty:
                    {
                        ++bsLightingProperties;
                        auto shaderprop = static_cast<const Nif::BSLightingShaderProperty*>(property);
                        bsLightingType = static_cast<int>(shaderprop->mType);
                        if (Bgsm::MaterialFilePtr shaderMat = getShaderMaterial(shaderprop->mName, mMaterialManager))
                        {
                            shaderMaterialName = shaderprop->mName;
                            shaderMaterialType = static_cast<int>(shaderMat->mShaderType);
                            handleShaderMaterialDrawableProperties(shaderMat.get(), mat, *node, hasSortAlpha);
                            if (shaderMat->mShaderType == Bgsm::ShaderType::Lighting)
                            {
                                auto bgsm = static_cast<const Bgsm::BGSMFile*>(shaderMat.get());
                                specEnabled = false; // bgsm->mSpecularEnabled; TODO: PBR specular lighting
                                specStrength = 1.f; // bgsm->mSpecularMult;
                                emissiveMult = bgsm->mEmittanceMult;
                            }
                            break;
                        }
                        mat->setAlpha(osg::Material::FRONT_AND_BACK, shaderprop->mAlpha);
                        mat->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(shaderprop->mEmissive, 1.f));
                        mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(shaderprop->mSpecular, 1.f));
                        float glossiness = std::clamp(shaderprop->mGlossiness, 0.f, 128.f);
                        mat->setShininess(osg::Material::FRONT_AND_BACK, glossiness);
                        emissiveMult = shaderprop->mEmissiveMult;
                        specStrength = shaderprop->mSpecStrength;
                        specEnabled = shaderprop->specular();
                        if ((mBethVersion == Nif::NIFFile::BethVersion::BETHVER_SKY
                                || mBethVersion == Nif::NIFFile::BethVersion::BETHVER_SSE)
                            && shaderprop->mType
                                == static_cast<unsigned int>(Nif::BSLightingShaderType::ShaderType_SkinTint)
                            && isWorldViewerActorMeshPath(
                                Misc::StringUtils::lowerCase(mFilename.generic_string())))
                        {
                            // Skyrim's skin shader uses its gloss/specular data with a dedicated skin-lighting
                            // model. Feeding those values into OpenMW's generic Blinn-Phong path produces the
                            // hard white hand and forearm highlights seen on otherwise correctly textured actors.
                            // Preserve diffuse and normal textures, but use the stable non-specular fallback until
                            // that dedicated shader is implemented.
                            specEnabled = false;
                        }
                        handleDecal(shaderprop->decal(), hasSortAlpha, *node);
                        break;
                    }
                    case Nif::RC_BSEffectShaderProperty:
                    {
                        ++bsEffectProperties;
                        auto shaderprop = static_cast<const Nif::BSEffectShaderProperty*>(property);
                        if (Bgsm::MaterialFilePtr shaderMat = getShaderMaterial(shaderprop->mName, mMaterialManager))
                        {
                            shaderMaterialName = shaderprop->mName;
                            shaderMaterialType = static_cast<int>(shaderMat->mShaderType);
                            handleShaderMaterialDrawableProperties(shaderMat.get(), mat, *node, hasSortAlpha);
                            break;
                        }
                        handleDecal(shaderprop->decal(), hasSortAlpha, *node);
                        if (shaderprop->softEffect() && Loader::getSoftEffectEnabled())
                            SceneUtil::setupSoftEffect(*node,
                                {
                                    .mSize = shaderprop->mFalloffDepth,
                                    .mFalloffDepth = shaderprop->mFalloffDepth,
                                    .mFalloff = true,
                                });
                        break;
                    }
                    default:
                        break;
                }
            }

            const bool fallout3GenerationDefaultPPLighting
                = mVersion == Nif::NIFFile::NIFVersion::VER_BGS && mUserVersion == 11
                && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3 && bsPPLightingProperties > 0
                && bsShaderType == static_cast<int>(Nif::BSShaderType::ShaderType_Default);
            if (fallout3GenerationDefaultPPLighting && niAlphaProperties == 0 && !ppLightingUsesDiffuseAlpha)
            {
                // FO3/FNV diffuse textures often pack unrelated masks in alpha. The retail PP-lighting path only
                // consumes that channel when the material authors an alpha contract. Keep material/controller alpha
                // available for fades, but do not let an unflagged texture punch holes into the scene or VR composite.
                node->getOrCreateStateSet()->setDefine("IGNORE_DIFFUSE_ALPHA", "1", osg::StateAttribute::ON);
            }

            const bool falloutNvStaticDirectionalSls = fallout3GenerationDefaultPPLighting
                && bsPPLightingProperties == 1 && !hasVertexColors && !ppLightingSpecular
                && ppLightingRemappableTextures && !ppLightingUsesFalloutSlsPointLights && niAlphaProperties == 1
                && hasAlphaTestWithoutBlending;
            if (falloutNvStaticDirectionalSls)
            {
                // FNV SLS1009/1010 consumes only the base/normal maps, global ambient and directional sun.
                // Select it from the authored material contract used by static alpha-tested street signs;
                // do not bleed this into specular shells, blended foliage, vertex-lit meshes or point-light variants.
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("falloutSlsMode", 2));
            }

            if (hasNoLightingShader)
            {
                // Retail NOLIGHTTEX consumes MaterialColor independently of vertex color. For FO3/FNV that
                // constant comes from NiMaterial emission * emissiveMult; NOLIGHTTEXVC multiplies the vertex
                // stream as a separate stage. Do not fold either input into OpenGL's color-mode selection.
                osg::StateSet* stateSet = node->getOrCreateStateSet();
                stateSet->addUniform(new osg::Uniform("useNoLightingEmission", true));
                stateSet->addUniform(new osg::Uniform("useNoLightingVertexColor", hasVertexColors));
            }

            if (falloutVertexAlphaOnly)
            {
                // FO3/FNV's SLS vertex-alpha toggle consumes only the vertex
                // alpha channel. Some authored meshes deliberately store
                // black RGB alongside their fade alpha (the Goodsprings
                // hanging saloon sign is one); treating that as ordinary
                // AMBIENT_AND_DIFFUSE vertex colour turns the surface black.
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("falloutVertexAlphaOnly", true));
            }

            if (bsShaderType == static_cast<int>(Nif::BSShaderType::ShaderType_Skin))
            {
                // SKIN2002's Toggles.x selects the authored vertex RGB multiplication. Keep that
                // input separate from AmbientColor: the retail shader consumes both independently.
                node->getOrCreateStateSet()->addUniform(
                    new osg::Uniform("falloutSkinUseVertexColor", hasVertexColors));
            }

            const bool falloutNvActorMaterial = mVersion == Nif::NIFFile::NIFVersion::VER_BGS
                && mUserVersion == 11 && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3
                && isWorldViewerActorMeshPath(Misc::StringUtils::lowerCase(mFilename.generic_string()));
            if (falloutNvActorMaterial && specEnabled
                && bsShaderType != static_cast<int>(Nif::BSShaderType::ShaderType_Skin))
            {
                // FNV's SLS actor shaders take gloss from c27 and the normal-map alpha;
                // they do not multiply by NiMaterial's specular RGB.  OpenMW's generic
                // shader does, so black authored material values incorrectly erased eye,
                // beard, hair, and headgear highlights.  White makes that extra factor neutral.
                mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            }

            // While NetImmerse and Gamebryo support specular lighting, Morrowind has its support disabled.
            if (mVersion <= Nif::NIFFile::VER_MW || !specEnabled)
            {
                mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
                mat->setShininess(osg::Material::FRONT_AND_BACK, 0.f);
                specStrength = 1.f;
            }

            if (lightmode == Nif::NiVertexColorProperty::LightMode::LightMode_Emissive)
            {
                osg::Vec4f diffuse = mat->getDiffuse(osg::Material::FRONT_AND_BACK);
                diffuse = osg::Vec4f(0, 0, 0, diffuse.a());
                mat->setDiffuse(osg::Material::FRONT_AND_BACK, diffuse);
                mat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f());
            }

            // If we're told to use vertex colors but there are none to use, use a default color instead.
            if (!hasVertexColors)
            {
                switch (mat->getColorMode())
                {
                    case osg::Material::AMBIENT:
                        mat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
                        break;
                    case osg::Material::AMBIENT_AND_DIFFUSE:
                        mat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
                        mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
                        break;
                    case osg::Material::EMISSION:
                        mat->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
                        break;
                    default:
                        break;
                }
                mat->setColorMode(osg::Material::OFF);
            }

            if (hasMatCtrl || mat->getColorMode() != osg::Material::OFF
                || mat->getEmission(osg::Material::FRONT_AND_BACK) != osg::Vec4f(0, 0, 0, 1)
                || mat->getDiffuse(osg::Material::FRONT_AND_BACK) != osg::Vec4f(1, 1, 1, 1)
                || mat->getAmbient(osg::Material::FRONT_AND_BACK) != osg::Vec4f(1, 1, 1, 1)
                || mat->getShininess(osg::Material::FRONT_AND_BACK) != 0
                || mat->getSpecular(osg::Material::FRONT_AND_BACK) != osg::Vec4f(0.f, 0.f, 0.f, 0.f))
            {
                mat = shareAttribute(mat);
                node->getOrCreateStateSet()->setAttributeAndModes(mat, osg::StateAttribute::ON);
            }

            if (emissiveMult != 1.f)
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("emissiveMult", emissiveMult));

            if (specStrength != 1.f)
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("specStrength", specStrength));

            if (worldViewerMaterialTelemetryEnabled()
                && isWorldViewerActorTelemetryMeshPath(Misc::StringUtils::lowerCase(mFilename.generic_string())))
            {
                std::string shaderPrefix;
                bool shaderRequired = false;
                node->getUserValue("shaderPrefix", shaderPrefix);
                node->getUserValue("shaderRequired", shaderRequired);
                const osg::Vec4f diffuse = mat->getDiffuse(osg::Material::FRONT_AND_BACK);
                const osg::Vec4f ambient = mat->getAmbient(osg::Material::FRONT_AND_BACK);
                const osg::Vec4f emission = mat->getEmission(osg::Material::FRONT_AND_BACK);
                const osg::Vec4f specular = mat->getSpecular(osg::Material::FRONT_AND_BACK);
                const osg::StateSet* stateset = node->getStateSet();
                unsigned int stateTextureUnits = 0;
                const unsigned int textureProbeLimit
                    = std::max<unsigned int>(8, boundTextures != nullptr ? boundTextures->size() + 2 : 8);
                if (stateset != nullptr)
                {
                    for (unsigned int i = 0; i < textureProbeLimit; ++i)
                    {
                        if (stateset->getTextureAttribute(i, osg::StateAttribute::TEXTURE) != nullptr)
                            ++stateTextureUnits;
                    }
                }
                Log(Debug::Info) << "World viewer material ledger: file=\"" << mFilename.generic_string()
                                 << "\" node=\"" << node->getName() << "\""
                                 << " properties=" << properties.size()
                                 << " niMaterial=" << niMaterialProperties
                                 << " niVertexColor=" << niVertexColorProperties
                                 << " niAlpha=" << niAlphaProperties
                                 << " bsPPLighting=" << bsPPLightingProperties
                                 << " bsLighting=" << bsLightingProperties
                                 << " bsEffect=" << bsEffectProperties
                                 << " bsShaderType=" << bsShaderType
                                 << " bsLightingType=" << bsLightingType
                                 << " shaderPrefix=\"" << shaderPrefix << "\""
                                 << " shaderRequired=" << shaderRequired
                                 << " shaderMaterialType=" << shaderMaterialType
                                 << " shaderMaterial=\"" << shaderMaterialName << "\""
                                 << " hasVertexColors=" << hasVertexColors
                                 << " colorMode=" << static_cast<int>(mat->getColorMode())
                                 << " lightMode=" << static_cast<int>(lightmode)
                                 << " boundTextureSlots=" << (boundTextures != nullptr ? boundTextures->size() : 0)
                                 << " stateTextureUnits=" << stateTextureUnits
                                 << " hasSortAlpha=" << hasSortAlpha
                                 << " hasMatCtrl=" << hasMatCtrl
                                 << " specEnabled=" << specEnabled
                                 << " specStrength=" << specStrength
                                 << " emissiveMult=" << emissiveMult
                                 << " shininess=" << mat->getShininess(osg::Material::FRONT_AND_BACK)
                                 << " diffuse=(" << diffuse.r() << "," << diffuse.g() << "," << diffuse.b() << ","
                                 << diffuse.a() << ")"
                                 << " ambient=(" << ambient.r() << "," << ambient.g() << "," << ambient.b() << ","
                                 << ambient.a() << ")"
                                 << " emission=(" << emission.r() << "," << emission.g() << "," << emission.b() << ","
                                 << emission.a() << ")"
                                 << " specular=(" << specular.r() << "," << specular.g() << "," << specular.b() << ","
                                 << specular.a() << ")";
            }

            if (!mPushedSorter)
            {
                if (!hasSortAlpha && mHasStencilProperty)
                    setBinTraversal(node->getOrCreateStateSet());
                return;
            }

            osg::StateSet* stateset = node->getOrCreateStateSet();
            auto assignBin = [&](Nif::NiSortAdjustNode::SortingMode mode, int type) {
                if (mode == Nif::NiSortAdjustNode::SortingMode::Off)
                {
                    setBinTraversal(stateset);
                    return;
                }

                if (type == Nif::RC_NiAlphaAccumulator)
                {
                    if (hasSortAlpha)
                        setBinBackToFront(stateset);
                    else
                        setBinTraversal(stateset);
                }
                else if (type == Nif::RC_NiClusterAccumulator)
                    setBinBackToFront(stateset);
                else
                    Log(Debug::Error) << "Unrecognized NiAccumulator in " << mFilename;
            };

            switch (mPushedSorter->mMode)
            {
                case Nif::NiSortAdjustNode::SortingMode::Inherit:
                {
                    if (mLastAppliedNoInheritSorter)
                        assignBin(mLastAppliedNoInheritSorter->mMode, mLastAppliedNoInheritSorter->mSubSorter->recType);
                    else
                        assignBin(mPushedSorter->mMode, Nif::RC_NiAlphaAccumulator);
                    break;
                }
                case Nif::NiSortAdjustNode::SortingMode::Off:
                {
                    setBinTraversal(stateset);
                    break;
                }
                case Nif::NiSortAdjustNode::SortingMode::Subsort:
                {
                    assignBin(mPushedSorter->mMode, mPushedSorter->mSubSorter->recType);
                    break;
                }
            }
        }
    };

    osg::ref_ptr<osg::Node> Loader::load(
        Nif::FileView file, Resource::ImageManager* imageManager, Resource::BgsmFileManager* materialManager)
    {
        LoaderImpl impl(file.getFilename(), file.getVersion(), file.getUserVersion(), file.getBethVersion());
        impl.mMaterialManager = materialManager;
        impl.mImageManager = imageManager;
        return impl.load(file);
    }

    void Loader::loadKf(Nif::FileView kf, SceneUtil::KeyframeHolder& target)
    {
        LoaderImpl impl(kf.getFilename(), kf.getVersion(), kf.getUserVersion(), kf.getBethVersion());
        impl.loadKf(kf, target);
    }

}

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
#include "matrixtransform.hpp"
#include "particle.hpp"

namespace
{
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
                    continue;
                const std::size_t materialEnd = line.find('\t');
                if (materialEnd == std::string::npos)
                    continue;
                const std::size_t diffuseEnd = line.find('\t', materialEnd + 1);
                std::string material = normalizeStarfieldMaterialBridgePath(line.substr(0, materialEnd));
                std::string diffuse = normalizeStarfieldMaterialBridgePath(line.substr(materialEnd + 1,
                    diffuseEnd == std::string::npos ? std::string::npos : diffuseEnd - materialEnd - 1));
                if (!material.starts_with("materials/") || !material.ends_with(".mat")
                    || !diffuse.starts_with("textures/") || !diffuse.ends_with(".dds"))
                    continue;
                std::string evidence;
                if (diffuseEnd != std::string::npos)
                    evidence = line.substr(diffuseEnd + 1);
                result.insert_or_assign(std::move(material),
                    StarfieldMaterialBridgeEntry{ getStarfieldActorProofTexturePath(diffuse), std::move(evidence) });
            }
            Log(Debug::Info) << "World viewer: Starfield material bridge loaded entries=" << result.size();
            return result;
        }();
        return bridge;
    }

    const StarfieldMaterialBridgeEntry* findStarfieldMaterialBridge(std::string_view shaderMaterialName)
    {
        if (shaderMaterialName.empty())
            return nullptr;
        const auto& bridge = getStarfieldMaterialBridge();
        const auto found = bridge.find(normalizeStarfieldMaterialBridgePath(shaderMaterialName));
        return found == bridge.end() ? nullptr : &found->second;
    }

    bool isStarfieldMaterialBridgeConfigured()
    {
        const char* path = std::getenv("OPENMW_WORLD_VIEWER_STARFIELD_MATERIAL_MAP");
        return path != nullptr && *path != '\0';
    }

    osg::Vec4f getStarfieldWorldMaterialColor(std::string_view shaderMaterialName, bool hasDiffuse)
    {
        const std::string key = normalizeStarfieldMaterialBridgePath(shaderMaterialName);
        const auto has = [&](std::string_view value) { return key.find(value) != std::string::npos; };

        if (hasDiffuse)
        {
            // The compatibility path has no Starfield PBR shader yet. Keep authored albedo below full white so
            // grayscale procedural bases do not blow out, but preserve signs/decals whose albedo is already final.
            osg::Vec4f factor = has("sign") || has("decal") || has("poster") || has("terminal")
                ? osg::Vec4f(1.f, 1.f, 1.f, 1.f)
                : osg::Vec4f(0.82f, 0.82f, 0.82f, 1.f);
            if (has("black"))
                factor = osg::Vec4f(0.16f, 0.17f, 0.19f, 1.f);
            else if (has("grey") || has("gray") || has("dark"))
                factor = osg::Vec4f(0.52f, 0.54f, 0.58f, 1.f);
            else if (has("blue"))
                factor = osg::Vec4f(0.38f, 0.58f, 0.92f, 1.f);
            else if (has("green"))
                factor = osg::Vec4f(0.42f, 0.76f, 0.48f, 1.f);
            else if (has("red"))
                factor = osg::Vec4f(0.92f, 0.34f, 0.29f, 1.f);
            else if (has("orange"))
                factor = osg::Vec4f(1.f, 0.58f, 0.24f, 1.f);
            else if (has("yellow"))
                factor = osg::Vec4f(0.96f, 0.79f, 0.24f, 1.f);
            else if (has("brown"))
                factor = osg::Vec4f(0.60f, 0.42f, 0.28f, 1.f);
            return factor;
        }

        // These are bounded categorical fallbacks for compiled procedural layers that expose no color texture.
        // They are deliberately neutral and materially typed, rather than the previous one-texture-for-everything
        // guess. Exact layer constants still need BSLODMaterialInstanceDB decoding.
        if (has("glass") || has("translucent") || has("transparent"))
            return osg::Vec4f(0.40f, 0.56f, 0.66f, 0.28f);
        if (has("water"))
            return osg::Vec4f(0.10f, 0.28f, 0.38f, 0.58f);
        if (has("glow") || has("lightstrip") || has("streetlamp"))
        {
            if (has("orange"))
                return osg::Vec4f(1.f, 0.38f, 0.08f, 1.f);
            if (has("red"))
                return osg::Vec4f(1.f, 0.10f, 0.06f, 1.f);
            return osg::Vec4f(0.84f, 0.94f, 1.f, 1.f);
        }
        if (has("black") || has("pitchblack"))
            return osg::Vec4f(0.055f, 0.065f, 0.08f, 1.f);
        if (has("white"))
            return osg::Vec4f(0.76f, 0.77f, 0.75f, 1.f);
        if (has("grey") || has("gray") || has("dark"))
            return osg::Vec4f(0.32f, 0.34f, 0.37f, 1.f);
        if (has("blue"))
            return osg::Vec4f(0.12f, 0.27f, 0.48f, 1.f);
        if (has("green"))
            return osg::Vec4f(0.12f, 0.34f, 0.18f, 1.f);
        if (has("red"))
            return osg::Vec4f(0.52f, 0.09f, 0.07f, 1.f);
        if (has("orange"))
            return osg::Vec4f(0.72f, 0.25f, 0.05f, 1.f);
        if (has("yellow"))
            return osg::Vec4f(0.72f, 0.54f, 0.08f, 1.f);
        if (has("brown"))
            return osg::Vec4f(0.31f, 0.18f, 0.10f, 1.f);
        if (has("rubber"))
            return osg::Vec4f(0.10f, 0.11f, 0.12f, 1.f);
        if (has("chrome") || has("metal") || has("aluminium") || has("aluminum") || has("steel"))
            return osg::Vec4f(0.38f, 0.40f, 0.43f, 1.f);
        if (has("plastic"))
            return osg::Vec4f(0.34f, 0.36f, 0.39f, 1.f);
        if (has("concrete") || has("tarmac") || has("stone") || has("rock"))
            return osg::Vec4f(0.45f, 0.44f, 0.41f, 1.f);
        if (has("wood"))
            return osg::Vec4f(0.34f, 0.23f, 0.15f, 1.f);
        if (has("plant") || has("leaf") || has("shrub") || has("vine") || has("grass"))
            return osg::Vec4f(0.16f, 0.30f, 0.13f, 1.f);
        if (has("decal") || has("letter") || has("overlay"))
            return osg::Vec4f(0.14f, 0.15f, 0.16f, 1.f);
        return osg::Vec4f(0.42f, 0.43f, 0.44f, 1.f);
    }

    bool isStarfieldWorldMaterialTranslucent(std::string_view shaderMaterialName)
    {
        const std::string key = normalizeStarfieldMaterialBridgePath(shaderMaterialName);
        return key.find("glass") != std::string::npos || key.find("water") != std::string::npos
            || key.find("translucent") != std::string::npos || key.find("transparent") != std::string::npos
            || key.find("distortion") != std::string::npos;
    }

    bool isStarfieldWorldMaterialCutout(
        std::string_view nifPath, std::string_view shaderMaterialName)
    {
        std::string key(nifPath);
        key += ' ';
        key += std::string(shaderMaterialName);
        key = Misc::StringUtils::lowerCase(key);
        return key.find("plant") != std::string::npos || key.find("shrub") != std::string::npos
            || key.find("leaf") != std::string::npos || key.find("vine") != std::string::npos
            || key.find("grass") != std::string::npos || key.find("decal") != std::string::npos;
    }

    bool isStarfieldWorldMaterialDoubleSided(
        std::string_view nifPath, std::string_view shaderMaterialName)
    {
        std::string key(nifPath);
        key += ' ';
        key += std::string(shaderMaterialName);
        key = Misc::StringUtils::lowerCase(key);
        return containsAny(key,
            { "twosided", "two_sided", "glass", "translucent", "leaf", "leaves", "flats", "grass",
                "shrub", "vine" });
    }

    std::string getStarfieldWorldProofDiffuse(
        std::string_view nifPath, std::string_view shapeName, std::string_view shaderMaterialName)
    {
        std::string key(nifPath);
        key += ' ';
        key += std::string(shapeName);
        key += ' ';
        key += std::string(shaderMaterialName);
        key = Misc::StringUtils::lowerCase(key);

        const bool looksLikeNewAtlantis = key.find("newatlantis") != std::string::npos
            || key.find("city/newatlantis") != std::string::npos || key.find("city\\newatlantis") != std::string::npos
            || key.find("na_") != std::string::npos || key.find("/na") != std::string::npos
            || key.find("\\na") != std::string::npos;
        const auto has = [&](std::string_view needle) { return key.find(needle) != std::string::npos; };
        const bool looksLikeProofWorld = looksLikeNewAtlantis
            || has("architecture/spaceport") || has("architecture\\spaceport")
            || has("architecture/spacestationkit") || has("architecture\\spacestationkit")
            || has("architecture/outpost/outpostindustrial") || has("architecture\\outpost\\outpostindustrial")
            || has("architecture/industrialkit") || has("architecture\\industrialkit")
            || has("architecture/sciencekit") || has("architecture\\sciencekit")
            || has("architecture/generic") || has("architecture\\generic")
            || has("architecture/opmine") || has("architecture\\opmine")
            || has("architecture/terrabrew") || has("architecture\\terrabrew")
            || has("starstations/") || has("starstations\\")
            || has("ships/interior") || has("ships\\interior")
            || has("effects/ambient") || has("effects\\ambient")
            || has("setdressing/") || has("setdressing\\")
            || has("landscape/") || has("landscape\\")
            || has("materials/common/") || has("materials\\common\\");
        if (!looksLikeProofWorld)
            return {};

        const auto texture = [](std::string_view path) { return getStarfieldActorProofTexturePath(path); };

        if (has("black"))
            return texture("textures/architecture/city/newatlantis/naconcreteprinted02_color.dds");
        if (key.find("glass") != std::string::npos)
            return texture("textures/architecture/city/newatlantis/naplasticcirclepattern01_color.dds");
        if (has("skylight"))
            return texture("textures/architecture/city/newatlantis/naplasticcirclepattern01_color.dds");
        if (has("label") || has("letter") || has("warning")
            || (has("sign") && !has("signage") && !has("terminalsignage")))
            return texture("textures/architecture/city/newatlantis/natilefloor01_color.dds");
        if (has("terminalsignage") || has("terminal_pad") || has("terminalpad") || has("terminal"))
            return texture("textures/architecture/city/newatlantis/naplasticcirclepattern01_color.dds");
        if (has("signage/") || has("signage\\") || has("na_signage"))
            return texture("textures/architecture/city/newatlantis/naplasticcirclepattern01_color.dds");
        if (has("screen") || has("glow") || has("light") || has("neon"))
            return texture("textures/architecture/city/newatlantis/nametalbrasspattern01_color.dds");
        if (key.find("grass") != std::string::npos || key.find("astro") != std::string::npos)
            return texture("textures/architecture/city/newatlantis/nagrassastroturf01_color.dds");
        if (has("plant") || has("shrub") || has("landscape/trees") || has("landscape\\trees")
            || has("treemesa") || has("treeroot") || has("leaf") || has("vine")
            || has("lilypad") || has("groundcover") || has("canopy") || has("flats"))
            return texture("textures/architecture/city/newatlantis/nagrassastroturf01_color.dds");
        if (key.find("carpet") != std::string::npos || key.find("rug") != std::string::npos)
            return texture("textures/architecture/city/newatlantis/nacarpet01b_color.dds");
        if (has("bark") || has("root"))
            return texture("textures/architecture/city/newatlantis/nastone01mossy01_color.dds");
        if (key.find("metal") != std::string::npos || key.find("brass") != std::string::npos
            || key.find("gold") != std::string::npos || has("beam") || has("trim") || has("bevel")
            || has("bolt") || has("wire") || has("crate") || has("luggybot"))
            return texture("textures/architecture/city/newatlantis/nametalbrasspattern01_color.dds");
        if (key.find("stone") != std::string::npos || has("rock"))
            return texture("textures/architecture/city/newatlantis/nastone01mossy01_color.dds");
        if (key.find("plastic") != std::string::npos || has("rubber") || has("matte"))
            return texture("textures/architecture/city/newatlantis/naplasticcirclepattern01_color.dds");
        if (key.find("floor") != std::string::npos || key.find("tile") != std::string::npos
            || has("tarmac") || has("landingpad") || has("deck"))
            return texture("textures/architecture/city/newatlantis/natilefloor01_color.dds");
        if (has("panel") || has("paint") || has("ceiling") || has("wall"))
            return texture("textures/architecture/city/newatlantis/naconcreteprinted02_color.dds");
        if (has("waterfall") || has("water"))
            return texture("textures/architecture/city/newatlantis/nascreenpattern01_color.dds");
        if (key.find("moss") != std::string::npos)
            return texture("textures/architecture/city/newatlantis/naconcretemossy01_color.dds");

        return texture("textures/architecture/city/newatlantis/naconcreteprinted02_color.dds");
    }

    bool worldViewerStarfieldProofSkipBlackOccluders()
    {
        const char* explicitSetting = std::getenv("OPENMW_WORLD_VIEWER_STARFIELD_SKIP_BLACK_OCCLUDERS");
        if (explicitSetting != nullptr)
            return *explicitSetting != '\0' && explicitSetting[0] != '0';

        return worldViewerMeshLoadTelemetryEnabled()
            || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_STARFIELD_ACTOR_PNG_TEXTURES");
    }

    bool worldViewerStarfieldWorldProofTexturesEnabled()
    {
        const char* explicitSetting = std::getenv("OPENMW_WORLD_VIEWER_STARFIELD_WORLD_PROOF_TEXTURES");
        if (explicitSetting != nullptr)
            return *explicitSetting != '\0' && explicitSetting[0] != '0';

        return true;
    }

    bool shouldSkipStarfieldWorldProofGeometry(
        std::string_view nifPath, std::string_view shapeName, std::string_view shaderMaterialName)
    {
        if (!worldViewerStarfieldProofSkipBlackOccluders())
            return false;

        const std::string lowerNifPath = Misc::StringUtils::lowerCase(std::string(nifPath));
        if (isWorldViewerActorMeshPath(lowerNifPath))
            return false;

        std::string key = lowerNifPath;
        key += ' ';
        key += Misc::StringUtils::lowerCase(std::string(shapeName));
        key += ' ';
        key += Misc::StringUtils::lowerCase(std::string(shaderMaterialName));

        if (isStarfieldMaterialBridgeConfigured()
            && containsAny(key, { "waterfountain", "waterfall", "waterplaceholder", "materials/water/",
                   "materials\\water\\" }))
            return true;

        return containsAny(key,
            { "loaddoor_viewblocker", "viewblocker", "occlusionplane", "occluder",
                "fillplane_black", "blackfade", "decalpitchblack", "pitchblack" });
    }

    std::optional<StarfieldExternalMeshData> readStarfieldExternalMesh(std::istream& stream, std::string& error)
    {
        static constexpr std::uint32_t maxIndices = 12'000'000;
        static constexpr std::uint32_t maxVertices = 4'000'000;

        StarfieldExternalMeshData result;
        if (!readStarfieldMeshPod(stream, result.mVersion))
        {
            error = "missing mesh version";
            return std::nullopt;
        }
        if (result.mVersion > 2)
        {
            error = "unsupported mesh version " + std::to_string(result.mVersion);
            return std::nullopt;
        }

        std::uint32_t triIndexCount = 0;
        if (!readStarfieldMeshPod(stream, triIndexCount) || triIndexCount % 3 != 0 || triIndexCount > maxIndices)
        {
            error = "invalid triangle index count";
            return std::nullopt;
        }

        result.mIndices.reserve(triIndexCount);
        for (std::uint32_t i = 0; i < triIndexCount; ++i)
        {
            std::uint16_t index = 0;
            if (!readStarfieldMeshPod(stream, index))
            {
                error = "truncated triangle indices";
                return std::nullopt;
            }
            result.mIndices.push_back(index);
        }

        if (!readStarfieldMeshPod(stream, result.mScale) || result.mScale <= 0.f || !std::isfinite(result.mScale))
        {
            error = "invalid mesh scale";
            return std::nullopt;
        }

        if (!readStarfieldMeshPod(stream, result.mWeightCountPerVertex))
        {
            error = "missing weight count";
            return std::nullopt;
        }

        std::uint32_t vertexCount = 0;
        if (!readStarfieldMeshPod(stream, vertexCount) || vertexCount > maxVertices)
        {
            error = "invalid vertex count";
            return std::nullopt;
        }

        result.mVertices.reserve(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i)
        {
            std::int16_t x = 0;
            std::int16_t y = 0;
            std::int16_t z = 0;
            if (!readStarfieldMeshPod(stream, x) || !readStarfieldMeshPod(stream, y) || !readStarfieldMeshPod(stream, z))
            {
                error = "truncated vertices";
                return std::nullopt;
            }
            result.mVertices.emplace_back(
                unpackStarfieldMeshPosition(x, result.mScale),
                unpackStarfieldMeshPosition(y, result.mScale),
                unpackStarfieldMeshPosition(z, result.mScale));
        }

        auto readAndSkipArray = [&](std::uint32_t& count, std::uint32_t elementSize, std::string_view label) {
            if (!readStarfieldMeshPod(stream, count))
            {
                error = "missing " + std::string(label) + " count";
                return false;
            }
            if (!skipStarfieldMeshArray(stream, count, elementSize))
            {
                error = "invalid/truncated " + std::string(label) + " data";
                return false;
            }
            return true;
        };

        if (!readStarfieldMeshPod(stream, result.mUv1Count))
        {
            error = "missing uv1 count";
            return std::nullopt;
        }
        if (result.mUv1Count > maxVertices)
        {
            error = "invalid uv1 count";
            return std::nullopt;
        }
        result.mUv1.reserve(result.mUv1Count);
        for (std::uint32_t i = 0; i < result.mUv1Count; ++i)
        {
            std::uint16_t u = 0;
            std::uint16_t v = 0;
            if (!readStarfieldMeshPod(stream, u) || !readStarfieldMeshPod(stream, v))
            {
                error = "truncated uv1";
                return std::nullopt;
            }
            result.mUv1.emplace_back(unpackStarfieldMeshHalf(u), 1.f - unpackStarfieldMeshHalf(v));
        }
        if (!readAndSkipArray(result.mUv2Count, 4, "uv2"))
            return std::nullopt;
        if (!readAndSkipArray(result.mColorCount, 4, "colors"))
            return std::nullopt;

        if (!readStarfieldMeshPod(stream, result.mNormalCount))
        {
            error = "missing normals count";
            return std::nullopt;
        }
        if (result.mNormalCount > maxVertices)
        {
            error = "invalid normals count";
            return std::nullopt;
        }
        result.mNormals.reserve(result.mNormalCount);
        for (std::uint32_t i = 0; i < result.mNormalCount; ++i)
        {
            std::uint32_t data = 0;
            if (!readStarfieldMeshPod(stream, data))
            {
                error = "truncated normals";
                return std::nullopt;
            }
            result.mNormals.push_back(unpackStarfieldUdec3Normal(data));
        }

        if (!readAndSkipArray(result.mTangentCount, 4, "tangents"))
            return std::nullopt;

        std::uint32_t totalWeights = 0;
        if (!readStarfieldMeshPod(stream, totalWeights))
        {
            error = "missing weights count";
            return std::nullopt;
        }
        const std::uint64_t expectedWeightLimit
            = std::max<std::uint64_t>(static_cast<std::uint64_t>(vertexCount) * 16ull, 1024ull);
        if (totalWeights > expectedWeightLimit || totalWeights > maxVertices * 16u)
        {
            error = "invalid weights count";
            return std::nullopt;
        }
        result.mWeights.resize(totalWeights);
        for (std::uint32_t& weight : result.mWeights)
        {
            if (!readStarfieldMeshPod(stream, weight))
            {
                error = "truncated weights";
                return std::nullopt;
            }
        }

        if (!readStarfieldMeshPod(stream, result.mLodCount))
        {
            error = "missing lod count";
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < result.mLodCount; ++i)
        {
            std::uint32_t lodIndexCount = 0;
            if (!readStarfieldMeshPod(stream, lodIndexCount) || !skipStarfieldMeshArray(stream, lodIndexCount, 2))
            {
                error = "invalid/truncated lod data";
                return std::nullopt;
            }
        }

        // Generated Starfield FaceMeshes use the same version-2 vertex/index stream but terminate immediately
        // after the LOD arrays. Ordinary world and equipment meshes append meshlet and cull-data arrays.
        // Treat those two acceleration structures as optional trailing data; they are not needed by OSG.
        if (stream.peek() != std::char_traits<char>::eof())
        {
            if (!readAndSkipArray(result.mMeshletCount, 16, "meshlets"))
                return std::nullopt;
            if (stream.peek() != std::char_traits<char>::eof()
                && !readAndSkipArray(result.mCullDataCount, 24, "cull data"))
                return std::nullopt;
        }

        if (result.mVertices.empty() || result.mIndices.empty())
        {
            error = "empty mesh";
            return std::nullopt;
        }

        for (unsigned int index : result.mIndices)
        {
            if (index >= result.mVertices.size())
            {
                error = "triangle index outside vertex buffer";
                return std::nullopt;
            }
        }

        return result;
    }

    bool isTypeBSGeometry(int type)
    {
        switch (type)
        {
            case Nif::RC_BSTriShape:
            case Nif::RC_BSDynamicTriShape:
            case Nif::RC_BSMeshLODTriShape:
            case Nif::RC_BSSubIndexTriShape:
                return true;
        }
        return false;
    }

    float bsNormByteToFloat(uint8_t value)
    {
        return value / 255.f * 2.f - 1.f;
    }

    float bsHalfToFloat(uint16_t value)
    {
        uint32_t bits = static_cast<uint32_t>(value & 0x8000) << 16;

        const uint32_t exp16 = (value & 0x7c00) >> 10;
        uint32_t frac16 = value & 0x3ff;
        if (exp16)
            bits |= (exp16 + 0x70) << 23;
        else if (frac16)
        {
            uint8_t offset = 0;
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

    void appendBSVertexDataArrays(const std::vector<Nif::BSVertexData>& vertexData, uint16_t flags,
        std::vector<osg::Vec3f>& vertices, std::vector<osg::Vec3f>& normals, std::vector<osg::Vec4ub>& colors,
        std::vector<osg::Vec2f>& uvlist)
    {
        const bool fullPrec = flags & Nif::BSVertexDesc::VertexAttribute::Full_Precision;
        const bool hasVertices = flags & Nif::BSVertexDesc::VertexAttribute::Vertex;
        const bool hasNormals = flags & Nif::BSVertexDesc::VertexAttribute::Normals;
        const bool hasColors = flags & Nif::BSVertexDesc::VertexAttribute::Vertex_Colors;
        const bool hasUV = flags & Nif::BSVertexDesc::VertexAttribute::UVs;

        for (const Nif::BSVertexData& elem : vertexData)
        {
            if (hasVertices)
            {
                if (fullPrec)
                    vertices.emplace_back(elem.mVertex.x(), elem.mVertex.y(), elem.mVertex.z());
                else
                    vertices.emplace_back(bsHalfToFloat(elem.mHalfVertex[0]), bsHalfToFloat(elem.mHalfVertex[1]),
                        bsHalfToFloat(elem.mHalfVertex[2]));
            }
            if (hasNormals)
                normals.emplace_back(bsNormByteToFloat(elem.mNormal[0]), bsNormByteToFloat(elem.mNormal[1]),
                    bsNormByteToFloat(elem.mNormal[2]));
            if (hasColors)
                colors.emplace_back(elem.mVertColor[0], elem.mVertColor[1], elem.mVertColor[2], elem.mVertColor[3]);
            if (hasUV)
                uvlist.emplace_back(bsHalfToFloat(elem.mUV[0]), 1.0f - bsHalfToFloat(elem.mUV[1]));
        }
    }

    bool isSkinAuxTexture(std::string_view texture)
    {
        const std::string lower = Misc::StringUtils::lowerCase(texture);
        return Misc::StringUtils::ciEndsWith(lower, "_sk.dds") || Misc::StringUtils::ciEndsWith(lower, "_sk.dds\\")
            || lower.find("_sk.dds") != std::string::npos;
    }

    // Collect all properties affecting the given drawable that should be handled on drawable basis rather than on the
    // node hierarchy above it.
    void collectDrawableProperties(
        const Nif::NiAVObject* nifNode, const Nif::Parent* parent, std::vector<const Nif::NiProperty*>& out)
    {
        if (parent != nullptr)
            collectDrawableProperties(&parent->mNiNode, parent->mParent, out);
        for (const auto& property : nifNode->mProperties)
        {
            if (!property.empty())
            {
                switch (property->recType)
                {
                    case Nif::RC_NiMaterialProperty:
                    case Nif::RC_NiVertexColorProperty:
                    case Nif::RC_NiSpecularProperty:
                    case Nif::RC_NiAlphaProperty:
                    case Nif::RC_BSShaderPPLightingProperty:
                    case Nif::RC_BSShaderNoLightingProperty:
                    case Nif::RC_BSLightingShaderProperty:
                    case Nif::RC_BSEffectShaderProperty:
                        out.push_back(property.getPtr());
                        break;
                    default:
                        break;
                }
            }
        }
    }

    // NodeCallback used to have a node always oriented towards the camera. The node can have translation and scale
    // set just like a regular MatrixTransform, but the rotation set will be overridden in order to face the camera.
    class BillboardCallback : public SceneUtil::NodeCallback<BillboardCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        BillboardCallback() {}
        BillboardCallback(const BillboardCallback& copy, const osg::CopyOp& copyop)
            : SceneUtil::NodeCallback<BillboardCallback, osg::Node*, osgUtil::CullVisitor*>(copy, copyop)
        {
        }

        META_Object(NifOsg, BillboardCallback)

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
        {
            osg::Matrix modelView = *cv->getModelViewMatrix();

            // attempt to preserve scale
            float mag[3];
            for (int i = 0; i < 3; ++i)
            {
                mag[i] = std::sqrt(modelView(0, i) * modelView(0, i) + modelView(1, i) * modelView(1, i)
                    + modelView(2, i) * modelView(2, i));
            }

            modelView.setRotate(osg::Quat());
            modelView(0, 0) = mag[0];
            modelView(1, 1) = mag[1];
            modelView(2, 2) = mag[2];

            cv->pushModelViewMatrix(new osg::RefMatrix(modelView), osg::Transform::RELATIVE_RF);

            traverse(node, cv);

            cv->popModelViewMatrix();
        }
    };

    void extractTextKeys(const Nif::NiTextKeyExtraData* tk, SceneUtil::TextKeyMap& textkeys)
    {
        for (const Nif::NiTextKeyExtraData::TextKey& key : tk->mList)
        {
            std::vector<std::string> results;
            Misc::StringUtils::split(key.mText, results, "\r\n");
            for (std::string& result : results)
            {
                Misc::StringUtils::trim(result);
                Misc::StringUtils::lowerCaseInPlace(result);
                if (!result.empty())
                    textkeys.emplace(key.mTime, std::move(result));
            }
        }
    }

    void addLoopingTextKeys(SceneUtil::TextKeyMap& textkeys, float start, float stop, const std::string& group)
    {
        textkeys.emplace(start, group + ": start");
        textkeys.emplace(start, group + ": loop start");
        textkeys.emplace(stop, group + ": loop stop");
        textkeys.emplace(stop, group + ": stop");
    }

    void synthesizeFalloutTextKeys(const Nif::NiControllerSequence* sequence, SceneUtil::TextKeyMap& textkeys,
        const std::filesystem::path& filename)
    {
        std::string stem = filename.stem().generic_string();
        Misc::StringUtils::lowerCaseInPlace(stem);

        std::vector<std::string> groups;
        if (stem.find("flyaway") != std::string::npos)
        {
            groups.emplace_back("idle");
            groups.emplace_back("idle2");
            groups.emplace_back("flyforward");
            groups.emplace_back("walkforward");
        }
        else if (stem.find("specialidle") != std::string::npos)
        {
            groups.emplace_back("idle2");
            groups.emplace_back("idle");
        }
        else if (stem == "2hrcrouch")
            groups.emplace_back("kneel");
        else if (stem == "floorsleepdynamicidle")
            groups.emplace_back("prone");
        else if (stem == "talk_handsatside_moving")
            groups.emplace_back("talk");
        else if (stem == "wavehello")
            groups.emplace_back("wave");
        // Directional words on action clips describe the action variant, not locomotion. In particular,
        // 2hrattackleft/right were being exposed as walkleft/right and, because later animation sources win,
        // stealing the actor's authored locomotion source. The animation layer synthesizes the semantic attack,
        // reload, equip, and unequip groups for these clips; do not also infer a looping movement group here.
        else if (stem.find("attack") != std::string::npos || stem.find("reload") != std::string::npos
            || stem.find("equip") != std::string::npos)
            return;
        else if (stem == "mtidle" || stem == "pamtidle" || stem == "talk_handsatside_still2"
            || stem == "2hrloiter" || stem == "2hrloiteronehanded"
            || stem == "3rdp_specialidle_1hmidlela" || stem == "3rdp_specialidle_1hmidlelb"
            || stem == "dlcanch1hpistolpose" || Misc::StringUtils::ciEndsWith(stem, "idle"))
            groups.emplace_back("idle");
        else if (stem == "mtturnleft" || Misc::StringUtils::ciEndsWith(stem, "turnleft"))
            groups.emplace_back("turnleft");
        else if (stem == "mtturnright" || Misc::StringUtils::ciEndsWith(stem, "turnright"))
            groups.emplace_back("turnright");
        else if (stem == "mtforward")
        {
            groups.emplace_back("walkforward");
            groups.emplace_back("runforward");
        }
        else if (stem == "mtbackward")
        {
            groups.emplace_back("walkback");
            groups.emplace_back("runback");
        }
        else if (stem == "mtleft")
        {
            groups.emplace_back("walkleft");
            groups.emplace_back("runleft");
        }
        else if (stem == "mtright")
        {
            groups.emplace_back("walkright");
            groups.emplace_back("runright");
        }
        else if (Misc::StringUtils::ciEndsWith(stem, "fastforward"))
            groups.emplace_back("runforward");
        else if (Misc::StringUtils::ciEndsWith(stem, "fastbackward"))
            groups.emplace_back("runback");
        else if (Misc::StringUtils::ciEndsWith(stem, "fastleft"))
            groups.emplace_back("runleft");
        else if (Misc::StringUtils::ciEndsWith(stem, "fastright"))
            groups.emplace_back("runright");
        else if (Misc::StringUtils::ciEndsWith(stem, "forward"))
            groups.emplace_back("walkforward");
        else if (Misc::StringUtils::ciEndsWith(stem, "backward"))
            groups.emplace_back("walkback");
        else if (Misc::StringUtils::ciEndsWith(stem, "left"))
            groups.emplace_back("walkleft");
        else if (Misc::StringUtils::ciEndsWith(stem, "right"))
            groups.emplace_back("walkright");
        if (groups.empty())
            return;

        bool hasAllGroups = true;
        for (const std::string& group : groups)
            hasAllGroups = hasAllGroups && textkeys.hasGroupStart(group);
        if (hasAllGroups)
            return;

        const float start = std::isfinite(sequence->mStartTime) ? sequence->mStartTime : 0.f;
        float stop = std::isfinite(sequence->mStopTime) ? sequence->mStopTime : start;
        if (stop <= start)
            stop = start + 1.f;

        for (const std::string& group : groups)
            addLoopingTextKeys(textkeys, start, stop, group);

        Log(Debug::Verbose) << "FNV/ESM4 diag: synthesized Fallout KF text key group(s) for " << filename;
    }

    void handleExtraData(const std::string& data, osg::Group* node)
    {
        YAML::Node root = YAML::Load(data);

        for (const auto& it : root["shader"])
        {
            std::string key = it.first.as<std::string>();

            if (key == "soft_effect" && NifOsg::Loader::getSoftEffectEnabled())
            {
                SceneUtil::SoftEffectConfig config;
                config.mSize = it.second["size"].as<float>(config.mSize);
                config.mFalloff = it.second["falloff"].as<bool>(config.mFalloff);
                config.mFalloffDepth = it.second["falloffDepth"].as<float>(config.mFalloffDepth);

                SceneUtil::setupSoftEffect(*node, config);
            }
            else if (key == "distortion")
            {
                SceneUtil::DistortionConfig config;
                config.mStrength = it.second["strength"].as<float>(config.mStrength);

                SceneUtil::setupDistortion(*node, config);
            }
        }
    }
}

namespace NifOsg
{
    bool Loader::sShowMarkers = false;

    void Loader::setShowMarkers(bool show)
    {
        sShowMarkers = show;
    }

    bool Loader::getShowMarkers()
    {
        return sShowMarkers;
    }

    unsigned int Loader::sHiddenNodeMask = 0;

    void Loader::setHiddenNodeMask(unsigned int mask)
    {
        sHiddenNodeMask = mask;
    }
    unsigned int Loader::getHiddenNodeMask()
    {
        return sHiddenNodeMask;
    }

    unsigned int Loader::sIntersectionDisabledNodeMask = ~0u;

    void Loader::setIntersectionDisabledNodeMask(unsigned int mask)
    {
        sIntersectionDisabledNodeMask = mask;
    }

    unsigned int Loader::getIntersectionDisabledNodeMask()
    {
        return sIntersectionDisabledNodeMask;
    }

    bool Loader::sSoftEffectEnabled = false;

    void Loader::setSoftEffectEnabled(bool enabled)
    {
        sSoftEffectEnabled = enabled;
    }

    bool Loader::getSoftEffectEnabled()
    {
        return sSoftEffectEnabled;
    }

    class LoaderImpl
    {
    public:
        /// @param filename used for warning messages.
        LoaderImpl(const std::filesystem::path& filename, unsigned int ver, unsigned int userver, unsigned int bethver)
            : mFilename(filename)
            , mVersion(ver)
            , mUserVersion(userver)
            , mBethVersion(bethver)
        {
        }
        std::filesystem::path mFilename;
        unsigned int mVersion, mUserVersion, mBethVersion;
        Resource::BgsmFileManager* mMaterialManager{ nullptr };
        Resource::ImageManager* mImageManager{ nullptr };

        size_t mFirstRootTextureIndex{ ~0u };
        bool mFoundFirstRootTexturingProperty = false;

        bool mHasNightDayLabel = false;
        bool mHasHerbalismLabel = false;
        bool mHasStencilProperty = false;

        const Nif::NiSortAdjustNode* mPushedSorter = nullptr;
        const Nif::NiSortAdjustNode* mLastAppliedNoInheritSorter = nullptr;

        // This is used to queue emitters that weren't attached to their node yet.
        std::vector<std::pair<size_t, osg::ref_ptr<Emitter>>> mEmitterQueue;
        std::unordered_map<const Nif::NiAVObject*, osg::Node*> mNodesByNif;
        std::unordered_map<std::string, osg::Node*> mNodesByName;
        std::unordered_map<std::string, unsigned int> mBethesdaBoneLodGroups;
        std::vector<const Nif::NiControllerManager*> mControllerManagers;

        void collectBethesdaBoneLodGroups(const Nif::NiAVObject* object)
        {
            if (object == nullptr)
                return;

            for (Nif::NiTimeControllerPtr controller = object->mController; !controller.empty();
                 controller = controller->mNext)
            {
                const auto* boneLod = dynamic_cast<const Nif::NiBoneLODController*>(controller.getPtr());
                if (boneLod == nullptr)
                    continue;

                for (std::size_t groupIndex = 0; groupIndex < boneLod->mNodeGroups.size(); ++groupIndex)
                {
                    for (const auto& node : boneLod->mNodeGroups[groupIndex])
                    {
                        if (!node.empty() && !node->mName.empty())
                            mBethesdaBoneLodGroups[Misc::StringUtils::lowerCase(node->mName)]
                                = static_cast<unsigned int>(groupIndex);
                    }
                }
            }

            const auto* node = dynamic_cast<const Nif::NiNode*>(object);
            if (node == nullptr)
                return;
            for (const auto& child : node->mChildren)
                if (!child.empty())
                    collectBethesdaBoneLodGroups(child.getPtr());
        }

        void loadKf(Nif::FileView nif, SceneUtil::KeyframeHolder& target) const
        {
            const Nif::NiSequenceStreamHelper* seq = nullptr;
            const Nif::NiControllerSequence* controllerSequence = nullptr;
            const size_t numRoots = nif.numRoots();
            for (size_t i = 0; i < numRoots; ++i)
            {
                const Nif::Record* r = nif.getRoot(i);
                if (r && r->recType == Nif::RC_NiSequenceStreamHelper)
                {
                    seq = static_cast<const Nif::NiSequenceStreamHelper*>(r);
                    break;
                }
                if (r && r->recType == Nif::RC_NiControllerSequence)
                    controllerSequence = static_cast<const Nif::NiControllerSequence*>(r);
            }

            if (!seq)
            {
                if (controllerSequence)
                    loadControllerSequenceKf(controllerSequence, target, nif.getFilename());
                else
                    Log(Debug::Warning) << "NIFFile Warning: Found no NiSequenceStreamHelper root record. File: "
                                        << nif.getFilename();
                return;
            }

            Nif::ExtraList extraList = seq->getExtraList();
            if (extraList.empty())
            {
                Log(Debug::Warning) << "NIFFile Warning: NiSequenceStreamHelper has no text keys. File: "
                                    << nif.getFilename();
                return;
            }

            if (extraList[0]->recType != Nif::RC_NiTextKeyExtraData)
            {
                Log(Debug::Warning) << "NIFFile Warning: First extra data was not a NiTextKeyExtraData, but a "
                                    << std::string_view(extraList[0]->recName) << ". File: " << nif.getFilename();
                return;
            }

            auto textKeyExtraData = static_cast<const Nif::NiTextKeyExtraData*>(extraList[0].getPtr());
            extractTextKeys(textKeyExtraData, target.mTextKeys);

            Nif::NiTimeControllerPtr ctrl = seq->mController;
            for (size_t i = 1; i < extraList.size() && !ctrl.empty(); i++, (ctrl = ctrl->mNext))
            {
                Nif::ExtraPtr extra = extraList[i];
                if (extra->recType != Nif::RC_NiStringExtraData || ctrl->recType != Nif::RC_NiKeyframeController)
                {
                    Log(Debug::Warning) << "NIFFile Warning: Unexpected extra data " << extra->recName
                                        << " with controller " << ctrl->recName << ". File: " << nif.getFilename();
                    continue;
                }

                // Vanilla seems to ignore the "active" flag for NiKeyframeController,
                // so we don't want to skip inactive controllers here.

                const Nif::NiStringExtraData* strdata = static_cast<const Nif::NiStringExtraData*>(extra.getPtr());
                const Nif::NiKeyframeController* key = static_cast<const Nif::NiKeyframeController*>(ctrl.getPtr());

                if (key->mData.empty() && key->mInterpolator.empty())
                    continue;

                if (!key->mInterpolator.empty() && key->mInterpolator->recType != Nif::RC_NiTransformInterpolator)
                {
                    Log(Debug::Error) << "Unsupported interpolator type for NiKeyframeController " << key->recIndex
                                      << " in " << mFilename;
                    continue;
                }

                osg::ref_ptr<SceneUtil::KeyframeController> callback = new NifOsg::KeyframeController(key);
                setupController(key, callback, /*animflags*/ 0);

                if (!target.mKeyframeControllers.emplace(strdata->mData, callback).second)
                    Log(Debug::Verbose) << "Controller " << strdata->mData << " present more than once in "
                                        << nif.getFilename() << ", ignoring later version";
            }
        }

        void loadControllerSequenceKf(
            const Nif::NiControllerSequence* sequence, SceneUtil::KeyframeHolder& target,
            const std::filesystem::path& filename) const
        {
            if (!sequence->mTextKeys.empty() && sequence->mTextKeys->recType == Nif::RC_NiTextKeyExtraData)
                extractTextKeys(static_cast<const Nif::NiTextKeyExtraData*>(sequence->mTextKeys.getPtr()),
                    target.mTextKeys);
            synthesizeFalloutTextKeys(sequence, target.mTextKeys, filename);

            unsigned int loaded = 0;
            unsigned int unsupported = 0;
            unsigned int headAnimTracks = 0;
            unsigned int propertyBlocks = 0;
            struct PendingPropertyBlock
            {
                std::string mTargetName;
                std::optional<Nif::NiMaterialColorController::TargetColor> mMaterialTarget;
                std::optional<ExternalTextureTransformRoute> mTextureTarget;
                const Nif::NiInterpolator* mInterpolator = nullptr;
                std::string mControllerId;
            };
            std::vector<PendingPropertyBlock> pendingPropertyBlocks;
            std::unordered_map<std::string, osg::ref_ptr<NifOsg::KeyframeController>> sequenceControllers;
            for (const Nif::ControlledBlock& block : sequence->mControlledBlocks)
            {
                const std::string targetName = resolveControlledBlockTargetName(sequence, block);
                if (targetName.empty() || block.mInterpolator.empty())
                    continue;

                const std::string propertyType = resolveControlledBlockString(
                    sequence, block, block.mPropertyType, &Nif::ControlledBlock::mPropertyTypeOffset);
                const std::string controllerType = resolveControlledBlockString(
                    sequence, block, block.mControllerType, &Nif::ControlledBlock::mControllerTypeOffset);
                const std::string controllerId = resolveControlledBlockString(
                    sequence, block, block.mControllerId, &Nif::ControlledBlock::mControllerIdOffset);
                // Transform and FaceGen controlled blocks also carry a controller type.  A non-empty property type is
                // the discriminator that says this block targets render state rather than the node itself.
                const bool hasPropertyRoute = !propertyType.empty();
                if (Misc::StringUtils::ciEqual(propertyType, "NiMaterialProperty")
                    && Misc::StringUtils::ciEqual(controllerType, "NiMaterialColorController"))
                {
                    const auto targetColor = parseExternalMaterialColorControllerId(controllerId);
                    if (targetColor)
                    {
                        pendingPropertyBlocks.push_back(
                            { targetName, targetColor, std::nullopt, block.mInterpolator.getPtr(), controllerId });
                        continue;
                    }

                    ++unsupported;
                    Log(Debug::Warning) << "Unsupported external NiMaterialColorController route target='"
                                        << targetName << "' id='" << controllerId << "' interpolator="
                                        << block.mInterpolator->recName << " in " << filename;
                    continue;
                }
                if (Misc::StringUtils::ciEqual(propertyType, "NiTexturingProperty")
                    && Misc::StringUtils::ciEqual(controllerType, "NiTextureTransformController"))
                {
                    const auto route = parseExternalTextureTransformControllerId(controllerId);
                    if (route)
                    {
                        pendingPropertyBlocks.push_back(
                            { targetName, std::nullopt, route, block.mInterpolator.getPtr(), controllerId });
                        continue;
                    }

                    ++unsupported;
                    Log(Debug::Warning) << "Unsupported external NiTextureTransformController route target='"
                                        << targetName << "' id='" << controllerId << "' interpolator="
                                        << block.mInterpolator->recName << " in " << filename;
                    continue;
                }
                if (hasPropertyRoute)
                {
                    ++unsupported;
                    if (unsupported <= 8)
                        Log(Debug::Verbose) << "FNV/ESM4 diag: unsupported external KF property route target='"
                                            << targetName << "' property='" << propertyType << "' controller='"
                                            << controllerType << "' id='" << controllerId << "' in " << filename;
                    continue;
                }

                if (std::getenv("OPENMW_FNV_KF_BLOCK_AUDIT") != nullptr)
                {
                    const std::string lowerTarget = Misc::StringUtils::lowerCase(targetName);
                    if (lowerTarget == "bip01" || lowerTarget.find("forearm") != std::string::npos
                        || lowerTarget.find("foretwist") != std::string::npos
                        || lowerTarget.find("hand") != std::string::npos
                        || lowerTarget.find("finger") != std::string::npos
                        || lowerTarget.find("thumb") != std::string::npos
                        || lowerTarget.find("head") != std::string::npos || lowerTarget == "weapon")
                    {
                        Log(Debug::Info) << "FNV/ESM4 KF BLOCK AUDIT file=" << filename.string()
                                         << " sequence=" << sequence->mName
                                         << " target=\"" << targetName << "\" interpolator="
                                         << block.mInterpolator->recName << " recType="
                                         << block.mInterpolator->recType;
                        if (block.mInterpolator->recType == Nif::RC_NiTransformInterpolator)
                        {
                            const auto* transform
                                = static_cast<const Nif::NiTransformInterpolator*>(block.mInterpolator.getPtr());
                            const auto& value = transform->mDefaultValue;
                            Log(Debug::Info) << "FNV/ESM4 KF DEFAULT TRANSFORM file=" << filename.string()
                                             << " sequence=" << sequence->mName << " target=\"" << targetName
                                             << "\" translation=(" << value.mTranslation.x() << ","
                                             << value.mTranslation.y() << "," << value.mTranslation.z()
                                             << ") quaternionXYZW=(" << value.mRotation.x() << ","
                                             << value.mRotation.y() << "," << value.mRotation.z() << ","
                                             << value.mRotation.w() << ") scale=" << value.mScale
                                             << " hasData=" << !transform->mData.empty();
                        }
                        else if (block.mInterpolator->recType == Nif::RC_NiBSplineTransformInterpolator
                            || block.mInterpolator->recType == Nif::RC_NiBSplineCompTransformInterpolator)
                        {
                            const auto* transform
                                = static_cast<const Nif::NiBSplineTransformInterpolator*>(block.mInterpolator.getPtr());
                            const auto& value = transform->mValue;
                            Log(Debug::Info) << "FNV/ESM4 KF BSPLINE TRANSFORM file=" << filename.string()
                                             << " sequence=" << sequence->mName << " target=\"" << targetName
                                             << "\" translation=(" << value.mTranslation.x() << ","
                                             << value.mTranslation.y() << "," << value.mTranslation.z()
                                             << ") quaternionXYZW=(" << value.mRotation.x() << ","
                                             << value.mRotation.y() << "," << value.mRotation.z() << ","
                                             << value.mRotation.w() << ") scale=" << value.mScale
                                             << " translationHandle=" << transform->mTranslationHandle
                                             << " rotationHandle=" << transform->mRotationHandle
                                             << " scaleHandle=" << transform->mScaleHandle;
                            if (block.mInterpolator->recType == Nif::RC_NiBSplineCompTransformInterpolator)
                            {
                                const auto* compact = static_cast<const Nif::NiBSplineCompTransformInterpolator*>(
                                    block.mInterpolator.getPtr());
                                Log(Debug::Info) << "FNV/ESM4 KF BSPLINE COMPRESSION file=" << filename.string()
                                                 << " sequence=" << sequence->mName << " target=\"" << targetName
                                                 << "\" translationOffset=" << compact->mTranslationOffset
                                                 << " translationHalfRange=" << compact->mTranslationHalfRange
                                                 << " rotationOffset=" << compact->mRotationOffset
                                                 << " rotationHalfRange=" << compact->mRotationHalfRange
                                                 << " scaleOffset=" << compact->mScaleOffset
                                                 << " scaleHalfRange=" << compact->mScaleHalfRange;
                                if (lowerTarget == "bip01" && !compact->mSplineData.empty()
                                    && !compact->mBasisData.empty()
                                    && compact->mTranslationHandle != std::numeric_limits<uint16_t>::max())
                                {
                                    const auto& points = compact->mSplineData->mCompactControlPoints;
                                    const uint32_t count = compact->mBasisData->mNumControlPoints;
                                    const auto sample = [&](uint32_t point, uint32_t component) {
                                        const size_t index = static_cast<size_t>(compact->mTranslationHandle)
                                            + static_cast<size_t>(point) * 3 + component;
                                        return index < points.size()
                                            ? compact->mTranslationOffset
                                                + compact->mTranslationHalfRange
                                                    * (static_cast<float>(points[index]) / 32767.f)
                                            : std::numeric_limits<float>::quiet_NaN();
                                    };
                                    if (count > 0)
                                        Log(Debug::Info)
                                            << "FNV/ESM4 KF BSPLINE ROOT CONTROL file=" << filename.string()
                                            << " sequence=" << sequence->mName << " count=" << count
                                            << " first=(" << sample(0, 0) << "," << sample(0, 1) << ","
                                            << sample(0, 2) << ")"
                                            << " second=(" << sample(std::min<uint32_t>(1, count - 1), 0) << ","
                                            << sample(std::min<uint32_t>(1, count - 1), 1) << ","
                                            << sample(std::min<uint32_t>(1, count - 1), 2) << ")"
                                            << " last=(" << sample(count - 1, 0) << "," << sample(count - 1, 1)
                                            << "," << sample(count - 1, 2) << ")";
                                }
                            }
                        }
                    }
                }

                osg::ref_ptr<SceneUtil::KeyframeController> callback;
                osg::ref_ptr<NifOsg::KeyframeController> nifCallback;
                if (block.mInterpolator->recType == Nif::RC_NiTransformInterpolator)
                {
                    nifCallback = new NifOsg::KeyframeController(
                        static_cast<const Nif::NiTransformInterpolator*>(block.mInterpolator.getPtr()));
                    callback = nifCallback;
                }
                else if (block.mInterpolator->recType == Nif::RC_NiBSplineTransformInterpolator
                    || block.mInterpolator->recType == Nif::RC_NiBSplineCompTransformInterpolator)
                {
                    nifCallback = new NifOsg::KeyframeController(
                        static_cast<const Nif::NiBSplineTransformInterpolator*>(block.mInterpolator.getPtr()));
                    callback = nifCallback;
                }
                else if (block.mInterpolator->recType == Nif::RC_NiBlendTransformInterpolator)
                {
                    nifCallback = new NifOsg::KeyframeController(
                        static_cast<const Nif::NiBlendTransformInterpolator*>(block.mInterpolator.getPtr()));
                    callback = nifCallback;
                }
                else if (block.mInterpolator->recType == Nif::RC_NiFloatInterpolator || block.mInterpolator->recType == Nif::RC_NiBoolInterpolator)
                {
                    SceneUtil::KeyframeHolder::FalloutHeadAnimTrack track;
                    if (block.mInterpolator->recType == Nif::RC_NiFloatInterpolator)
                    {
                        const auto* interpolator
                            = static_cast<const Nif::NiFloatInterpolator*>(block.mInterpolator.getPtr());
                        track.mType = SceneUtil::KeyframeHolder::FalloutHeadAnimTrack::Type::Float;
                        track.mDefaultValue = interpolator->mDefaultValue;
                        if (!interpolator->mData.empty() && interpolator->mData->mKeyList)
                        {
                            for (const auto& [time, key] : interpolator->mData->mKeyList->mKeys)
                                track.mKeys.emplace_back(time, key.mValue);
                        }
                    }
                    else if (block.mInterpolator->recType == Nif::RC_NiBoolInterpolator)
                    {
                        const auto* interpolator
                            = static_cast<const Nif::NiBoolInterpolator*>(block.mInterpolator.getPtr());
                        track.mType = SceneUtil::KeyframeHolder::FalloutHeadAnimTrack::Type::Bool;
                        track.mDefaultValue = interpolator->mDefaultValue ? 1.f : 0.f;
                        if (!interpolator->mData.empty() && interpolator->mData->mKeyList)
                        {
                            for (const auto& [time, key] : interpolator->mData->mKeyList->mKeys)
                                track.mKeys.emplace_back(time, key.mValue ? 1.f : 0.f);
                        }
                    }

                    if (target.mFalloutHeadAnimTracks.emplace(targetName, std::move(track)).second)
                        ++headAnimTracks;
                    continue;
                }
                else
                {
                    ++unsupported;
                    if (unsupported <= 8)
                        Log(Debug::Verbose) << "FNV/ESM4 diag: unsupported Fallout KF interpolator target='"
                                         << targetName << "' type=" << block.mInterpolator->recType << " name="
                                         << block.mInterpolator->recName << " in " << filename;
                    continue;
                }

                setupController(sequence, callback, false);
                if (target.mKeyframeControllers.emplace(targetName, callback).second)
                {
                    ++loaded;
                    sequenceControllers.emplace(targetName, nifCallback);
                }
            }

            std::vector<std::string> propertyTargets;
            for (const PendingPropertyBlock& block : pendingPropertyBlocks)
            {
                const auto existing = sequenceControllers.find(block.mTargetName);
                osg::ref_ptr<NifOsg::KeyframeController> callback
                    = existing != sequenceControllers.end() ? existing->second : nullptr;
                const bool propertyOnlyTarget = callback == nullptr;
                if (callback == nullptr)
                {
                    callback = new NifOsg::KeyframeController;
                    setupController(sequence, callback, false);
                }

                const bool added = block.mMaterialTarget
                    ? callback->addMaterialColorChannel(*block.mMaterialTarget, block.mInterpolator)
                    : block.mTextureTarget
                    && callback->addTextureTransformChannel(block.mTextureTarget->mShaderMap,
                        block.mTextureTarget->mTextureSlot, block.mTextureTarget->mTransformMember,
                        block.mInterpolator);
                if (!added)
                {
                    ++unsupported;
                    Log(Debug::Warning) << "Unsupported external KF property interpolator target='"
                                        << block.mTargetName << "' id='" << block.mControllerId << "' in "
                                        << filename;
                    continue;
                }

                if (propertyOnlyTarget)
                {
                    if (!target.mKeyframeControllers.emplace(block.mTargetName, callback).second)
                    {
                        ++unsupported;
                        Log(Debug::Warning) << "Unable to merge external KF property controller target '"
                                            << block.mTargetName << "' in " << filename;
                        continue;
                    }
                    sequenceControllers.emplace(block.mTargetName, callback);
                    ++loaded;
                }

                ++propertyBlocks;
                if (std::find(propertyTargets.begin(), propertyTargets.end(), block.mTargetName)
                    == propertyTargets.end())
                    propertyTargets.push_back(block.mTargetName);
            }

            if (loaded > 0)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: loaded " << loaded
                                 << " Fallout NiControllerSequence KF controller(s) from " << filename;
            }
            else
            {
                std::ostringstream sample;
                const unsigned int sampleCount = std::min<unsigned int>(sequence->mControlledBlocks.size(), 5);
                for (unsigned int i = 0; i < sampleCount; ++i)
                {
                    if (i != 0)
                        sample << " | ";
                    const Nif::ControlledBlock& block = sequence->mControlledBlocks[i];
                    sample << "target='" << block.mTargetName << "' node='" << block.mNodeName << "' controller='"
                           << block.mControllerId << "' interpolator='" << block.mInterpolatorId << "' type="
                           << (block.mInterpolator.empty() ? 0 : block.mInterpolator->recType) << " nodeOffset="
                           << block.mNodeNameOffset << " paletteNode='"
                           << getStringPaletteValue(block.mStringPalette, block.mNodeNameOffset) << "' seqNode='"
                           << getStringPaletteValue(sequence->mStringPalette, block.mNodeNameOffset) << "'";
                }
                Log(Debug::Verbose) << "FNV/ESM4 diag: loaded 0 Fallout NiControllerSequence KF controller(s) from "
                                 << filename << " blocks=" << sequence->mControlledBlocks.size() << " sample=["
                                 << sample.str() << "]";
            }
            if (unsupported > 0)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped " << unsupported
                                 << " unsupported Fallout KF interpolator(s) in " << filename;
            }
            if (headAnimTracks > 0)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: loaded " << headAnimTracks
                                 << " Fallout HeadAnims track(s) from " << filename;
            }
            if (propertyBlocks > 0)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: loaded " << propertyBlocks
                                    << " external Fallout KF property block(s) across "
                                    << propertyTargets.size() << " target(s) from " << filename;
            }
        }

        struct HandleNodeArgs
        {
            unsigned int mNifVersion;
            SceneUtil::TextKeyMap* mTextKeys;
            std::vector<unsigned int> mBoundTextures = {};
            int mAnimFlags = 0;
            bool mSkipMeshes = false;
            bool mHasMarkers = false;
            bool mHasAnimatedParents = false;
            osg::Node* mRootNode = nullptr;
        };

        osg::ref_ptr<osg::Node> load(Nif::FileView nif)
        {
            const size_t numRoots = nif.numRoots();
            std::vector<const Nif::NiAVObject*> roots;
            for (size_t i = 0; i < numRoots; ++i)
            {
                const Nif::Record* r = nif.getRoot(i);
                if (!r)
                    continue;
                const Nif::NiAVObject* nifNode = dynamic_cast<const Nif::NiAVObject*>(r);
                if (nifNode)
                    roots.emplace_back(nifNode);
            }
            if (roots.empty())
                throw Nif::Exception("Found no root nodes", nif.getFilename());

            for (const Nif::NiAVObject* root : roots)
                collectBethesdaBoneLodGroups(root);

            osg::ref_ptr<SceneUtil::TextKeyMapHolder> textkeys(new SceneUtil::TextKeyMapHolder);

            osg::ref_ptr<osg::Group> created(new osg::Group);
            created->setDataVariance(osg::Object::STATIC);
            if (roots.size() == 1 && mVersion == Nif::NIFFile::NIFVersion::VER_BGS && mUserVersion == 11
                && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3
                && isWorldViewerActorMeshPath(Misc::StringUtils::lowerCase(mFilename.generic_string())))
            {
                // Static FO3/FNV face children are instanced beneath Bip01 Head after the
                // model-root transform has been flattened away. Most face pieces need only
                // the common head basis, but scalp-hair roots carry additional authored
                // rotations (HairBun is a full cyclic axis permutation). Preserve the exact
                // 4x4 matrix as root metadata so actor assembly can restore it without a
                // filename allow-list or a bounds-based rotation guess.
                const Nif::NiNode* rootNode = dynamic_cast<const Nif::NiNode*>(roots.front());
                const osg::Matrixf rootTransform = rootNode != nullptr && rootNode->mHasDiscardedRootTransform
                    ? rootNode->mDiscardedRootTransform.toMatrix()
                    : roots.front()->mTransform.toMatrix();
                for (unsigned int row = 0; row < 4; ++row)
                {
                    const osg::Vec4f values(rootTransform(row, 0), rootTransform(row, 1),
                        rootTransform(row, 2), rootTransform(row, 3));
                    created->setUserValue("OpenMW.NifRootTransformRow" + std::to_string(row), values);
                }
            }
            if (mVersion == Nif::NIFFile::NIFVersion::VER_BGS && mUserVersion == 11
                && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3)
            {
                // The SLS selector is a program uniform, so an absent value can retain the
                // preceding drawable's value in GL state.  Establish a scoped zero at every
                // FNV NIF root; oracle-routed child geometries override it locally and OSG
                // restores zero when their state stack unwinds.
                created->getOrCreateStateSet()->addUniform(new osg::Uniform("falloutSlsMode", 0));
            }
            for (const Nif::NiAVObject* root : roots)
            {
                auto node = handleNode(
                    root, nullptr, nullptr, { .mNifVersion = nif.getVersion(), .mTextKeys = &textkeys->mTextKeys });
                std::string nifPrn;
                if (node->getUserValue("OpenMW.NifPrn", nifPrn) && !nifPrn.empty())
                    created->setUserValue("OpenMW.NifPrn", nifPrn);
                created->addChild(node);
            }
            if (mHasNightDayLabel)
                created->getOrCreateUserDataContainer()->addDescription(Constants::NightDayLabel);
            if (mHasHerbalismLabel)
                created->getOrCreateUserDataContainer()->addDescription(Constants::HerbalismLabel);

            // Attach particle emitters to their nodes which should all be loaded by now.
            handleQueuedParticleEmitters(created, nif);
            handleControllerManagers();

            if (nif.getUseSkinning())
            {
                osg::ref_ptr<SceneUtil::Skeleton> skel = new SceneUtil::Skeleton;
                skel->setStateSet(created->getStateSet());
                skel->setName(created->getName());
                skel->setUserDataContainer(created->getUserDataContainer());
                for (unsigned int i = 0; i < created->getNumChildren(); ++i)
                    skel->addChild(created->getChild(i));
                created->removeChildren(0, created->getNumChildren());
                created = skel;
            }

            if (!textkeys->mTextKeys.empty())
                created->getOrCreateUserDataContainer()->addUserObject(textkeys);

            created->setUserValue(Misc::OsgUserValues::sFileHash, nif.getHash());

            return created;
        }

        void applyNodeProperties(const Nif::NiAVObject* nifNode, osg::Node* applyTo,
            SceneUtil::CompositeStateSetUpdater* composite, std::vector<unsigned int>& boundTextures, int animflags)
        {
            bool hasStencilProperty = false;

            for (const auto& property : nifNode->mProperties)
            {
                if (property.empty())
                    continue;

                if (property.getPtr()->recType == Nif::RC_NiStencilProperty)
                {
                    const Nif::NiStencilProperty* stencilprop
                        = static_cast<const Nif::NiStencilProperty*>(property.getPtr());
                    if (stencilprop->mEnabled)
                    {
                        hasStencilProperty = true;
                        break;
                    }
                }
            }

            for (const auto& property : nifNode->mProperties)
            {
                if (!property.empty())
                {
                    // Get the lowest numbered recIndex of the NiTexturingProperty root node.
                    // This is what is overridden when a spell effect "particle texture" is used.
                    if (nifNode->mParents.empty() && !mFoundFirstRootTexturingProperty
                        && property.getPtr()->recType == Nif::RC_NiTexturingProperty)
                    {
                        mFirstRootTextureIndex = property.getPtr()->recIndex;
                        mFoundFirstRootTexturingProperty = true;
                    }
                    else if (property.getPtr()->recType == Nif::RC_NiTexturingProperty)
                    {
                        if (property.getPtr()->recIndex == mFirstRootTextureIndex)
                            applyTo->setUserValue("overrideFx", 1);
                    }
                    handleProperty(property.getPtr(), applyTo, composite, boundTextures, animflags, hasStencilProperty);
                }
            }

            // NiAlphaProperty is handled as a drawable property
            Nif::BSShaderPropertyPtr shaderprop = nullptr;
            if (isTypeNiGeometry(nifNode->recType))
                shaderprop = static_cast<const Nif::NiGeometry*>(nifNode)->mShaderProperty;
            else if (isTypeBSGeometry(nifNode->recType))
                shaderprop = static_cast<const Nif::BSTriShape*>(nifNode)->mShaderProperty;

            if (!shaderprop.empty())
                handleProperty(shaderprop.getPtr(), applyTo, composite, boundTextures, animflags, hasStencilProperty);
        }

        static void setupController(const Nif::NiTimeController* ctrl, SceneUtil::Controller* toSetup, int animflags)
        {
            bool autoPlay = animflags & Nif::NiNode::AnimFlag_AutoPlay;
            if (autoPlay)
                toSetup->setSource(std::make_shared<SceneUtil::FrameTimeSource>());

            toSetup->setFunction(std::make_shared<ControllerFunction>(ctrl));
        }

        static void setupController(
            const Nif::NiControllerSequence* sequence, SceneUtil::Controller* toSetup, bool autoPlay)
        {
            if (autoPlay)
                toSetup->setSource(std::make_shared<SceneUtil::FrameTimeSource>());
            toSetup->setFunction(std::make_shared<ControllerFunction>(sequence->mFrequency, sequence->mPhase,
                sequence->mStartTime, sequence->mStopTime, sequence->mExtrapolationMode));
        }

        osg::Node* findControllerSequenceTarget(
            const Nif::NiControllerManager* manager, const Nif::ControlledBlock& block) const
        {
            const auto findByName = [&](const std::string& name) -> osg::Node* {
                if (name.empty())
                    return nullptr;

                if (!manager->mObjectPalette.empty())
                {
                    const auto& objects = manager->mObjectPalette->mObjects;
                    auto object = objects.find(name);
                    if (object != objects.end() && !object->second.empty())
                    {
                        auto found = mNodesByNif.find(object->second.getPtr());
                        if (found != mNodesByNif.end())
                            return found->second;
                    }
                }

                auto found = mNodesByName.find(Misc::StringUtils::lowerCase(name));
                return found != mNodesByName.end() ? found->second : nullptr;
            };

            if (osg::Node* node = findByName(block.mNodeName))
                return node;
            if (osg::Node* node = findByName(block.mTargetName))
                return node;

            if (!block.mController.empty() && !block.mController->mTarget.empty())
            {
                if (const auto* target = dynamic_cast<const Nif::NiAVObject*>(block.mController->mTarget.getPtr()))
                {
                    auto found = mNodesByNif.find(target);
                    if (found != mNodesByNif.end())
                        return found->second;
                }
            }

            return nullptr;
        }

        void handleControllerManagers() const
        {
            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            const bool falloutFlagPath = isFalloutFlagPath(filename);
            for (const Nif::NiControllerManager* manager : mControllerManagers)
            {
                unsigned int attached = 0;

                for (const auto& sequencePtr : manager->mSequences)
                {
                    if (sequencePtr.empty())
                        continue;

                    const Nif::NiControllerSequence* sequence = sequencePtr.getPtr();
                    if (!shouldAutoplayEmbeddedSequence(*sequence, filename))
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: left embedded NiControllerSequence '" << sequence->mName
                                         << "' dormant in " << mFilename;
                        continue;
                    }

                    for (const Nif::ControlledBlock& block : sequence->mControlledBlocks)
                    {
                        if (block.mInterpolator.empty())
                            continue;

                        if (falloutFlagPath)
                        {
                            std::string blockKey = Misc::StringUtils::lowerCase(block.mTargetName);
                            blockKey += ' ';
                            blockKey += Misc::StringUtils::lowerCase(block.mNodeName);
                            blockKey += ' ';
                            blockKey += Misc::StringUtils::lowerCase(block.mControllerId);
                            blockKey += ' ';
                            blockKey += Misc::StringUtils::lowerCase(block.mInterpolatorId);
                            blockKey += ' ';
                            blockKey += Misc::StringUtils::lowerCase(resolveControlledBlockTargetName(sequence, block));
                            if (std::getenv("OPENMW_FNV_FLAG_CONTROLLER_AUDIT") != nullptr)
                            {
                                Log(Debug::Info) << "FNV/ESM4 flag audit: sequence='" << sequence->mName
                                                 << "' target='" << block.mTargetName << "' node='"
                                                 << block.mNodeName << "' controller='" << block.mControllerId
                                                 << "' interpolator='" << block.mInterpolatorId << "' resolved='"
                                                 << resolveControlledBlockTargetName(sequence, block) << "' in "
                                                 << mFilename;
                            }
                            if (containsAny(blockKey,
                                    { "rootbone", "bip01 root", "root bone", "pole", "staff", "flagpole", "marker",
                                        "base" }))
                            {
                                Log(Debug::Verbose)
                                    << "FNV/ESM4 diag: skipped Fallout flag anchor controller '"
                                    << sequence->mName << "' block '" << block.mNodeName << "' in " << mFilename;
                                continue;
                            }
                        }

                        osg::Node* node = findControllerSequenceTarget(manager, block);
                        if (!node)
                        {
                            Log(Debug::Verbose)
                                << "FNV/ESM4 diag: unable to attach NiControllerSequence '" << sequence->mName
                                << "' block '" << block.mNodeName << "' in " << mFilename;
                            continue;
                        }

                        if (block.mInterpolator->recType == Nif::RC_NiTransformInterpolator)
                        {
                            auto* transform = dynamic_cast<NifOsg::MatrixTransform*>(node);
                            if (!transform)
                            {
                                Log(Debug::Verbose) << "FNV/ESM4 diag: unable to attach transform NiControllerSequence '"
                                                 << sequence->mName << "' block '" << block.mNodeName << "' in "
                                                 << mFilename;
                                continue;
                            }

                            const auto* interp
                                = static_cast<const Nif::NiTransformInterpolator*>(block.mInterpolator.getPtr());
                            osg::ref_ptr<KeyframeController> callback = new KeyframeController(interp);
                            setupController(sequence, callback, true);
                            transform->addUpdateCallback(callback);
                            transform->setDataVariance(osg::Object::DYNAMIC);
                            ++attached;
                        }
                        else if (block.mInterpolator->recType == Nif::RC_NiBoolInterpolator)
                        {
                            const auto* interp = static_cast<const Nif::NiBoolInterpolator*>(block.mInterpolator.getPtr());
                            osg::ref_ptr<VisController> callback = new VisController(interp, Loader::getHiddenNodeMask());
                            setupController(sequence, callback, true);
                            node->addUpdateCallback(callback);
                            node->setDataVariance(osg::Object::DYNAMIC);
                            ++attached;
                        }
                    }
                }

                if (attached > 0)
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: attached " << attached
                                     << " embedded NiControllerSequence transform controller(s) in " << mFilename;
                }
            }
        }

        static osg::ref_ptr<osg::LOD> handleLodNode(const Nif::NiLODNode* niLodNode)
        {
            osg::ref_ptr<osg::LOD> lod(new osg::LOD);
            lod->setName(niLodNode->mName);
            lod->setCenterMode(osg::LOD::USER_DEFINED_CENTER);
            lod->setCenter(niLodNode->mLODCenter);
            for (unsigned int i = 0; i < niLodNode->mLODLevels.size(); ++i)
            {
                const Nif::NiLODNode::LODRange& range = niLodNode->mLODLevels[i];
                lod->setRange(i, range.mMinRange, range.mMaxRange);
            }
            lod->setRangeMode(osg::LOD::DISTANCE_FROM_EYE_POINT);
            return lod;
        }

        static osg::ref_ptr<osg::Switch> handleSwitchNode(const Nif::NiSwitchNode* niSwitchNode)
        {
            osg::ref_ptr<osg::Switch> switchNode(new osg::Switch);
            switchNode->setName(niSwitchNode->mName);
            switchNode->setNewChildDefaultValue(false);
            return switchNode;
        }

        static osg::ref_ptr<osg::Sequence> prepareSequenceNode(const Nif::NiAVObject* nifNode)
        {
            const Nif::NiFltAnimationNode* niFltAnimationNode = static_cast<const Nif::NiFltAnimationNode*>(nifNode);
            osg::ref_ptr<osg::Sequence> sequenceNode(new osg::Sequence);
            sequenceNode->setName(niFltAnimationNode->mName);
            if (!niFltAnimationNode->mChildren.empty())
            {
                if (niFltAnimationNode->swing())
                    sequenceNode->setDefaultTime(
                        niFltAnimationNode->mDuration / (niFltAnimationNode->mChildren.size() * 2));
                else
                    sequenceNode->setDefaultTime(niFltAnimationNode->mDuration / niFltAnimationNode->mChildren.size());
            }
            return sequenceNode;
        }

        void activateSequenceNode(osg::Group* osgNode, const Nif::NiAVObject* nifNode) const
        {
            const Nif::NiFltAnimationNode* niFltAnimationNode = static_cast<const Nif::NiFltAnimationNode*>(nifNode);
            osg::Sequence* sequenceNode = static_cast<osg::Sequence*>(osgNode);

            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            if (!shouldAutoplayFltAnimationNode(*niFltAnimationNode, filename))
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: left NiFltAnimationNode '" << niFltAnimationNode->mName
                                 << "' dormant in " << mFilename;
                return;
            }

            if (niFltAnimationNode->swing())
                sequenceNode->setInterval(osg::Sequence::SWING, 0, -1);
            else
                sequenceNode->setInterval(osg::Sequence::LOOP, 0, -1);
            sequenceNode->setDuration(1.0f, -1);
            sequenceNode->setMode(osg::Sequence::START);
            Log(Debug::Verbose) << "FNV/ESM4 diag: activated NiFltAnimationNode '" << niFltAnimationNode->mName << "' in "
                             << mFilename;
        }

        osg::ref_ptr<osg::Image> handleSourceTexture(const Nif::NiSourceTexture* st) const
        {
            if (st)
            {
                if (st->mExternal)
                    return getTextureImage(st->mFile);

                if (!st->mData.empty())
                    return handleInternalTexture(st->mData.getPtr());
            }

            return nullptr;
        }

        bool handleEffect(const Nif::NiAVObject* nifNode, osg::StateSet* stateset) const
        {
            if (nifNode->recType != Nif::RC_NiTextureEffect)
            {
                Log(Debug::Info) << "Unhandled effect " << nifNode->recName << " in " << mFilename;
                return false;
            }

            const Nif::NiTextureEffect* textureEffect = static_cast<const Nif::NiTextureEffect*>(nifNode);
            if (!textureEffect->mSwitchState)
                return false;

            if (textureEffect->mTextureType != Nif::NiTextureEffect::TextureType::EnvironmentMap)
            {
                Log(Debug::Info) << "Unhandled NiTextureEffect type "
                                 << static_cast<uint32_t>(textureEffect->mTextureType) << " in " << mFilename;
                return false;
            }

            if (textureEffect->mTexture.empty())
            {
                Log(Debug::Info) << "NiTextureEffect missing source texture in " << mFilename;
                return false;
            }

            osg::ref_ptr<osg::TexGen> texGen(new osg::TexGen);
            switch (textureEffect->mCoordGenType)
            {
                case Nif::NiTextureEffect::CoordGenType::WorldParallel:
                    texGen->setMode(osg::TexGen::OBJECT_LINEAR);
                    break;
                case Nif::NiTextureEffect::CoordGenType::WorldPerspective:
                    texGen->setMode(osg::TexGen::EYE_LINEAR);
                    break;
                case Nif::NiTextureEffect::CoordGenType::SphereMap:
                    texGen->setMode(osg::TexGen::SPHERE_MAP);
                    break;
                default:
                    Log(Debug::Info) << "Unhandled NiTextureEffect CoordGenType "
                                     << static_cast<uint32_t>(textureEffect->mCoordGenType) << " in " << mFilename;
                    return false;
            }

            const unsigned int uvSet = 0;
            const unsigned int texUnit = 3; // FIXME
            std::vector<unsigned int> boundTextures;
            boundTextures.resize(3); // Dummy vector for attachNiSourceTexture
            attachNiSourceTexture("envMap", textureEffect->mTexture.getPtr(), textureEffect->wrapS(),
                textureEffect->wrapT(), uvSet, stateset, boundTextures);
            stateset->setTextureAttributeAndModes(texUnit, texGen, osg::StateAttribute::ON);
            stateset->setTextureAttributeAndModes(texUnit, createEmissiveTexEnv(), osg::StateAttribute::ON);

            stateset->addUniform(new osg::Uniform("envMapColor", osg::Vec4f(1, 1, 1, 1)));
            return true;
        }

        // Get a default dataVariance for this node to be used as a hint by optimization (post)routines
        static osg::ref_ptr<osg::Group> createNode(const Nif::NiAVObject* nifNode)
        {
            osg::ref_ptr<osg::Group> node;
            osg::Object::DataVariance dataVariance = osg::Object::UNSPECIFIED;

            switch (nifNode->recType)
            {
                case Nif::RC_NiBillboardNode:
                    dataVariance = osg::Object::DYNAMIC;
                    break;
                default:
                    const bool authoredAttachmentTarget
                        = Misc::StringUtils::ciEqual(nifNode->mName, "Weapon");
                    bool hasAuthoredParent = false;
                    if (nifNode->mParents.empty())
                    {
                        for (const auto& extra : nifNode->getExtraList())
                        {
                            if (extra->recType != Nif::RC_NiStringExtraData)
                                continue;
                            const auto* stringData = static_cast<const Nif::NiStringExtraData*>(extra.getPtr());
                            if (Misc::StringUtils::ciEqual(stringData->mName, "Prn")
                                && !stringData->mData.empty())
                            {
                                hasAuthoredParent = true;
                                break;
                            }
                        }
                    }
                    // The Root node can be created as a Group if no transformation is required.
                    // A Bethesda Prn root remains an external KF target after it is
                    // attached to the declared actor bone, so it must stay transformable
                    // and visible to NodeMapVisitor.
                    if (nifNode->mParents.empty() && nifNode->mController.empty() && nifNode->mTransform.isIdentity()
                        && !hasAuthoredParent)
                        node = new osg::Group;

                    // FO3/FNV animate the empty Weapon node under Bip01 R Hand.
                    // Keeping the authored node dynamic prevents the optimizer
                    // from folding it away and removes the need for a synthetic
                    // replacement transform in actor assembly.
                    dataVariance = nifNode->mIsBone || hasAuthoredParent || authoredAttachmentTarget
                        ? osg::Object::DYNAMIC
                        : osg::Object::STATIC;

                    break;
            }
            if (!node)
                node = new NifOsg::MatrixTransform(nifNode->mTransform);

            node->setDataVariance(dataVariance);

            return node;
        }

        osg::ref_ptr<osg::Node> handleNode(
            const Nif::NiAVObject* nifNode, const Nif::Parent* parent, osg::Group* parentNode, HandleNodeArgs args)
        {
            if (args.mRootNode && Misc::StringUtils::ciEqual(nifNode->mName, "Bounding Box"))
                return nullptr;

            const bool isRootNode = args.mRootNode == nullptr;
            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            const bool isBethesdaActorSkeleton
                = mBethVersion >= Nif::NIFFile::BethVersion::BETHVER_SKY
                && (filename.ends_with("actors/character/characterassets/skeleton.nif")
                    || filename.ends_with("actors\\character\\characterassets\\skeleton.nif")
                    || filename.ends_with("actors/character/character assets/skeleton.nif")
                    || filename.ends_with("actors\\character\\character assets\\skeleton.nif")
                    || filename.ends_with("actors/character/character assets female/skeleton_female.nif")
                    || filename.ends_with("actors\\character\\character assets female\\skeleton_female.nif")
                    || filename.ends_with("actors/character/characterassetsfemale/skeleton_female.nif")
                    || filename.ends_with("actors\\character\\characterassetsfemale\\skeleton_female.nif")
                    || filename.ends_with("actors/human/characterassets/skeleton.nif")
                    || filename.ends_with("actors\\human\\characterassets\\skeleton.nif")
                    || filename.ends_with("actors/human/characterassets/skeleton_facebones.nif")
                    || filename.ends_with("actors\\human\\characterassets\\skeleton_facebones.nif")
                    || filename.ends_with("actors/human/characterassets/female/skeleton.nif")
                    || filename.ends_with("actors\\human\\characterassets\\female\\skeleton.nif")
                    || filename.ends_with("actors/human/characterassets/female/skeleton_facebones.nif")
                    || filename.ends_with("actors\\human\\characterassets\\female\\skeleton_facebones.nif")
                    || filename.ends_with("actors/robot/characterassets/skeleton.nif")
                    || filename.ends_with("actors\\robot\\characterassets\\skeleton.nif")
                    || filename.ends_with("actors/robot/characterassets/mrhandy.nif")
                    || filename.ends_with("actors\\robot\\characterassets\\mrhandy.nif"));
            if (!(args.mAnimFlags & Nif::NiNode::AnimFlag_AutoPlay) && isAmbientEmbeddedAnimationPath(filename))
            {
                args.mAnimFlags |= Nif::NiNode::AnimFlag_AutoPlay;
                if (isRootNode)
                    Log(Debug::Verbose) << "FNV/ESM4 diag: forced ambient controller autoplay for " << mFilename;
            }

            osg::ref_ptr<osg::Group> node = createNode(nifNode);

            const bool preserveFalloutHairSurfaceTransform
                = mVersion == Nif::NIFFile::NIFVersion::VER_BGS && mUserVersion == 11
                && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3
                && containsAny(filename, { "characters/hair/", "characters\\hair\\" })
                && (Misc::StringUtils::ciEqual(nifNode->mName, "Hat")
                    || Misc::StringUtils::ciEqual(nifNode->mName, "NoHat"))
                && dynamic_cast<NifOsg::MatrixTransform*>(node.get()) != nullptr;
            if (preserveFalloutHairSurfaceTransform)
            {
                // Retail discards the hair model root but preserves the authored
                // Hat/NoHat child translation and scale, replacing only that child's
                // rotation with the common +90Y FaceGen basis. Keep this transform
                // out of the static optimizer so actor assembly can perform the same
                // operation on each private instance before applying EGM deltas.
                node->setDataVariance(osg::Object::DYNAMIC);
                node->setUserValue("OpenMW.FalloutHairSurface", true);
            }

            if (nifNode->recType == Nif::RC_NiBillboardNode)
            {
                node->addCullCallback(new BillboardCallback);
            }

            node->setName(nifNode->mName);
            const auto boneLod = mBethesdaBoneLodGroups.find(Misc::StringUtils::lowerCase(nifNode->mName));
            if (boneLod != mBethesdaBoneLodGroups.end())
                node->setUserValue("bethesdaBoneLodGroup", boneLod->second);

            if (parentNode)
                parentNode->addChild(node);

            if (!args.mRootNode)
                args.mRootNode = node;

            // The original NIF record index is used for a variety of features:
            // - finding the correct emitter node for a particle system
            // - establishing connections to the animated collision shapes, which are handled in a separate loader
            // - finding a random child NiNode in NiBspArrayController
            node->setUserValue("recIndex", nifNode->recIndex);
            mNodesByNif[nifNode] = node.get();
            if (!nifNode->mName.empty())
                mNodesByName[Misc::StringUtils::lowerCase(nifNode->mName)] = node.get();

            std::string extraData;

            for (const auto& e : nifNode->getExtraList())
            {
                if (e->recType == Nif::RC_NiTextKeyExtraData && args.mTextKeys)
                {
                    const Nif::NiTextKeyExtraData* tk = static_cast<const Nif::NiTextKeyExtraData*>(e.getPtr());
                    extractTextKeys(tk, *args.mTextKeys);
                }
                else if (e->recType == Nif::RC_NiStringExtraData)
                {
                    const Nif::NiStringExtraData* sd = static_cast<const Nif::NiStringExtraData*>(e.getPtr());

                    constexpr std::string_view extraDataIdentifer = "omw:data";

                    // Bethesda static actor add-ons declare their skeleton parent in
                    // a named NiStringExtraData entry (for example Prn=Bip01 Screen10).
                    // Preserve it so actor assembly can honor the authored parent.
                    if (Misc::StringUtils::ciEqual(sd->mName, "Prn"))
                        node->setUserValue("OpenMW.NifPrn", sd->mData);

                    // String markers may contain important information
                    // affecting the entire subtree of this obj
                    if (sd->mData == "MRK")
                    {
                        // Marker objects. These meshes are only visible in the editor.
                        if (!Loader::getShowMarkers() && args.mRootNode == node)
                            args.mHasMarkers = true;
                    }
                    else if (sd->mData == "BONE")
                    {
                        node->getOrCreateUserDataContainer()->addDescription("CustomBone");
                    }
                    else if (sd->mData.rfind(extraDataIdentifer, 0) == 0)
                    {
                        extraData = sd->mData.substr(extraDataIdentifer.length());
                    }
                }
                else if (e->recType == Nif::RC_BSXFlags)
                {
                    if (args.mRootNode != node)
                        continue;

                    auto bsxFlags = static_cast<const Nif::NiIntegerExtraData*>(e.getPtr());
                    // Marker objects.
                    if (!Loader::getShowMarkers() && (bsxFlags->mData & 32))
                        args.mHasMarkers = true;
                }
            }

            if (nifNode->recType == Nif::RC_NiBSAnimationNode || nifNode->recType == Nif::RC_NiBSParticleNode)
                args.mAnimFlags = nifNode->mFlags;

            if (nifNode->recType == Nif::RC_NiSortAdjustNode)
            {
                auto sortNode = static_cast<const Nif::NiSortAdjustNode*>(nifNode);

                if (sortNode->mSubSorter.empty())
                {
                    Log(Debug::Warning) << "Empty accumulator found in '" << nifNode->recName << "' node "
                                        << nifNode->recIndex;
                }
                else
                {
                    if (mPushedSorter && !mPushedSorter->mSubSorter.empty()
                        && mPushedSorter->mMode != Nif::NiSortAdjustNode::SortingMode::Inherit)
                        mLastAppliedNoInheritSorter = mPushedSorter;
                    mPushedSorter = sortNode;
                }
            }

            // Hide collision shapes, but don't skip the subgraph
            // We still need to animate the hidden bones so the physics system can access them
            if (nifNode->recType == Nif::RC_RootCollisionNode)
            {
                args.mSkipMeshes = true;
                node->setNodeMask(Loader::getHiddenNodeMask());
            }

            // We can skip creating meshes for hidden nodes if they don't have a VisController that
            // might make them visible later
            if (nifNode->isHidden() && !isBethesdaActorSkeleton)
            {
                bool hasVisController = false;
                for (Nif::NiTimeControllerPtr ctrl = nifNode->mController; !ctrl.empty(); ctrl = ctrl->mNext)
                {
                    hasVisController |= (ctrl->recType == Nif::RC_NiVisController);
                    if (hasVisController)
                        break;
                }

                if (!hasVisController)
                    args.mSkipMeshes = true; // skip child meshes, but still create the child node hierarchy for
                                             // animating collision shapes

                node->setNodeMask(Loader::getHiddenNodeMask());
            }
            else if (isBethesdaActorSkeleton)
            {
                // Skyrim, Fallout 4, and Starfield mark authored actor skeleton nodes hidden because they are
                // transforms, not editor geometry. OpenMW's base-only cleanup otherwise interprets that flag as
                // an editor-hidden subtree and prunes most of the rig. Keep these source-authored transforms alive
                // so separately stored skin parts, and FO4's shipped Mr Handy composite, bind by bone name.
                node->setDataVariance(osg::Object::DYNAMIC);
            }

            if (nifNode->recType == Nif::RC_NiCollisionSwitch && !nifNode->collisionActive())
            {
                node->setNodeMask(Loader::getIntersectionDisabledNodeMask());
                // Don't let the optimizer mess with this node
                node->setDataVariance(osg::Object::DYNAMIC);
            }

            osg::ref_ptr<SceneUtil::CompositeStateSetUpdater> composite = new SceneUtil::CompositeStateSetUpdater;

            applyNodeProperties(nifNode, node, composite, args.mBoundTextures, args.mAnimFlags);

            if (nifNode->recType == Nif::RC_NiParticles || nifNode->recType == Nif::RC_NiParticleSystem)
                handleParticleSystem(nifNode, parent, node, composite, args.mAnimFlags);

            const bool isNiGeometry = isTypeNiGeometry(nifNode->recType);
            const bool isBSGeometry = isTypeBSGeometry(nifNode->recType);
            const bool isGeometry = isNiGeometry || isBSGeometry;

            if (isGeometry && !args.mSkipMeshes)
            {
                bool skip = false;
                if (isFalloutHiddenMorphShape(nifNode->mName)
                    && containsAny(filename, { "meshes/characters/", "meshes\\characters\\", "meshes/armor/",
                           "meshes\\armor\\", "characters/", "characters\\", "armor/", "armor\\" }))
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: skipped Fallout hidden morph shape " << nifNode->mName
                                     << " in " << mFilename;
                    skip = true;
                }
                if (args.mNifVersion <= Nif::NIFFile::NIFVersion::VER_MW)
                {
                    skip = skip
                        || (args.mHasMarkers && Misc::StringUtils::ciStartsWith(nifNode->mName, "tri editormarker"))
                        || Misc::StringUtils::ciStartsWith(nifNode->mName, "shadow")
                        || Misc::StringUtils::ciStartsWith(nifNode->mName, "tri shadow");
                }
                else
                {
                    if (args.mHasMarkers)
                        skip = Misc::StringUtils::ciStartsWith(nifNode->mName, "EditorMarker")
                            || Misc::StringUtils::ciStartsWith(nifNode->mName, "VisibilityEditorMarker");
                    if (!skip && args.mNifVersion >= Nif::NIFFile::NIFVersion::VER_BGS
                        && isFalloutHiddenMorphShape(nifNode->mName))
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: skipped Fallout hidden morph shape " << nifNode->mName
                                         << " in " << mFilename;
                        skip = true;
                    }
                }
                if (!skip)
                {
                    if (isNiGeometry)
                        handleNiGeometry(nifNode, parent, node, composite, args.mBoundTextures, args.mAnimFlags,
                            args.mNifVersion);
                    else // isBSGeometry
                        handleBSGeometry(nifNode, parent, node, composite, args.mBoundTextures, args.mAnimFlags);

                    if (!nifNode->mController.empty())
                        handleMeshControllers(nifNode, node, composite, args.mBoundTextures, args.mAnimFlags);
                }
            }

            // Apply any extra effects after processing the nodes children and particle system handling
            if (!extraData.empty())
                handleExtraData(extraData, node);

            if (composite->getNumControllers() > 0)
            {
                osg::Callback* cb = composite;
                if (composite->getNumControllers() == 1)
                    cb = composite->getController(0);
                if (args.mAnimFlags & Nif::NiNode::AnimFlag_AutoPlay)
                    node->addCullCallback(cb);
                else
                    node->addUpdateCallback(
                        cb); // have to remain as UpdateCallback so AssignControllerSourcesVisitor can find it.
            }

            bool isAnimated = false;
            handleNodeControllers(nifNode, node, args.mAnimFlags, isAnimated);
            args.mHasAnimatedParents |= isAnimated;
            // Make sure empty nodes and animated shapes are not optimized away so the physics system can find them.
            if (isAnimated || (args.mHasAnimatedParents && ((args.mSkipMeshes || args.mHasMarkers) || isGeometry)))
                node->setDataVariance(osg::Object::DYNAMIC);

            // LOD and Switch nodes must be wrapped by a transform (the current node) to support transformations
            // properly and we need to attach their children to the osg::LOD/osg::Switch nodes but we must return that
            // transform to the caller of handleNode instead of the actual LOD/Switch nodes.
            osg::ref_ptr<osg::Group> currentNode = node;

            if (nifNode->recType == Nif::RC_NiSwitchNode)
            {
                const Nif::NiSwitchNode* niSwitchNode = static_cast<const Nif::NiSwitchNode*>(nifNode);
                osg::ref_ptr<osg::Switch> switchNode = handleSwitchNode(niSwitchNode);
                node->addChild(switchNode);
                if (niSwitchNode->mName == Constants::NightDayLabel)
                    mHasNightDayLabel = true;
                else if (niSwitchNode->mName == Constants::HerbalismLabel)
                    mHasHerbalismLabel = true;

                currentNode = switchNode;
            }
            else if (nifNode->recType == Nif::RC_NiLODNode)
            {
                const Nif::NiLODNode* niLodNode = static_cast<const Nif::NiLODNode*>(nifNode);
                osg::ref_ptr<osg::LOD> lodNode = handleLodNode(niLodNode);
                node->addChild(lodNode);
                currentNode = lodNode;
            }
            else if (nifNode->recType == Nif::RC_NiFltAnimationNode)
            {
                osg::ref_ptr<osg::Sequence> sequenceNode = prepareSequenceNode(nifNode);
                node->addChild(sequenceNode);
                currentNode = sequenceNode;
            }

            const Nif::NiNode* ninode = dynamic_cast<const Nif::NiNode*>(nifNode);
            if (ninode)
            {
                const Nif::NiAVObjectList& children = ninode->mChildren;
                const Nif::Parent currentParent{ *ninode, parent };
                for (const auto& child : children)
                    if (!child.empty())
                        handleNode(child.getPtr(), &currentParent, currentNode, args);

                // osg::Switch stores one enable bit per existing child. Selecting the
                // NIF's initial child before handleNode has added those children is a
                // no-op, after which setNewChildDefaultValue(false) leaves every child
                // disabled. Update/compile visitors still see those subtrees because
                // they traverse all children, but camera traversal sees none of them.
                // Apply the authored initial selection only after the child list exists.
                if (nifNode->recType == Nif::RC_NiSwitchNode)
                {
                    const auto* niSwitchNode = static_cast<const Nif::NiSwitchNode*>(nifNode);
                    if (osg::Switch* switchNode = currentNode->asSwitch())
                        switchNode->setSingleChildOn(niSwitchNode->mInitialIndex);
                }

                // Propagate effects to the the direct subgraph instead of the node itself
                // This simulates their "affected node list" which Morrowind appears to replace with the subgraph (?)
                // Note that the serialized affected node list is actually unused
                for (const auto& effect : ninode->mEffects)
                    if (!effect.empty())
                    {
                        osg::ref_ptr<osg::StateSet> effectStateSet = new osg::StateSet;
                        if (handleEffect(effect.getPtr(), effectStateSet))
                            for (unsigned int i = 0; i < currentNode->getNumChildren(); ++i)
                                currentNode->getChild(i)->getOrCreateStateSet()->merge(*effectStateSet);
                    }
            }

            if (nifNode->recType == Nif::RC_NiFltAnimationNode)
                activateSequenceNode(currentNode, nifNode);

            return node;
        }

        static void handleMeshControllers(const Nif::NiAVObject* nifNode, osg::Node* node,
            SceneUtil::CompositeStateSetUpdater* composite, const std::vector<unsigned int>& boundTextures,
            int animflags)
        {
            for (Nif::NiTimeControllerPtr ctrl = nifNode->mController; !ctrl.empty(); ctrl = ctrl->mNext)
            {
                if (!ctrl->isActive())
                    continue;
                if (ctrl->recType == Nif::RC_NiUVController)
                {
                    const Nif::NiUVController* niuvctrl = static_cast<const Nif::NiUVController*>(ctrl.getPtr());
                    if (niuvctrl->mData.empty())
                        continue;
                    std::set<unsigned int> texUnits;
                    // UVController should only work for textures which use the given UV Set.
                    for (unsigned int i = 0; i < boundTextures.size(); ++i)
                    {
                        if (boundTextures[i] == niuvctrl->mUvSet)
                            texUnits.insert(i);
                    }

                    osg::ref_ptr<UVController> uvctrl = new UVController(niuvctrl->mData.getPtr(), texUnits);
                    setupController(niuvctrl, uvctrl, animflags);
                    composite->addController(uvctrl);
                }
            }
        }

        void handleNodeControllers(const Nif::NiAVObject* nifNode, osg::Node* node, int animflags, bool& isAnimated)
        {
            for (Nif::NiTimeControllerPtr ctrl = nifNode->mController; !ctrl.empty(); ctrl = ctrl->mNext)
            {
                if (!ctrl->isActive())
                    continue;
                if (ctrl->recType == Nif::RC_NiKeyframeController)
                {
                    const Nif::NiKeyframeController* key = static_cast<const Nif::NiKeyframeController*>(ctrl.getPtr());
                    if (key->mData.empty() && key->mInterpolator.empty())
                        continue;
                    if (!key->mInterpolator.empty() && key->mInterpolator->recType != Nif::RC_NiTransformInterpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for NiKeyframeController " << key->recIndex
                                          << " in " << mFilename << ": " << key->mInterpolator->recName;
                        continue;
                    }
                    osg::ref_ptr<KeyframeController> callback = new KeyframeController(key);
                    setupController(key, callback, animflags);
                    node->addUpdateCallback(callback);
                    isAnimated = true;
                }
                else if (ctrl->recType == Nif::RC_NiPathController)
                {
                    const Nif::NiPathController* path = static_cast<const Nif::NiPathController*>(ctrl.getPtr());
                    if (path->mPathData.empty() || path->mPercentData.empty())
                        continue;
                    osg::ref_ptr<PathController> callback(new PathController(path));
                    setupController(path, callback, animflags);
                    node->addUpdateCallback(callback);
                    isAnimated = true;
                }
                else if (ctrl->recType == Nif::RC_NiVisController)
                {
                    const Nif::NiVisController* visctrl = static_cast<const Nif::NiVisController*>(ctrl.getPtr());
                    if (visctrl->mData.empty() && visctrl->mInterpolator.empty())
                        continue;
                    if (!visctrl->mInterpolator.empty()
                        && visctrl->mInterpolator->recType != Nif::RC_NiBoolInterpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for NiVisController " << visctrl->recIndex
                                          << " in " << mFilename << ": " << visctrl->mInterpolator->recName;
                        continue;
                    }
                    osg::ref_ptr<VisController> callback(new VisController(visctrl, Loader::getHiddenNodeMask()));
                    setupController(visctrl, callback, animflags);
                    node->addUpdateCallback(callback);
                }
                else if (ctrl->recType == Nif::RC_NiRollController)
                {
                    const Nif::NiRollController* rollctrl = static_cast<const Nif::NiRollController*>(ctrl.getPtr());
                    if (rollctrl->mData.empty() && rollctrl->mInterpolator.empty())
                        continue;
                    if (!rollctrl->mInterpolator.empty()
                        && rollctrl->mInterpolator->recType != Nif::RC_NiFloatInterpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for NiRollController " << rollctrl->recIndex
                                          << " in " << mFilename << ": " << rollctrl->mInterpolator->recName;
                        continue;
                    }
                    osg::ref_ptr<RollController> callback = new RollController(rollctrl);
                    setupController(rollctrl, callback, animflags);
                    node->addUpdateCallback(callback);
                    isAnimated = true;
                }
                else if (ctrl->recType == Nif::RC_NiControllerManager)
                {
                    mControllerManagers.push_back(static_cast<const Nif::NiControllerManager*>(ctrl.getPtr()));
                }
                else if (ctrl->recType == Nif::RC_NiGeomMorpherController
                    || ctrl->recType == Nif::RC_NiParticleSystemController
                    || ctrl->recType == Nif::RC_NiBSPArrayController || ctrl->recType == Nif::RC_NiUVController
                    || ctrl->recType == Nif::RC_NiMultiTargetTransformController
                    || isNiPSysControllerRecord(ctrl->recType))
                {
                    // These controllers are handled elsewhere
                }
                else
                    Log(Debug::Info) << "Unhandled controller " << ctrl->recName << " on node " << nifNode->recIndex
                                     << " in " << mFilename;
            }
        }

        void handleMaterialControllers(const Nif::NiProperty* materialProperty,
            SceneUtil::CompositeStateSetUpdater* composite, int animflags, const osg::Material* baseMaterial) const
        {
            for (Nif::NiTimeControllerPtr ctrl = materialProperty->mController; !ctrl.empty(); ctrl = ctrl->mNext)
            {
                if (!ctrl->isActive())
                    continue;
                if (ctrl->recType == Nif::RC_NiAlphaController)
                {
                    const Nif::NiAlphaController* alphactrl = static_cast<const Nif::NiAlphaController*>(ctrl.getPtr());
                    if (alphactrl->mData.empty() && alphactrl->mInterpolator.empty())
                        continue;
                    if (!alphactrl->mInterpolator.empty()
                        && alphactrl->mInterpolator->recType != Nif::RC_NiFloatInterpolator
                        && alphactrl->mInterpolator->recType != Nif::RC_NiBlendFloatInterpolator)
                    {
                        Log(Debug::Error)
                            << "Unsupported interpolator type for NiAlphaController " << alphactrl->recIndex << " in "
                            << mFilename << ": " << alphactrl->mInterpolator->recName;
                        continue;
                    }
                    osg::ref_ptr<AlphaController> osgctrl = new AlphaController(alphactrl, baseMaterial);
                    setupController(alphactrl, osgctrl, animflags);
                    composite->addController(osgctrl);
                }
                else if (ctrl->recType == Nif::RC_NiMaterialColorController)
                {
                    const Nif::NiMaterialColorController* matctrl
                        = static_cast<const Nif::NiMaterialColorController*>(ctrl.getPtr());
                    Nif::NiInterpolatorPtr interp = matctrl->mInterpolator;
                    if (matctrl->mData.empty() && interp.empty())
                        continue;
                    if (mVersion <= Nif::NIFFile::VER_MW
                        && matctrl->mTargetColor == Nif::NiMaterialColorController::TargetColor::Specular)
                        continue;
                    if (!interp.empty() && interp->recType != Nif::RC_NiPoint3Interpolator
                        && interp->recType != Nif::RC_NiBlendPoint3Interpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for NiMaterialColorController "
                                          << matctrl->recIndex << " in " << mFilename << ": " << interp->recName;
                        continue;
                    }
                    osg::ref_ptr<MaterialColorController> osgctrl = new MaterialColorController(matctrl, baseMaterial);
                    setupController(matctrl, osgctrl, animflags);
                    composite->addController(osgctrl);
                }
                else if (ctrl->recType == Nif::RC_BSMaterialEmittanceMultController)
                {
                    const Nif::NiFloatInterpController* emctrl
                        = static_cast<const Nif::NiFloatInterpController*>(ctrl.getPtr());
                    Nif::NiInterpolatorPtr interp = emctrl->mInterpolator;
                    if (interp.empty())
                        continue;
                    if (interp->recType != Nif::RC_NiFloatInterpolator
                        && interp->recType != Nif::RC_NiBlendFloatInterpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for BSMaterialEmittanceMultController "
                                          << emctrl->recIndex << " in " << mFilename << ": " << interp->recName;
                        continue;
                    }
                    osg::ref_ptr<MaterialEmittanceMultController> osgctrl
                        = new MaterialEmittanceMultController(emctrl, baseMaterial);
                    setupController(emctrl, osgctrl, animflags);
                    composite->addController(osgctrl);
                }
                else
                    Log(Debug::Info) << "Unexpected material controller " << ctrl->recType << " in " << mFilename;
            }
        }

        osg::ref_ptr<osg::Image> getTextureImage(std::string_view path) const
        {
            if (!mImageManager)
                return nullptr;

            return mImageManager->getImage(
                VFS::Path::toNormalized(Misc::ResourceHelpers::correctTexturePath(path, mImageManager->getVFS())));
        }

        static osg::ref_ptr<osg::Texture2D> attachTexture(const std::string& name, osg::ref_ptr<osg::Image> image,
            bool wrapS, bool wrapT, unsigned int uvSet, osg::StateSet* stateset,
            std::vector<unsigned int>& boundTextures)
        {
            osg::ref_ptr<osg::Texture2D> texture2d = new osg::Texture2D(image);
            if (image)
                texture2d->setTextureSize(image->s(), image->t());
            texture2d->setWrap(osg::Texture::WRAP_S, wrapS ? osg::Texture::REPEAT : osg::Texture::CLAMP_TO_EDGE);
            texture2d->setWrap(osg::Texture::WRAP_T, wrapT ? osg::Texture::REPEAT : osg::Texture::CLAMP_TO_EDGE);
            unsigned int texUnit = boundTextures.size();
            if (stateset)
            {
                stateset->setTextureAttributeAndModes(texUnit, texture2d, osg::StateAttribute::ON);
                stateset->setTextureAttributeAndModes(
                    texUnit, new osg::TexEnv(osg::TexEnv::MODULATE), osg::StateAttribute::ON);
                osg::ref_ptr<SceneUtil::TextureType> textureType = new SceneUtil::TextureType(name);
                textureType = shareAttribute(textureType);
                stateset->setTextureAttributeAndModes(texUnit, textureType, osg::StateAttribute::ON);
            }
            boundTextures.emplace_back(uvSet);
            return texture2d;
        }

        static osg::ref_ptr<osg::Image> getNeutralFaceGenImage(bool bodyColor)
        {
            static osg::ref_ptr<osg::Image> faceDetail;
            static osg::ref_ptr<osg::Image> bodyTint;
            osg::ref_ptr<osg::Image>& image = bodyColor ? bodyTint : faceDetail;
            if (image == nullptr)
            {
                image = new osg::Image;
                image->allocateImage(1, 1, 1, GL_RGBA, GL_FLOAT);
                const float neutral = bodyColor ? 0.25f : 0.5f;
                image->setColor(osg::Vec4f(neutral, neutral, neutral, 1.f), 0, 0, 0);
                image->setFileName(bodyColor ? "runtime/fallout/neutral-facegen1"
                                             : "runtime/fallout/neutral-facegen0");
            }
            return image;
        }

        static void attachTextureAtUnit(const std::string& name, osg::Image* image, unsigned int unit,
            unsigned int uvSet, osg::StateSet* stateset, std::vector<unsigned int>& boundTextures)
        {
            if (stateset == nullptr || image == nullptr)
                return;
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setTextureSize(image->s(), image->t());
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            stateset->setTextureAttributeAndModes(unit, texture, osg::StateAttribute::ON);
            stateset->setTextureAttributeAndModes(
                unit, new osg::TexEnv(osg::TexEnv::MODULATE), osg::StateAttribute::ON);
            stateset->setTextureAttributeAndModes(
                unit, new SceneUtil::TextureType(name), osg::StateAttribute::ON);
            if (boundTextures.size() <= unit)
                boundTextures.resize(unit + 1, uvSet);
            boundTextures[unit] = uvSet;
        }

        osg::ref_ptr<osg::Texture2D> attachExternalTexture(const std::string& name, const std::string& path, bool wrapS,
            bool wrapT, unsigned int uvSet, osg::StateSet* stateset, std::vector<unsigned int>& boundTextures) const
        {
            osg::ref_ptr<osg::Image> image;
            VFS::Path::Normalized normalizedPath;
            if (mImageManager)
            {
                const std::string loweredPath = Misc::StringUtils::lowerCase(path);
                if (loweredPath.size() >= 4 && loweredPath.compare(loweredPath.size() - 4, 4, ".png") == 0)
                    normalizedPath = VFS::Path::toNormalized(path);
                else
                    normalizedPath = VFS::Path::toNormalized(
                        Misc::ResourceHelpers::correctTexturePath(path, mImageManager->getVFS()));
                image = mImageManager->getImage(normalizedPath);
            }
            if (worldViewerMeshLoadTelemetryEnabled())
            {
                Log(Debug::Info) << "World viewer texture ledger: file=\"" << mFilename.generic_string()
                                 << "\" role=\"" << name << "\""
                                 << " path=\"" << path << "\""
                                 << " normalized=\"" << normalizedPath.value() << "\""
                                 << " image=" << (image != nullptr)
                                 << " width=" << (image != nullptr ? image->s() : 0)
                                 << " height=" << (image != nullptr ? image->t() : 0)
                                 << " uvSet=" << uvSet;
            }
            if (image == nullptr)
                return nullptr;
            return attachTexture(name, image, wrapS, wrapT, uvSet, stateset, boundTextures);
        }

        osg::ref_ptr<osg::Texture2D> attachNiSourceTexture(const std::string& name, const Nif::NiSourceTexture* st,
            bool wrapS, bool wrapT, unsigned int uvSet, osg::StateSet* stateset,
            std::vector<unsigned int>& boundTextures) const
        {
            return attachTexture(name, handleSourceTexture(st), wrapS, wrapT, uvSet, stateset, boundTextures);
        }

        static void clearBoundTextures(osg::StateSet* stateset, std::vector<unsigned int>& boundTextures)
        {
            if (!boundTextures.empty())
            {
                for (unsigned int i = 0; i < boundTextures.size(); ++i)
                    stateset->setTextureMode(i, GL_TEXTURE_2D, osg::StateAttribute::OFF);
                boundTextures.clear();
            }
        }

        void handleTextureControllers(const Nif::NiProperty* texProperty,
            SceneUtil::CompositeStateSetUpdater* composite, osg::StateSet* stateset, int animflags,
            const std::vector<int>& textureSlotToUnit = {}) const
        {
            struct TextureTransformGroup
            {
                int mTextureSlot = 0;
                unsigned int mTextureUnit = 0;
                bool mShaderMap = false;
                std::vector<const Nif::NiTextureTransformController*> mControllers;
                const Nif::NiTextureTransformController* mTimingController = nullptr;
            };

            std::vector<TextureTransformGroup> transformGroups;
            for (Nif::NiTimeControllerPtr ctrl = texProperty->mController; !ctrl.empty(); ctrl = ctrl->mNext)
            {
                if (!ctrl->isActive())
                    continue;
                if (ctrl->recType == Nif::RC_NiFlipController)
                {
                    const Nif::NiFlipController* flipctrl = static_cast<const Nif::NiFlipController*>(ctrl.getPtr());
                    if (!flipctrl->mInterpolator.empty()
                        && flipctrl->mInterpolator->recType != Nif::RC_NiFloatInterpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for NiFlipController " << flipctrl->recIndex
                                          << " in " << mFilename << ": " << flipctrl->mInterpolator->recName;
                        continue;
                    }
                    std::vector<osg::ref_ptr<osg::Texture2D>> textures;

                    // inherit wrap settings from the target slot
                    osg::Texture2D* inherit
                        = dynamic_cast<osg::Texture2D*>(stateset->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
                    osg::Texture2D::WrapMode wrapS = osg::Texture2D::REPEAT;
                    osg::Texture2D::WrapMode wrapT = osg::Texture2D::REPEAT;
                    if (inherit)
                    {
                        wrapS = inherit->getWrap(osg::Texture2D::WRAP_S);
                        wrapT = inherit->getWrap(osg::Texture2D::WRAP_T);
                    }

                    const unsigned int uvSet = 0;
                    std::vector<unsigned int> boundTextures; // Dummy list for attachTexture
                    for (const auto& source : flipctrl->mSources)
                    {
                        if (source.empty())
                            continue;

                        // NB: not changing the stateset
                        osg::ref_ptr<osg::Texture2D> texture
                            = attachNiSourceTexture({}, source.getPtr(), wrapS, wrapT, uvSet, nullptr, boundTextures);
                        textures.push_back(texture);
                    }
                    osg::ref_ptr<FlipController> callback(new FlipController(flipctrl, textures));
                    setupController(ctrl.getPtr(), callback, animflags);
                    composite->addController(callback);
                }
                else if (ctrl->recType == Nif::RC_NiTextureTransformController)
                {
                    const Nif::NiTextureTransformController* transformCtrl
                        = static_cast<const Nif::NiTextureTransformController*>(ctrl.getPtr());
                    if (!transformCtrl->mInterpolator.empty()
                        && transformCtrl->mInterpolator->recType != Nif::RC_NiFloatInterpolator
                        && transformCtrl->mInterpolator->recType != Nif::RC_NiBlendFloatInterpolator)
                    {
                        Log(Debug::Error) << "Unsupported interpolator type for NiTextureTransformController "
                                          << transformCtrl->recIndex << " in " << mFilename << ": "
                                          << transformCtrl->mInterpolator->recName;
                        continue;
                    }

                    const int textureSlot = static_cast<int>(transformCtrl->mTexSlot);
                    unsigned int textureUnit = 0;
                    if (textureSlot >= 0 && static_cast<std::size_t>(textureSlot) < textureSlotToUnit.size()
                        && textureSlotToUnit[textureSlot] >= 0)
                        textureUnit = static_cast<unsigned int>(textureSlotToUnit[textureSlot]);

                    if (transformCtrl->mTransformMember >= 5)
                    {
                        Log(Debug::Error) << "Unsupported transform member for NiTextureTransformController "
                                          << transformCtrl->recIndex << " in " << mFilename << ": "
                                          << transformCtrl->mTransformMember;
                        continue;
                    }

                    auto group = std::find_if(transformGroups.begin(), transformGroups.end(),
                        [textureSlot, textureUnit, shaderMap = transformCtrl->mShaderMap](const auto& candidate) {
                            return candidate.mTextureSlot == textureSlot && candidate.mTextureUnit == textureUnit
                                && candidate.mShaderMap == shaderMap;
                        });
                    if (group == transformGroups.end())
                    {
                        transformGroups.push_back({ textureSlot, textureUnit, transformCtrl->mShaderMap });
                        group = std::prev(transformGroups.end());
                    }
                    group->mControllers.push_back(transformCtrl);
                    if (group->mTimingController == nullptr
                        || transformCtrl->mTimeStop > group->mTimingController->mTimeStop)
                        group->mTimingController = transformCtrl;
                }
                else
                    Log(Debug::Info) << "Unexpected texture controller " << ctrl->recName << " in " << mFilename;
            }

            const Nif::NiTexturingProperty* texturingProperty
                = texProperty->recType == Nif::RC_NiTexturingProperty
                ? static_cast<const Nif::NiTexturingProperty*>(texProperty)
                : nullptr;
            for (const TextureTransformGroup& group : transformGroups)
            {
                const Nif::NiTextureTransform* defaultTransform = nullptr;
                if (texturingProperty != nullptr && group.mTextureSlot >= 0)
                {
                    const auto& textures = group.mShaderMap ? texturingProperty->mShaderTextures
                                                           : texturingProperty->mTextures;
                    const std::size_t slot = static_cast<std::size_t>(group.mTextureSlot);
                    if (slot < textures.size() && textures[slot].mHasTransform)
                        defaultTransform = &textures[slot].mTransform;
                }

                osg::ref_ptr<TextureTransformController> callback
                    = new TextureTransformController(group.mControllers, group.mTextureUnit, defaultTransform);
                setupController(group.mTimingController, callback, animflags);
                composite->addController(callback);
                Log(Debug::Verbose) << "FNV/ESM4 diag: attached aggregate NiTextureTransformController channels="
                                    << group.mControllers.size() << " slot=" << group.mTextureSlot
                                    << " unit=" << group.mTextureUnit << " shaderMap=" << group.mShaderMap
                                    << " authoredBase=" << (defaultTransform != nullptr) << " in " << mFilename;
            }
        }

        void handleParticlePrograms(Nif::NiParticleModifierPtr modifier, Nif::NiParticleModifierPtr collider,
            osg::Group* attachTo, osgParticle::ParticleSystem* partsys,
            osgParticle::ParticleProcessor::ReferenceFrame rf) const
        {
            osgParticle::ModularProgram* program = new osgParticle::ModularProgram;
            attachTo->addChild(program);
            program->setParticleSystem(partsys);
            program->setReferenceFrame(rf);
            for (; !modifier.empty(); modifier = modifier->mNext)
            {
                if (modifier->recType == Nif::RC_NiParticleGrowFade)
                {
                    const Nif::NiParticleGrowFade* gf = static_cast<const Nif::NiParticleGrowFade*>(modifier.getPtr());
                    program->addOperator(new GrowFadeAffector(gf->mGrowTime, gf->mFadeTime));
                }
                else if (modifier->recType == Nif::RC_NiGravity)
                {
                    const Nif::NiGravity* gr = static_cast<const Nif::NiGravity*>(modifier.getPtr());
                    program->addOperator(new GravityAffector(gr));
                }
                else if (modifier->recType == Nif::RC_NiParticleBomb)
                {
                    auto bomb = static_cast<const Nif::NiParticleBomb*>(modifier.getPtr());
                    osg::ref_ptr<osgParticle::ModularProgram> bombProgram(new osgParticle::ModularProgram);
                    attachTo->addChild(bombProgram);
                    bombProgram->setParticleSystem(partsys);
                    bombProgram->setReferenceFrame(rf);
                    bombProgram->setStartTime(bomb->mStartTime);
                    bombProgram->setLifeTime(bomb->mDuration);
                    bombProgram->setEndless(false);
                    bombProgram->addOperator(new ParticleBomb(bomb));
                }
                else if (modifier->recType == Nif::RC_NiParticleColorModifier)
                {
                    const Nif::NiParticleColorModifier* cl
                        = static_cast<const Nif::NiParticleColorModifier*>(modifier.getPtr());
                    if (cl->mData.empty())
                        continue;
                    const Nif::NiColorData* clrdata = cl->mData.getPtr();
                    program->addOperator(new ParticleColorAffector(clrdata));
                }
                else if (modifier->recType == Nif::RC_NiParticleRotation)
                {
                    // unused
                }
                else
                    Log(Debug::Info) << "Unhandled particle modifier " << modifier->recName << " in " << mFilename;
            }
            for (; !collider.empty(); collider = collider->mNext)
            {
                if (collider->recType == Nif::RC_NiPlanarCollider)
                {
                    const Nif::NiPlanarCollider* planarcollider
                        = static_cast<const Nif::NiPlanarCollider*>(collider.getPtr());
                    program->addOperator(new PlanarCollider(planarcollider));
                }
                else if (collider->recType == Nif::RC_NiSphericalCollider)
                {
                    const Nif::NiSphericalCollider* sphericalcollider
                        = static_cast<const Nif::NiSphericalCollider*>(collider.getPtr());
                    program->addOperator(new SphericalCollider(sphericalcollider));
                }
                else
                    Log(Debug::Info) << "Unhandled particle collider " << collider->recName << " in " << mFilename;
            }
        }

        // Load the initial state of the particle system, i.e. the initial particles and their positions, velocity and
        // colors.
        static void handleParticleInitialState(
            const Nif::NiAVObject* nifNode, ParticleSystem* partsys, const Nif::NiParticleSystemController* partctrl)
        {
            auto particleNode = static_cast<const Nif::NiParticles*>(nifNode);
            if (particleNode->mData.empty())
            {
                partsys->setQuota(partctrl->mParticles.size());
                return;
            }

            auto particledata = static_cast<const Nif::NiParticlesData*>(particleNode->mData.getPtr());
            partsys->setQuota(particledata->mNumParticles);

            osg::BoundingBox box;

            int i = 0;
            for (const auto& particle : partctrl->mParticles)
            {
                if (i++ >= particledata->mActiveCount)
                    break;

                if (particle.mLifespan <= 0)
                    continue;

                if (particle.mCode >= particledata->mVertices.size())
                    continue;

                ParticleAgeSetter particletemplate(std::max(0.f, particle.mAge));

                osgParticle::Particle* created = partsys->createParticle(&particletemplate);
                created->setLifeTime(particle.mLifespan);

                // Note this position and velocity is not correct for a particle system with absolute reference frame,
                // which can not be done in this loader since we are not attached to the scene yet. Will be fixed up
                // post-load in the SceneManager.
                created->setVelocity(particle.mVelocity);
                const osg::Vec3f& position = particledata->mVertices[particle.mCode];
                created->setPosition(position);

                created->setColorRange(osgParticle::rangev4(partctrl->mInitialColor, partctrl->mInitialColor));
                created->setAlphaRange(osgParticle::rangef(1.f, 1.f));

                float size = partctrl->mInitialSize;
                if (particle.mCode < particledata->mSizes.size())
                    size *= particledata->mSizes[particle.mCode];

                created->setSizeRange(osgParticle::rangef(size, size));
                box.expandBy(osg::BoundingSphere(position, size));
            }

            // radius may be used to force a larger bounding box
            box.expandBy(osg::BoundingSphere(osg::Vec3(0, 0, 0), particledata->mBoundingSphere.radius()));

            partsys->setInitialBound(box);
        }

        static osg::ref_ptr<Emitter> handleParticleEmitter(const Nif::NiParticleSystemController* partctrl)
        {
            std::vector<int> targets;
            if (partctrl->recType == Nif::RC_NiBSPArrayController && !partctrl->emitAtVertex())
            {
                getAllNiNodes(partctrl->mEmitter.getPtr(), targets);
            }

            osg::ref_ptr<Emitter> emitter = new Emitter(targets);

            osgParticle::ConstantRateCounter* counter = new osgParticle::ConstantRateCounter;
            if (partctrl->noAutoAdjust())
                counter->setNumberOfParticlesPerSecondToCreate(partctrl->mBirthRate);
            else if (partctrl->mLifetime == 0 && partctrl->mLifetimeVariation == 0)
                counter->setNumberOfParticlesPerSecondToCreate(0);
            else
                counter->setNumberOfParticlesPerSecondToCreate(
                    partctrl->mParticles.size() / (partctrl->mLifetime + partctrl->mLifetimeVariation / 2));

            emitter->setCounter(counter);

            ParticleShooter* shooter = new ParticleShooter(partctrl->mSpeed - partctrl->mSpeedVariation * 0.5f,
                partctrl->mSpeed + partctrl->mSpeedVariation * 0.5f, partctrl->mPlanarAngle,
                partctrl->mPlanarAngleVariation, partctrl->mDeclination, partctrl->mDeclinationVariation,
                partctrl->mLifetime, partctrl->mLifetimeVariation);
            emitter->setShooter(shooter);
            emitter->setFlags(partctrl->mFlags);

            if (partctrl->recType == Nif::RC_NiBSPArrayController && partctrl->emitAtVertex())
            {
                emitter->setGeometryEmitterTarget(partctrl->mEmitter->recIndex);
            }
            else
            {
                osgParticle::BoxPlacer* placer = new osgParticle::BoxPlacer;
                placer->setXRange(-partctrl->mEmitterDimensions.x() / 2.f, partctrl->mEmitterDimensions.x() / 2.f);
                placer->setYRange(-partctrl->mEmitterDimensions.y() / 2.f, partctrl->mEmitterDimensions.y() / 2.f);
                placer->setZRange(-partctrl->mEmitterDimensions.z() / 2.f, partctrl->mEmitterDimensions.z() / 2.f);
                emitter->setPlacer(placer);
            }

            return emitter;
        }

        static const Nif::NiPSysEmitter* findModernParticleEmitter(const Nif::NiParticleSystem* particleSystem)
        {
            for (const auto& modifier : particleSystem->mModifiers)
            {
                if (!modifier.empty() && isNiPSysEmitterRecord(modifier->recType))
                    return static_cast<const Nif::NiPSysEmitter*>(modifier.getPtr());
            }

            return nullptr;
        }

        static const Nif::NiAVObject* getModernEmitterObject(const Nif::NiPSysEmitter* emitter)
        {
            if (!emitter)
                return nullptr;

            if (emitter->recType == Nif::RC_NiPSysBoxEmitter || emitter->recType == Nif::RC_NiPSysCylinderEmitter
                || emitter->recType == Nif::RC_NiPSysSphereEmitter || emitter->recType == Nif::RC_BSPSysArrayEmitter)
            {
                const auto* volumeEmitter = static_cast<const Nif::NiPSysVolumeEmitter*>(emitter);
                if (!volumeEmitter->mEmitterObject.empty())
                    return volumeEmitter->mEmitterObject.getPtr();
            }
            else if (emitter->recType == Nif::RC_NiPSysMeshEmitter)
            {
                const auto* meshEmitter = static_cast<const Nif::NiPSysMeshEmitter*>(emitter);
                if (!meshEmitter->mEmitterMeshes.empty() && !meshEmitter->mEmitterMeshes.front().empty())
                    return meshEmitter->mEmitterMeshes.front().getPtr();
            }

            return nullptr;
        }

        static unsigned int getModernParticleQuota(const Nif::NiParticleSystem* particleSystem)
        {
            if (!particleSystem->mData.empty())
            {
                const auto* data = dynamic_cast<const Nif::NiParticlesData*>(particleSystem->mData.getPtr());
                if (data && data->mNumParticles > 0)
                    return data->mNumParticles;
            }

            return 128;
        }

        struct ModernParticleControllerChannels
        {
            const Nif::NiControllerSequence* mRateSequence{ nullptr };
            const Nif::NiFloatInterpolator* mRate{ nullptr };
            const Nif::NiControllerSequence* mActiveSequence{ nullptr };
            const Nif::NiBoolInterpolator* mActive{ nullptr };
            const Nif::NiControllerSequence* mAlphaSequence{ nullptr };
            const Nif::NiFloatInterpolator* mAlpha{ nullptr };
        };

        ModernParticleControllerChannels findModernParticleControllerChannels(
            const Nif::NiParticleSystem* particleSystem, const Nif::NiPSysEmitter* emitter) const
        {
            ModernParticleControllerChannels result;
            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());

            for (const Nif::NiControllerManager* manager : mControllerManagers)
            {
                for (const auto& sequencePtr : manager->mSequences)
                {
                    if (sequencePtr.empty())
                        continue;
                    const Nif::NiControllerSequence* sequence = sequencePtr.getPtr();
                    if (!shouldAutoplayEmbeddedSequence(*sequence, filename))
                        continue;

                    for (const Nif::ControlledBlock& block : sequence->mControlledBlocks)
                    {
                        if (!Misc::StringUtils::ciEqual(
                                resolveControlledBlockTargetName(sequence, block), particleSystem->mName))
                            continue;

                        const std::string controllerType = resolveControlledBlockString(sequence, block,
                            block.mControllerType, &Nif::ControlledBlock::mControllerTypeOffset);
                        const std::string controllerId = resolveControlledBlockString(sequence, block,
                            block.mControllerId, &Nif::ControlledBlock::mControllerIdOffset);
                        const std::string interpolatorId = resolveControlledBlockString(sequence, block,
                            block.mInterpolatorId, &Nif::ControlledBlock::mInterpolatorIdOffset);

                        if (Misc::StringUtils::ciEqual(controllerType, "NiPSysEmitterCtlr")
                            && Misc::StringUtils::ciEqual(controllerId, emitter->mName)
                            && Misc::StringUtils::ciEqual(interpolatorId, "BirthRate"))
                        {
                            if (const auto* value
                                = dynamic_cast<const Nif::NiFloatInterpolator*>(block.mInterpolator.getPtr()))
                            {
                                result.mRateSequence = sequence;
                                result.mRate = value;
                            }
                        }
                        else if (Misc::StringUtils::ciEqual(controllerType, "NiPSysEmitterCtlr")
                            && Misc::StringUtils::ciEqual(controllerId, emitter->mName)
                            && Misc::StringUtils::ciEqual(interpolatorId, "EmitterActive"))
                        {
                            if (const auto* value
                                = dynamic_cast<const Nif::NiBoolInterpolator*>(block.mInterpolator.getPtr()))
                            {
                                result.mActiveSequence = sequence;
                                result.mActive = value;
                            }
                        }
                        else if (Misc::StringUtils::ciEqual(controllerType, "NiAlphaController"))
                        {
                            if (const auto* value
                                = dynamic_cast<const Nif::NiFloatInterpolator*>(block.mInterpolator.getPtr()))
                            {
                                result.mAlphaSequence = sequence;
                                result.mAlpha = value;
                            }
                        }
                    }
                }
            }

            return result;
        }

        static float getInitialModernParticleRate(
            const ModernParticleControllerChannels& channels, const Nif::NiPSysEmitter* emitter, unsigned int quota)
        {
            if (channels.mRateSequence && channels.mRate)
            {
                FloatInterpolator interpolator(channels.mRate);
                ControllerFunction function(channels.mRateSequence->mFrequency, channels.mRateSequence->mPhase,
                    channels.mRateSequence->mStartTime, channels.mRateSequence->mStopTime,
                    channels.mRateSequence->mExtrapolationMode);
                const float authoredRate = interpolator.interpKey(function.calculate(0.f));
                if (std::isfinite(authoredRate) && authoredRate >= 0.f)
                    return authoredRate;
            }

            const float lifetime = emitter && emitter->mLifespan > 0.f ? emitter->mLifespan : 2.f;
            return std::clamp(static_cast<float>(quota) / lifetime, 1.f, 256.f);
        }

        static osg::Matrixf getNifObjectWorldTransform(const Nif::NiAVObject* object)
        {
            osg::Matrixf result;
            if (!object)
                return result;

            result = object->mTransform.toMatrix();
            const Nif::NiAVObject* current = object;
            while (!current->mParents.empty() && current->mParents.front())
            {
                current = current->mParents.front();
                result *= current->mTransform.toMatrix();
            }
            return result;
        }

        static osg::Matrixf getModernModifierObjectToParticleSystem(
            const Nif::NiAVObject* object, const Nif::NiParticleSystem* particleSystem)
        {
            const osg::Matrixf objectToWorld = getNifObjectWorldTransform(object);
            const osg::Matrixf particleSystemToWorld = getNifObjectWorldTransform(particleSystem);
            osg::Matrixf worldToParticleSystem;
            worldToParticleSystem.invert(particleSystemToWorld);
            return objectToWorld * worldToParticleSystem;
        }

        static osg::ref_ptr<Emitter> handleModernParticleEmitter(
            const Nif::NiPSysEmitter* emitter, float rate)
        {
            osg::ref_ptr<Emitter> osgEmitter = new Emitter({});

            osgParticle::ConstantRateCounter* counter = new osgParticle::ConstantRateCounter;
            const float lifetime = emitter && emitter->mLifespan > 0.f ? emitter->mLifespan : 2.f;
            counter->setNumberOfParticlesPerSecondToCreate(rate);
            osgEmitter->setCounter(counter);

            const float speed = emitter ? emitter->mSpeed : 0.f;
            const float speedVariation = emitter ? emitter->mSpeedVariation : 0.f;
            const float planarAngle = emitter ? emitter->mPlanarAngle : 0.f;
            const float planarAngleVariation = emitter ? emitter->mPlanarAngleVariation : osg::PI * 2.f;
            const float declination = emitter ? emitter->mDeclination : 0.f;
            const float declinationVariation = emitter ? emitter->mDeclinationVariation : osg::PI;
            const float lifespanVariation = emitter ? emitter->mLifespanVariation : 0.f;
            ParticleShooter* shooter = new ParticleShooter(speed - speedVariation * 0.5f,
                speed + speedVariation * 0.5f, planarAngle, planarAngleVariation, declination, declinationVariation,
                lifetime, lifespanVariation);
            osgEmitter->setShooter(shooter);
            if (emitter)
                osgEmitter->setParticleRadius(emitter->mInitialRadius, emitter->mRadiusVariation);

            osgParticle::BoxPlacer* placer = new osgParticle::BoxPlacer;
            if (emitter && emitter->recType == Nif::RC_NiPSysBoxEmitter)
            {
                const auto* box = static_cast<const Nif::NiPSysBoxEmitter*>(emitter);
                placer->setXRange(-box->mWidth / 2.f, box->mWidth / 2.f);
                placer->setYRange(-box->mDepth / 2.f, box->mDepth / 2.f);
                placer->setZRange(-box->mHeight / 2.f, box->mHeight / 2.f);
            }
            else if (emitter && emitter->recType == Nif::RC_NiPSysCylinderEmitter)
            {
                const auto* cylinder = static_cast<const Nif::NiPSysCylinderEmitter*>(emitter);
                placer->setXRange(-cylinder->mRadius, cylinder->mRadius);
                placer->setYRange(-cylinder->mRadius, cylinder->mRadius);
                placer->setZRange(-cylinder->mHeight / 2.f, cylinder->mHeight / 2.f);
            }
            else if (emitter && emitter->recType == Nif::RC_NiPSysSphereEmitter)
            {
                const auto* sphere = static_cast<const Nif::NiPSysSphereEmitter*>(emitter);
                placer->setXRange(-sphere->mRadius, sphere->mRadius);
                placer->setYRange(-sphere->mRadius, sphere->mRadius);
                placer->setZRange(-sphere->mRadius, sphere->mRadius);
            }
            else
            {
                placer->setXRange(0.f, 0.f);
                placer->setYRange(0.f, 0.f);
                placer->setZRange(0.f, 0.f);
            }
            osgEmitter->setPlacer(placer);

            if (emitter && emitter->recType == Nif::RC_NiPSysMeshEmitter)
            {
                const auto* meshEmitter = static_cast<const Nif::NiPSysMeshEmitter*>(emitter);
                if (!meshEmitter->mEmitterMeshes.empty() && !meshEmitter->mEmitterMeshes.front().empty())
                {
                    osgEmitter->setGeometryEmitterTarget(meshEmitter->mEmitterMeshes.front()->recIndex);
                    osgEmitter->setFlags(Nif::NiParticleSystemController::BSPArrayController_AtVertex);
                }
                osgEmitter->setModernMeshEmission(
                    meshEmitter->mInitialVelocityType, meshEmitter->mEmissionType, meshEmitter->mEmissionAxis);
            }

            return osgEmitter;
        }

        void handleModernParticlePrograms(
            const Nif::NiPSysModifierList& modifiers, osg::Group* attachTo, osgParticle::ParticleSystem* partsys,
            osgParticle::ParticleProcessor::ReferenceFrame rf, const Nif::NiParticleSystem* particleSystem) const
        {
            osgParticle::ModularProgram* program = new osgParticle::ModularProgram;
            attachTo->addChild(program);
            program->setParticleSystem(partsys);
            program->setReferenceFrame(rf);

            for (const auto& modifier : modifiers)
            {
                if (modifier.empty() || !modifier->mActive)
                    continue;

                if (modifier->recType == Nif::RC_NiPSysGrowFadeModifier)
                {
                    const auto* growFade = static_cast<const Nif::NiPSysGrowFadeModifier*>(modifier.getPtr());
                    program->addOperator(new GrowFadeAffector(growFade->mGrowTime, growFade->mFadeTime));
                }
                else if (modifier->recType == Nif::RC_NiPSysColorModifier)
                {
                    const auto* color = static_cast<const Nif::NiPSysColorModifier*>(modifier.getPtr());
                    if (!color->mData.empty())
                        program->addOperator(new ParticleColorAffector(color->mData.getPtr()));
                }
                else if (modifier->recType == Nif::RC_BSPSysSimpleColorModifier)
                {
                    const auto* color = static_cast<const Nif::BSPSysSimpleColorModifier*>(modifier.getPtr());
                    program->addOperator(new BethesdaParticleColorAffector(color));
                }
                else if (modifier->recType == Nif::RC_NiPSysGravityModifier)
                {
                    const auto* gravity = static_cast<const Nif::NiPSysGravityModifier*>(modifier.getPtr());
                    const osg::Matrixf objectToParticleSystem = getModernModifierObjectToParticleSystem(
                        gravity->mGravityObject.empty() ? nullptr : gravity->mGravityObject.getPtr(), particleSystem);
                    osg::Vec3f direction
                        = osg::Matrixf::transform3x3(gravity->mGravityAxis, objectToParticleSystem);
                    if (direction.length2() == 0.f)
                        direction = gravity->mGravityAxis;
                    program->addOperator(new GravityAffector(gravity->mStrength, gravity->mForceType,
                        objectToParticleSystem.getTrans(), direction, gravity->mDecay));
                }
                else if (modifier->recType == Nif::RC_NiPSysBombModifier)
                {
                    const auto* bomb = static_cast<const Nif::NiPSysBombModifier*>(modifier.getPtr());
                    const osg::Matrixf objectToParticleSystem = getModernModifierObjectToParticleSystem(
                        bomb->mBombObject.empty() ? nullptr : bomb->mBombObject.getPtr(), particleSystem);
                    osg::Vec3f direction = osg::Matrixf::transform3x3(bomb->mBombAxis, objectToParticleSystem);
                    if (direction.length2() == 0.f)
                        direction = bomb->mBombAxis;
                    program->addOperator(new ParticleBomb(bomb->mRange, bomb->mStrength, bomb->mDecayType,
                        bomb->mSymmetryType, objectToParticleSystem.getTrans(), direction));
                }
            }
        }

        bool handleModernParticleSystem(const Nif::NiAVObject* nifNode, const Nif::Parent* parent,
            osg::Group* parentNode, SceneUtil::CompositeStateSetUpdater* composite, int animflags)
        {
            const auto* modernParticleSystem = static_cast<const Nif::NiParticleSystem*>(nifNode);
            const Nif::NiPSysEmitter* emitter = findModernParticleEmitter(modernParticleSystem);
            if (!emitter)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: no NiPSys emitter found in " << mFilename << " node="
                                 << nifNode->mName;
                return false;
            }

            const unsigned int quota = getModernParticleQuota(modernParticleSystem);
            const ModernParticleControllerChannels controllerChannels
                = findModernParticleControllerChannels(modernParticleSystem, emitter);
            const float authoredRate = getInitialModernParticleRate(controllerChannels, emitter, quota);
            osg::ref_ptr<ParticleSystem> partsys(new ParticleSystem);
            partsys->setSortMode(osgParticle::ParticleSystem::SORT_BACK_TO_FRONT);
            partsys->setQuota(quota);
            partsys->setParticleScaleReferenceFrame(osgParticle::ParticleSystem::LOCAL_COORDINATES);

            const float initialSize = std::max(0.01f, emitter->mInitialRadius);
            partsys->getDefaultParticleTemplate().setSizeRange(osgParticle::rangef(initialSize, initialSize));
            const osg::Vec4f initialColor(
                emitter->mInitialColor.r(), emitter->mInitialColor.g(), emitter->mInitialColor.b(), 1.f);
            partsys->getDefaultParticleTemplate().setColorRange(
                osgParticle::rangev4(initialColor, initialColor));
            partsys->getDefaultParticleTemplate().setAlphaRange(
                osgParticle::rangef(emitter->mInitialColor.a(), emitter->mInitialColor.a()));
            partsys->getDefaultParticleTemplate().setLifeTime(std::max(0.01f, emitter->mLifespan));

            osgParticle::ParticleProcessor::ReferenceFrame rf = modernParticleSystem->mWorldSpace
                ? osgParticle::ParticleProcessor::ABSOLUTE_RF
                : osgParticle::ParticleProcessor::RELATIVE_RF;
            if (rf == osgParticle::ParticleProcessor::ABSOLUTE_RF)
                partsys->getOrCreateUserDataContainer()->addDescription("worldspace");

            osg::ref_ptr<Emitter> osgEmitter = handleModernParticleEmitter(emitter, authoredRate);
            osgEmitter->setParticleSystem(partsys);
            osgEmitter->setReferenceFrame(osgParticle::ParticleProcessor::RELATIVE_RF);

            const Nif::NiAVObject* emitterObject = getModernEmitterObject(emitter);
            if (emitterObject)
                mEmitterQueue.emplace_back(emitterObject->recIndex, osgEmitter);
            else
                parentNode->addChild(osgEmitter);

            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            if (!isAmbientEmbeddedAnimationPath(filename) && !(animflags & Nif::NiNode::ParticleFlag_AutoPlay))
                partsys->setFrozen(true);

            handleModernParticlePrograms(
                modernParticleSystem->mModifiers, parentNode, partsys.get(), rf, modernParticleSystem);

            if (controllerChannels.mRate || controllerChannels.mActive || controllerChannels.mAlpha)
            {
                parentNode->addUpdateCallback(new ModernParticleController(osgEmitter.get(), partsys.get(),
                    controllerChannels.mRateSequence, controllerChannels.mRate,
                    controllerChannels.mActiveSequence, controllerChannels.mActive,
                    controllerChannels.mAlphaSequence, controllerChannels.mAlpha));
                parentNode->setDataVariance(osg::Object::DYNAMIC);
            }

            std::vector<const Nif::NiProperty*> drawableProps;
            collectDrawableProperties(nifNode, parent, drawableProps);
            applyDrawableProperties(parentNode, drawableProps, composite, true, animflags);

            osg::ref_ptr<osgParticle::ParticleSystemUpdater> updater = new osgParticle::ParticleSystemUpdater;
            updater->addParticleSystem(partsys);
            parentNode->addChild(updater);

            if (rf == osgParticle::ParticleProcessor::RELATIVE_RF)
                parentNode->addChild(partsys);
            else
            {
                osg::MatrixTransform* trans = new osg::MatrixTransform;
                trans->setUpdateCallback(new InverseWorldMatrix);
                trans->addChild(partsys);
                parentNode->addChild(trans);
            }

            if (Loader::getSoftEffectEnabled())
                SceneUtil::setupSoftEffect(*partsys,
                    {
                        .mSize = initialSize,
                    });

            Log(Debug::Verbose) << "FNV/ESM4 diag: built NiPSys particle system in " << mFilename
                             << " node=" << nifNode->mName << " emitter=" << emitter->mName << " quota=" << quota
                             << " authoredRate=" << authoredRate << " frozen=" << partsys->isFrozen();
            return true;
        }

        void handleQueuedParticleEmitters(osg::Group* rootNode, Nif::FileView nif)
        {
            for (const auto& emitterPair : mEmitterQueue)
            {
                size_t recIndex = emitterPair.first;
                FindGroupByRecIndex findEmitterNode(recIndex);
                rootNode->accept(findEmitterNode);
                osg::Group* emitterNode = findEmitterNode.mFound;
                if (!emitterNode)
                {
                    Log(Debug::Warning)
                        << "NIFFile Warning: Failed to find particle emitter emitter node (node record index "
                        << recIndex << "). File: " << nif.getFilename();
                    continue;
                }

                // Emitter attached to the emitter node. Note one side effect of the emitter using the CullVisitor is
                // that hiding its node actually causes the emitter to stop firing. Convenient, because MW behaves this
                // way too!
                emitterNode->addChild(emitterPair.second);

                DisableOptimizer disableOptimizer;
                emitterNode->accept(disableOptimizer);
            }
            mEmitterQueue.clear();
        }

        void handleParticleSystem(const Nif::NiAVObject* nifNode, const Nif::Parent* parent, osg::Group* parentNode,
            SceneUtil::CompositeStateSetUpdater* composite, int animflags)
        {
            if (nifNode->recType == Nif::RC_NiParticleSystem
                && handleModernParticleSystem(nifNode, parent, parentNode, composite, animflags))
                return;

            osg::ref_ptr<ParticleSystem> partsys(new ParticleSystem);
            partsys->setSortMode(osgParticle::ParticleSystem::SORT_BACK_TO_FRONT);

            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            int particleAnimFlags = animflags;
            if (isAmbientEmbeddedAnimationPath(filename))
                particleAnimFlags |= Nif::NiNode::ParticleFlag_AutoPlay;

            const Nif::NiParticleSystemController* partctrl = nullptr;
            for (Nif::NiTimeControllerPtr ctrl = nifNode->mController; !ctrl.empty(); ctrl = ctrl->mNext)
            {
                if (!ctrl->isActive())
                    continue;
                if (ctrl->recType == Nif::RC_NiParticleSystemController
                    || ctrl->recType == Nif::RC_NiBSPArrayController)
                    partctrl = static_cast<Nif::NiParticleSystemController*>(ctrl.getPtr());
            }
            if (!partctrl)
            {
                Log(Debug::Info) << "No particle controller found in " << mFilename;
                return;
            }

            osgParticle::ParticleProcessor::ReferenceFrame rf = (animflags & Nif::NiNode::ParticleFlag_LocalSpace)
                ? osgParticle::ParticleProcessor::RELATIVE_RF
                : osgParticle::ParticleProcessor::ABSOLUTE_RF;

            // HACK: ParticleSystem has no setReferenceFrame method
            if (rf == osgParticle::ParticleProcessor::ABSOLUTE_RF)
            {
                partsys->getOrCreateUserDataContainer()->addDescription("worldspace");
            }

            partsys->setParticleScaleReferenceFrame(osgParticle::ParticleSystem::LOCAL_COORDINATES);

            handleParticleInitialState(nifNode, partsys, partctrl);

            partsys->getDefaultParticleTemplate().setSizeRange(
                osgParticle::rangef(partctrl->mInitialSize, partctrl->mInitialSize));
            partsys->getDefaultParticleTemplate().setColorRange(
                osgParticle::rangev4(partctrl->mInitialColor, partctrl->mInitialColor));
            partsys->getDefaultParticleTemplate().setAlphaRange(osgParticle::rangef(1.f, 1.f));

            if (!partctrl->mEmitter.empty())
            {
                osg::ref_ptr<Emitter> emitter = handleParticleEmitter(partctrl);
                emitter->setParticleSystem(partsys);
                emitter->setReferenceFrame(osgParticle::ParticleProcessor::RELATIVE_RF);

                // The emitter node may not actually be handled yet, so let's delay attaching the emitter to a later
                // moment. If the emitter node is placed later than the particle node, it'll have a single frame delay
                // in particle processing. But that shouldn't be a game-breaking issue.
                mEmitterQueue.emplace_back(partctrl->mEmitter->recIndex, emitter);

                osg::ref_ptr<ParticleSystemController> callback(new ParticleSystemController(partctrl));
                setupController(partctrl, callback, particleAnimFlags);
                emitter->setUpdateCallback(callback);

                if (!(particleAnimFlags & Nif::NiNode::ParticleFlag_AutoPlay))
                {
                    partsys->setFrozen(true);
                }
                else if (!(animflags & Nif::NiNode::ParticleFlag_AutoPlay)
                    && isAmbientEmbeddedAnimationPath(filename))
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: forced ambient particle autoplay in " << mFilename
                                     << " node=" << nifNode->mName;
                }

                // Due to odd code in the ParticleSystemUpdater, particle systems will not be updated in the first frame
                // So do that update manually
                osg::NodeVisitor nv;
                partsys->update(0.0, nv);
            }

            // modifiers should be attached *after* the emitter in the scene graph for correct update order
            // attach to same node as the ParticleSystem, we need osgParticle Operators to get the correct
            // localToWorldMatrix for transforming to particle space
            handleParticlePrograms(partctrl->mModifier, partctrl->mCollider, parentNode, partsys.get(), rf);

            std::vector<const Nif::NiProperty*> drawableProps;
            collectDrawableProperties(nifNode, parent, drawableProps);
            applyDrawableProperties(parentNode, drawableProps, composite, true, animflags);

            // particle system updater (after the emitters and modifiers in the scene graph)
            // I think for correct culling needs to be *before* the ParticleSystem, though osg examples do it the other
            // way
            osg::ref_ptr<osgParticle::ParticleSystemUpdater> updater = new osgParticle::ParticleSystemUpdater;
            updater->addParticleSystem(partsys);
            parentNode->addChild(updater);

            osg::Node* toAttach = partsys.get();

            if (rf == osgParticle::ParticleProcessor::RELATIVE_RF)
                parentNode->addChild(toAttach);
            else
            {
                osg::MatrixTransform* trans = new osg::MatrixTransform;
                trans->setUpdateCallback(new InverseWorldMatrix);
                trans->addChild(toAttach);
                parentNode->addChild(trans);
            }

            if (Loader::getSoftEffectEnabled())
                SceneUtil::setupSoftEffect(*partsys,
                    {
                        .mSize = partsys->getDefaultParticleTemplate().getSizeRange().maximum,
                    });
        }

        void handleNiGeometryData(const Nif::NiAVObject* nifNode, const Nif::Parent* parent, osg::Geometry* geometry,
            osg::Node* parentNode, SceneUtil::CompositeStateSetUpdater* composite,
            const std::vector<unsigned int>& boundTextures, int animflags)
        {
            const Nif::NiGeometry* niGeometry = static_cast<const Nif::NiGeometry*>(nifNode);
            if (niGeometry->mData.empty())
                return;

            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            bool hasPartitions = false;
            std::size_t partitionCount = 0;
            std::size_t partitionTriangleIndexCount = 0;
            std::size_t partitionStripCount = 0;
            if (!niGeometry->mSkin.empty())
            {
                const Nif::NiSkinInstance* skin = niGeometry->mSkin.getPtr();
                const Nif::NiSkinPartition* partitions = skin->getPartitions();
                hasPartitions = partitions != nullptr;
                if (hasPartitions)
                {
                    partitionCount = partitions->mPartitions.size();
                    const Nif::BSDismemberSkinInstance* dismemberSkin = nullptr;
                    if (skin->recType == Nif::RC_BSDismemberSkinInstance)
                        dismemberSkin = static_cast<const Nif::BSDismemberSkinInstance*>(skin);
                    const bool logDismemberParts = dismemberSkin != nullptr
                        && (filename.find("meshes/characters/") != std::string::npos
                            || filename.find("meshes/armor/") != std::string::npos
                            || filename.find("meshes/creatures/") != std::string::npos);

                    for (std::size_t partitionIndex = 0; partitionIndex < partitions->mPartitions.size();
                         ++partitionIndex)
                    {
                        if (dismemberSkin != nullptr && partitionIndex < dismemberSkin->mParts.size())
                        {
                            const Nif::BSDismemberSkinInstance::BodyPart& part = dismemberSkin->mParts[partitionIndex];
                            if (logDismemberParts)
                                Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout dismember partition index="
                                                 << partitionIndex << " type=" << part.mType << " flags="
                                                 << part.mFlags << " triangles="
                                                 << partitions->mPartitions[partitionIndex].mTrueTriangles.size() / 3
                                                 << " strips="
                                                 << partitions->mPartitions[partitionIndex].mTrueStrips.size()
                                                 << " in " << mFilename << " shape " << nifNode->mName;
                            if (isFalloutDismemberCapShape(nifNode->mName)
                                || isFalloutConditionalDismemberCapPartition(part.mType))
                            {
                                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped Fallout dismember cap shape "
                                                 << nifNode->mName << " partition type=" << part.mType
                                                 << " flags=" << part.mFlags << " in " << mFilename;
                                continue;
                            }
                        }

                        const Nif::NiSkinPartition::Partition& partition = partitions->mPartitions[partitionIndex];
                        const std::vector<unsigned short>& trueTriangles = partition.mTrueTriangles;
                        partitionTriangleIndexCount += trueTriangles.size();
                        partitionStripCount += partition.mTrueStrips.size();
                        if (!trueTriangles.empty())
                        {
                            geometry->addPrimitiveSet(new osg::DrawElementsUShort(
                                osg::PrimitiveSet::TRIANGLES, trueTriangles.size(), trueTriangles.data()));
                        }
                        for (const auto& strip : partition.mTrueStrips)
                        {
                            if (strip.size() < 3)
                                continue;
                            geometry->addPrimitiveSet(new osg::DrawElementsUShort(
                                osg::PrimitiveSet::TRIANGLE_STRIP, strip.size(), strip.data()));
                        }
                    }
                }
            }

            const Nif::NiGeometryData* niGeometryData = niGeometry->mData.getPtr();
            const auto addRawPrimitiveSets = [&]() -> unsigned int {
                unsigned int added = 0;
                if (niGeometry->recType == Nif::RC_NiTriShape || nifNode->recType == Nif::RC_BSLODTriShape)
                {
                    auto data = static_cast<const Nif::NiTriShapeData*>(niGeometryData);
                    const std::vector<unsigned short>& triangles = data->mTriangles;
                    if (!triangles.empty())
                    {
                        geometry->addPrimitiveSet(new osg::DrawElementsUShort(
                            osg::PrimitiveSet::TRIANGLES, triangles.size(), triangles.data()));
                        ++added;
                    }
                }
                else if (niGeometry->recType == Nif::RC_NiTriStrips)
                {
                    auto data = static_cast<const Nif::NiTriStripsData*>(niGeometryData);
                    for (const std::vector<unsigned short>& strip : data->mStrips)
                    {
                        if (strip.size() < 3)
                            continue;
                        geometry->addPrimitiveSet(new osg::DrawElementsUShort(
                            osg::PrimitiveSet::TRIANGLE_STRIP, strip.size(), strip.data()));
                        ++added;
                    }
                }
                else if (niGeometry->recType == Nif::RC_NiLines)
                {
                    auto data = static_cast<const Nif::NiLinesData*>(niGeometryData);
                    const auto& line = data->mLines;
                    if (!line.empty())
                    {
                        geometry->addPrimitiveSet(
                            new osg::DrawElementsUShort(osg::PrimitiveSet::LINES, line.size(), line.data()));
                        ++added;
                    }
                }
                return added;
            };
            bool usedRawFallback = false;
            unsigned int rawFallbackPrimitiveSets = 0;
            if (hasPartitions && geometry->getNumPrimitiveSets() == 0 && worldViewerSkinPartitionFallbackEnabled()
                && isWorldViewerActorMeshPath(filename) && !isFalloutDismemberCapShape(nifNode->mName)
                && !isFalloutHiddenMorphShape(nifNode->mName))
            {
                rawFallbackPrimitiveSets = addRawPrimitiveSets();
                usedRawFallback = rawFallbackPrimitiveSets > 0;
            }
            if (!hasPartitions)
            {
                if (addRawPrimitiveSets() == 0)
                    return;
            }

            const auto& vertices = niGeometryData->mVertices;
            const auto& normals = niGeometryData->mNormals;
            const auto& colors = niGeometryData->mColors;
            if (!vertices.empty())
                geometry->setVertexArray(new osg::Vec3Array(vertices.size(), vertices.data()));
            if (!normals.empty())
                geometry->setNormalArray(
                    new osg::Vec3Array(normals.size(), normals.data()), osg::Array::BIND_PER_VERTEX);
            if (!colors.empty())
                geometry->setColorArray(new osg::Vec4Array(colors.size(), colors.data()), osg::Array::BIND_PER_VERTEX);

            const auto& tangents = niGeometryData->mTangents;
            const auto& bitangents = niGeometryData->mBitangents;
            if (tangents.size() == vertices.size() && bitangents.size() == vertices.size()
                && normals.size() == vertices.size())
            {
                const bool falloutNvTangentConvention = mVersion == Nif::NIFFile::NIFVersion::VER_BGS
                    && mUserVersion == 11 && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3;
                osg::ref_ptr<osg::Vec4Array> tangentFrame = new osg::Vec4Array;
                tangentFrame->reserve(tangents.size());
                for (std::size_t i = 0; i < tangents.size(); ++i)
                {
                    // The compatibility shaders reconstruct B as cross(T, N) * w.
                    // FO3/FNV NiGeometry stores the shader tangent axis in mBitangents
                    // and the negated shader bitangent axis in mTangents.  Other NIF
                    // generations use the conventional field mapping.
                    const osg::Vec3f shaderTangent = falloutNvTangentConvention ? bitangents[i] : tangents[i];
                    const osg::Vec3f shaderBitangent = falloutNvTangentConvention ? -tangents[i] : bitangents[i];
                    const float handedness
                        = ((shaderTangent ^ normals[i]) * shaderBitangent) < 0.f ? -1.f : 1.f;
                    tangentFrame->push_back(osg::Vec4f(
                        shaderTangent.x(), shaderTangent.y(), shaderTangent.z(), handedness));
                }
                geometry->setTexCoordArray(7, tangentFrame, osg::Array::BIND_PER_VERTEX);
            }

            if (worldViewerMeshLoadTelemetryEnabled() && isWorldViewerActorMeshPath(filename))
            {
                Log(Debug::Info) << "World viewer nif geometry ledger: file=\"" << mFilename.generic_string()
                                 << "\" shape=\"" << nifNode->mName << "\""
                                 << " recType=" << niGeometry->recType
                                 << " hasSkin=" << !niGeometry->mSkin.empty()
                                 << " hasPartitions=" << hasPartitions
                                 << " partitionCount=" << partitionCount
                                 << " partitionTriangleIndices=" << partitionTriangleIndexCount
                                 << " partitionStrips=" << partitionStripCount
                                 << " rawFallback=" << usedRawFallback
                                 << " rawFallbackPrimitiveSets=" << rawFallbackPrimitiveSets
                                 << " primitiveSets=" << geometry->getNumPrimitiveSets()
                                 << " vertices=" << vertices.size()
                                 << " normals=" << normals.size()
                                 << " colors=" << colors.size();
            }

            const auto& uvlist = niGeometryData->mUVList;
            const bool hasLegacySkyShaderProperty = std::any_of(nifNode->mProperties.begin(),
                nifNode->mProperties.end(), [](const auto& property) {
                    return !property.empty()
                        && (property->recType == Nif::RC_SkyShaderProperty
                            || property->recType == Nif::RC_BSSkyShaderProperty);
                });
            const bool hasDedicatedSkyShaderProperty = !niGeometry->mShaderProperty.empty()
                && (niGeometry->mShaderProperty->recType == Nif::RC_SkyShaderProperty
                    || niGeometry->mShaderProperty->recType == Nif::RC_BSSkyShaderProperty);
            const bool hasSkyShaderUv
                = !uvlist.empty() && (hasLegacySkyShaderProperty || hasDedicatedSkyShaderProperty);
            if (boundTextures.empty() && hasSkyShaderUv)
            {
                // SkyShaderProperty handling is intentionally optional, but FO3/FNV sky
                // geometry still needs its authored UV0 when SkyManager supplies the live
                // WTHR texture. Without this array every vertex samples one texel, turning
                // the cloud dome's vertex-alpha rows into opaque concentric bands.
                geometry->setTexCoordArray(
                    0, new osg::Vec2Array(uvlist[0].size(), uvlist[0].data()), osg::Array::BIND_PER_VERTEX);
            }
            int textureStage = 0;
            for (std::vector<unsigned int>::const_iterator it = boundTextures.begin(); it != boundTextures.end();
                 ++it, ++textureStage)
            {
                unsigned int uvSet = *it;
                if (uvSet >= uvlist.size())
                {
                    Log(Debug::Verbose) << "Out of bounds UV set " << uvSet << " on shape \"" << nifNode->mName
                                        << "\" in " << mFilename;
                    if (uvlist.empty())
                        continue;
                    uvSet = 0;
                }

                geometry->setTexCoordArray(textureStage, new osg::Vec2Array(uvlist[uvSet].size(), uvlist[uvSet].data()),
                    osg::Array::BIND_PER_VERTEX);
            }

            // osg::Material properties are handled here for two reasons:
            // - if there are no vertex colors, we need to disable colorMode.
            // - there are 3 "overlapping" nif properties that all affect the osg::Material, handling them
            //   above the actual renderable would be tedious.
            std::vector<const Nif::NiProperty*> drawableProps;
            collectDrawableProperties(nifNode, parent, drawableProps);
            if (!niGeometry->mShaderProperty.empty())
                drawableProps.emplace_back(niGeometry->mShaderProperty.getPtr());
            if (!niGeometry->mAlphaProperty.empty())
                drawableProps.emplace_back(niGeometry->mAlphaProperty.getPtr());
            applyDrawableProperties(
                parentNode, drawableProps, composite, !niGeometryData->mColors.empty(), animflags, &boundTextures);

            const bool falloutNvRepublicanOutfit = mVersion == Nif::NIFFile::NIFVersion::VER_BGS
                && mUserVersion == 11 && mBethVersion == Nif::NIFFile::BethVersion::BETHVER_FO3
                && !niGeometry->mSkin.empty()
                && (filename.find("meshes/armor/republicans/republican_02.nif") != std::string::npos
                    || filename.find("meshes\\armor\\republicans\\republican_02.nif") != std::string::npos);
            if (falloutNvRepublicanOutfit)
            {
                // Retail Easy Pete draws this skinned outfit with SLS2011: exact
                // attenuation-LUT diffuse lighting and no generic Blinn specular.
                // Keep the first rollout tied to the oracle-proven mesh; broader
                // SLS variant routing follows per captured shader signature.
                parentNode->getOrCreateStateSet()->addUniform(new osg::Uniform("falloutSlsMode", 1));
            }
        }

        void handleNiGeometry(const Nif::NiAVObject* nifNode, const Nif::Parent* parent, osg::Group* parentNode,
            SceneUtil::CompositeStateSetUpdater* composite, const std::vector<unsigned int>& boundTextures,
            int animflags, unsigned int nifVersion)
        {
            assert(isTypeNiGeometry(nifNode->recType));

            auto niGeometry = static_cast<const Nif::NiGeometry*>(nifNode);
            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            const bool falloutNif = nifVersion >= Nif::NIFFile::NIFVersion::VER_BGS;
            if (falloutNif && (isFalloutFlagHelperGeometry(filename, *niGeometry)
                                  || isFalloutActorAddonHelperGeometry(filename, *niGeometry, boundTextures)))
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped Fallout non-render helper geometry "
                                    << nifNode->mName << " in " << mFilename;
                return;
            }

            osg::ref_ptr<osg::Geometry> geom(new osg::Geometry);
            handleNiGeometryData(nifNode, parent, geom, parentNode, composite, boundTextures, animflags);
            // If the record had no valid geometry data in it, early-out
            if (geom->empty() || geom->getNumPrimitiveSets() == 0)
                return;

            osg::ref_ptr<osg::Drawable> drawable = geom;

            if (!niGeometry->mSkin.empty())
            {
                osg::ref_ptr<SceneUtil::RigGeometry> rig(new SceneUtil::RigGeometry);
                rig->setSourceGeometry(geom);

                const Nif::NiSkinInstance* skin = niGeometry->mSkin.getPtr();
                const Nif::NiSkinData* data = skin->mData.getPtr();
                const Nif::NiAVObjectList& bones = skin->mBones;
                const bool falloutFlagPath = falloutNif && isFalloutFlagPath(filename);
                if (falloutFlagPath)
                    rig->setFalloutFlagSkinning(true);
                const bool falloutSkinProbe = filename.find("characters/head/headhuman.nif") != std::string::npos
                    || filename.find("characters\\head\\headhuman.nif") != std::string::npos
                    || filename.find("characters/_male/lefthand") != std::string::npos
                    || filename.find("characters\\_male\\lefthand") != std::string::npos
                    || filename.find("characters/_male/righthand") != std::string::npos
                    || filename.find("characters\\_male\\righthand") != std::string::npos
                    || falloutFlagPath;
                if (falloutSkinProbe)
                {
                    const osg::Matrixf skinTransform = data->mTransform.toMatrix();
                    const Nif::NiAVObject* rootBone = skin->mRoot.getPtr();
                    Log(Debug::Info) << "FNV/ESM4 skin data: file=" << mFilename << " geom=" << nifNode->mName
                                     << " root=" << (rootBone != nullptr ? rootBone->mName : std::string("<none>"))
                                     << " geomLocal=(" << nifNode->mTransform.mTranslation.x() << ","
                                     << nifNode->mTransform.mTranslation.y() << ","
                                     << nifNode->mTransform.mTranslation.z() << ")"
                                     << " skinTransformT=(" << skinTransform.getTrans().x() << ","
                                     << skinTransform.getTrans().y() << "," << skinTransform.getTrans().z()
                                     << ") bones=" << bones.size();
                }

                // Assign bone weights
                std::vector<SceneUtil::RigGeometry::BoneInfo> boneInfo;
                std::vector<SceneUtil::RigGeometry::VertexWeights> influences;
                boneInfo.resize(bones.size());
                influences.resize(bones.size());
                for (std::size_t i = 0; i < bones.size(); ++i)
                {
                    boneInfo[i].mName = Misc::StringUtils::lowerCase(bones[i].getPtr()->mName);
                    boneInfo[i].mInvBindMatrix = data->mBones[i].mTransform.toMatrix();
                    boneInfo[i].mBoundSphere = data->mBones[i].mBoundSphere;
                    influences[i] = data->mBones[i].mWeights;
                    if (falloutSkinProbe && i < 12)
                    {
                        const osg::Matrixf boneTransform = data->mBones[i].mTransform.toMatrix();
                        Log(Debug::Info) << "FNV/ESM4 skin data: file=" << mFilename << " geom="
                                         << nifNode->mName << " bone[" << i << "]="
                                         << bones[i].getPtr()->mName << " skinT=("
                                         << boneTransform.getTrans().x() << ","
                                         << boneTransform.getTrans().y() << ","
                                         << boneTransform.getTrans().z() << ") weights="
                                         << data->mBones[i].mWeights.size();
                    }
                }
                rig->setBoneInfo(std::move(boneInfo));
                rig->setInfluences(influences);
                rig->setTransform(data->mTransform.toMatrix());
                if (const Nif::NiAVObject* rootBone = skin->mRoot.getPtr())
                    rig->setRootBone(rootBone->mName);

                drawable = rig;
            }

            for (Nif::NiTimeControllerPtr ctrl = nifNode->mController; !ctrl.empty(); ctrl = ctrl->mNext)
            {
                if (!ctrl->isActive())
                    continue;
                if (ctrl->recType == Nif::RC_NiGeomMorpherController)
                {
                    if (!niGeometry->mSkin.empty())
                        continue;

                    auto nimorphctrl = static_cast<const Nif::NiGeomMorpherController*>(ctrl.getPtr());
                    if (nimorphctrl->mData.empty())
                        continue;

                    const std::vector<Nif::NiMorphData::MorphData>& morphs = nimorphctrl->mData.getPtr()->mMorphs;
                    if (morphs.empty()
                        || morphs[0].mVertices.size()
                            != static_cast<const osg::Vec3Array*>(geom->getVertexArray())->size())
                        continue;

                    osg::ref_ptr<SceneUtil::MorphGeometry> morphGeom = new SceneUtil::MorphGeometry;
                    morphGeom->setSourceGeometry(geom);
                    for (unsigned int i = 0; i < morphs.size(); ++i)
                        morphGeom->addMorphTarget(
                            new osg::Vec3Array(morphs[i].mVertices.size(), morphs[i].mVertices.data()), 0.f);

                    osg::ref_ptr<GeomMorpherController> morphctrl = new GeomMorpherController(nimorphctrl);
                    setupController(ctrl.getPtr(), morphctrl, animflags);
                    if ((animflags & Nif::NiNode::AnimFlag_AutoPlay) == 0 && isAmbientEmbeddedAnimationPath(filename))
                    {
                        morphctrl->setSource(std::make_shared<SceneUtil::FrameTimeSource>());
                        Log(Debug::Verbose) << "FNV/ESM4 diag: forced ambient morph autoplay for " << nifNode->mName
                                         << " in " << mFilename;
                    }
                    morphGeom->setUpdateCallback(morphctrl);

                    drawable = morphGeom;
                    break;
                }
            }

            drawable->setName(nifNode->mName);
            applyWorldViewerFlatDrawable(*drawable, filename, nifNode->mName);
            parentNode->addChild(drawable);
        }

        void handleBSGeometry(const Nif::NiAVObject* nifNode, const Nif::Parent* parent, osg::Group* parentNode,
            SceneUtil::CompositeStateSetUpdater* composite, const std::vector<unsigned int>& boundTextures,
            int animflags)
        {
            assert(isTypeBSGeometry(nifNode->recType));

            auto bsTriShape = static_cast<const Nif::BSTriShape*>(nifNode);
            const std::vector<unsigned short>& triangles = bsTriShape->mTriangles;
            const std::string filename = Misc::StringUtils::lowerCase(mFilename.generic_string());
            const bool worldViewerBSLedger = worldViewerMeshLoadTelemetryEnabled() && isWorldViewerActorMeshPath(filename);
            const Nif::NiSkinPartition* niSkinPartitions = nullptr;
            const Nif::NiSkinInstance* niSkinInstance = nullptr;
            if (!bsTriShape->mSkin.empty() && bsTriShape->mSkin->recType == Nif::RC_BSDismemberSkinInstance)
            {
                const auto* skin = static_cast<const Nif::BSDismemberSkinInstance*>(bsTriShape->mSkin.getPtr());
                niSkinInstance = skin;
                niSkinPartitions = skin->getPartitions();
            }
            else if (!bsTriShape->mSkin.empty() && bsTriShape->mSkin->recType == Nif::RC_NiSkinInstance)
            {
                const auto* skin = static_cast<const Nif::NiSkinInstance*>(bsTriShape->mSkin.getPtr());
                niSkinInstance = skin;
                niSkinPartitions = skin->getPartitions();
            }
            else if (!bsTriShape->mSkin.empty() && bsTriShape->mSkin->recType == Nif::RC_NiSkinData)
            {
                const auto* skinData = static_cast<const Nif::NiSkinData*>(bsTriShape->mSkin.getPtr());
                if (!skinData->mPartitions.empty())
                    niSkinPartitions = skinData->mPartitions.getPtr();
            }
            if (worldViewerBSLedger)
            {
                std::size_t partitionTriangles = 0;
                std::size_t partitionStrips = 0;
                std::size_t partitionMappedVertices = 0;
                if (niSkinPartitions != nullptr)
                {
                    for (const Nif::NiSkinPartition::Partition& partition : niSkinPartitions->mPartitions)
                    {
                        partitionTriangles += partition.mTrueTriangles.size();
                        partitionStrips += partition.mTrueStrips.size();
                        partitionMappedVertices += partition.mVertexMap.size();
                    }
                }
                Log(Debug::Info) << "World viewer bs geometry ledger: file=\"" << mFilename.generic_string()
                                 << "\" shape=\"" << nifNode->mName << "\""
                                 << " recName=\"" << bsTriShape->recName << "\""
                                 << " recType=" << bsTriShape->recType
                                 << " triangles=" << triangles.size()
                                 << " vertices=" << bsTriShape->mVertData.size()
                                 << " dataSize=" << bsTriShape->mDataSize
                                 << " particleTriangles=" << bsTriShape->mParticleTriangles.size()
                                 << " particleVerts=" << bsTriShape->mParticleVerts.size()
                                 << " externalGeometry=" << bsTriShape->mExternalGeometry.size()
                                 << " niSkinPartitions=" << (niSkinPartitions != nullptr)
                                 << " partitionCount="
                                 << (niSkinPartitions != nullptr ? niSkinPartitions->mPartitions.size() : 0)
                                 << " partitionVertices="
                                 << (niSkinPartitions != nullptr ? niSkinPartitions->mVertexData.size() : 0)
                                 << " partitionMappedVertices=" << partitionMappedVertices
                                 << " partitionTriangles=" << partitionTriangles
                                 << " partitionStrips=" << partitionStrips
                                 << " skin=" << !bsTriShape->mSkin.empty()
                                 << " skinRecType="
                                 << (!bsTriShape->mSkin.empty() ? bsTriShape->mSkin->recType : 0)
                                 << " flags=0x" << std::hex << bsTriShape->mVertDesc.mFlags << std::dec;
            }
            if (worldViewerQuarantineFo4ActorSubIndexTriShapeEnabled()
                && bsTriShape->recType == Nif::RC_BSSubIndexTriShape && isWorldViewerActorMeshPath(filename))
            {
                Log(Debug::Warning) << "World viewer bs geometry quarantine: file=\"" << mFilename.generic_string()
                                    << "\" shape=\"" << nifNode->mName << "\""
                                    << " recName=\"" << bsTriShape->recName << "\""
                                    << " recType=" << bsTriShape->recType
                                    << " reason=\"fo4 actor BSSubIndexTriShape attach crash guard\""
                                    << " triangles=" << triangles.size()
                                    << " vertices=" << bsTriShape->mVertData.size()
                                    << " skin=" << !bsTriShape->mSkin.empty()
                                    << " skinRecType="
                                    << (!bsTriShape->mSkin.empty() ? bsTriShape->mSkin->recType : 0);
                return;
            }
            if (triangles.empty())
            {
                if (bsTriShape->recName == "BSGeometry" && !bsTriShape->mExternalGeometry.empty())
                {
                    if (!handleStarfieldExternalGeometry(bsTriShape, parentNode))
                        handleBSGeometryProxy(bsTriShape, parentNode);
                }
                else if (worldViewerSkinPartitionFallbackEnabled() && isWorldViewerActorMeshPath(filename)
                    && niSkinPartitions != nullptr && !niSkinPartitions->mVertexData.empty())
                {
                    osg::ref_ptr<osg::Geometry> geometry(new osg::Geometry);

                    std::size_t partitionPrimitiveSets = 0;
                    std::size_t remappedPrimitiveSets = 0;
                    std::size_t invalidPrimitiveSets = 0;
                    unsigned int maxIndexSeen = 0;
                    const std::size_t vertexCount = niSkinPartitions->mVertexData.size();
                    std::vector<unsigned int> fallbackTriangleIndices;

                    auto addPartitionPrimitiveSet
                        = [&](osg::PrimitiveSet::Mode mode, const std::vector<unsigned short>& source,
                              const Nif::NiSkinPartition::Partition& partition) {
                              if (source.empty())
                                  return;

                              std::vector<unsigned short> remapped;
                              const std::vector<unsigned short>* indices = &source;
                              bool needsRemap = false;
                              bool valid = true;
                              for (unsigned short index : source)
                              {
                                  maxIndexSeen = std::max(maxIndexSeen, static_cast<unsigned int>(index));
                                  if (index >= vertexCount)
                                  {
                                      needsRemap = true;
                                      break;
                                  }
                              }

                              if (needsRemap)
                              {
                                  valid = !partition.mVertexMap.empty();
                                  if (valid)
                                  {
                                      remapped.reserve(source.size());
                                      for (unsigned short index : source)
                                      {
                                          if (index >= partition.mVertexMap.size()
                                              || partition.mVertexMap[index] >= vertexCount)
                                          {
                                              valid = false;
                                              break;
                                          }
                                          remapped.push_back(partition.mVertexMap[index]);
                                          maxIndexSeen = std::max(
                                              maxIndexSeen, static_cast<unsigned int>(partition.mVertexMap[index]));
                                      }
                                  }
                                  if (valid)
                                  {
                                      indices = &remapped;
                                      ++remappedPrimitiveSets;
                                  }
                              }

                              if (!valid)
                              {
                                  ++invalidPrimitiveSets;
                                  return;
                              }

                              osg::ref_ptr<osg::DrawElementsUShort> draw = new osg::DrawElementsUShort(mode);
                              draw->reserve(indices->size());
                              for (unsigned short index : *indices)
                                  draw->push_back(index);
                              geometry->addPrimitiveSet(draw);
                              if (mode == osg::PrimitiveSet::TRIANGLES)
                              {
                                  for (std::size_t i = 0; i + 2 < indices->size(); i += 3)
                                  {
                                      fallbackTriangleIndices.push_back((*indices)[i]);
                                      fallbackTriangleIndices.push_back((*indices)[i + 1]);
                                      fallbackTriangleIndices.push_back((*indices)[i + 2]);
                                  }
                              }
                              else if (mode == osg::PrimitiveSet::TRIANGLE_STRIP)
                              {
                                  for (std::size_t i = 2; i < indices->size(); ++i)
                                  {
                                      const unsigned int i0 = (*indices)[i - 2];
                                      const unsigned int i1 = (*indices)[i - 1];
                                      const unsigned int i2 = (*indices)[i];
                                      if (i0 == i1 || i1 == i2 || i0 == i2)
                                          continue;
                                      if ((i % 2) == 0)
                                      {
                                          fallbackTriangleIndices.push_back(i0);
                                          fallbackTriangleIndices.push_back(i1);
                                          fallbackTriangleIndices.push_back(i2);
                                      }
                                      else
                                      {
                                          fallbackTriangleIndices.push_back(i1);
                                          fallbackTriangleIndices.push_back(i0);
                                          fallbackTriangleIndices.push_back(i2);
                                      }
                                  }
                              }
                              ++partitionPrimitiveSets;
                          };

                    for (const Nif::NiSkinPartition::Partition& partition : niSkinPartitions->mPartitions)
                    {
                        addPartitionPrimitiveSet(
                            osg::PrimitiveSet::TRIANGLES, partition.mTrueTriangles, partition);
                        for (const std::vector<unsigned short>& strip : partition.mTrueStrips)
                        {
                            if (strip.size() < 3)
                                continue;
                            addPartitionPrimitiveSet(osg::PrimitiveSet::TRIANGLE_STRIP, strip, partition);
                        }
                    }

                    std::vector<osg::Vec3f> vertices;
                    std::vector<osg::Vec3f> normals;
                    std::vector<osg::Vec4ub> colors;
                    std::vector<osg::Vec2f> uvlist;
                    appendBSVertexDataArrays(
                        niSkinPartitions->mVertexData, niSkinPartitions->mVertexDesc.mFlags, vertices, normals,
                        colors, uvlist);
                    const bool ignoredVertexColors
                        = !colors.empty() && worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_IGNORE_BS_PARTITION_VERTEX_COLORS");
                    if (ignoredVertexColors)
                        colors.clear();
                    bool usedDynamicVertices = false;
                    if (vertices.empty() && bsTriShape->recType == Nif::RC_BSDynamicTriShape)
                    {
                        const auto* dynamicTriShape = static_cast<const Nif::BSDynamicTriShape*>(bsTriShape);
                        if (dynamicTriShape->mDynamicData.size() == vertexCount)
                        {
                            vertices.reserve(dynamicTriShape->mDynamicData.size());
                            for (const osg::Vec4f& dynamicVertex : dynamicTriShape->mDynamicData)
                                vertices.emplace_back(dynamicVertex.x(), dynamicVertex.y(), dynamicVertex.z());
                            usedDynamicVertices = true;
                        }
                    }
                    bool generatedNormals = false;
                    if (normals.empty() && worldViewerGenerateMissingBSNormalsEnabled() && !vertices.empty()
                        && !fallbackTriangleIndices.empty())
                    {
                        normals.assign(vertices.size(), osg::Vec3f());
                        for (std::size_t i = 0; i + 2 < fallbackTriangleIndices.size(); i += 3)
                        {
                            const unsigned int i0 = fallbackTriangleIndices[i];
                            const unsigned int i1 = fallbackTriangleIndices[i + 1];
                            const unsigned int i2 = fallbackTriangleIndices[i + 2];
                            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                                continue;
                            const osg::Vec3f edge1 = vertices[i1] - vertices[i0];
                            const osg::Vec3f edge2 = vertices[i2] - vertices[i0];
                            osg::Vec3f normal = edge1 ^ edge2;
                            if (normal.length2() <= 1e-10f)
                                continue;
                            normals[i0] += normal;
                            normals[i1] += normal;
                            normals[i2] += normal;
                        }
                        for (osg::Vec3f& normal : normals)
                        {
                            if (normal.length2() <= 1e-10f)
                                normal.set(0.f, 0.f, 1.f);
                            else
                                normal.normalize();
                        }
                        generatedNormals = true;
                    }

                    if (!vertices.empty())
                        geometry->setVertexArray(new osg::Vec3Array(vertices.size(), vertices.data()));
                    if (!normals.empty())
                        geometry->setNormalArray(
                            new osg::Vec3Array(normals.size(), normals.data()), osg::Array::BIND_PER_VERTEX);
                    if (!colors.empty())
                        geometry->setColorArray(
                            new osg::Vec4ubArray(colors.size(), colors.data()), osg::Array::BIND_PER_VERTEX);
                    if (!uvlist.empty())
                        geometry->setTexCoordArray(
                            0, new osg::Vec2Array(uvlist.size(), uvlist.data()), osg::Array::BIND_PER_VERTEX);

                    osg::ref_ptr<osg::Drawable> drawable = geometry;
                    std::size_t partitionInfluenceAssignments = 0;
                    std::size_t weightedVertices = 0;
                    std::size_t skinBoneCount = 0;
                    if (niSkinInstance != nullptr && !niSkinInstance->mData.empty()
                        && !niSkinInstance->mBones.empty() && !vertices.empty())
                    {
                        const Nif::NiSkinData* skinData = niSkinInstance->mData.getPtr();
                        const Nif::NiAVObjectList& bones = niSkinInstance->mBones;
                        std::vector<SceneUtil::RigGeometry::BoneInfo> boneInfo(bones.size());
                        std::vector<SceneUtil::RigGeometry::BoneWeights> influences(vertices.size());
                        for (std::size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex)
                        {
                            if (!bones[boneIndex].empty())
                                boneInfo[boneIndex].mName
                                    = Misc::StringUtils::lowerCase(bones[boneIndex].getPtr()->mName);
                            boneInfo[boneIndex].mInvBindMatrix.makeIdentity();
                            if (boneIndex < skinData->mBones.size())
                            {
                                boneInfo[boneIndex].mInvBindMatrix
                                    = skinData->mBones[boneIndex].mTransform.toMatrix();
                                boneInfo[boneIndex].mBoundSphere = skinData->mBones[boneIndex].mBoundSphere;
                            }
                        }

                        const auto addInfluence = [&](std::size_t vertexIndex, std::size_t boneIndex, float weight) {
                            if (vertexIndex >= influences.size() || boneIndex >= boneInfo.size() || weight <= 0.f)
                                return;
                            SceneUtil::RigGeometry::BoneWeights& vertexWeights = influences[vertexIndex];
                            const auto existing = std::find_if(vertexWeights.begin(), vertexWeights.end(),
                                [boneIndex](const auto& value) { return value.first == boneIndex; });
                            if (existing == vertexWeights.end())
                                vertexWeights.emplace_back(boneIndex, weight);
                            else
                                existing->second = std::max(existing->second, weight);
                            ++partitionInfluenceAssignments;
                        };

                        for (const Nif::NiSkinPartition::Partition& partition : niSkinPartitions->mPartitions)
                        {
                            const std::size_t partitionVertexCount = partition.mVertexMap.size();
                            if (partitionVertexCount == 0 || partition.mWeights.empty()
                                || partition.mBoneIndices.size() != partition.mWeights.size()
                                || partition.mWeights.size() % partitionVertexCount != 0)
                                continue;
                            const std::size_t bonesPerVertex = partition.mWeights.size() / partitionVertexCount;
                            for (std::size_t localVertex = 0; localVertex < partitionVertexCount; ++localVertex)
                            {
                                const std::size_t globalVertex = partition.mVertexMap[localVertex];
                                for (std::size_t influenceIndex = 0; influenceIndex < bonesPerVertex;
                                     ++influenceIndex)
                                {
                                    const std::size_t flatIndex = localVertex * bonesPerVertex + influenceIndex;
                                    const std::size_t partitionBone
                                        = static_cast<unsigned char>(partition.mBoneIndices[flatIndex]);
                                    if (partitionBone >= partition.mBones.size())
                                        continue;
                                    addInfluence(globalVertex, partition.mBones[partitionBone],
                                        partition.mWeights[flatIndex]);
                                }
                            }
                        }

                        // Older layouts can retain the classic per-bone weights even when their
                        // draw data moved into NiSkinPartition. Use them only if the partition map
                        // did not provide influences.
                        if (partitionInfluenceAssignments == 0)
                        {
                            for (std::size_t boneIndex = 0;
                                 boneIndex < std::min(bones.size(), skinData->mBones.size()); ++boneIndex)
                            {
                                for (const auto& [vertexIndex, weight] : skinData->mBones[boneIndex].mWeights)
                                    addInfluence(vertexIndex, boneIndex, weight);
                            }
                        }

                        for (SceneUtil::RigGeometry::BoneWeights& vertexWeights : influences)
                        {
                            float total = 0.f;
                            for (const auto& [boneIndex, weight] : vertexWeights)
                                total += weight;
                            if (total <= 0.f)
                                continue;
                            for (auto& [boneIndex, weight] : vertexWeights)
                                weight /= total;
                            ++weightedVertices;
                        }

                        if (weightedVertices > 0)
                        {
                            osg::ref_ptr<SceneUtil::RigGeometry> rig(new SceneUtil::RigGeometry);
                            rig->setSourceGeometry(geometry);
                            rig->setBoneInfo(std::move(boneInfo));
                            rig->setInfluences(influences);
                            rig->setTransform(skinData->mTransform.toMatrix());
                            // The partition path is used by SSE actor pieces and needs the same
                            // bind-pose diagnostics/fallback selection as other Bethesda rigs.
                            rig->setFalloutCharacterSkinning(true);
                            const bool skyrimSourceFrameFacePart
                                = mBethVersion >= Nif::NIFFile::BethVersion::BETHVER_SKY
                                && mBethVersion < Nif::NIFFile::BethVersion::BETHVER_FO4
                                && (containsAny(filename,
                                        { "/character assets/hair/", "\\character assets\\hair\\",
                                            "/character assets/beards/", "\\character assets\\beards\\",
                                            "/character assets/faceparts/", "\\character assets\\faceparts\\",
                                            "/character assets/mouth/", "\\character assets\\mouth\\" })
                                    || filename.ends_with("/character assets/eyesmale.nif")
                                    || filename.ends_with("\\character assets\\eyesmale.nif")
                                    || filename.ends_with("/character assets/eyesfemale.nif")
                                    || filename.ends_with("\\character assets\\eyesfemale.nif"));
                            // These small SSE face children are authored in the actor's source frame, but their
                            // one/two-bone skin records omit the inverse root bind carried by the body. Applying the
                            // full head matrix therefore adds the skeleton's 120-unit root twice. Keep these surfaces
                            // in source space while the base head/body and equipment remain genuinely skinned.
                            rig->setSourceFrameSkinning(skyrimSourceFrameFacePart);
                            // SSE actor parts are mounted below the authored skeleton root, whose transform already
                            // supplies the skin-to-skeleton frame. Applying its inverse again lifts heads, hands, and
                            // bodies one full character height above their armor. Keep the inverse available only for
                            // bounded diagnostics of unusual partition assets.
                            rig->setInverseSkinToSkeletonMatrix(
                                worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_USE_INVERSE_SSE_SKIN_ROOT"));
                            if (const Nif::NiAVObject* rootBone = niSkinInstance->mRoot.getPtr())
                                rig->setRootBone(rootBone->mName);
                            skinBoneCount = bones.size();
                            drawable = rig;
                        }
                    }

                    if (geometry->getNumPrimitiveSets() != 0 && geometry->getVertexArray() != nullptr)
                    {
                        std::vector<const Nif::NiProperty*> drawableProps;
                        collectDrawableProperties(nifNode, parent, drawableProps);
                        if (!bsTriShape->mShaderProperty.empty())
                            drawableProps.emplace_back(bsTriShape->mShaderProperty.getPtr());
                        if (!bsTriShape->mAlphaProperty.empty())
                            drawableProps.emplace_back(bsTriShape->mAlphaProperty.getPtr());
                        applyDrawableProperties(
                            parentNode, drawableProps, composite, !colors.empty(), animflags, &boundTextures);

                        drawable->setName(nifNode->mName);
                        applyWorldViewerFlatDrawable(
                            *drawable, filename, nifNode->mName, getStarfieldShaderMaterialName(bsTriShape));
                        parentNode->addChild(drawable);
                    }

                    if (worldViewerBSLedger)
                    {
                        Log(Debug::Info)
                            << "World viewer bs geometry partition-fallback: file=\"" << mFilename.generic_string()
                            << "\" shape=\"" << nifNode->mName << "\""
                            << " primitiveSets=" << partitionPrimitiveSets
                            << " remappedPrimitiveSets=" << remappedPrimitiveSets
                            << " invalidPrimitiveSets=" << invalidPrimitiveSets
                            << " vertices=" << vertices.size()
                            << " normals=" << normals.size()
                            << " generatedNormals=" << generatedNormals
                            << " colors=" << colors.size()
                            << " ignoredVertexColors=" << ignoredVertexColors
                            << " uvs=" << uvlist.size()
                            << " maxIndex=" << maxIndexSeen
                            << " dynamicVertices=" << usedDynamicVertices
                            << " skinRig=" << (dynamic_cast<SceneUtil::RigGeometry*>(drawable.get()) != nullptr)
                            << " skinBones=" << skinBoneCount
                            << " weightedVertices=" << weightedVertices
                            << " influenceAssignments=" << partitionInfluenceAssignments
                            << " vertexFlags=0x" << std::hex << niSkinPartitions->mVertexDesc.mFlags << std::dec
                            << " attached="
                            << (geometry->getNumPrimitiveSets() != 0 && geometry->getVertexArray() != nullptr);
                    }
                }
                return;
            }

            osg::ref_ptr<osg::Geometry> geometry(new osg::Geometry);
            geometry->addPrimitiveSet(
                new osg::DrawElementsUShort(osg::PrimitiveSet::TRIANGLES, triangles.size(), triangles.data()));

            osg::ref_ptr<osg::Drawable> drawable = geometry;

            // Some input geometry may not be used as is so it needs to be converted.
            // Normals, tangents and bitangents use a special normal map-like format not equivalent to snorm8 or unorm8
            auto normbyteToFloat = [](uint8_t value) { return value / 255.f * 2.f - 1.f; };
            // Vertices and UV sets may be half-precision.
            // OSG doesn't have a way to pass half-precision data at the moment.
            auto halfToFloat = [](uint16_t value) {
                uint32_t bits = static_cast<uint32_t>(value & 0x8000) << 16;

                const uint32_t exp16 = (value & 0x7c00) >> 10;
                uint32_t frac16 = value & 0x3ff;
                if (exp16)
                    bits |= (exp16 + 0x70) << 23;
                else if (frac16)
                {
                    uint8_t offset = 0;
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
            };

            const bool fullPrec = bsTriShape->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Full_Precision;
            const bool hasVertices = bsTriShape->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Vertex;
            const bool hasNormals = bsTriShape->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Normals;
            const bool hasColors = bsTriShape->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Vertex_Colors;
            const bool hasUV = bsTriShape->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::UVs;

            std::vector<osg::Vec3f> vertices;
            std::vector<osg::Vec3f> normals;
            std::vector<osg::Vec4ub> colors;
            std::vector<osg::Vec2f> uvlist;
            for (auto& elem : bsTriShape->mVertData)
            {
                if (hasVertices)
                {
                    if (fullPrec)
                        vertices.emplace_back(elem.mVertex.x(), elem.mVertex.y(), elem.mVertex.z());
                    else
                        vertices.emplace_back(halfToFloat(elem.mHalfVertex[0]), halfToFloat(elem.mHalfVertex[1]),
                            halfToFloat(elem.mHalfVertex[2]));
                }
                if (hasNormals)
                    normals.emplace_back(normbyteToFloat(elem.mNormal[0]), normbyteToFloat(elem.mNormal[1]),
                        normbyteToFloat(elem.mNormal[2]));
                if (hasColors)
                    colors.emplace_back(elem.mVertColor[0], elem.mVertColor[1], elem.mVertColor[2], elem.mVertColor[3]);
                if (hasUV)
                    uvlist.emplace_back(halfToFloat(elem.mUV[0]), 1.0f - halfToFloat(elem.mUV[1]));
            }

            if (!vertices.empty())
                geometry->setVertexArray(new osg::Vec3Array(vertices.size(), vertices.data()));
            if (!normals.empty())
                geometry->setNormalArray(
                    new osg::Vec3Array(normals.size(), normals.data()), osg::Array::BIND_PER_VERTEX);
            if (!colors.empty())
                geometry->setColorArray(
                    new osg::Vec4ubArray(colors.size(), colors.data()), osg::Array::BIND_PER_VERTEX);
            if (!uvlist.empty())
                geometry->setTexCoordArray(
                    0, new osg::Vec2Array(uvlist.size(), uvlist.data()), osg::Array::BIND_PER_VERTEX);

            // This is the skinning data Fallout 4 provides
            // TODO: support Skyrim SE skinning data
            if (!bsTriShape->mSkin.empty() && bsTriShape->mSkin->recType == Nif::RC_BSSkinInstance
                && bsTriShape->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Skinned)
            {
                osg::ref_ptr<SceneUtil::RigGeometry> rig(new SceneUtil::RigGeometry);
                rig->setSourceGeometry(std::move(geometry));

                const Nif::BSSkinInstance* skin = static_cast<const Nif::BSSkinInstance*>(bsTriShape->mSkin.getPtr());
                const Nif::BSSkinBoneData* data = skin->mData.getPtr();
                const Nif::NiAVObjectList& bones = skin->mBones;

                std::vector<SceneUtil::RigGeometry::BoneInfo> boneInfo;
                std::vector<SceneUtil::RigGeometry::BoneWeights> influences;
                boneInfo.resize(bones.size());
                influences.resize(vertices.size());
                for (std::size_t i = 0; i < bones.size(); ++i)
                {
                    boneInfo[i].mName = Misc::StringUtils::lowerCase(bones[i].getPtr()->mName);
                    boneInfo[i].mInvBindMatrix = data->mBones[i].mTransform.toMatrix();
                    boneInfo[i].mBoundSphere = data->mBones[i].mBoundSphere;
                }

                for (size_t i = 0; i < vertices.size(); i++)
                {
                    const Nif::BSVertexData& vertData = bsTriShape->mVertData[i];
                    for (int j = 0; j < 4; j++)
                        influences[i].emplace_back(vertData.mBoneIndices[j], halfToFloat(vertData.mBoneWeights[j]));
                }
                rig->setBoneInfo(std::move(boneInfo));
                rig->setInfluences(influences);
                if (const Nif::NiAVObject* rootBone = skin->mRoot.getPtr())
                    rig->setRootBone(rootBone->mName);

                drawable = rig;
            }

            std::vector<const Nif::NiProperty*> drawableProps;
            collectDrawableProperties(nifNode, parent, drawableProps);
            if (!bsTriShape->mShaderProperty.empty())
                drawableProps.emplace_back(bsTriShape->mShaderProperty.getPtr());
            if (!bsTriShape->mAlphaProperty.empty())
                drawableProps.emplace_back(bsTriShape->mAlphaProperty.getPtr());
            applyDrawableProperties(parentNode, drawableProps, composite, !colors.empty(), animflags, &boundTextures);

            drawable->setName(nifNode->mName);
            applyWorldViewerFlatDrawable(
                *drawable, filename, nifNode->mName, getStarfieldShaderMaterialName(bsTriShape));
            parentNode->addChild(drawable);
            if (worldViewerBSLedger)
            {
                Log(Debug::Info) << "World viewer bs geometry attached: file=\"" << mFilename.generic_string()
                                 << "\" shape=\"" << nifNode->mName << "\""
                                 << " primitiveSets=" << geometry->getNumPrimitiveSets()
                                 << " drawableClass=\"" << drawable->className() << "\""
                                 << " vertices=" << vertices.size()
                                 << " normals=" << normals.size()
                                 << " colors=" << colors.size()
                                 << " skinRig=" << (dynamic_cast<SceneUtil::RigGeometry*>(drawable.get()) != nullptr);
            }
        }

        bool handleStarfieldExternalGeometry(const Nif::BSTriShape* bsTriShape, osg::Group* parentNode)
        {
            if (bsTriShape->mExternalGeometry.empty())
                return false;

            const VFS::Manager* vfs = nullptr;
            if (mMaterialManager)
                vfs = mMaterialManager->getVFS();
            if (!vfs && mImageManager)
                vfs = mImageManager->getVFS();
            if (!vfs)
                return false;

            const auto& meshRef = bsTriShape->mExternalGeometry.front();
            const std::string nifPath = mFilename.generic_string();
            const std::string shapeName = bsTriShape->mName.empty() ? bsTriShape->recName : bsTriShape->mName;
            const std::string shaderMaterialName = getStarfieldShaderMaterialName(bsTriShape);
            std::string actorProofKey = nifPath + "|" + shapeName + "|" + shaderMaterialName;
            std::replace(actorProofKey.begin(), actorProofKey.end(), '\\', '/');
            if (shouldSkipStarfieldWorldProofGeometry(nifPath, shapeName, shaderMaterialName))
            {
                static std::atomic<int> skipLogCount{ 0 };
                const int logIndex = skipLogCount.fetch_add(1);
                if (logIndex < 160)
                {
                    Log(Debug::Info) << "World viewer: Starfield world proof skipped geometry nif=\"" << nifPath
                                     << "\" shape=\"" << shapeName << "\""
                                     << " material=\"" << shaderMaterialName << "\""
                                     << " reason=\"unsupported occluder or Starfield water shader\"";
                }
                else if (logIndex == 160)
                    Log(Debug::Info) << "World viewer: further Starfield skipped geometry logs suppressed";
                return true;
            }
            const osg::Vec4f proofColor = getStarfieldActorProofBaseColor(actorProofKey);
            std::string meshPath = meshRef.mMeshPath;
            std::replace(meshPath.begin(), meshPath.end(), '\\', '/');
            const std::string loweredMeshPath = Misc::StringUtils::lowerCase(meshPath);
            if (loweredMeshPath.rfind("geometries/", 0) != 0)
                meshPath.insert(0, "geometries/");
            if (!Misc::StringUtils::ciEndsWith(meshPath, ".mesh"))
                meshPath += ".mesh";

            VFS::Path::Normalized normalizedPath(meshPath);
            Files::IStreamPtr stream = vfs->find(normalizedPath);
            if (!stream)
            {
                logStarfieldMeshFailure(normalizedPath.view(), "not found");
                return false;
            }

            std::string error;
            std::optional<StarfieldExternalMeshData> mesh = readStarfieldExternalMesh(*stream, error);
            if (!mesh)
            {
                logStarfieldMeshFailure(normalizedPath.view(), error);
                return false;
            }

            const bool actorSurface = isWorldViewerActorMeshPath(Misc::StringUtils::lowerCase(nifPath));
            const bool wantsExternalSkinning
                = worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_STARFIELD_EXTERNAL_SKINNING") && actorSurface;
            const Nif::BSSkinInstance* skin = nullptr;
            const Nif::SkinAttach* skinAttach = nullptr;
            if (wantsExternalSkinning && !bsTriShape->mSkin.empty()
                && bsTriShape->mSkin->recType == Nif::RC_BSSkinInstance)
            {
                skin = static_cast<const Nif::BSSkinInstance*>(bsTriShape->mSkin.getPtr());
                skinAttach = findStarfieldSkinAttach(bsTriShape);
                if (skinAttach == nullptr && !skin->mRoot.empty())
                    skinAttach = findStarfieldSkinAttach(skin->mRoot.getPtr());
            }

            const std::size_t boneCount = skin != nullptr && !skin->mData.empty() ? skin->mData->mBones.size() : 0;
            const std::size_t expectedWeights
                = mesh->mVertices.size() * static_cast<std::size_t>(mesh->mWeightCountPerVertex);
            const bool canSkin = skin != nullptr && skinAttach != nullptr && boneCount > 0
                && skinAttach->mBones.size() == boneCount && mesh->mWeightCountPerVertex > 0
                && mesh->mWeights.size() >= expectedWeights;
            const StarfieldWeightLayout weightLayout = canSkin
                ? chooseStarfieldWeightLayout(*mesh, boneCount)
                : StarfieldWeightLayout::LowIndexHighHalf;

            std::vector<osg::Vec3f> renderVertices = mesh->mVertices;
            if (canSkin)
            {
                // Starfield stores actor meshes and skeleton bind transforms in meters, while placed world geometry
                // is converted to Bethesda world units by OPENMW_STARFIELD_MESH_POSITION_SCALE. Keep the skinned
                // source in its native meter frame; the actor root wrapper applies the same scale coherently to the
                // skeleton, its bone translations, and every assembled body part.
                const float actorScale = getStarfieldMeshPositionScale();
                for (osg::Vec3f& vertex : renderVertices)
                    vertex /= actorScale;
            }

            osg::ref_ptr<osg::Geometry> geometry(new osg::Geometry);
            geometry->setVertexArray(new osg::Vec3Array(renderVertices.size(), renderVertices.data()));
            geometry->addPrimitiveSet(new osg::DrawElementsUInt(
                osg::PrimitiveSet::TRIANGLES, mesh->mIndices.size(), mesh->mIndices.data()));
            if (mesh->mUv1.size() == mesh->mVertices.size())
                geometry->setTexCoordArray(0, new osg::Vec2Array(mesh->mUv1.size(), mesh->mUv1.data()));

            osg::ref_ptr<osg::Vec4Array> colors(new osg::Vec4Array);
            const std::string actorDiffuse = getStarfieldActorProofDiffuse(actorProofKey);
            const StarfieldMaterialBridgeEntry* authoredMaterial = !actorSurface
                && worldViewerStarfieldWorldProofTexturesEnabled()
                ? findStarfieldMaterialBridge(shaderMaterialName)
                : nullptr;
            std::string worldDiffuse = authoredMaterial != nullptr
                ? authoredMaterial->mDiffuse
                : std::string();
            if (worldDiffuse.empty() && !actorSurface
                && worldViewerStarfieldWorldProofTexturesEnabled() && !isStarfieldMaterialBridgeConfigured())
                worldDiffuse = getStarfieldWorldProofDiffuse(nifPath, shapeName, shaderMaterialName);
            std::string diffuse = actorSurface ? actorDiffuse : worldDiffuse;
            if (!diffuse.empty() && !vfs->exists(VFS::Path::Normalized(diffuse)))
            {
                static std::atomic<int> missingMappedTextureLogCount{ 0 };
                const int logIndex = missingMappedTextureLogCount.fetch_add(1);
                if (logIndex < 80)
                {
                    Log(Debug::Warning) << "World viewer: Starfield mapped texture unavailable material=\""
                                        << shaderMaterialName << "\" diffuse=\"" << diffuse << "\"";
                }
                else if (logIndex == 80)
                    Log(Debug::Warning) << "World viewer: further missing Starfield mapped texture logs suppressed";
                diffuse.clear();
                if (authoredMaterial != nullptr)
                    authoredMaterial = nullptr;
            }
            const bool hasDiffuse = !diffuse.empty();
            const osg::Vec4f surfaceColor = actorSurface
                ? (hasDiffuse ? osg::Vec4f(1.f, 1.f, 1.f, 1.f) : proofColor)
                : getStarfieldWorldMaterialColor(shaderMaterialName, hasDiffuse);
            colors->push_back(surfaceColor);
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);

            if (mesh->mNormals.size() == mesh->mVertices.size())
                geometry->setNormalArray(new osg::Vec3Array(mesh->mNormals.size(), mesh->mNormals.data()),
                    osg::Array::BIND_PER_VERTEX);
            else
            {
                osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
                normals->push_back(osg::Vec3f(0.f, 0.f, 1.f));
                geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
            }

            osg::ref_ptr<osg::Material> material(new osg::Material);
            const osg::Vec4f materialColor = surfaceColor;
            material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
            material->setDiffuse(osg::Material::FRONT_AND_BACK, materialColor);
            material->setAmbient(osg::Material::FRONT_AND_BACK, materialColor);
            material->setEmission(osg::Material::FRONT_AND_BACK, materialColor);

            osg::StateSet* stateset = geometry->getOrCreateStateSet();
            stateset->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            const bool translucent = !actorSurface && isStarfieldWorldMaterialTranslucent(shaderMaterialName);
            const bool doubleSided = actorSurface
                || (!actorSurface && isStarfieldWorldMaterialDoubleSided(nifPath, shaderMaterialName));
            if (doubleSided)
                stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            else
            {
                stateset->setMode(GL_CULL_FACE, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateset->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK),
                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }
            handleDepthFlags(stateset, true, !translucent);
            if (translucent)
            {
                stateset->setMode(GL_BLEND, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateset->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
            }
            else
                stateset->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            const bool cutout = actorSurface ? isStarfieldActorProofCutout(actorProofKey)
                                             : isStarfieldWorldMaterialCutout(nifPath, shaderMaterialName);
            if (cutout)
            {
                stateset->setMode(GL_ALPHA_TEST, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateset->setAttributeAndModes(
                    new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.35f), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateset->setRenderingHint(osg::StateSet::DEFAULT_BIN);
            }
            else
                stateset->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            if (!diffuse.empty() && mesh->mUv1.size() == mesh->mVertices.size())
            {
                std::vector<unsigned int> boundTextures;
                attachExternalTexture("diffuseMap", diffuse, true, true, 0, stateset, boundTextures);
                if (!boundTextures.empty())
                {
                    stateset->setTextureMode(0, GL_TEXTURE_2D, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                    stateset->setTextureAttributeAndModes(
                        0, new osg::TexEnv(actorSurface ? osg::TexEnv::REPLACE : osg::TexEnv::MODULATE),
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                }
                if (!actorDiffuse.empty())
                {
                    Log(Debug::Info) << "World viewer: Starfield actor proof texture nif=\"" << nifPath << "\""
                                     << " shape=\"" << shapeName << "\""
                                     << " material=\"" << shaderMaterialName << "\""
                                     << " diffuse=\"" << diffuse << "\""
                                     << " uv1=" << mesh->mUv1.size()
                                     << " boundTextureUnits=" << boundTextures.size()
                                     << " alphaCutout=" << cutout;
                }
                else
                {
                    Log(Debug::Info) << "World viewer: Starfield world proof texture nif=\"" << nifPath << "\""
                                     << " shape=\"" << shapeName << "\""
                                     << " material=\"" << shaderMaterialName << "\""
                                     << " diffuse=\"" << diffuse << "\""
                                     << " source=\"" << (authoredMaterial != nullptr ? "materialsbeta.cdb" : "heuristic") << "\""
                                     << " uv1=" << mesh->mUv1.size()
                                     << " boundTextureUnits=" << boundTextures.size();
                }
            }
            else
            {
                for (unsigned int texUnit = 0; texUnit < 8; ++texUnit)
                    stateset->setTextureMode(texUnit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                if (isStarfieldActorProofEyeSurface(actorProofKey))
                    Log(Debug::Info) << "World viewer: Starfield actor proof eye material fallback nif=" << mFilename
                                     << " color=(" << proofColor.r() << "," << proofColor.g() << ","
                                     << proofColor.b() << "," << proofColor.a() << ")";
                else if (!actorSurface)
                    Log(Debug::Info) << "World viewer: Starfield world material fallback nif=\"" << nifPath << "\""
                                     << " shape=\"" << shapeName << "\""
                                     << " material=\"" << shaderMaterialName << "\""
                                     << " uv1=" << mesh->mUv1.size()
                                     << " vertices=" << mesh->mVertices.size()
                                     << " reason=\"" << (diffuse.empty() ? "no diffuse bridge" : "missing uv") << "\"";
            }

            osg::ref_ptr<osg::Drawable> drawable = geometry;
            if (canSkin)
            {
                std::vector<SceneUtil::RigGeometry::BoneInfo> boneInfo(boneCount);
                for (std::size_t bone = 0; bone < boneCount; ++bone)
                {
                    // Starfield's shared Skeleton stores its native case-sensitive bone keys (for example C_Head).
                    // SkinAttach already carries the exact contract names; lower-casing them makes every otherwise
                    // valid influence miss the live skeleton.
                    boneInfo[bone].mName = skinAttach->mBones[bone];
                    boneInfo[bone].mInvBindMatrix = skin->mData->mBones[bone].mTransform.toMatrix();
                    boneInfo[bone].mBoundSphere = skin->mData->mBones[bone].mBoundSphere;
                }

                std::vector<SceneUtil::RigGeometry::BoneWeights> influences(renderVertices.size());
                std::size_t weightedVertices = 0;
                std::size_t invalidWeights = 0;
                float minWeightSum = std::numeric_limits<float>::max();
                float maxWeightSum = 0.f;
                for (std::size_t vertex = 0; vertex < renderVertices.size(); ++vertex)
                {
                    float sum = 0.f;
                    SceneUtil::RigGeometry::BoneWeights& vertexInfluences = influences[vertex];
                    for (std::uint32_t slot = 0; slot < mesh->mWeightCountPerVertex; ++slot)
                    {
                        const std::size_t weightIndex
                            = vertex * static_cast<std::size_t>(mesh->mWeightCountPerVertex) + slot;
                        const auto [bone, weight] = decodeStarfieldWeight(mesh->mWeights[weightIndex], weightLayout);
                        if (bone >= boneCount || !std::isfinite(weight) || weight < 0.f || weight > 1.001f)
                        {
                            ++invalidWeights;
                            continue;
                        }
                        if (weight <= 0.00001f)
                            continue;
                        vertexInfluences.emplace_back(bone, weight);
                        sum += weight;
                    }
                    if (sum > 0.00001f)
                    {
                        ++weightedVertices;
                        if (std::abs(sum - 1.f) > 0.001f)
                        {
                            for (auto& [bone, weight] : vertexInfluences)
                                weight /= sum;
                            sum = 1.f;
                        }
                    }
                    minWeightSum = std::min(minWeightSum, sum);
                    maxWeightSum = std::max(maxWeightSum, sum);
                }

                osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
                rig->setSourceGeometry(geometry);
                rig->setBoneInfo(std::move(boneInfo));
                rig->setInfluences(influences);
                // ExportScene is a file container, not a bone in the shared live human skeleton. An empty skin root
                // lets RigGeometry derive the attachment-to-skeleton transform from the actual scene path.
                rig->setRootBone({});
                drawable = rig;

                std::ostringstream rawSample;
                const std::size_t sampleCount = std::min<std::size_t>(mesh->mWeights.size(), 12);
                for (std::size_t i = 0; i < sampleCount; ++i)
                {
                    if (i != 0)
                        rawSample << ',';
                    rawSample << "0x" << std::hex << mesh->mWeights[i] << std::dec;
                }
                Log(Debug::Info) << "World viewer: Starfield external skin attached nif=\"" << nifPath
                                 << "\" shape=\"" << shapeName << "\""
                                 << " bones=" << boneCount
                                 << " skinAttachBones=" << skinAttach->mBones.size()
                                 << " weightsPerVertex=" << mesh->mWeightCountPerVertex
                                 << " weights=" << mesh->mWeights.size()
                                 << " weightedVertices=" << weightedVertices
                                 << " invalidWeights=" << invalidWeights
                                 << " weightSum=(" << minWeightSum << ',' << maxWeightSum << ')'
                                 << " layout=\"" << getStarfieldWeightLayoutName(weightLayout) << "\""
                                 << " raw=[" << rawSample.str() << ']';
            }
            else if (wantsExternalSkinning)
            {
                Log(Debug::Warning) << "World viewer: Starfield external skin unavailable nif=\"" << nifPath
                                    << "\" shape=\"" << shapeName << "\""
                                    << " skin=" << (skin != nullptr)
                                    << " skinAttach=" << (skinAttach != nullptr)
                                    << " skinAttachBones=" << (skinAttach != nullptr ? skinAttach->mBones.size() : 0)
                                    << " boneData=" << boneCount
                                    << " weightsPerVertex=" << mesh->mWeightCountPerVertex
                                    << " weights=" << mesh->mWeights.size()
                                    << " expectedWeights=" << expectedWeights;
            }

            geometry->setName(shapeName);
            drawable->setName(shapeName);
            parentNode->addChild(drawable);

            logStarfieldMeshLoaded(normalizedPath.view(), *mesh, meshRef, nifPath, shapeName, shaderMaterialName, diffuse);
            return true;
        }

        static void logStarfieldMeshLoaded(std::string_view path, const StarfieldExternalMeshData& mesh,
            const Nif::BSTriShape::BSGeometryMeshRef& meshRef, std::string_view nifPath, std::string_view shapeName,
            std::string_view shaderMaterialName, std::string_view diffuse)
        {
            static std::atomic<int> meshLogCount{ 0 };
            const int logIndex = meshLogCount.fetch_add(1);
            if (logIndex < 80)
            {
                Log(Debug::Info) << "World viewer: Starfield mesh loaded path=" << path
                                 << " nif=\"" << nifPath << "\""
                                 << " shape=\"" << shapeName << "\""
                                 << " material=\"" << shaderMaterialName << "\""
                                 << " diffuse=\"" << diffuse << "\""
                                 << " vertices=" << mesh.mVertices.size()
                                 << " indices=" << mesh.mIndices.size()
                                 << " scale=" << mesh.mScale
                                 << " positionScale=" << getStarfieldMeshPositionScale()
                                 << " refVerts=" << meshRef.mVertexCount
                                 << " refTriIndices=" << meshRef.mTriangleIndexCount
                                 << " uv1=" << mesh.mUv1Count
                                 << " normals=" << mesh.mNormalCount
                                 << " weightsPerVertex=" << mesh.mWeightCountPerVertex
                                 << " weights=" << mesh.mWeights.size()
                                 << " lods=" << mesh.mLodCount
                                 << " meshlets=" << mesh.mMeshletCount;
            }
            else if (logIndex == 80)
                Log(Debug::Info) << "World viewer: further Starfield mesh load logs suppressed";
        }

        static void logStarfieldMeshFailure(std::string_view path, std::string_view reason)
        {
            static std::atomic<int> meshFailureLogCount{ 0 };
            const int logIndex = meshFailureLogCount.fetch_add(1);
            if (logIndex < 80)
                Log(Debug::Warning) << "World viewer: Starfield mesh load failed path=" << path
                                    << " reason=" << reason;
            else if (logIndex == 80)
                Log(Debug::Warning) << "World viewer: further Starfield mesh failure logs suppressed";
        }

        void handleBSGeometryProxy(const Nif::BSTriShape* bsTriShape, osg::Group* parentNode)
        {
            osg::Vec3f minBound(bsTriShape->mBoundMinMax[0], bsTriShape->mBoundMinMax[1], bsTriShape->mBoundMinMax[2]);
            osg::Vec3f maxBound(bsTriShape->mBoundMinMax[3], bsTriShape->mBoundMinMax[4], bsTriShape->mBoundMinMax[5]);

            const bool hasMinMax = minBound.x() < maxBound.x() && minBound.y() < maxBound.y()
                && minBound.z() < maxBound.z();
            if (!hasMinMax)
            {
                const osg::Vec3f center = bsTriShape->mBoundingSphere.center();
                const float radius = std::max(bsTriShape->mBoundingSphere.radius(), 8.f);
                minBound = center - osg::Vec3f(radius, radius, radius);
                maxBound = center + osg::Vec3f(radius, radius, radius);
            }

            osg::ref_ptr<osg::Geometry> geometry(new osg::Geometry);
            osg::ref_ptr<osg::Vec3Array> vertices(new osg::Vec3Array);
            vertices->reserve(8);
            vertices->push_back(osg::Vec3f(minBound.x(), minBound.y(), minBound.z()));
            vertices->push_back(osg::Vec3f(maxBound.x(), minBound.y(), minBound.z()));
            vertices->push_back(osg::Vec3f(maxBound.x(), maxBound.y(), minBound.z()));
            vertices->push_back(osg::Vec3f(minBound.x(), maxBound.y(), minBound.z()));
            vertices->push_back(osg::Vec3f(minBound.x(), minBound.y(), maxBound.z()));
            vertices->push_back(osg::Vec3f(maxBound.x(), minBound.y(), maxBound.z()));
            vertices->push_back(osg::Vec3f(maxBound.x(), maxBound.y(), maxBound.z()));
            vertices->push_back(osg::Vec3f(minBound.x(), maxBound.y(), maxBound.z()));
            geometry->setVertexArray(vertices);

            static constexpr std::array<unsigned short, 36> indices = {
                0, 1, 2, 0, 2, 3, // bottom
                4, 6, 5, 4, 7, 6, // top
                0, 4, 5, 0, 5, 1, // front
                1, 5, 6, 1, 6, 2, // right
                2, 6, 7, 2, 7, 3, // back
                3, 7, 4, 3, 4, 0, // left
            };
            geometry->addPrimitiveSet(new osg::DrawElementsUShort(
                osg::PrimitiveSet::TRIANGLES, indices.size(), indices.data()));

            osg::ref_ptr<osg::Vec4Array> colors(new osg::Vec4Array);
            colors->push_back(osg::Vec4f(0.84f, 0.86f, 0.82f, 1.f));
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);

            osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
            normals->push_back(osg::Vec3f(0.f, 0.f, 1.f));
            geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);

            osg::ref_ptr<osg::Material> material(new osg::Material);
            const osg::Vec4f proofColor = colors->front();
            material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
            material->setDiffuse(osg::Material::FRONT_AND_BACK, proofColor);
            material->setAmbient(osg::Material::FRONT_AND_BACK, proofColor);
            material->setEmission(osg::Material::FRONT_AND_BACK, proofColor);
            osg::StateSet* stateset = geometry->getOrCreateStateSet();
            stateset->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateset->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateset->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            for (unsigned int texUnit = 0; texUnit < 8; ++texUnit)
                stateset->setTextureMode(texUnit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            geometry->setName(bsTriShape->mName.empty() ? bsTriShape->recName : bsTriShape->mName);
            parentNode->addChild(geometry);

            static std::atomic<int> proxyLogCount{ 0 };
            const int logIndex = proxyLogCount.fetch_add(1);
            if (logIndex < 80)
            {
                const auto& mesh = bsTriShape->mExternalGeometry.front();
                Log(Debug::Info) << "World viewer: Starfield BSGeometry proxy mesh=geometries/" << mesh.mMeshPath
                                 << ".mesh verts=" << mesh.mVertexCount
                                 << " triIndices=" << mesh.mTriangleIndexCount << " nif=" << mFilename;
            }
            else if (logIndex == 80)
                Log(Debug::Info) << "World viewer: further Starfield BSGeometry proxy logs suppressed";
        }

        osg::BlendFunc::BlendFuncMode getBlendMode(int mode) const
        {
            switch (mode)
            {
                case 0:
                    return osg::BlendFunc::ONE;
                case 1:
                    return osg::BlendFunc::ZERO;
                case 2:
                    return osg::BlendFunc::SRC_COLOR;
                case 3:
                    return osg::BlendFunc::ONE_MINUS_SRC_COLOR;
                case 4:
                    return osg::BlendFunc::DST_COLOR;
                case 5:
                    return osg::BlendFunc::ONE_MINUS_DST_COLOR;
                case 6:
                    return osg::BlendFunc::SRC_ALPHA;
                case 7:
                    return osg::BlendFunc::ONE_MINUS_SRC_ALPHA;
                case 8:
                    return osg::BlendFunc::DST_ALPHA;
                case 9:
                    return osg::BlendFunc::ONE_MINUS_DST_ALPHA;
                case 10:
                    return osg::BlendFunc::SRC_ALPHA_SATURATE;
                default:
                    Log(Debug::Info) << "Unexpected blend mode: " << mode << " in " << mFilename;
                    return osg::BlendFunc::SRC_ALPHA;
            }
        }

        osg::AlphaFunc::ComparisonFunction getTestMode(int mode) const
        {
            switch (mode)
            {
                case 0:
                    return osg::AlphaFunc::ALWAYS;
                case 1:
                    return osg::AlphaFunc::LESS;
                case 2:
                    return osg::AlphaFunc::EQUAL;
                case 3:
                    return osg::AlphaFunc::LEQUAL;
                case 4:
                    return osg::AlphaFunc::GREATER;
                case 5:
                    return osg::AlphaFunc::NOTEQUAL;
                case 6:
                    return osg::AlphaFunc::GEQUAL;
                case 7:
                    return osg::AlphaFunc::NEVER;
                default:
                    Log(Debug::Info) << "Unexpected blend mode: " << mode << " in " << mFilename;
                    return osg::AlphaFunc::LEQUAL;
            }
        }

        osg::Stencil::Function getStencilFunction(Nif::NiStencilProperty::TestFunc func) const
        {
            using TestFunc = Nif::NiStencilProperty::TestFunc;
            switch (func)
            {
                case TestFunc::Never:
                    return osg::Stencil::NEVER;
                case TestFunc::Less:
                    return osg::Stencil::LESS;
                case TestFunc::Equal:
                    return osg::Stencil::EQUAL;
                case TestFunc::LessEqual:
                    return osg::Stencil::LEQUAL;
                case TestFunc::Greater:
                    return osg::Stencil::GREATER;
                case TestFunc::NotEqual:
                    return osg::Stencil::NOTEQUAL;
                case TestFunc::GreaterEqual:
                    return osg::Stencil::GEQUAL;
                case TestFunc::Always:
                    return osg::Stencil::ALWAYS;
                default:
                    Log(Debug::Info) << "Unexpected stencil function: " << static_cast<uint32_t>(func) << " in "
                                     << mFilename;
                    return osg::Stencil::NEVER;
            }
        }

        osg::Stencil::Operation getStencilOperation(Nif::NiStencilProperty::Action op) const
        {
            using Action = Nif::NiStencilProperty::Action;
            switch (op)
            {
                case Action::Keep:
                    return osg::Stencil::KEEP;
                case Action::Zero:
                    return osg::Stencil::ZERO;
                case Action::Replace:
                    return osg::Stencil::REPLACE;
                case Action::Increment:
                    return osg::Stencil::INCR;
                case Action::Decrement:
                    return osg::Stencil::DECR;
                case Action::Invert:
                    return osg::Stencil::INVERT;
                default:
                    Log(Debug::Info) << "Unexpected stencil operation: " << static_cast<uint32_t>(op) << " in "
                                     << mFilename;
                    return osg::Stencil::KEEP;
            }
        }

        osg::ref_ptr<osg::Image> handleInternalTexture(const Nif::NiPixelData* pixelData) const
        {
            if (pixelData->mMipmaps.empty())
                return nullptr;

            // Not fatal, but warn the user
            if (pixelData->mNumFaces != 1)
                Log(Debug::Info) << "Unsupported multifaceted internal texture in " << mFilename;

            using Nif::NiPixelFormat;
            NiPixelFormat niPixelFormat = pixelData->mPixelFormat;
            GLenum pixelformat = 0;
            // Pixel row alignment. Defining it to be consistent with OSG DDS plugin
            int packing = 1;
            switch (niPixelFormat.mFormat)
            {
                case NiPixelFormat::Format::RGB:
                    pixelformat = GL_RGB;
                    break;
                case NiPixelFormat::Format::RGBA:
                    pixelformat = GL_RGBA;
                    break;
                case NiPixelFormat::Format::Palette:
                case NiPixelFormat::Format::PaletteAlpha:
                    pixelformat = GL_RED; // Each color is defined by a byte.
                    break;
                case NiPixelFormat::Format::BGR:
                    pixelformat = GL_BGR;
                    break;
                case NiPixelFormat::Format::BGRA:
                    pixelformat = GL_BGRA;
                    break;
                case NiPixelFormat::Format::DXT1:
                    pixelformat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
                    packing = 2;
                    break;
                case NiPixelFormat::Format::DXT3:
                    pixelformat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
                    packing = 4;
                    break;
                case NiPixelFormat::Format::DXT5:
                    pixelformat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                    packing = 4;
                    break;
                default:
                    Log(Debug::Info) << "Unhandled internal pixel format "
                                     << static_cast<uint32_t>(niPixelFormat.mFormat) << " in " << mFilename;
                    return nullptr;
            }

            int width = 0;
            int height = 0;

            std::vector<unsigned int> mipmapOffsets;
            for (unsigned int i = 0; i < pixelData->mMipmaps.size(); ++i)
            {
                const Nif::NiPixelData::Mipmap& mip = pixelData->mMipmaps[i];

                size_t mipSize = osg::Image::computeImageSizeInBytes(
                    mip.mWidth, mip.mHeight, 1, pixelformat, GL_UNSIGNED_BYTE, packing);
                if (mipSize + mip.mOffset > pixelData->mData.size())
                {
                    Log(Debug::Info) << "Internal texture's mipmap data out of bounds, ignoring texture";
                    return nullptr;
                }

                if (i != 0)
                    mipmapOffsets.push_back(mip.mOffset);
                else
                {
                    width = mip.mWidth;
                    height = mip.mHeight;
                }
            }

            if (width <= 0 || height <= 0)
            {
                Log(Debug::Info) << "Internal Texture Width and height must be non zero, ignoring texture";
                return nullptr;
            }

            osg::ref_ptr<osg::Image> image(new osg::Image);
            const std::vector<unsigned char>& pixels = pixelData->mData;
            switch (niPixelFormat.mFormat)
            {
                case NiPixelFormat::Format::RGB:
                case NiPixelFormat::Format::RGBA:
                case NiPixelFormat::Format::BGR:
                case NiPixelFormat::Format::BGRA:
                case NiPixelFormat::Format::DXT1:
                case NiPixelFormat::Format::DXT3:
                case NiPixelFormat::Format::DXT5:
                {
                    unsigned char* data = new unsigned char[pixels.size()];
                    memcpy(data, pixels.data(), pixels.size());
                    image->setImage(width, height, 1, pixelformat, pixelformat, GL_UNSIGNED_BYTE, data,
                        osg::Image::USE_NEW_DELETE, packing);
                    break;
                }
                case NiPixelFormat::Format::Palette:
                case NiPixelFormat::Format::PaletteAlpha:
                {
                    if (pixelData->mPalette.empty() || niPixelFormat.mBitsPerPixel != 8)
                    {
                        Log(Debug::Info) << "Palettized texture in " << mFilename << " is invalid, ignoring";
                        return nullptr;
                    }
                    pixelformat = niPixelFormat.mFormat == NiPixelFormat::Format::PaletteAlpha ? GL_RGBA : GL_RGB;
                    // We're going to convert the indices that pixel data contains
                    // into real colors using the palette.
                    const auto& palette = pixelData->mPalette->mColors;
                    const int numChannels = pixelformat == GL_RGBA ? 4 : 3;
                    unsigned char* data = new unsigned char[pixels.size() * numChannels];
                    unsigned char* pixel = data;
                    for (unsigned char index : pixels)
                    {
                        memcpy(pixel, &palette[index], sizeof(unsigned char) * numChannels);
                        pixel += numChannels;
                    }
                    for (unsigned int& offset : mipmapOffsets)
                        offset *= numChannels;
                    image->setImage(width, height, 1, pixelformat, pixelformat, GL_UNSIGNED_BYTE, data,
                        osg::Image::USE_NEW_DELETE, packing);
                    break;
                }
                default:
                    return nullptr;
            }

            image->setMipmapLevels(mipmapOffsets);
            image->flipVertical();

            return image;
        }

        static osg::ref_ptr<osg::TexEnvCombine> createEmissiveTexEnv()
        {
            osg::ref_ptr<osg::TexEnvCombine> texEnv(new osg::TexEnvCombine);
            // Sum the previous colour and the emissive colour.
            texEnv->setCombine_RGB(osg::TexEnvCombine::ADD);
            texEnv->setSource0_RGB(osg::TexEnvCombine::PREVIOUS);
            texEnv->setSource1_RGB(osg::TexEnvCombine::TEXTURE);
            // Keep the previous alpha.
            texEnv->setCombine_Alpha(osg::TexEnvCombine::REPLACE);
            texEnv->setSource0_Alpha(osg::TexEnvCombine::PREVIOUS);
            texEnv->setOperand0_Alpha(osg::TexEnvCombine::SRC_ALPHA);
            return texEnv;
        }

        static void handleDepthFlags(osg::StateSet* stateset, bool depthTest, bool depthWrite)
        {
            if (!depthWrite && !depthTest)
            {
                stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
                return;
            }
            osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
            depth->setWriteMask(depthWrite);
            if (!depthTest)
                depth->setFunction(osg::Depth::ALWAYS);
            depth = shareAttribute(depth);
            stateset->setAttributeAndModes(depth, osg::StateAttribute::ON);
        }

        void handleTextureProperty(const Nif::NiTexturingProperty* texprop, const std::string& nodeName,
            osg::StateSet* stateset, SceneUtil::CompositeStateSetUpdater* composite,
            std::vector<unsigned int>& boundTextures, int animflags) const
        {
            // overriding a parent NiTexturingProperty, so remove what was previously bound
            clearBoundTextures(stateset, boundTextures);

            // If this loop is changed such that the base texture isn't guaranteed to end up in texture unit 0, the
            // shadow casting shader will need to be updated accordingly.
            std::vector<int> textureSlotToUnit(16, -1);
            for (size_t i = 0; i < texprop->mTextures.size(); ++i)
            {
                const Nif::NiTexturingProperty::Texture& tex = texprop->mTextures[i];
                if (tex.mEnabled || (i == Nif::NiTexturingProperty::BaseTexture && !texprop->mController.empty()))
                {
                    std::string textureName;
                    switch (i)
                    {
                        // These are handled later on
                        case Nif::NiTexturingProperty::BaseTexture:
                            textureName = "diffuseMap";
                            break;
                        case Nif::NiTexturingProperty::GlowTexture:
                            textureName = "emissiveMap";
                            break;
                        case Nif::NiTexturingProperty::DarkTexture:
                            textureName = "darkMap";
                            break;
                        case Nif::NiTexturingProperty::BumpTexture:
                            textureName = "bumpMap";
                            break;
                        case Nif::NiTexturingProperty::DetailTexture:
                            textureName = "detailMap";
                            break;
                        case Nif::NiTexturingProperty::DecalTexture:
                            textureName = "decalMap";
                            break;
                        case Nif::NiTexturingProperty::GlossTexture:
                            textureName = "glossMap";
                            break;
                        default:
                        {
                            Log(Debug::Info) << "Unhandled texture stage " << i << " on shape \"" << nodeName
                                             << "\" in " << mFilename;
                            continue;
                        }
                    }

                    const unsigned int texUnit = boundTextures.size();
                    if (i < textureSlotToUnit.size())
                        textureSlotToUnit[i] = static_cast<int>(texUnit);
                    if (tex.mEnabled)
                    {
                        if (tex.mSourceTexture.empty() && texprop->mController.empty())
                        {
                            if (i == 0)
                                Log(Debug::Warning) << "Base texture is in use but empty on shape \"" << nodeName
                                                    << "\" in " << mFilename;
                            continue;
                        }

                        if (!tex.mSourceTexture.empty())
                            attachNiSourceTexture(textureName, tex.mSourceTexture.getPtr(), tex.wrapS(), tex.wrapT(),
                                tex.mUVSet, stateset, boundTextures);
                        else
                            attachTexture(
                                textureName, nullptr, tex.wrapS(), tex.wrapT(), tex.mUVSet, stateset, boundTextures);
                    }
                    else
                    {
                        // Texture only comes from NiFlipController, so tex is ignored, set defaults
                        attachTexture(textureName, nullptr, true, true, 0, stateset, boundTextures);
                    }

                    if (tex.mHasTransform)
                    {
                        setTextureTransformDefaults(stateset, texUnit, tex.mTransform);
                        osg::ref_ptr<osg::TexMat> texMat = new osg::TexMat;
                        texMat->setMatrix(makeTextureTransformMatrix(tex.mTransform));
                        stateset->setTextureAttributeAndModes(texUnit, texMat, osg::StateAttribute::ON);
                    }

                    if (i == Nif::NiTexturingProperty::GlowTexture)
                    {
                        stateset->setTextureAttributeAndModes(texUnit, createEmissiveTexEnv(), osg::StateAttribute::ON);
                    }
                    else if (i == Nif::NiTexturingProperty::DarkTexture)
                    {
                        osg::TexEnv* texEnv = new osg::TexEnv;
                        // Modulate both the colour and the alpha with the dark map.
                        texEnv->setMode(osg::TexEnv::MODULATE);
                        stateset->setTextureAttributeAndModes(texUnit, texEnv, osg::StateAttribute::ON);
                    }
                    else if (i == Nif::NiTexturingProperty::DetailTexture)
                    {
                        osg::TexEnvCombine* texEnv = new osg::TexEnvCombine;
                        // Modulate previous colour...
                        texEnv->setCombine_RGB(osg::TexEnvCombine::MODULATE);
                        texEnv->setSource0_RGB(osg::TexEnvCombine::PREVIOUS);
                        texEnv->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);
                        // with the detail map's colour,
                        texEnv->setSource1_RGB(osg::TexEnvCombine::TEXTURE);
                        texEnv->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
                        // and a twist:
                        texEnv->setScale_RGB(2.f);
                        // Keep the previous alpha.
                        texEnv->setCombine_Alpha(osg::TexEnvCombine::REPLACE);
                        texEnv->setSource0_Alpha(osg::TexEnvCombine::PREVIOUS);
                        texEnv->setOperand0_Alpha(osg::TexEnvCombine::SRC_ALPHA);
                        stateset->setTextureAttributeAndModes(texUnit, texEnv, osg::StateAttribute::ON);
                    }
                    else if (i == Nif::NiTexturingProperty::BumpTexture)
                    {
                        // Bump maps offset the environment map.
                        // Set this texture to Off by default since we can't render it with the fixed-function pipeline
                        stateset->setTextureMode(texUnit, GL_TEXTURE_2D, osg::StateAttribute::OFF);
                        osg::Matrix2 bumpMapMatrix(texprop->mBumpMapMatrix.x(), texprop->mBumpMapMatrix.y(),
                            texprop->mBumpMapMatrix.z(), texprop->mBumpMapMatrix.w());
                        stateset->addUniform(new osg::Uniform("bumpMapMatrix", bumpMapMatrix));
                        stateset->addUniform(new osg::Uniform("envMapLumaBias", texprop->mEnvMapLumaBias));
                    }
                    else if (i == Nif::NiTexturingProperty::GlossTexture)
                    {
                        // A gloss map is an environment map mask.
                        // Gloss maps are only implemented in the object shaders as well.
                        stateset->setTextureMode(texUnit, GL_TEXTURE_2D, osg::StateAttribute::OFF);
                    }
                    else if (i == Nif::NiTexturingProperty::DecalTexture)
                    {
                        // This is only an inaccurate imitation of the original implementation,
                        // see https://github.com/niftools/nifskope/issues/184

                        osg::TexEnvCombine* texEnv = new osg::TexEnvCombine;
                        // Interpolate to the decal texture's colour...
                        texEnv->setCombine_RGB(osg::TexEnvCombine::INTERPOLATE);
                        texEnv->setSource0_RGB(osg::TexEnvCombine::TEXTURE);
                        texEnv->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);
                        // ...from the previous colour...
                        texEnv->setSource1_RGB(osg::TexEnvCombine::PREVIOUS);
                        texEnv->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
                        // using the decal texture's alpha as the factor.
                        texEnv->setSource2_RGB(osg::TexEnvCombine::TEXTURE);
                        texEnv->setOperand2_RGB(osg::TexEnvCombine::SRC_ALPHA);
                        // Keep the previous alpha.
                        texEnv->setCombine_Alpha(osg::TexEnvCombine::REPLACE);
                        texEnv->setSource0_Alpha(osg::TexEnvCombine::PREVIOUS);
                        texEnv->setOperand0_Alpha(osg::TexEnvCombine::SRC_ALPHA);
                        stateset->setTextureAttributeAndModes(texUnit, texEnv, osg::StateAttribute::ON);
                    }
                }
            }
            handleTextureControllers(texprop, composite, stateset, animflags, textureSlotToUnit);
        }

        static Bgsm::MaterialFilePtr getShaderMaterial(
            std::string_view path, Resource::BgsmFileManager* materialManager)
        {
            if (!materialManager)
                return nullptr;

            if (!Misc::StringUtils::ciEndsWith(path, ".bgem") && !Misc::StringUtils::ciEndsWith(path, ".bgsm"))
                return nullptr;

            std::string normalizedPath = Misc::ResourceHelpers::correctMaterialPath(path, materialManager->getVFS());
            try
            {
                return materialManager->get(VFS::Path::Normalized(normalizedPath));
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to load shader material: " << e.what();
                return nullptr;
            }
        }

        void handleShaderMaterialNodeProperties(
            const Bgsm::MaterialFile* material, osg::StateSet* stateset, std::vector<unsigned int>& boundTextures) const
        {
            const unsigned int uvSet = 0;
            const bool wrapS = material->wrapS();
            const bool wrapT = material->wrapT();
            if (material->mShaderType == Bgsm::ShaderType::Lighting)
            {
                const Bgsm::BGSMFile* bgsm = static_cast<const Bgsm::BGSMFile*>(material);

                if (!bgsm->mDiffuseMap.empty())
                    attachExternalTexture(
                        "diffuseMap", bgsm->mDiffuseMap, wrapS, wrapT, uvSet, stateset, boundTextures);

                if (!bgsm->mNormalMap.empty())
                    attachExternalTexture("normalMap", bgsm->mNormalMap, wrapS, wrapT, uvSet, stateset, boundTextures);

                if (bgsm->mGlowMapEnabled && !bgsm->mGlowMap.empty())
                    attachExternalTexture("emissiveMap", bgsm->mGlowMap, wrapS, wrapT, uvSet, stateset, boundTextures);

                if (bgsm->mTree)
                    stateset->addUniform(new osg::Uniform("useTreeAnim", true));
            }
            else if (material->mShaderType == Bgsm::ShaderType::Effect)
            {
                const Bgsm::BGEMFile* bgem = static_cast<const Bgsm::BGEMFile*>(material);

                if (!bgem->mBaseMap.empty())
                    attachExternalTexture("diffuseMap", bgem->mBaseMap, wrapS, wrapT, uvSet, stateset, boundTextures);

                bool useFalloff = bgem->mFalloff;
                stateset->addUniform(new osg::Uniform("useFalloff", useFalloff));
                if (useFalloff)
                    stateset->addUniform(new osg::Uniform("falloffParams", bgem->mFalloffParams));
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
            bool ppLightingUsesDiffuseAlpha = false;
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
                        ppLightingUsesDiffuseAlpha = ppLightingUsesDiffuseAlpha || shaderprop->alphaTexture()
                            || shaderprop->refraction() || shaderprop->fireRefraction();
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

            if (hasNoLightingShader)
            {
                // Retail NOLIGHTTEX consumes MaterialColor independently of vertex color. For FO3/FNV that
                // constant comes from NiMaterial emission * emissiveMult; NOLIGHTTEXVC multiplies the vertex
                // stream as a separate stage. Do not fold either input into OpenGL's color-mode selection.
                osg::StateSet* stateSet = node->getOrCreateStateSet();
                stateSet->addUniform(new osg::Uniform("useNoLightingEmission", true));
                stateSet->addUniform(new osg::Uniform("useNoLightingVertexColor", hasVertexColors));
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

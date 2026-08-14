/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2020 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#ifndef GLTF_READER_H
#define GLTF_READER_H

#include <osg/Node>
#include <osg/AlphaFunc>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Material>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/ReaderWriter>
#include <osgUtil/SmoothingVisitor>
#include <osgUtil/TangentSpaceGenerator>
#include <components/sceneutil/depth.hpp>
#if ENABLE_OE
#  include <osgEarth/URI>
#endif // ENABLE_OE
#include <unordered_map>
#include <cstdlib>

/**
 * Simple convenience construct to make another type "lockable"
 * as long as it has a default constructor
 */
template<typename T>
struct Lockable : public T {
    inline void lock() const {
        _lockable_mutex.lock();
    }
    inline void unlock() const {
        _lockable_mutex.unlock();
    }
    inline OpenThreads::Mutex& mutex() const {
        return _lockable_mutex;
    }
private:
    mutable OpenThreads::Mutex _lockable_mutex;
};

class GLTFReader
{
public:
    typedef Lockable<
        std::unordered_map<std::string, osg::ref_ptr<osg::Texture2D> > 
    > TextureCache;

    static std::string ExpandFilePath(const std::string &filepath, void * userData)
    {
        const std::string& referrer = *(const std::string*)userData;
        std::string path = osgDB::getRealPath(osgDB::isAbsolutePath(filepath) ? filepath : osgDB::concatPaths(osgDB::getFilePath(referrer), filepath));
        OSG_NOTICE << "ExpandFilePath: expanded " << filepath << " to " << path << std::endl;
        return tinygltf::ExpandFilePath(path, userData);
    }

    struct Env
    {
        Env(const std::string& loc, const osgDB::Options* opt) : referrer(loc), readOptions(opt) { }
        const std::string referrer;
        const osgDB::Options* readOptions;
    };

public:
    mutable TextureCache* _texCache;

    GLTFReader() : _texCache(NULL)
    {
        //NOP
    }

    void setTextureCache(TextureCache* cache) const
    {
        _texCache = cache;
    }

    osgDB::ReaderWriter::ReadResult read(const std::string& location,
                                         bool isBinary,
                                         const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists = &tinygltf::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile = &tinygltf::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data = (void*)&location;
        loader.SetFsCallbacks(fs);

        tinygltf::Options opt;
        opt.skip_imagery = readOptions && readOptions->getOptionString().find("gltfSkipImagery") != std::string::npos;

        if (osgDB::containsServerAddress(location))
        {
#if ENABLE_OE
            osgEarth::ReadResult rr = osgEarth::URI(location).readString(readOptions);
            if (rr.failed())
            {
                return osgDB::ReaderWriter::ReadResult::FILE_NOT_FOUND;
            }

            std::string mem = rr.getString();

            if (isBinary)
            {
                loader.LoadBinaryFromMemory(&model, &err, &warn, (const unsigned char*)mem.data(), mem.size(), location, REQUIRE_VERSION, &opt);
            }
            else
            {
                loader.LoadASCIIFromString(&model, &err, &warn, mem.data(), mem.size(), location, REQUIRE_VERSION, &opt);
            }
#else
            OSG_FATAL << "Loading of gltf from server location '" << location << "' not supported." << std::endl;
            return osgDB::ReaderWriter::ReadResult::FILE_NOT_HANDLED;
#endif // ENABLE_OE
        }
        else
        {
            if (isBinary)
            {
                loader.LoadBinaryFromFile(&model, &err, &warn, location, REQUIRE_VERSION, &opt);
            }
            else
            {
                loader.LoadASCIIFromFile(&model, &err, &warn, location, REQUIRE_VERSION, &opt);
            }
        }

        if (!err.empty()) {
            OSG_WARN << "gltf Error loading " << location << std::endl;
            OSG_WARN << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        Env env(location, readOptions);
        return makeNodeFromModel(model, env);
    }

    osg::Node* makeNodeFromModel(const tinygltf::Model &model, const Env& env) const
    {
        // Rotate y-up to z-up
        osg::MatrixTransform* transform = new osg::MatrixTransform;
        transform->setMatrix(osg::Matrixd::rotate(osg::Vec3d(0.0, 1.0, 0.0), osg::Vec3d(0.0, 0.0, 1.0)));

        for (unsigned int i = 0; i < model.scenes.size(); i++)
        {
            const tinygltf::Scene &scene = model.scenes[i];

            for (size_t j = 0; j < scene.nodes.size(); j++) {
                osg::Node* node = createNode(model, model.nodes[scene.nodes[j]], env);
                if (node)
                {
                    transform->addChild(node);
                }
            }
        }

        return transform;
    }

    osg::Node* createNode(const tinygltf::Model &model, const tinygltf::Node& node, const Env& env) const
    {
        osg::MatrixTransform* mt = new osg::MatrixTransform;
        mt->setName(node.name);
        if (node.matrix.size() == 16)
        {
            osg::Matrixd mat;
            mat.set(node.matrix.data());
            mt->setMatrix(mat);
        }
        else
        {
            osg::Matrixd scale, translation, rotation;
            if (node.scale.size() == 3)
            {
                scale = osg::Matrixd::scale(node.scale[0], node.scale[1], node.scale[2]);
            }

            if (node.rotation.size() == 4) {
                osg::Quat quat(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);
                rotation.makeRotate(quat);
            }

            if (node.translation.size() == 3) {
                translation = osg::Matrixd::translate(node.translation[0], node.translation[1], node.translation[2]);
            }

            mt->setMatrix(scale * rotation * translation);
        }


        // todo transformation
        if (node.mesh >= 0)
        {
            mt->addChild(makeMesh(model, model.meshes[node.mesh], env));
        }

        // Load any children.
        for (unsigned int i = 0; i < node.children.size(); i++)
        {            
            osg::Node* child = createNode(model, model.nodes[node.children[i]], env);
            if (child)
            {
                mt->addChild(child);
            }
        }
        return mt;
    }


    osg::ref_ptr<osg::Texture2D> makeTexture(
        const tinygltf::Model& model, int textureIndex, const Env& env) const
    {
        if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
            return nullptr;
        const tinygltf::Texture& texture = model.textures[textureIndex];
        if (texture.source < 0 || texture.source >= static_cast<int>(model.images.size()))
            return nullptr;
        const tinygltf::Image& image = model.images[texture.source];
        osg::ref_ptr<osg::Image> osgImage;
        if (!image.image.empty())
        {
            GLenum format = image.component == 4 ? GL_RGBA : GL_RGB;
            GLenum internalFormat = image.component == 4 ? GL_RGBA8 : GL_RGB8;
            unsigned char* data = new unsigned char[image.image.size()];
            memcpy(data, image.image.data(), image.image.size());
            osgImage = new osg::Image;
            osgImage->setImage(image.width, image.height, 1, internalFormat, format,
                GL_UNSIGNED_BYTE, data, osg::Image::AllocationMode::USE_NEW_DELETE);
            // Keep tinygltf's decoded row order. glTF UVs and embedded images
            // share the same top-origin convention; flipping here a second
            // time makes atlas-based faces and buildings sample another row.
        }
        else if (!image.uri.empty())
        {
            const std::string path = osgDB::isAbsolutePath(image.uri)
                ? image.uri
                : osgDB::concatPaths(osgDB::getFilePath(env.referrer), image.uri);
            osgImage = osgDB::readRefImageFile(path, env.readOptions);
        }
        if (!osgImage)
            return nullptr;
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D(osgImage);
        result->setUnRefImageDataAfterApply(true);
        result->setResizeNonPowerOfTwoHint(false);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        if (texture.sampler >= 0 && texture.sampler < static_cast<int>(model.samplers.size()))
        {
            const tinygltf::Sampler& sampler = model.samplers[texture.sampler];
            result->setWrap(osg::Texture::WRAP_S, static_cast<osg::Texture::WrapMode>(sampler.wrapS));
            result->setWrap(osg::Texture::WRAP_T, static_cast<osg::Texture::WrapMode>(sampler.wrapT));
        }
        return result;
    }

    osg::ref_ptr<osg::Texture2D> makeDaoTexture(const std::string& path) const
    {
        osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(path);
        if (!image)
            return nullptr;
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D(image);
        result->setUnRefImageDataAfterApply(true);
        result->setResizeNonPowerOfTwoHint(false);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        return result;
    }

    osg::Node* makeMesh(const tinygltf::Model &model, const tinygltf::Mesh& mesh, const Env& env) const
    {
        osg::Group *group = new osg::Group;

        // Haven exports DAO's bald-cap helper as an opaque mesh whose base
        // color slot incorrectly contains its tangent-space normal sphere.
        // It encloses the real face and appears as a black head in OpenMW.
        // The authored face plus hairstyle meshes are separate and complete.
        if (mesh.name.find("_HAR_BLD_") != std::string::npos)
        {
            OSG_NOTICE << "OpenDAO: suppressing invalid bald-cap helper " << mesh.name << std::endl;
            return group;
        }
        if (std::getenv("OPENMW_GLTF_DAO_HIDE_HAIR") != nullptr
            && mesh.name.find("_HAR_") != std::string::npos)
        {
            OSG_NOTICE << "OpenDAO: diagnostic hair suppression " << mesh.name << std::endl;
            return group;
        }
        std::vector< osg::ref_ptr< osg::Array > > arrays;
        extractArrays(model, arrays);

        OSG_DEBUG << "Drawing " << mesh.primitives.size() << " primitives in mesh" << std::endl;

        for (size_t i = 0; i < mesh.primitives.size(); i++) {

            OSG_DEBUG << " Processing primitive " << i << std::endl;
            const tinygltf::Primitive &primitive = mesh.primitives[i];
            if (primitive.indices < 0)
            {
                // Hmm, should delete group here
                return 0;
            }

            osg::ref_ptr< osg::Geometry > geom = new osg::Geometry;
            geom->setName(mesh.name);
            geom->setUseVertexBufferObjects(true);

            group->addChild(geom.get());

            // The base color factor of the material
            osg::Vec4 baseColorFactor(1.0f, 1.0f, 1.0f, 1.0f);
            float roughnessFactor = 1.0f;
            float metallicFactor = 1.0f;
            bool hasNormalTexture = false;
            bool daoTerrain = false;
            bool daoWater = false;
            // Haven's composed actor GLB preserves the real DAO resource
            // names (HM_UHM_*, HM_HAR_*, HM_CTH_*, HM_ARM_*), not the
            // temporary RuntimeBaked label. Classify those meshes directly so
            // faces, hair and clothing receive the actor lighting branch.
            const bool daoActor = mesh.name.find("RuntimeBaked") != std::string::npos
                || mesh.name.rfind("HM_", 0) == 0 || mesh.name.rfind("HF_", 0) == 0
                || mesh.name.rfind("EM_", 0) == 0 || mesh.name.rfind("EF_", 0) == 0
                || mesh.name.rfind("DM_", 0) == 0 || mesh.name.rfind("DF_", 0) == 0;
            const bool daoHair = daoActor && mesh.name.find("_HAR_") != std::string::npos;
            const bool daoHairCutout = daoActor
                && mesh.name.find("_HAR_") != std::string::npos
                && mesh.name.find("HairM1") != std::string::npos;
            const bool daoFace = daoActor && (mesh.name.find("Face") != std::string::npos
                || mesh.name.find("_UHM_") != std::string::npos
                || mesh.name.find("_UHF_") != std::string::npos);
            const bool daoEye = daoActor && mesh.name.find("_UEM_") != std::string::npos;
            const bool daoRobe = daoActor && mesh.name.find("_ROB_") != std::string::npos;
            const bool daoRobeCutout = daoActor && mesh.name.find("_ROB_") != std::string::npos;

            if (primitive.material >= 0 && primitive.material < model.materials.size())
            {
                const tinygltf::Material& material = model.materials[primitive.material];
                // glTF materials are single-sided unless doubleSided is set.
                // Drawing both sides exposed the insides of DAO hair shells,
                // roofs, walls, and terrain skirts, producing view-dependent
                // occlusion and apparent material swaps.
                // Godot's Haven actor import deliberately disables culling
                // for skinned face, clothing, and hair meshes; preserve that
                // runtime override while retaining spec-correct culling for
                // environment glTF primitives.
                // Haven/Blender's composed DAO exports blanket every material
                // with doubleSided=true. Treating that generated flag as
                // authoritative exposes the enormous back faces of terrain
                // skirts and building shells from the lake and through the
                // town. Actors and alpha-card foliage still need both sides;
                // opaque DAO environment geometry must retain back-face cull.
                const bool daoTerrainMaterial
                    = (material.name.find("lak100d_") == 0 || material.name.find("brc997d_") == 0)
                    && material.name.find(".mao") != std::string::npos;
                const bool daoAravelMaterial = material.name.find("prp_aravel") == 0
                    || material.name.find("Prp_Aravel") == 0;
                // Haven's direct DAO terrain cells retain DAO's winding while
                // their root node performs the Z-up/Y-up conversion.  The
                // converted top surface is clockwise to OpenGL, so culling it
                // leaves only the vertical skirts visible.  Render both sides
                // for terrain; the opaque depth state still prevents skirt
                // transparency and overlap artifacts.
                const bool needsDaoDoubleSided
                    = daoActor || daoTerrainMaterial || daoAravelMaterial
                    || material.alphaMode != "OPAQUE";
                geom->getOrCreateStateSet()->setMode(GL_CULL_FACE,
                    needsDaoDoubleSided
                        ? (osg::StateAttribute::OFF
                            | (daoAravelMaterial ? osg::StateAttribute::PROTECTED : 0))
                        : osg::StateAttribute::ON);
                daoTerrain = daoTerrainMaterial;
                daoWater = material.name.find("OpenDAO_Water::") != std::string::npos;

                /*
                OSG_NOTICE << "extCommonValues=" << material.extCommonValues.size() << std::endl;
                for (ParameterMap::iterator paramItr = material.extCommonValues.begin(); paramItr != material.extCommonValues.end(); ++paramItr)
                {
                    OSG_NOTICE << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                }
                */

                OSG_DEBUG << "additionalValues=" << material.additionalValues.size() << std::endl;
                for (tinygltf::ParameterMap::const_iterator paramItr = material.additionalValues.begin(); paramItr != material.additionalValues.end(); ++paramItr)
                {
                    OSG_DEBUG << "    " << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                }

                //OSG_NOTICE << "values=" << material.values.size() << std::endl;
                for (tinygltf::ParameterMap::const_iterator paramItr = material.values.begin(); paramItr != material.values.end(); ++paramItr)
                {
                    if (paramItr->first == "baseColorFactor")
                    {
                        tinygltf::ColorValue color = paramItr->second.ColorFactor();
                        baseColorFactor = osg::Vec4(color[0], color[1], color[2], color[3]);
                    }
                    else if (paramItr->first == "roughnessFactor")
                        roughnessFactor = static_cast<float>(paramItr->second.Factor());
                    else if (paramItr->first == "metallicFactor")
                        metallicFactor = static_cast<float>(paramItr->second.Factor());
                    else
                    {
                        OSG_DEBUG << "    " << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                    }

                }
                /*
                OSG_NOTICE << "extPBRValues=" << material.extPBRValues.size() << std::endl;
                for (ParameterMap::iterator paramItr = material.extPBRValues.begin(); paramItr != material.extPBRValues.end(); ++paramItr)
                {
                    OSG_NOTICE << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                }
                */

                for (tinygltf::ParameterMap::const_iterator paramItr = material.values.begin(); paramItr != material.values.end(); ++paramItr)
                {
                    if (paramItr->first == "baseColorTexture")
                    {
                        std::map< std::string, double>::const_iterator i = paramItr->second.json_double_value.find("index");
                        if (i != paramItr->second.json_double_value.end())
                        {
                            int index = i->second;

                            const tinygltf::Texture& texture = model.textures[index];
                            const tinygltf::Image& image = model.images[texture.source];

                            // don't cache embedded textures!
                            bool imageEmbedded = 
                                tinygltf::IsDataURI(image.uri) ||
                                image.image.size() > 0;

#if ENABLE_OE
                            osgEarth::URI imageURI(image.uri, env.referrer);
#endif // ENABLE_OE

                            osg::ref_ptr<osg::Texture2D> tex = NULL;
                            osg::ref_ptr<osg::Texture2D>* cachedTex = NULL;

                            if (!imageEmbedded && _texCache)
                            {
                                _texCache->lock();
#if ENABLE_OE
                                cachedTex = &(*_texCache)[imageURI.full()];
#else
                                cachedTex = &(*_texCache)[image.uri];
#endif // ENABLE_OE
                                tex = cachedTex->get();
                            }

                            if (!tex.valid())
                            {
#if ENABLE_OE
                                OSG_DEBUG << "New Texture: " << imageURI.full() << ", embedded=" << imageEmbedded << std::endl;
#else
                                OSG_DEBUG << "New Texture: " << image.uri << ", embedded=" << imageEmbedded << std::endl;
#endif // ENABLE_OE

                                // First load the image
                                const tinygltf::Image& image = model.images[texture.source];
                                osg::ref_ptr<osg::Image> img;

                                if (image.image.size() > 0)
                                {
                                    GLenum format = GL_RGB, texFormat = GL_RGB8;
                                    if (image.component == 4) format = GL_RGBA, texFormat = GL_RGBA8;

                                    img = new osg::Image();
                                    //OSG_NOTICE << "Loading image of size " << image.width << "x" << image.height << " components = " << image.component << " totalSize=" << image.image.size() << std::endl;
                                    unsigned char *imgData = new unsigned char[image.image.size()];
                                    memcpy(imgData, &image.image[0], image.image.size());
                                    img->setImage(image.width, image.height, 1, texFormat, format, GL_UNSIGNED_BYTE, imgData, osg::Image::AllocationMode::USE_NEW_DELETE);
                                }      

                                else if (!imageEmbedded) // load from URI
                                {
#if ENABLE_OE
                                    osgEarth::ReadResult rr = imageURI.readImage(env.readOptions);
                                    if(rr.succeeded())
                                    {
                                        img = rr.releaseImage();          
                                        if (img.valid())
                                        {
                                            img->flipVertical();
                                        }
                                    }
#endif // ENABLE_OE
                                }

                                // If the image loaded OK, create the texture
                                if (img.valid())
                                {
                                    if(img->getPixelFormat() == GL_RGB)
                                        img->setInternalTextureFormat(GL_RGB8);
                                    else if (img->getPixelFormat() == GL_RGBA)
                                        img->setInternalTextureFormat(GL_RGBA8);
                                    
                                    tex = new osg::Texture2D(img.get());
                                    tex->setUnRefImageDataAfterApply(imageEmbedded);
                                    tex->setResizeNonPowerOfTwoHint(false);
                                    tex->setDataVariance(osg::Object::STATIC);

                                    if (texture.sampler >= 0 && texture.sampler < model.samplers.size())
                                    {
                                        const tinygltf::Sampler& sampler = model.samplers[texture.sampler];
                                        //tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)sampler.minFilter);
                                        //tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)sampler.magFilter);
                                        tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR); //sampler.minFilter);
                                        tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR); //sampler.magFilter);
                                        tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)sampler.wrapS);
                                        tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)sampler.wrapT);
                                        tex->setWrap(osg::Texture::WRAP_R, (osg::Texture::WrapMode)sampler.wrapR);
                                    }
                                    else
                                    {
                                        tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR);
                                        tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR);
                                        tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                                        tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                                    }
                                }
                            }

                            if (tex.valid())
                            {
                                if (cachedTex && !cachedTex->valid())
                                {
                                    (*cachedTex) = tex.get();
                                }
                                int unit = 0;
                                geom->getOrCreateStateSet()->setTextureAttributeAndModes(unit, tex.get(), osg::StateAttribute::ON);
                            }
                            
                            if (material.alphaMode == "OPAQUE")
                            {
                                // OPAQUE is authoritative in glTF. Do not let
                                // a parent/foreground blend state reinterpret
                                // DAO's palette/specular alpha as opacity.
                                osg::StateSet* opaqueState = geom->getOrCreateStateSet();
                                opaqueState->setMode(GL_BLEND,
                                    osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                                opaqueState->setMode(GL_ALPHA_TEST,
                                    osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                                opaqueState->setRenderingHint(osg::StateSet::OPAQUE_BIN);
                                opaqueState->setRenderBinDetails(
                                    0, "RenderBin", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
                            }
                            else
                            {
                                if (material.alphaMode == "BLEND")
                                {                                    
                                    geom->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
                                    geom->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                                    //osgEarth::Util::DiscardAlphaFragments().install(geom->getOrCreateStateSet(), 0.15);
                                }
                                else if (material.alphaMode == "MASK")
                                {
                                    // Alpha-masked foliage, fire cards, and
                                    // decals are cutouts, not sorted glass.
                                    // Keep depth writes and use the authored
                                    // threshold; the DAO GLSL path mirrors
                                    // this with its alphaCutoff uniform.
                                    osg::StateSet* alphaState = geom->getOrCreateStateSet();
                                    alphaState->setMode(GL_BLEND, osg::StateAttribute::OFF);
                                    alphaState->setMode(GL_ALPHA_TEST, osg::StateAttribute::ON);
                                    alphaState->setAttributeAndModes(new osg::AlphaFunc(
                                        osg::AlphaFunc::GREATER, static_cast<float>(material.alphaCutoff)),
                                        osg::StateAttribute::ON);
                                    alphaState->setRenderingHint(osg::StateSet::OPAQUE_BIN);
                                    alphaState->setRenderBinDetails(
                                        0, "RenderBin", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
                                }
                            }

                            if (cachedTex)
                            {
                                _texCache->unlock();
                            }
                        }
                    }
                }

                if (material.normalTexture.index >= 0)
                {
                    osg::ref_ptr<osg::Texture2D> normal = makeTexture(
                        model, material.normalTexture.index, env);
                    if (normal)
                    {
                        geom->getOrCreateStateSet()->setTextureAttributeAndModes(
                            1, normal, osg::StateAttribute::ON);
                        hasNormalTexture = true;
                    }
                }

                const char* terrainAssetDir = std::getenv("OPENMW_GLTF_DAO_TERRAIN_ASSET_DIR");
                if (daoTerrain && terrainAssetDir)
                {
                    const std::string prefix = osgDB::concatPaths(terrainAssetDir, material.name);
                    osg::ref_ptr<osg::Texture2D> maskA = makeDaoTexture(prefix + "_maska.png");
                    osg::ref_ptr<osg::Texture2D> maskA2 = makeDaoTexture(prefix + "_maska2.png");
                    if (maskA && maskA2)
                    {
                        geom->getOrCreateStateSet()->setTextureAttributeAndModes(
                            2, maskA, osg::StateAttribute::ON);
                        geom->getOrCreateStateSet()->setTextureAttributeAndModes(
                            3, maskA2, osg::StateAttribute::ON);
                    }
                    else
                    {
                        daoTerrain = false;
                        OSG_WARN << "OpenDAO terrain masks missing for " << material.name << std::endl;
                    }
                }
            }
            
            std::map<std::string, int>::const_iterator it(primitive.attributes.begin());
            std::map<std::string, int>::const_iterator itEnd(
                primitive.attributes.end());

            for (; it != itEnd; it++)
            {
                const tinygltf::Accessor &accessor = model.accessors[it->second];

                if (it->first.compare("POSITION") == 0)
                {
                    geom->setVertexArray(arrays[it->second].get());
                }
                else if (it->first.compare("NORMAL") == 0)
                {
                    geom->setNormalArray(arrays[it->second].get(), osg::Array::BIND_PER_VERTEX);
                }
                else if (it->first.compare("TEXCOORD_0") == 0)
                {
                    geom->setTexCoordArray(0, arrays[it->second].get());
                    // The DAO PBR program uses an explicit UV attribute. The
                    // compatibility-profile gl_MultiTexCoord0 path can retain
                    // parent texture-coordinate state and was demonstrably
                    // feeding the aravel wheel its plank/slat atlas region.
                    geom->setVertexAttribArray(8, arrays[it->second].get(), osg::Array::BIND_PER_VERTEX);
                }
                else if (it->first.compare("TEXCOORD_1") == 0)
                {
                    geom->setTexCoordArray(1, arrays[it->second].get());
                }
                else if (it->first.compare("COLOR_0") == 0)
                {
                    // TODO:  Multipy by the baseColorFactor here?
                    OSG_DEBUG << "Setting color array " << arrays[it->second].get() << std::endl;
                    geom->setColorArray(arrays[it->second].get());
                }
                else if (it->first.compare("TANGENT") == 0)
                {
                    // glTF tangent xyz and handedness w are required to apply
                    // DAO normal maps in the same basis as Godot.
                    geom->setVertexAttribArray(6, arrays[it->second].get(), osg::Array::BIND_PER_VERTEX);
                }
                else
                {
                    OSG_DEBUG << "Skipping array " << it->first << std::endl;
                }
            }

            // Blender writes glTF mesh data Y-up and carries the axis conversion on
            // the node. The DAO extractor's terrain cells are already Z-up, while
            // Blender-assembled terrain rings are not. Normalize only those arrays
            // here, before OpenMW applies the node transform.
            if (daoTerrain)
            {
                // Terrain cells frequently enclose the portrait camera inside
                // their skirt bounding volume. Reverse-Z's infinite projection
                // is not understood by OSG's legacy small-feature/frustum
                // culler, so submit the cell and let raster clipping decide.
                geom->setCullingActive(false);
                osg::StateSet* terrainState = geom->getOrCreateStateSet();
                terrainState->setMode(GL_BLEND,
                    osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                if (std::getenv("OPENMW_GLTF_DAO_TERRAIN_DEPTH_DIAGNOSTIC") != nullptr)
                    terrainState->setMode(
                        GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

                osg::Vec3Array* positions = dynamic_cast<osg::Vec3Array*>(geom->getVertexArray());
                if (positions && !positions->empty())
                {
                    osg::BoundingBox rawBounds;
                    for (const osg::Vec3& position : *positions)
                        rawBounds.expandBy(position);

                    const float yRange = rawBounds.yMax() - rawBounds.yMin();
                    const float zRange = rawBounds.zMax() - rawBounds.zMin();
                    if (yRange < zRange * 0.75f)
                    {
                        for (osg::Vec3& position : *positions)
                        {
                            const float oldY = position.y();
                            position.y() = -position.z();
                            position.z() = oldY;
                        }

                        osg::Vec3Array* normals = dynamic_cast<osg::Vec3Array*>(geom->getNormalArray());
                        if (normals)
                        {
                            for (osg::Vec3& normal : *normals)
                            {
                                const float oldY = normal.y();
                                normal.y() = -normal.z();
                                normal.z() = oldY;
                            }
                            normals->dirty();
                        }

                        positions->dirty();
                        geom->dirtyBound();
                        OSG_NOTICE << "OPENDAO_GLTF_TERRAIN_AXIS normalized=y-up-to-z-up"
                                   << " mesh=" << mesh.name
                                   << " yRange=" << yRange << " zRange=" << zRange << std::endl;
                    }
                }
            }

            if (daoTerrain)
            {
                osg::Vec2 uvMin(1e9f, 1e9f);
                osg::Vec2 uvMax(-1e9f, -1e9f);
                const osg::Vec2Array* uvs = dynamic_cast<const osg::Vec2Array*>(geom->getTexCoordArray(0));
                if (uvs)
                {
                    for (const osg::Vec2& uv : *uvs)
                    {
                        uvMin.x() = std::min(uvMin.x(), uv.x());
                        uvMin.y() = std::min(uvMin.y(), uv.y());
                        uvMax.x() = std::max(uvMax.x(), uv.x());
                        uvMax.y() = std::max(uvMax.y(), uv.y());
                    }
                }
                const tinygltf::Material& material = model.materials[primitive.material];
                const osg::StateSet* state = geom->getStateSet();
                OSG_NOTICE << "OPENDAO_GLTF_TERRAIN material=" << material.name
                           << " uvMin=(" << uvMin.x() << ',' << uvMin.y() << ")"
                           << " uvMax=(" << uvMax.x() << ',' << uvMax.y() << ")"
                           << " vertices=" << (geom->getVertexArray() ? geom->getVertexArray()->getNumElements() : 0)
                           << " base=" << (state && state->getTextureAttribute(0, osg::StateAttribute::TEXTURE) ? 1 : 0)
                           << " normal=" << (state && state->getTextureAttribute(1, osg::StateAttribute::TEXTURE) ? 1 : 0)
                           << " maskA=" << (state && state->getTextureAttribute(2, osg::StateAttribute::TEXTURE) ? 1 : 0)
                           << " maskA2=" << (state && state->getTextureAttribute(3, osg::StateAttribute::TEXTURE) ? 1 : 0)
                           << std::endl;
            }

            // If there is no color array just add one that has the base color factor in it.
            if (!geom->getColorArray())
            {
                osg::Vec4Array* colors = new osg::Vec4Array();
                osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
                for (unsigned int i = 0; i < verts->size(); i++)
                {
                    colors->push_back(baseColorFactor);
                }
                geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            }

            // glTF base color is a material input, not an emissive/fullbright
            // output. Give the compatibility path an explicit fixed-function
            // material so imported normals and OpenMW lights modulate the
            // texture. Without this state the terrain atlas was effectively
            // copied raw to the framebuffer, producing washed-out hard tiles.
            osg::ref_ptr<osg::Material> fixedMaterial = new osg::Material;
            fixedMaterial->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
            fixedMaterial->setAmbient(
                osg::Material::FRONT_AND_BACK, osg::Vec4(0.20f, 0.20f, 0.20f, baseColorFactor.a()));
            fixedMaterial->setDiffuse(
                osg::Material::FRONT_AND_BACK, osg::Vec4(1.0f, 1.0f, 1.0f, baseColorFactor.a()));
            fixedMaterial->setSpecular(
                osg::Material::FRONT_AND_BACK, osg::Vec4(0.04f, 0.04f, 0.04f, 1.0f));
            fixedMaterial->setShininess(osg::Material::FRONT_AND_BACK, 4.0f);
            osg::StateSet* fixedState = geom->getOrCreateStateSet();
            fixedState->setAttributeAndModes(fixedMaterial, osg::StateAttribute::ON);
            fixedState->setMode(GL_LIGHTING, osg::StateAttribute::ON);
            fixedState->setMode(GL_COLOR_MATERIAL, osg::StateAttribute::ON);

            // The legacy reader only supplied glTF base colour to OpenGL's
            // fixed-function pipeline. DAO parity needs a linear-light path:
            // authored albedo must be decoded from sRGB, lit, tone mapped, and
            // encoded again. Keep this opt-in so unrelated glTF consumers do
            // not change while the compatibility layer is being proven.
            if (std::getenv("OPENMW_GLTF_DAO_PBR") != nullptr)
            {
                static osg::ref_ptr<osg::Program> daoProgram;
                if (!daoProgram)
                {
                    daoProgram = new osg::Program;
                    daoProgram->setName("OpenDAO glTF linear material program");
                    daoProgram->addBindAttribLocation("daoTangent", 6);
                    daoProgram->addBindAttribLocation("daoUv", 8);
                    daoProgram->addShader(new osg::Shader(osg::Shader::VERTEX, R"GLSL(
#version 120
attribute vec4 daoTangent;
attribute vec2 daoUv;
varying vec2 vUv;
varying vec3 vNormal;
varying vec3 vTangent;
varying vec3 vBitangent;
varying vec4 vColor;
varying vec3 vDaoPosition;
varying vec3 vViewPosition;
uniform float daoWater;
uniform float hasTangent;
uniform float osg_SimulationTime;
void main()
{
    vec4 position = gl_Vertex;
    if (daoWater > 0.5)
    {
        float wave = sin(position.x * 0.115 + osg_SimulationTime * 0.85) * 0.035
            + sin(position.y * 0.173 - osg_SimulationTime * 1.12) * 0.022;
        position.z += wave;
    }
    gl_Position = gl_ModelViewProjectionMatrix * position;
    vUv = daoUv;
    vNormal = normalize(gl_NormalMatrix * gl_Normal);
    vec3 tangent = hasTangent > 0.5
        ? normalize(gl_NormalMatrix * daoTangent.xyz)
        : normalize(gl_NormalMatrix * vec3(1.0, 0.0, 0.0));
    tangent = normalize(tangent - vNormal * dot(tangent, vNormal));
    vTangent = tangent;
    vBitangent = normalize(cross(vNormal, tangent))
        * (hasTangent > 0.5 ? daoTangent.w : 1.0);
    vColor = gl_Color;
    vDaoPosition = gl_Vertex.xyz;
    vViewPosition = (gl_ModelViewMatrix * position).xyz;
}
)GLSL"));
                    daoProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT, R"GLSL(
#version 120
uniform sampler2D baseColorTexture;
uniform sampler2D normalTexture;
uniform sampler2D daoMaskA;
uniform sampler2D daoMaskA2;
uniform sampler2D daoFaceTintMask;
uniform sampler2D daoFaceAgeDiffuse;
uniform sampler2D daoFaceTattooMask;
uniform sampler2D daoFaceAgeNormal;
uniform sampler2D daoFaceMicroDetail;
uniform sampler2D daoRobeTintMask;
uniform float alphaCutoff;
uniform float hasNormalTexture;
uniform float roughnessFactor;
uniform float metallicFactor;
uniform float daoPortraitLight;
uniform float daoTerrain;
uniform float daoTerrainDalish;
uniform float daoSetPiece;
uniform float daoSetPieceUnlit;
uniform float daoWater;
uniform float daoActor;
uniform float daoFace;
uniform float daoHair;
uniform float daoEye;
uniform float daoRobe;
uniform float osg_SimulationTime;
varying vec2 vUv;
varying vec3 vNormal;
varying vec3 vTangent;
varying vec3 vBitangent;
varying vec4 vColor;
varying vec3 vDaoPosition;
varying vec3 vViewPosition;

vec3 acesFilm(vec3 value)
{
    return clamp((value * (2.51 * value + 0.03)) /
        (value * (2.43 * value + 0.59) + 0.14), 0.0, 1.0);
}
vec3 godotFilmic(vec3 color)
{
    const float A = 0.88;
    const float B = 0.60;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.01;
    const float F = 0.30;
    vec3 mapped = ((color * (A * color + C * B) + D * E)
        / (color * (A * color + B) + D * F)) - E / F;
    const float whiteMapped = 0.57835496;
    return clamp(mapped / whiteMapped, 0.0, 1.0);
}
vec3 linearToSrgb(vec3 color)
{
    vec3 lo = color * 12.92;
    vec3 hi = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return vec3(color.r < 0.0031308 ? lo.r : hi.r,
        color.g < 0.0031308 ? lo.g : hi.g,
        color.b < 0.0031308 ? lo.b : hi.b);
}
vec3 srgbToLinear(vec3 color)
{
    vec3 lo = color / 12.92;
    vec3 hi = pow((color + 0.055) / 1.055, vec3(2.4));
    return vec3(color.r <= 0.04045 ? lo.r : hi.r,
        color.g <= 0.04045 ? lo.g : hi.g,
        color.b <= 0.04045 ? lo.b : hi.b);
}
vec3 overlayColor(vec3 base, vec3 blend)
{
    vec3 limit = step(vec3(0.5), base);
    return mix(2.0 * base * blend,
        vec3(1.0) - 2.0 * (vec3(1.0) - base) * (vec3(1.0) - blend), limit);
}

float distributionGGX(float ndoth, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float d = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(3.14159265 * d * d, 0.0001);
}
float visibilitySchlickGGX(float ndotv, float roughness)
{
    float k = roughness + 1.0;
    k = k * k * 0.125;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}
float burleyDiffuse(float ndotv, float ndotl, float ldoth, float roughness)
{
    float fd90 = 0.5 + 2.0 * roughness * ldoth * ldoth;
    float lightScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - ndotl, 5.0);
    float viewScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - ndotv, 5.0);
    return lightScatter * viewScatter;
}

void addTerrainLayer(float layer, float scale, float weight, vec2 uv,
    inout vec3 color, inout vec3 normalSum, inout float specularSum)
{
    float column = floor(layer / 4.0);
    float row = mod(layer, 4.0);
    vec2 atlasUv = vec2(column * 0.5 + 0.0625, row * 0.25 + 0.03125)
        + fract(uv * scale) * vec2(0.375, 0.1875);
    vec4 layerColor = texture2D(baseColorTexture, atlasUv);
    color += pow(max(layerColor.rgb, vec3(0.0)), vec3(2.2)) * weight;
    normalSum += (texture2D(normalTexture, atlasUv).rgb * 2.0 - 1.0) * weight;
    specularSum += layerColor.a * weight;
}

void main()
{
    vec2 materialUv = vUv;
    if (daoEye > 0.5)
        materialUv = vUv;
    vec4 texel = texture2D(baseColorTexture, materialUv) * vColor;
    if (daoRobe > 0.5)
    {
        // Keeper robe's four-channel tint-mask composition. The cool zone
        // values match the accepted Godot portrait after its source-color
        // conversion and environment grade.
        vec4 robeMask = texture2D(daoRobeTintMask, vUv);
        float w1 = robeMask.r * 0.914;
        float w2 = robeMask.g * 0.881;
        float w3 = robeMask.b * 0.864;
        float wSum = clamp(w1 + w2 + w3, 0.0, 1.0);
        vec3 zoneSum = vec3(0.15, 0.19, 0.18) * w1
            + vec3(0.25, 0.34, 0.36) * w2
            + vec3(0.22, 0.30, 0.28) * w3;
        texel.rgb = texel.rgb * (1.0 - wSum) + zoneSum;
    }
    if (daoFace > 0.5)
    {
        // Reconstruct the material-per-object Face0 complexion stack on the
        // stable glTF program: old-age diffuse, three .tnt tint zones, then
        // Marethari's channel-zero tattoo. These are colour operations and
        // must happen before linearisation and cinematic lighting.
        texel.rgb = texture2D(daoFaceAgeDiffuse, vUv).rgb;
        vec3 tintMask = texture2D(daoFaceTintMask, vUv).rgb;
        texel.rgb = mix(texel.rgb, texel.rgb * vec3(0.37, 0.18, 0.19), tintMask.r * 0.937);
        texel.rgb = mix(texel.rgb, texel.rgb * vec3(0.73, 0.42, 0.35), tintMask.g * 0.200);
        texel.rgb = mix(texel.rgb, texel.rgb * vec3(0.61, 0.21, 0.23), tintMask.b * 0.336);
        float tattoo = texture2D(daoFaceTattooMask, vUv).r * 0.8;
        texel.rgb = mix(texel.rgb, texel.rgb * vec3(0.60, 0.47, 0.80), clamp(tattoo, 0.0, 1.0));
        // HumanShaders/skin_shader.gdshader applies the pore texture's blue
        // channel as an overlay before multiplying by the glTF material's
        // albedo factor. Preserve that exact order.
        vec4 microDetail = texture2D(daoFaceMicroDetail, vUv * 52.0);
        float microOverlay = mix(0.5, microDetail.b, 0.36);
        texel.rgb = overlayColor(texel.rgb, vec3(microOverlay));
        texel.rgb *= vColor.rgb;
    }
    if (daoWater > 0.5)
    {
        vec2 p = vDaoPosition.xy * 0.085;
        float broad = sin(p.x * 0.72 + p.y * 0.46 + osg_SimulationTime * 0.55) * 0.48
            + sin(p.x * -0.31 + p.y * 1.03 - osg_SimulationTime * 0.72) * 0.30;
        float ripples = sin(p.x * 2.7 - p.y * 1.9 + osg_SimulationTime * 1.25) * 0.5 + 0.5;
        float variation = clamp(0.48 + broad * 0.10 + ripples * 0.08, 0.0, 1.0);
        vec3 deep = vec3(0.025, 0.13, 0.19);
        vec3 shallow = vec3(0.10, 0.34, 0.39);
        vec3 water = mix(deep, shallow, variation);
        float facing = clamp(abs(vNormal.z), 0.0, 1.0);
        water = mix(water, vec3(0.30, 0.43, 0.53), (1.0 - facing) * 0.42);
        float glint = pow(ripples, 18.0) * 0.12;
        gl_FragColor = vec4(clamp(water + glint, 0.0, 1.0), 0.96);
        return;
    }
    vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
    float authoredSpecular = 0.0;
    if (daoTerrain > 0.5)
    {
        vec4 weights0 = texture2D(daoMaskA, vUv);
        vec4 weights1 = texture2D(daoMaskA2, vUv);
        vec3 color = vec3(0.0);
        vec3 normalSum = vec3(0.0);
        vec4 scales0 = mix(vec4(18.0, 23.0, 20.0, 19.0),
            vec4(24.0, 24.0, 10.0, 10.0), daoTerrainDalish);
        vec4 scales1 = mix(vec4(22.0, 20.0, 15.0, 20.0),
            vec4(24.0, 24.0, 3.0, 24.0), daoTerrainDalish);
        addTerrainLayer(0.0, scales0.r, weights0.r, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(1.0, scales0.g, weights0.g, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(2.0, scales0.b, weights0.b, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(3.0, scales0.a, weights0.a, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(4.0, scales1.r, weights1.r, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(5.0, scales1.g, weights1.g, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(6.0, scales1.b, weights1.b, vUv, color, normalSum, authoredSpecular);
        addTerrainLayer(7.0, scales1.a, weights1.a, vUv, color, normalSum, authoredSpecular);
        float total = dot(weights0, vec4(1.0)) + dot(weights1, vec4(1.0));
        total = max(total, 0.0001);
        texel.rgb = linearToSrgb(max(color / total, vec3(0.0)));
        tangentNormal = normalize(normalSum / total);
        authoredSpecular /= total;
    }
    else if (hasNormalTexture > 0.5)
    {
        tangentNormal = normalize(texture2D(normalTexture, vUv).rgb * 2.0 - 1.0);
    }
    if (daoFace > 0.5)
    {
        // Godot fully replaces the base face normal with the old-age normal,
        // then blends in the 52x pore normal at strength 0.24.
        vec4 agePacked = texture2D(daoFaceAgeNormal, vUv);
        // BioWare's face normal is DXT5nm-packed: X lives in alpha and Y in
        // green. Reconstruct Z before combining it with the pore normal.
        vec2 ageXY = vec2(agePacked.a, agePacked.g) * 2.0 - 1.0;
        float ageZ = sqrt(max(1.0 - dot(ageXY, ageXY), 0.0));
        vec3 ageNormal = vec3(ageXY * 0.5 + 0.5, ageZ * 0.5 + 0.5);
        vec3 microNormal = mix(vec3(0.5, 0.5, 1.0),
            vec3(texture2D(daoFaceMicroDetail, vUv * 52.0).xy, 1.0), 0.24);
        vec3 blendedNormal = vec3(ageNormal.xy + microNormal.xy - 0.5, ageNormal.z);
        tangentNormal = normalize(blendedNormal * 2.0 - 1.0);
    }
    if (texel.a < alphaCutoff)
        discard;
    if (daoSetPiece > 0.5 && daoSetPieceUnlit > 0.5)
    {
        gl_FragColor = vec4(texel.rgb, 1.0);
        return;
    }
    vec3 normal = normalize(mat3(vTangent, vBitangent, vNormal) * tangentNormal);
    if (daoSetPiece > 0.5 && !gl_FrontFacing)
        normal = -normal;
    // OSG supplies light positions in eye space. Marethari's proof uses slot
    // 6 for the Dalish sun because slot 7 is Godot's portrait omni light.
    vec3 lightDirection = normalize(daoPortraitLight > 0.5
        ? gl_LightSource[6].position.xyz : gl_LightSource[7].position.xyz);
    float ndotl = max(dot(normal, lightDirection), 0.0);
    vec3 albedo = srgbToLinear(max(texel.rgb, vec3(0.0)));
    if (daoHair > 0.5)
    {
        float detail = clamp(dot(albedo, vec3(0.30, 0.59, 0.11)) * 2.5, 0.0, 1.0);
        // These are linear values. The previous sRGB-looking constants were
        // encoded a second time after tonemapping and made Marethari's silver
        // hair clip to white instead of matching Godot's mid-grey strands.
        albedo = mix(vec3(0.18, 0.19, 0.20), vec3(0.45, 0.44, 0.42), detail);
    }
    if (daoEye > 0.5)
    {
        // DAO's eye atlas carries a nearly black packed pupil and a warm
        // low-alpha sclera. Alpha is not coverage here; reconstruct the
        // intended eye colour from luminance before the glossy BRDF branch.
        float eyeLuma = dot(albedo, vec3(0.30, 0.59, 0.11));
        vec3 iris = mix(vec3(0.018, 0.025, 0.020), vec3(0.12, 0.30, 0.23),
            smoothstep(0.004, 0.16, eyeLuma));
        vec3 sclera = vec3(0.66, 0.57, 0.49);
        float irisMask = smoothstep(0.025, 0.32, texel.a);
        albedo = mix(sclera, iris, irisMask);
    }
    if (daoTerrain > 0.5)
    {
        // Godot's fog/ambient pass keeps DAO's very warm terrain palettes
        // from becoming neon orange. Apply the same restrained earth grade
        // before lighting while leaving authored buildings and actors intact.
        float terrainLuma = dot(albedo, vec3(0.30, 0.59, 0.11));
        albedo = mix(vec3(terrainLuma), albedo, 0.24) * vec3(1.08, 1.22, 1.34);
    }
    // Exact Godot Dalish environment: normalized authored sun colour
    // (0.462, 0.329, 0.231) at peak*1.55 energy.
    vec3 sunRadiance = vec3(0.7161, 0.5099, 0.3579);
    // brc997d.havenarea authors fog/ambient=(0.83, 0.70, 0.33), and Godot's
    // _apply_authored_environment applies it at energy 0.65.  The previous
    // (0.16, 0.19, 0.16) value was only main.gd's missing-data fallback; using
    // it on a valid Dalish area made the genuine brown aravel atlas read as
    // black/grey slabs. Keep actors on their isolated portrait grade and use
    // the actual area material environment for set pieces and terrain.
    vec3 ambient = daoActor > 0.5 ? vec3(0.46, 0.43, 0.38)
        : vec3(0.5395, 0.4550, 0.2145);
    if (daoFace > 0.5)
    {
        ambient = vec3(0.72, 0.62, 0.55);
        sunRadiance *= 0.58;
    }
    if (daoEye > 0.5)
        ambient = vec3(0.68, 0.64, 0.58);
    vec3 viewDirection = normalize(-vViewPosition);
    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float ndotv = max(dot(normal, viewDirection), 0.0001);
    float ndoth = max(dot(normal, halfDirection), 0.0);
    float ldoth = max(dot(lightDirection, halfDirection), 0.0);
    float roughness = clamp(roughnessFactor, 0.08, 1.0);
    vec3 f0 = mix(vec3(daoActor > 0.5 ? 0.018 : 0.04), albedo, metallicFactor);
    vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - ldoth, 5.0);
    float distribution = distributionGGX(ndoth, roughness);
    float visibility = visibilitySchlickGGX(ndotv, roughness)
        * visibilitySchlickGGX(ndotl, roughness);
    vec3 specular = fresnel * distribution * visibility
        / max(4.0 * ndotv * max(ndotl, 0.0001), 0.0001);
    float diffuseBurley = burleyDiffuse(ndotv, ndotl, ldoth, roughness);
    vec3 linearColor = albedo * ambient
        + (albedo * (1.0 - metallicFactor) * diffuseBurley + specular)
            * sunRadiance * ndotl;

    if (daoPortraitLight > 0.5)
    {
        vec3 keyVector = gl_LightSource[7].position.xyz - vViewPosition;
        float keyDistance = length(keyVector);
        vec3 keyDirection = keyVector / max(keyDistance, 0.0001);
        float keyNdotl = max(dot(normal, keyDirection), 0.0);
        vec3 keyHalf = normalize(keyDirection + viewDirection);
        float keyNdotH = max(dot(normal, keyHalf), 0.0);
        float keyLdotH = max(dot(keyDirection, keyHalf), 0.0);
        float keyDistribution = distributionGGX(keyNdotH, roughness);
        float keyVisibility = visibilitySchlickGGX(ndotv, roughness)
            * visibilitySchlickGGX(keyNdotl, roughness);
        vec3 keyFresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - keyLdotH, 5.0);
        vec3 keySpecular = keyFresnel * keyDistribution * keyVisibility
            / max(4.0 * ndotv * max(keyNdotl, 0.0001), 0.0001);
        float keyBurley = burleyDiffuse(ndotv, keyNdotl, keyLdotH, roughness);
        float keyRange = clamp(1.0 - keyDistance / 4.5, 0.0, 1.0);
        keyRange *= keyRange;
        vec3 keyRadiance = vec3(1.4, 1.148, 0.98) * keyRange;
        linearColor += (albedo * (1.0 - metallicFactor) * keyBurley + keySpecular)
            * keyRadiance * keyNdotl;
    }
    if (daoEye > 0.5)
    {
        float eyeSpecular = pow(max(dot(normal, halfDirection), 0.0), 64.0) * 0.38;
        linearColor += vec3(eyeSpecular);
    }
    linearColor += vec3(authoredSpecular * 0.035 * pow(ndotl, 8.0));
    vec3 displayColor = linearToSrgb(godotFilmic(linearColor));
    // Godot runtime telemetry: volumetric fog density=0.01, length=320 m,
    // scattering colour=(0.518, 0.553, 0.608). The DAO compatibility program
    // does not pass through OpenMW's standard fog shader, so apply the EXP2
    // extinction curve explicitly in view space.
    float fogDistance = min(length(vViewPosition), 320.0);
    // Godot's density is not numerically interchangeable with OpenGL EXP2.
    // Map its measured 320 m volumetric length to the extinction coefficient.
    float fogAmount = 1.0 - exp(-pow((1.0 / 320.0) * fogDistance, 2.0));
    displayColor = mix(displayColor, vec3(0.518, 0.553, 0.608), fogAmount);
    gl_FragColor = vec4(displayColor, daoEye > 0.5 ? 1.0 : texel.a);
}
)GLSL"));
                }
                fixedState->setAttributeAndModes(daoProgram, osg::StateAttribute::ON);
                fixedState->addUniform(new osg::Uniform("baseColorTexture", 0));
                fixedState->addUniform(new osg::Uniform("normalTexture", 1));
                fixedState->addUniform(new osg::Uniform("daoMaskA", 2));
                fixedState->addUniform(new osg::Uniform("daoMaskA2", 3));
                fixedState->addUniform(new osg::Uniform("daoFaceTintMask", 4));
                fixedState->addUniform(new osg::Uniform("daoFaceAgeDiffuse", 5));
                fixedState->addUniform(new osg::Uniform("daoFaceTattooMask", 6));
                fixedState->addUniform(new osg::Uniform("daoFaceAgeNormal", 7));
                fixedState->addUniform(new osg::Uniform("daoFaceMicroDetail", 8));
                fixedState->addUniform(new osg::Uniform("daoRobeTintMask", 9));
                if (daoFace)
                {
                    if (const char* faceMaterialDir = std::getenv("OPENMW_DAO_FACE_MATERIAL_DIR"))
                    {
                        const std::string root(faceMaterialDir);
                        if (osg::ref_ptr<osg::Texture2D> texture
                            = makeDaoTexture(osgDB::concatPaths(root, "uh_hed_maka_0t.dds")))
                            fixedState->setTextureAttributeAndModes(4, texture, osg::StateAttribute::ON);
                        if (osg::ref_ptr<osg::Texture2D> texture
                            = makeDaoTexture(osgDB::concatPaths(root, "uh_hed_olda_0d.dds")))
                            fixedState->setTextureAttributeAndModes(5, texture, osg::StateAttribute::ON);
                        if (osg::ref_ptr<osg::Texture2D> texture
                            = makeDaoTexture(osgDB::concatPaths(root, "uh_tat_ed1_0t.dds")))
                            fixedState->setTextureAttributeAndModes(6, texture, osg::StateAttribute::ON);
                        if (osg::ref_ptr<osg::Texture2D> texture
                            = makeDaoTexture(osgDB::concatPaths(root, "uh_hed_olda_0n.dds")))
                            fixedState->setTextureAttributeAndModes(7, texture, osg::StateAttribute::ON);
                        if (osg::ref_ptr<osg::Texture2D> texture
                            = makeDaoTexture(osgDB::concatPaths(root, "skin_micro_nrm_ao.png")))
                            fixedState->setTextureAttributeAndModes(8, texture, osg::StateAttribute::ON);
                    }
                }
                if (daoRobe)
                {
                    if (const char* robeTintPath = std::getenv("OPENMW_DAO_ROBE_TINT_MASK"))
                    {
                        if (osg::ref_ptr<osg::Texture2D> texture = makeDaoTexture(robeTintPath))
                            fixedState->setTextureAttributeAndModes(9, texture, osg::StateAttribute::ON);
                    }
                }
                fixedState->addUniform(new osg::Uniform(
                    "hasNormalTexture", hasNormalTexture ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("hasTangent",
                    primitive.attributes.find("TANGENT") != primitive.attributes.end() ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("roughnessFactor", roughnessFactor));
                fixedState->addUniform(new osg::Uniform("metallicFactor", metallicFactor));
                const char* daoCharacter = std::getenv("OPENMW_DAO_FACE_CHARACTER");
                const bool daoPortraitLight = daoCharacter != nullptr
                    && std::string(daoCharacter) == "keeper_marethari";
                fixedState->addUniform(new osg::Uniform(
                    "daoPortraitLight", daoPortraitLight ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoTerrain", daoTerrain ? 1.0f : 0.0f));
                const bool daoTerrainDalish = daoTerrain && primitive.material >= 0
                    && primitive.material < model.materials.size()
                    && model.materials[primitive.material].name.find("brc997d_") == 0;
                fixedState->addUniform(new osg::Uniform(
                    "daoTerrainDalish", daoTerrainDalish ? 1.0f : 0.0f));
                const bool daoSetPiece = primitive.material >= 0
                    && primitive.material < model.materials.size()
                    && (model.materials[primitive.material].name.find("prp_aravel") == 0
                        || model.materials[primitive.material].name.find("Prp_Aravel") == 0);
                fixedState->addUniform(new osg::Uniform("daoSetPiece", daoSetPiece ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoSetPieceUnlit",
                    std::getenv("OPENMW_GLTF_DAO_SETPIECE_UNLIT") != nullptr ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoWater", daoWater ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoActor", daoActor ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoFace", daoFace ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoHair", daoHair ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoEye", daoEye ? 1.0f : 0.0f));
                fixedState->addUniform(new osg::Uniform("daoRobe", daoRobe ? 1.0f : 0.0f));
                float cutoff = 0.0f;
                if (primitive.material >= 0 && primitive.material < model.materials.size())
                {
                    const tinygltf::Material& material = model.materials[primitive.material];
                    cutoff = material.alphaMode == "MASK"
                        ? static_cast<float>(material.alphaCutoff)
                        : (material.alphaMode == "BLEND" ? 0.01f : 0.0f);
                    // Haven leaves DAO's first hair pass marked opaque even
                    // though its embedded texture carries a low-range alpha
                    // mask. Discard the zero-alpha card background so it does
                    // not cover the face; HairM2 remains the solid/tint pass.
                    if (daoHairCutout)
                        cutoff = std::max(cutoff, 0.015f);
                    // Haven labels DAO's feather/fur robe atlas OPAQUE even
                    // though base-colour alpha is its coverage mask. The eye
                    // atlas is intentionally different: its near-zero sclera
                    // alpha is packed data and must remain opaque.
                    if (daoRobeCutout)
                        cutoff = std::max(cutoff, 0.03f);
                    if (material.name == "BonfireAsh" || material.name == "BonfireLogs")
                        cutoff = 0.12f;
                }
                fixedState->addUniform(new osg::Uniform("alphaCutoff", cutoff));
                fixedState->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            }
            
            const tinygltf::Accessor &indexAccessor =
                model.accessors[primitive.indices];

            int mode = -1;
            if (primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                mode = GL_TRIANGLES;
            }
            else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                mode = GL_TRIANGLE_STRIP;
            }
            else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN) {
                mode = GL_TRIANGLE_FAN;
            }
            else if (primitive.mode == TINYGLTF_MODE_POINTS) {
                mode = GL_POINTS;
            }
            else if (primitive.mode == TINYGLTF_MODE_LINE) {
                mode = GL_LINES;
            }
            else if (primitive.mode == TINYGLTF_MODE_LINE_LOOP) {
                mode = GL_LINE_LOOP;
            }

            {
                const tinygltf::BufferView& bufferView = model.bufferViews[indexAccessor.bufferView];
                const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

                if (indexAccessor.componentType == GL_UNSIGNED_SHORT)
                {
                    osg::DrawElementsUShort* drawElements = new osg::DrawElementsUShort(mode);
                    unsigned short* indices = (unsigned short*)(&buffer.data.at(0) + bufferView.byteOffset + indexAccessor.byteOffset);
                    drawElements->reserve(indexAccessor.count);
                    for (unsigned int j = 0; j < indexAccessor.count; j++)
                    {
                        unsigned short index = indices[j];
                        drawElements->push_back(index);
                    }
                    geom->addPrimitiveSet(drawElements);
                }
                else if (indexAccessor.componentType == GL_UNSIGNED_INT)
                {
                    osg::DrawElementsUInt* drawElements = new osg::DrawElementsUInt(mode);
                    drawElements->reserve(indexAccessor.count);
                    unsigned int* indices = (unsigned int*)(&buffer.data.at(0) + bufferView.byteOffset + indexAccessor.byteOffset);
                    for (unsigned int j = 0; j < indexAccessor.count; j++)
                    {
                        unsigned int index = indices[j];
                        drawElements->push_back(index);
                    }
                    geom->addPrimitiveSet(drawElements);
                }
                else if (indexAccessor.componentType == GL_UNSIGNED_BYTE)
                {
                    osg::DrawElementsUByte* drawElements = new osg::DrawElementsUByte(mode);
                    drawElements->reserve(indexAccessor.count);
                    unsigned char* indices = (unsigned char*)(&buffer.data.at(0) + bufferView.byteOffset + indexAccessor.byteOffset);
                    for (unsigned int j = 0; j < indexAccessor.count; j++)
                    {
                        unsigned char index = indices[j];
                        drawElements->push_back(index);
                    }
                    geom->addPrimitiveSet(drawElements);
                }
            }

            if (!env.readOptions || env.readOptions->getOptionString().find("gltfSkipNormals") == std::string::npos)
            {
                // Generate normals automatically if we're not given any in the file itself.
                if (!geom->getNormalArray())
                {
                    osgUtil::SmoothingVisitor sv;
                        geom->accept(sv);
                }
            }

            // HavenTools' DAO set-piece exports (including both aravel
            // carriage meshes) contain normal maps but no authored glTF
            // TANGENT accessor.  A fixed object-X tangent makes those maps
            // rotate across the mesh and produces the conspicuous painted
            // bands seen in the parity capture.  Generate the same
            // per-vertex tangent basis Godot creates on import.
            if (hasNormalTexture
                && primitive.attributes.find("TANGENT") == primitive.attributes.end()
                && geom->getVertexArray() != nullptr
                && geom->getNormalArray() != nullptr
                && geom->getTexCoordArray(0) != nullptr)
            {
                osg::ref_ptr<osgUtil::TangentSpaceGenerator> generator
                    = new osgUtil::TangentSpaceGenerator;
                generator->generate(geom.get(), 0);
                osg::Vec4Array* tangents = generator->getTangentArray();
                if (tangents != nullptr
                    && tangents->getNumElements() == geom->getVertexArray()->getNumElements())
                {
                    geom->setVertexAttribArray(6, tangents, osg::Array::BIND_PER_VERTEX);
                    if (osg::Uniform* tangentUniform
                        = geom->getOrCreateStateSet()->getUniform("hasTangent"))
                        tangentUniform->set(1.0f);
                }
            }

            if (daoTerrain)
            {
                const osg::BoundingBox bounds = geom->getBoundingBox();
                OSG_NOTICE << "OPENDAO_GLTF_TERRAIN_BOUNDS mesh=" << mesh.name
                           << " min=(" << bounds.xMin() << ',' << bounds.yMin() << ',' << bounds.zMin()
                           << ") max=(" << bounds.xMax() << ',' << bounds.yMax() << ',' << bounds.zMax()
                           << ")" << std::endl;
            }

            //osgEarth::Registry::shaderGenerator().run(geom.get());
        }

        return group;
    } // Turn all of the accessors and turn them into arrays
    void extractArrays(const tinygltf::Model &model, std::vector<osg::ref_ptr<osg::Array>> &arrays) const
    {
        for (unsigned int i = 0; i < model.accessors.size(); i++)
        {
            const tinygltf::Accessor& accessor = model.accessors[i];
            const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
            const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];


            osg::ref_ptr< osg::Array > osgArray;

            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                if (accessor.type == TINYGLTF_TYPE_SCALAR)
                {
                    osg::FloatArray* floatArray = new osg::FloatArray;
                    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

                    float* array = (float*)(&buffer.data.at(0) + accessor.byteOffset + bufferView.byteOffset);
                    unsigned int pos = 0;
                    floatArray->reserve(accessor.count);
                    for (unsigned int j = 0; j < accessor.count; j++)
                    {
                        float s = array[pos];
                        if (bufferView.byteStride > 0)
                        {
                            pos += (bufferView.byteStride / 4);
                        }
                        else
                        {
                            pos += 1;
                        }
                        floatArray->push_back(s);
                    }
                    osgArray = floatArray;
                }
                else if (accessor.type == TINYGLTF_TYPE_VEC2)
                {
                    osg::Vec2Array* vec2Array = new osg::Vec2Array;
                    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

                    float* array = (float*)(&buffer.data.at(0) + accessor.byteOffset + bufferView.byteOffset);
                    unsigned int pos = 0;
                    vec2Array->reserve(accessor.count);
                    for (unsigned int j = 0; j < accessor.count; j++)
                    {
                        float s = array[pos];
                        float t = array[pos + 1];
                        if (bufferView.byteStride > 0)
                        {
                            pos += (bufferView.byteStride / 4);
                        }
                        else
                        {
                            pos += 2;
                        }
                        vec2Array->push_back(osg::Vec2(s, t));
                    }
                    osgArray = vec2Array;
                }
                else if (accessor.type == TINYGLTF_TYPE_VEC3)
                {
                    osg::Vec3Array* vec3Array = new osg::Vec3Array;
                    float* array = (float*)(&buffer.data.at(0) + accessor.byteOffset + bufferView.byteOffset);
                    unsigned int pos = 0;
                    vec3Array->reserve(accessor.count);
                    for (unsigned int j = 0; j < accessor.count; j++)
                    {
                        float x = array[pos];
                        float y = array[pos + 1];
                        float z = array[pos + 2];
                        if (bufferView.byteStride > 0)
                        {
                            pos += (bufferView.byteStride / 4);
                        }
                        else
                        {
                            pos += 3;
                        }
                        vec3Array->push_back(osg::Vec3(x, y, z));
                        osgArray = vec3Array;
                    }
                }
                else if (accessor.type == TINYGLTF_TYPE_VEC4)
                {
                    osg::Vec4Array* vec4Array = new osg::Vec4Array;
                    float* array = (float*)(&buffer.data.at(0) + accessor.byteOffset + bufferView.byteOffset);
                    unsigned int pos = 0;
                    vec4Array->reserve(accessor.count);
                    for (unsigned int j = 0; j < accessor.count; j++)
                    {
                        float r = array[pos];
                        float g = array[pos + 1];
                        float b = array[pos + 2];
                        float a = array[pos + 3];
                        if (bufferView.byteStride > 0)
                        {
                            pos += (bufferView.byteStride / 4);
                        }
                        else
                        {
                            pos += 4;
                        }
                        vec4Array->push_back(osg::Vec4(r, g, b, a));
                        osgArray = vec4Array;
                    }
                }
            }

            if (osgArray.valid())
            {
                osgArray->setBinding(osg::Array::BIND_PER_VERTEX);
            }
            else
            {
                OSG_DEBUG << "Adding null array for " << i << std::endl;
            }
            arrays.push_back(osgArray);
        }
    }

};

#endif // GLTF_READER_H

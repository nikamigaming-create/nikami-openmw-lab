#version 120

#if @useUBO
    #extension GL_ARB_uniform_buffer_object : require
#endif

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"

#if @diffuseMap
varying vec2 diffuseMapUV;
#endif

#if @normalMap
varying vec2 normalMapUV;
#endif

#if @skinAuxMap
varying vec2 skinAuxMapUV;
#endif

#if @faceGenMap0
varying vec2 faceGenMap0UV;
#endif

#if @faceGenMap1
varying vec2 faceGenMap1UV;
#endif

varying float euclideanDepth;
varying float linearDepth;
varying vec3 passViewPos;
varying vec3 passNormal;

#include "lib/view/depth.glsl"
#include "lib/light/lighting_util.glsl"
#include "compatibility/vertexcolors.glsl"
#include "compatibility/normals.glsl"

void main(void)
{
    gl_Position = modelToClip(gl_Vertex);

    vec4 viewPos = modelToView(gl_Vertex);
    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);
    passColor = gl_Color;
    passViewPos = viewPos.xyz;
    passNormal = gl_Normal.xyz;
    normalToViewMatrix = gl_NormalMatrix;

#if @normalMap
    normalToViewMatrix *= generateTangentSpace(gl_MultiTexCoord7.xyzw, passNormal);
    normalMapUV = (gl_TextureMatrix[@normalMapUV] * gl_MultiTexCoord@normalMapUV).xy;
#endif

#if @diffuseMap
    diffuseMapUV = (gl_TextureMatrix[@diffuseMapUV] * gl_MultiTexCoord@diffuseMapUV).xy;
#endif

#if @skinAuxMap
    skinAuxMapUV = (gl_TextureMatrix[@skinAuxMapUV] * gl_MultiTexCoord@skinAuxMapUV).xy;
#endif

#if @faceGenMap0
    faceGenMap0UV = (gl_TextureMatrix[@faceGenMap0UV] * gl_MultiTexCoord@faceGenMap0UV).xy;
#endif

#if @faceGenMap1
    faceGenMap1UV = (gl_TextureMatrix[@faceGenMap1UV] * gl_MultiTexCoord@faceGenMap1UV).xy;
#endif
}

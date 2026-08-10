#version 120

#if @useUBO
    #extension GL_ARB_uniform_buffer_object : require
#endif

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"

varying float euclideanDepth;
varying float linearDepth;
varying vec3 passViewPos;
varying vec3 passNormal;
varying vec2 daoFaceUV;
varying vec2 daoFaceWeights;

#include "lib/view/depth.glsl"
#include "compatibility/vertexcolors.glsl"
#include "compatibility/normals.glsl"

void main(void)
{
    gl_Position = ftransform();

    vec4 viewPos = gl_ModelViewMatrix * gl_Vertex;
    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    passColor = gl_Color;
    passViewPos = viewPos.xyz;
    passNormal = gl_Normal.xyz;
    daoFaceUV = gl_MultiTexCoord0.xy;

    // Reconstructed face0 contract: TEXCOORD0.z is the head/SSS weight and
    // TEXCOORD0.w is the close-specular weight. Conventional meshes leave
    // these at OpenGL's (0, 1) defaults, which is a safe neutral fallback.
    daoFaceWeights = vec2(clamp(gl_MultiTexCoord0.z, 0.0, 1.0),
        clamp(gl_MultiTexCoord0.w, 0.0, 1.0));

    normalToViewMatrix = gl_NormalMatrix;
#if @normalMap || @bumpMap || @poreNormalMap || @daoAgeNormalMap || @daoEmotionNormalMap || @daoBrowStubbleNormalMap
    normalToViewMatrix *= generateTangentSpace(gl_MultiTexCoord7.xyzw, passNormal);
#endif
}

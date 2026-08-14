#version 120
#pragma import_defines(FORCE_OPAQUE)

#if @useUBO
    #extension GL_ARB_uniform_buffer_object : require
#endif

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#define PER_PIXEL_LIGHTING 1

#if @diffuseMap
uniform sampler2D diffuseMap;
varying vec2 diffuseMapUV;
#endif

#if @normalMap
uniform sampler2D normalMap;
varying vec2 normalMapUV;
#endif

#if @skinAuxMap
uniform sampler2D skinAuxMap;
varying vec2 skinAuxMapUV;
#endif

#if @faceGenMap0
uniform sampler2D faceGenMap0;
varying vec2 faceGenMap0UV;
#endif

#if @faceGenMap1
uniform sampler2D faceGenMap1;
varying vec2 faceGenMap1UV;
#endif

varying float euclideanDepth;
varying float linearDepth;
varying vec3 passViewPos;
varying vec3 passNormal;

uniform vec2 screenRes;
uniform float far;
uniform float alphaRef;
uniform bool falloutSkinUseVertexColor;

#include "lib/core/fragment.h.glsl"
#include "lib/light/lighting.glsl"
#include "lib/material/alpha.glsl"
#include "compatibility/vertexcolors.glsl"
#include "compatibility/fog.glsl"
#include "compatibility/normals.glsl"

// Analytic replay of FalloutNV.exe's 128x128 X8R8G8B8 attenuation texture.
// SKIN2002 samples it twice using the point-light vector normalized by the
// runtime light radius, then computes clamp(1 - xy - z, 0, 1).
float falloutSkinAttenuationLutTexel(vec2 texel)
{
    vec2 coordinate = 2.0 * clamp(texel, vec2(0.0), vec2(127.0)) / 127.0 - 1.0;
    return floor(255.0 * min(1.0, dot(coordinate, coordinate))) / 255.0;
}

float sampleFalloutSkinAttenuationLut(vec2 uv)
{
    vec2 position = clamp(uv, vec2(0.0), vec2(1.0)) * 128.0 - 0.5;
    vec2 lower = floor(position);
    vec2 blend = position - lower;
    float v00 = falloutSkinAttenuationLutTexel(lower);
    float v10 = falloutSkinAttenuationLutTexel(lower + vec2(1.0, 0.0));
    float v01 = falloutSkinAttenuationLutTexel(lower + vec2(0.0, 1.0));
    float v11 = falloutSkinAttenuationLutTexel(lower + vec2(1.0, 1.0));
    return mix(mix(v00, v10, blend.x), mix(v01, v11, blend.x), blend.y);
}

#if !@lightingMethodFFP
float falloutSkinPointAttenuation(int lightIndex, vec3 pointVector)
{
    // Gamebryo's runtime radius recorded in c26.w is 1.189032975 times the
    // authored FNV light radius exposed by OpenMW. This conversion is global
    // to the FNV light pipeline; it is not tied to a particular actor.
    const float falloutRuntimeRadiusScale = 1.189032975;
    float radius = max(lcalcRadius(lightIndex) * falloutRuntimeRadiusScale, 1e-6);
    vec3 q = pointVector / radius * 0.5 + 0.5;
    return clamp(1.0 - sampleFalloutSkinAttenuationLut(q.xy)
        - sampleFalloutSkinAttenuationLut(vec2(q.z, 0.5)), 0.0, 1.0);
}
#endif

vec3 falloutSkinLighting(vec3 viewPos, vec3 viewNormal, vec3 scatterColor)
{
    vec3 normal = normalize(viewNormal);
    vec3 viewDirection = normalize(-viewPos);
    float rimBase = 1.0 - clamp(dot(normal, viewDirection), 0.0, 1.0);
    rimBase *= rimBase;

    // c3 in SKIN2002 is the directional light. Its diffuse response and
    // half-strength view-facing rim term are emitted before the point light.
    vec3 sunDirection = normalize(lcalcPosition(0));
    vec3 sunColor = lcalcDiffuse(0);
    float sunLambert = clamp(dot(normal, sunDirection), 0.0, 1.0);
    float sunRim = clamp(dot(viewDirection, -sunDirection), 0.0, 1.0) * rimBase;
    vec3 light = sunColor * sunLambert + 0.5 * sunColor * sunRim;

    // SKIN2002 has room for exactly one selected point light (c4/c26). The
    // GlowMap is not emission: it supplies the color for wrapped subsurface
    // response and half of the point-light rim response.
    bool usedPointLight = false;
    for (int i = @startLight; i < @endLight; ++i)
    {
        if (usedPointLight)
            continue;
#if @lightingMethodUBO
        int lightIndex = PointLightIndex[i];
#else
        int lightIndex = i;
#endif
        vec3 pointVector = lcalcPosition(lightIndex) - viewPos;
        float pointDistance = length(pointVector);
#if !@classicFalloff && !@lightingMethodFFP
        if (pointDistance > lcalcRadius(lightIndex) * 2.0)
            continue;
#endif
        vec3 pointDirection = pointVector / max(pointDistance, 1e-6);
        float pointDot = dot(normal, pointDirection);
        float pointLambert = clamp(pointDot, 0.0, 1.0);
        float wrapped = clamp((pointDot + 0.300000012) * 0.769230783, 0.0, 1.0);
        float smoothLambert = (3.0 - 2.0 * pointLambert)
            * pointLambert * pointLambert;
        float smoothWrapped = (3.0 - 2.0 * wrapped) * wrapped * wrapped;
        float subsurface = clamp(smoothWrapped - smoothLambert, 0.0, 1.0);
        float pointRim = clamp(dot(viewDirection, -pointDirection), 0.0, 1.0)
            * rimBase;
        vec3 pointColor = lcalcDiffuse(lightIndex);
        vec3 pointLight = pointColor * pointLambert
            + pointRim * (0.5 * scatterColor + 0.5 * pointColor)
            + subsurface * scatterColor;
#if @lightingMethodFFP
        float attenuation = lcalcIllumination(lightIndex, pointDistance);
#else
        float attenuation = falloutSkinPointAttenuation(lightIndex, pointVector);
#endif
        light += pointLight * attenuation;
        usedPointLight = true;
    }

    // c1.xyz is already the complete AmbientColor in retail SKIN2002. The
    // bytecode adds it directly after directional and attenuated point light;
    // it never multiplies c1 by a material ambient term. Multiplying here a
    // second time suppresses the cool ambient contribution and leaves exposed
    // skin incorrectly dark and warm.
    light += gl_LightModel.ambient.xyz;
    return max(light, vec3(0.0));
}

void main()
{
#if @diffuseMap
    vec4 base = texture2D(diffuseMap, diffuseMapUV);
#else
    vec4 base = vec4(1.0);
#endif

    vec3 faceGen0 = vec3(0.5);
#if @faceGenMap0
    faceGen0 = texture2D(faceGenMap0, faceGenMap0UV).rgb;
#endif

    // A missing FaceGen1 is neutral at 0.25 because SKIN2002 applies 4 * FaceGen1.
    vec3 faceGen1 = vec3(0.25);
#if @faceGenMap1
    faceGen1 = texture2D(faceGenMap1, faceGenMap1UV).rgb;
#endif

    vec3 scatterColor = vec3(0.0);
#if @skinAuxMap
    scatterColor = texture2D(skinAuxMap, skinAuxMapUV).rgb;
#endif

    // Exact SKIN2002 FaceGen composition. There is deliberately no SkinDimmer:
    // the retail pixel shader has no such register or multiplication.
    vec3 albedo = (base.rgb + 2.0 * (faceGen0 - vec3(0.5))) * (4.0 * faceGen1);
    if (falloutSkinUseVertexColor)
        albedo *= passColor.rgb;

#if @normalMap
    vec3 tangentNormal = texture2D(normalMap, normalMapUV).rgb * 2.0 - 1.0;
#if @reconstructNormalZ
    tangentNormal.z = sqrt(max(0.0, 1.0 - dot(tangentNormal.xy, tangentNormal.xy)));
#endif
    vec3 viewNormal = normalToView(tangentNormal);
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

    vec3 light = falloutSkinLighting(passViewPos, viewNormal, scatterColor);

    vec4 result = vec4(albedo * max(light, vec3(0.0)), base.a * gl_FrontMaterial.diffuse.a);
    result.a = alphaTest(result.a, alphaRef);
    result = applyFogAtDist(result, euclideanDepth, linearDepth, far);

#if defined(FORCE_OPAQUE) && FORCE_OPAQUE
    result.a = 1.0;
#endif

    gl_FragData[0] = result;

#if !defined(FORCE_OPAQUE) && !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif
}

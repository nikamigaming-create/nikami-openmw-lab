#version 120
#pragma import_defines(FORCE_OPAQUE, DISTORTION)

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

#if @emissiveMap
uniform sampler2D emissiveMap;
varying vec2 emissiveMapUV;
#endif

#if @normalMap
uniform sampler2D normalMap;
varying vec2 normalMapUV;
#endif

#if @envMap
uniform sampler2D envMap;
varying vec2 envMapUV;
uniform vec4 envMapColor;
#endif

#if @hairPaletteMap
uniform sampler2D hairPaletteMap;
#endif

#if @glossMap
uniform sampler2D glossMap;
varying vec2 glossMapUV;
#endif

varying float euclideanDepth;
varying float linearDepth;

varying vec3 passViewPos;
varying vec3 passNormal;

uniform vec2 screenRes;
uniform float far;
uniform float alphaRef;
uniform float emissiveMult;
uniform float specStrength;
uniform bool useTreeAnim;
uniform float distortionStrength;
uniform int falloutSlsMode;
uniform bool falloutHairPaletteMode;
uniform float falloutHairColorIndex;

#include "lib/core/fragment.h.glsl"
#include "lib/light/lighting.glsl"
#include "lib/material/alpha.glsl"
#include "lib/util/distortion.glsl"

#include "compatibility/vertexcolors.glsl"
#include "compatibility/shadows_fragment.glsl"
#include "compatibility/fog.glsl"
#include "compatibility/normals.glsl"

vec3 falloutEnvMapEffect(vec3 viewNormal)
{
    vec3 envEffect = vec3(0.0);
#if @envMap
    if (falloutHairPaletteMode)
        return envEffect;
    vec2 envTexCoordGen = envMapUV;
#if @normalMap
    vec3 viewVec = normalize(passViewPos);
    vec3 r = reflect(viewVec, viewNormal);
    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
    envTexCoordGen = vec2(r.x / m + 0.5, r.y / m + 0.5);
#endif
    envEffect = texture2D(envMap, envTexCoordGen).xyz * envMapColor.xyz;
#if @glossMap
    envEffect *= texture2D(glossMap, glossMapUV).xyz;
#endif
#endif
    return envEffect;
}

float falloutSlsAttenuationLutTexel(vec2 texel)
{
    vec2 coordinate = 2.0 * clamp(texel, vec2(0.0), vec2(127.0)) / 127.0 - 1.0;
    return floor(255.0 * min(1.0, dot(coordinate, coordinate))) / 255.0;
}

float sampleFalloutSlsAttenuationLut(vec2 uv)
{
    // Analytic replay of FalloutNV.exe's 128x128 X8R8G8B8 attenuation map,
    // including D3D9 linear-clamp sampling of its independent 8-bit terms.
    vec2 position = clamp(uv, vec2(0.0), vec2(1.0)) * 128.0 - 0.5;
    vec2 lower = floor(position);
    vec2 blend = position - lower;
    float v00 = falloutSlsAttenuationLutTexel(lower);
    float v10 = falloutSlsAttenuationLutTexel(lower + vec2(1.0, 0.0));
    float v01 = falloutSlsAttenuationLutTexel(lower + vec2(0.0, 1.0));
    float v11 = falloutSlsAttenuationLutTexel(lower + vec2(1.0, 1.0));
    return mix(mix(v00, v10, blend.x), mix(v01, v11, blend.x), blend.y);
}

#if !@lightingMethodFFP
float falloutSlsPointAttenuation(int lightIndex, vec3 pointVector)
{
    // Easy Pete's retail SLS2009 c26.w is 237.806595 for the authored
    // radius-200 porch light.  This is the measured runtime conversion.
    const float falloutRuntimeRadiusScale = 1.189032975;
    float radius = max(lcalcRadius(lightIndex) * falloutRuntimeRadiusScale, 1e-6);
    vec3 q = pointVector / radius * 0.5 + 0.5;
    return clamp(1.0 - sampleFalloutSlsAttenuationLut(q.xy)
        - sampleFalloutSlsAttenuationLut(vec2(q.z, 0.5)), 0.0, 1.0);
}
#endif

void main()
{
#if @diffuseMap
    gl_FragData[0] = texture2D(diffuseMap, diffuseMapUV);

#if @hairPaletteMap
    if (falloutHairPaletteMode)
        gl_FragData[0].rgb = texture2D(hairPaletteMap,
            vec2(clamp(gl_FragData[0].r, 0.0, 1.0), clamp(falloutHairColorIndex, 0.0, 1.0))).rgb;
#endif

#if defined(DISTORTION) && DISTORTION
    vec2 screenCoords = gl_FragCoord.xy / (screenRes * @distorionRTRatio);
    gl_FragData[0].a *= getDiffuseColor().a;
    gl_FragData[0] = applyDistortion(gl_FragData[0], distortionStrength, gl_FragCoord.z, sampleOpaqueDepthTex(screenCoords).x);

    return;
#endif

    gl_FragData[0].a *= coveragePreservingAlphaScale(diffuseMap, diffuseMapUV);
#else
    gl_FragData[0] = vec4(1.0);
#endif

    vec4 diffuseColor = getDiffuseColor();
    if (!useTreeAnim)
        gl_FragData[0].a *= diffuseColor.a;
    gl_FragData[0].a = alphaTest(gl_FragData[0].a, alphaRef);

    vec3 specularColor = getSpecularColor().xyz;
#if @normalMap
    vec4 normalTex = texture2D(normalMap, normalMapUV);
    vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
    normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
    vec3 viewNormal = normalToView(normal);
    specularColor *= normalTex.a;
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

    if (falloutSlsMode == 1)
    {
        // Bytecode translation of FNV SLS2011.pso.  Unlike OpenMW's generic
        // path this shader has no specular, point ambient, or inverse-distance
        // falloff: base * vertex/material diffuse * (ambient + sun + LUT point).
        vec3 light = getAmbientColor().xyz * gl_LightModel.ambient.xyz;
        vec3 sunDirection = normalize(lcalcPosition(0));
        light += lcalcDiffuse(0) * clamp(dot(viewNormal, sunDirection), 0.0, 1.0);

        for (int i = @startLight; i < @endLight; ++i)
        {
#if @lightingMethodUBO
            int lightIndex = PointLightIndex[i];
#else
            int lightIndex = i;
#endif
            vec3 pointVector = lcalcPosition(lightIndex) - passViewPos;
            float pointDistance = length(pointVector);
            vec3 pointDirection = pointVector / max(pointDistance, 1e-6);
#if @lightingMethodFFP
            float attenuation = lcalcIllumination(lightIndex, pointDistance);
#else
            float attenuation = falloutSlsPointAttenuation(lightIndex, pointVector);
#endif
            light += lcalcDiffuse(lightIndex)
                * clamp(dot(viewNormal, pointDirection), 0.0, 1.0) * attenuation;
        }

        gl_FragData[0].xyz *= diffuseColor.xyz * max(light, vec3(0.0));
        gl_FragData[0].xyz += falloutEnvMapEffect(viewNormal);
        gl_FragData[0] = applyFogAtDist(gl_FragData[0], euclideanDepth, linearDepth, far);

#if defined(FORCE_OPAQUE) && FORCE_OPAQUE
        gl_FragData[0].a = 1.0;
#endif

#if !defined(FORCE_OPAQUE) && !@disableNormals
        gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

        applyShadowDebugOverlay();
        return;
    }

    float shadowing = unshadowedLightRatio(linearDepth);
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(passViewPos, viewNormal, gl_FrontMaterial.shininess, shadowing, diffuseLight, ambientLight, specularLight);
    vec3 diffuse = diffuseColor.xyz * diffuseLight;
    vec3 ambient = getAmbientColor().xyz * ambientLight;
    vec3 emission = getEmissionColor().xyz * emissiveMult;
#if @emissiveMap
    emission *= texture2D(emissiveMap, emissiveMapUV).xyz;
#endif
    vec3 lighting = diffuse + ambient + emission;
    vec3 specular = specularColor * specularLight * specStrength;

    clampLightingResult(lighting);

    gl_FragData[0].xyz = gl_FragData[0].xyz * lighting + specular;
    gl_FragData[0].xyz += falloutEnvMapEffect(viewNormal);

    gl_FragData[0] = applyFogAtDist(gl_FragData[0], euclideanDepth, linearDepth, far);

#if defined(FORCE_OPAQUE) && FORCE_OPAQUE
    // having testing & blending isn't enough - we need to write an opaque pixel to be opaque
    gl_FragData[0].a = 1.0;
#endif

#if !defined(FORCE_OPAQUE) && !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

    applyShadowDebugOverlay();
}

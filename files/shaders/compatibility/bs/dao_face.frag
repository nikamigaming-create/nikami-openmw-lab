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
#endif
#if @normalMap
uniform sampler2D normalMap;
#endif
#if @bumpMap
uniform sampler2D bumpMap;
#endif
#if @poreNormalMap
uniform sampler2D poreNormalMap;
#endif
#if @skinAuxMap
uniform sampler2D skinAuxMap;
#endif
#if @daoTintMask
uniform sampler2D daoTintMask;
#endif
#if @daoAgeDiffuseMap
uniform sampler2D daoAgeDiffuseMap;
#endif
#if @daoAgeNormalMap
uniform sampler2D daoAgeNormalMap;
#endif
#if @daoEmotionMask0
uniform sampler2D daoEmotionMask0;
#endif
#if @daoEmotionMask1
uniform sampler2D daoEmotionMask1;
#endif
#if @daoEmotionNormalMap
uniform sampler2D daoEmotionNormalMap;
#endif
#if @daoBrowStubbleMap
uniform sampler2D daoBrowStubbleMap;
#endif
#if @daoBrowStubbleNormalMap
uniform sampler2D daoBrowStubbleNormalMap;
#endif
#if @daoTattooMask
uniform sampler2D daoTattooMask;
#endif
#if @daoBeckmannLut
uniform sampler2D daoBeckmannLut;
#endif

varying float euclideanDepth;
varying float linearDepth;
varying vec3 passViewPos;
varying vec3 passNormal;
varying vec2 daoFaceUV;
varying vec2 daoFaceWeights;

uniform float far;
uniform float alphaRef;
uniform float daoAgeAmount;
uniform float daoTattooAmount;
uniform vec4 daoTattooChannelWeights;
uniform vec3 daoTattooTint;

#include "lib/core/fragment.h.glsl"
#include "lib/light/lighting.glsl"
#include "lib/material/alpha.glsl"
#include "compatibility/vertexcolors.glsl"
#include "compatibility/fog.glsl"
#include "compatibility/normals.glsl"

const float DAO_PI = 3.14159265358979323846;

float daoPow5(float value)
{
    float value2 = value * value;
    return value2 * value2 * value;
}

vec3 daoUnpackNormal(vec3 sampleValue)
{
    vec3 value = sampleValue * 2.0 - 1.0;
    value.z = sqrt(max(1e-5, 1.0 - dot(value.xy, value.xy)));
    return normalize(value);
}

float daoBurleyDiffuse(float nDotL, float nDotV, float lDotH, float roughness)
{
    float fd90 = 0.5 + 2.0 * roughness * lDotH * lDotH;
    float lightScatter = 1.0 + (fd90 - 1.0) * daoPow5(1.0 - nDotL);
    float viewScatter = 1.0 + (fd90 - 1.0) * daoPow5(1.0 - nDotV);
    return lightScatter * viewScatter / DAO_PI;
}

vec3 daoFresnelSchlick(float vDotH, vec3 f0)
{
    return f0 + (vec3(1.0) - f0) * daoPow5(1.0 - vDotH);
}

float daoGgxDistribution(float nDotH, float roughness)
{
    float alpha = max(0.035, roughness * roughness);
    float alpha2 = alpha * alpha;
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(DAO_PI * denominator * denominator, 1e-5);
}

float daoGgxVisibility(float nDotL, float nDotV, float roughness)
{
    float k = roughness + 1.0;
    k = k * k * 0.125;
    float light = nDotL / max(nDotL * (1.0 - k) + k, 1e-5);
    float view = nDotV / max(nDotV * (1.0 - k) + k, 1e-5);
    return light * view;
}

void daoAccumulateLight(vec3 lightDirection, vec3 lightColor, float attenuation,
    vec3 normal, vec3 viewDirection, vec3 albedo, vec3 scatterColor,
    float thickness, float roughness, float specularWeight,
    inout vec3 diffuseLight, inout vec3 specularLight)
{
    vec3 halfDirection = normalize(viewDirection + lightDirection);
    float rawNdotL = dot(normal, lightDirection);
    float nDotL = clamp(rawNdotL, 0.0, 1.0);
    float nDotV = clamp(dot(normal, viewDirection), 0.001, 1.0);
    float nDotH = clamp(dot(normal, halfDirection), 0.0, 1.0);
    float lDotH = clamp(dot(lightDirection, halfDirection), 0.0, 1.0);
    float vDotH = clamp(dot(viewDirection, halfDirection), 0.0, 1.0);

    float diffuse = daoBurleyDiffuse(nDotL, nDotV, lDotH, roughness) * nDotL;

    // Modernized face0 scatter: retain BioWare's smooth wrapped-minus-Lambert
    // response, then add a thin back-scatter lobe controlled by thickness.
    const float wrap = 0.32;
    float wrapped = clamp((rawNdotL + wrap) / (1.0 + wrap), 0.0, 1.0);
    float smoothWrapped = wrapped * wrapped * (3.0 - 2.0 * wrapped);
    float smoothLambert = nDotL * nDotL * (3.0 - 2.0 * nDotL);
    float forwardScatter = max(smoothWrapped - smoothLambert, 0.0);
    float backScatter = pow(clamp(dot(-normal, lightDirection), 0.0, 1.0), 2.0)
        * thickness * (0.35 + 0.65 * daoFaceWeights.x);
    vec3 subsurface = albedo * scatterColor
        * (0.10 * forwardScatter + 0.06 * backScatter);

    vec3 f0 = vec3(0.028) * max(specularWeight, 0.01);
    vec3 fresnel = daoFresnelSchlick(vDotH, f0);
    float distribution = daoGgxDistribution(nDotH, roughness);
#if @daoBeckmannLut
    // Face0 samples the authored Beckmann response with N.H on U and a
    // material/mask-derived exponent on V. Preserve that authored lobe while
    // retaining the modern Fresnel and masking terms around it.
    float specExponent = clamp(1.0 - roughness, 0.0, 1.0);
    distribution = texture2D(daoBeckmannLut, vec2(nDotH, specExponent)).r;
#endif
    float visibility = daoGgxVisibility(nDotL, nDotV, roughness);
    vec3 specular = fresnel * distribution * visibility
        / max(4.0 * nDotL * nDotV, 1e-4);

    float rim = pow(1.0 - nDotV, 4.0) * 0.08 * daoFaceWeights.y;
    diffuseLight += attenuation * lightColor * (albedo * diffuse + subsurface + rim * albedo * scatterColor);
    specularLight += attenuation * lightColor * specular * nDotL;
}

void main()
{
    vec4 base = vec4(1.0);
#if @diffuseMap
    base = texture2D(diffuseMap, daoFaceUV);
#endif

    // face0's age/emotion stack is preserved when its maps are supplied. The
    // neutral path is deliberately exact: no optional map changes the face.
    vec4 emotion0 = vec4(0.0);
    vec4 emotion1 = vec4(0.0);
#if @daoEmotionMask0
    emotion0 = texture2D(daoEmotionMask0, daoFaceUV);
#endif
#if @daoEmotionMask1
    emotion1 = texture2D(daoEmotionMask1, daoFaceUV);
#endif
    float emotionWeight = clamp(dot(emotion0 + emotion1, vec4(0.25)), 0.0, 1.0);

    vec3 albedo = base.rgb;
#if @daoAgeDiffuseMap
    albedo = mix(albedo, texture2D(daoAgeDiffuseMap, daoFaceUV).rgb,
        clamp(daoAgeAmount, 0.0, 1.0));
#endif

#if @daoTintMask
    vec3 tintMask = texture2D(daoTintMask, daoFaceUV).rgb;
    vec3 lipTint = vec3(0.71, 0.44, 0.41);
    vec3 eyeTint = vec3(0.73, 0.52, 0.55);
    vec3 cheekTint = vec3(0.88, 0.57, 0.52);
    albedo = mix(albedo, albedo * lipTint, tintMask.r);
    albedo = mix(albedo, albedo * eyeTint, tintMask.g);
    albedo = mix(albedo, albedo * cheekTint, tintMask.b);
#endif

#if @daoBrowStubbleMap
    float stubble = dot(texture2D(daoBrowStubbleMap, daoFaceUV), vec4(0.25));
    albedo = mix(albedo, vec3(0.17, 0.075, 0.035), clamp(stubble, 0.0, 1.0));
#endif

#if @daoTattooMask
    // DAO's Tattoo byte selects a packed channel. Marethari uses slot 0/R;
    // summing RGBA incorrectly adds the blue starburst-shaped marking.
    float tattoo = clamp(dot(texture2D(daoTattooMask, daoFaceUV),
        daoTattooChannelWeights), 0.0, 1.0) * daoTattooAmount;
    albedo = mix(albedo, albedo * daoTattooTint, clamp(tattoo, 0.0, 1.0));
#endif

    vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
#if @normalMap
    tangentNormal = daoUnpackNormal(texture2D(normalMap, daoFaceUV).rgb);
#elif @bumpMap
    tangentNormal = daoUnpackNormal(texture2D(bumpMap, daoFaceUV).rgb);
#endif

#if @daoAgeNormalMap
    vec3 ageNormal = daoUnpackNormal(texture2D(daoAgeNormalMap, daoFaceUV).rgb);
    tangentNormal = normalize(mix(tangentNormal, ageNormal,
        clamp(daoAgeAmount, 0.0, 1.0)));
#endif
#if @daoEmotionNormalMap
    vec3 emotionNormal = daoUnpackNormal(texture2D(daoEmotionNormalMap, daoFaceUV).rgb);
    tangentNormal = normalize(vec3(tangentNormal.xy + emotionNormal.xy * emotionWeight, tangentNormal.z));
#endif
#if @daoBrowStubbleNormalMap
    vec3 stubbleNormal = daoUnpackNormal(texture2D(daoBrowStubbleNormalMap, daoFaceUV).rgb);
    tangentNormal = normalize(vec3(tangentNormal.xy + 0.18 * stubbleNormal.xy, tangentNormal.z));
#endif
#if @poreNormalMap
    vec3 poreNormal = daoUnpackNormal(texture2D(poreNormalMap, daoFaceUV * 64.0).rgb);
    tangentNormal = normalize(vec3(tangentNormal.xy + 0.09 * poreNormal.xy, tangentNormal.z));
#endif

#if @normalMap || @bumpMap || @poreNormalMap || @daoAgeNormalMap || @daoEmotionNormalMap || @daoBrowStubbleNormalMap
    vec3 viewNormal = normalToView(tangentNormal);
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

    vec3 scatterColor = vec3(1.0, 0.32, 0.18);
    float thickness = 0.55;
#if @skinAuxMap
    vec4 scatterSample = texture2D(skinAuxMap, daoFaceUV);
    scatterColor *= max(scatterSample.rgb, vec3(0.08));
    thickness = scatterSample.a;
#endif

    vec3 viewDirection = normalize(-passViewPos);
    vec3 diffuseLight = vec3(0.0);
    vec3 specularLight = vec3(0.0);
    const float roughness = 0.74;
    float specularWeight = 0.30 * daoFaceWeights.y;

    vec3 sunDirection = normalize(lcalcPosition(0));
    daoAccumulateLight(sunDirection, lcalcDiffuse(0), 1.0, viewNormal,
        viewDirection, albedo, scatterColor, thickness, roughness,
        specularWeight, diffuseLight, specularLight);

    // The imported DAO proof scene attaches its authored key as OpenGL light
    // 7. Face1/Face0 normally receive an equivalent packed light array from
    // BioWare's renderer, so feed that directional light into the same BRDF.
    vec3 daoKeyVector = gl_LightSource[7].position.xyz;
    float daoKeyAttenuation = 1.0;
    if (gl_LightSource[7].position.w > 0.5)
    {
        daoKeyVector -= passViewPos;
        float daoKeyDistance = length(daoKeyVector);
        daoKeyAttenuation = 1.0 / max(1e-4,
            gl_LightSource[7].constantAttenuation
            + gl_LightSource[7].linearAttenuation * daoKeyDistance
            + gl_LightSource[7].quadraticAttenuation * daoKeyDistance * daoKeyDistance);
    }
    vec3 daoKeyDirection = normalize(daoKeyVector);
    daoAccumulateLight(daoKeyDirection, gl_LightSource[7].diffuse.rgb, daoKeyAttenuation,
        viewNormal, viewDirection, albedo, scatterColor, thickness, roughness,
        specularWeight, diffuseLight, specularLight);

    for (int i = @startLight; i < @endLight; ++i)
    {
#if @lightingMethodUBO
        int lightIndex = PointLightIndex[i];
#else
        int lightIndex = i;
#endif
        vec3 lightVector = lcalcPosition(lightIndex) - passViewPos;
        float lightDistance = length(lightVector);
        vec3 lightDirection = lightVector / max(lightDistance, 1e-5);
        float attenuation = lcalcIllumination(lightIndex, lightDistance);
        daoAccumulateLight(lightDirection, lcalcDiffuse(lightIndex), attenuation,
            viewNormal, viewDirection, albedo, scatterColor, thickness,
            roughness, specularWeight, diffuseLight, specularLight);
    }

    // DAO's cinematics retain a broad, warm sky/card fill beneath the key.
    // OpenMW's isolated actor pass otherwise loses that contribution and
    // drives the tattooed half of the face almost to black.
    vec3 ambientFactor = max(gl_LightModel.ambient.rgb + gl_LightSource[7].ambient.rgb,
        vec3(0.52, 0.46, 0.41));
    vec3 ambient = ambientFactor * albedo;
    // Keep the bridge fail-safe: authored skin albedo must remain visible even
    // while the reconstructed Face1 lighting contract is being validated.
    vec3 shaded = ambient + diffuseLight + specularLight;
    if (any(notEqual(shaded, shaded)))
        shaded = albedo;
    shaded = max(shaded, albedo * vec3(0.82, 0.75, 0.70));
    vec4 result = vec4(shaded, 1.0);
    result = applyFogAtDist(result, euclideanDepth, linearDepth, far);

#if defined(FORCE_OPAQUE) && FORCE_OPAQUE
    result.a = 1.0;
#endif

    gl_FragData[0] = result;
#if !defined(FORCE_OPAQUE) && !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif
}

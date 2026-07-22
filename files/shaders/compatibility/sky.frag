#version 120

#include "lib/sky/passes.glsl"

uniform int pass;
uniform sampler2D diffuseMap;
uniform sampler2D falloutCloudBlendMap; // PASS_CLOUDS
uniform sampler2D maskMap;      // PASS_MOON
uniform float opacity;          // PASS_CLOUDS, PASS_ATMOSPHERE_NIGHT
uniform bool useFalloutCloudShader; // PASS_CLOUDS
uniform float falloutCloudBlendFactor; // PASS_CLOUDS
uniform vec4 moonBlend;         // PASS_MOON
uniform vec4 atmosphereFade;    // PASS_MOON
uniform int useFalloutAtmosphereGradientColors; // PASS_ATMOSPHERE
uniform vec4 falloutAtmosphereSkyUpperColor;    // PASS_ATMOSPHERE
uniform vec4 falloutAtmosphereSkyLowerColor;    // PASS_ATMOSPHERE
uniform vec4 falloutAtmosphereSkyHorizonColor;  // PASS_ATMOSPHERE

varying vec2 diffuseMapUV;
varying vec4 passColor;

void paintAtmosphere(inout vec4 color)
{
    color = gl_FrontMaterial.emission;
    if (useFalloutAtmosphereGradientColors != 0)
    {
        float gradient = clamp(passColor.a, 0.0, 1.0);
        vec3 lowerBand = mix(
            falloutAtmosphereSkyHorizonColor.rgb, falloutAtmosphereSkyLowerColor.rgb, smoothstep(0.0, 0.22, gradient));
        color.rgb = mix(lowerBand, falloutAtmosphereSkyUpperColor.rgb, smoothstep(0.22, 0.58, gradient));
        color.a *= passColor.a;
    }
    else
        color.a *= passColor.a;
}

void paintAtmosphereNight(inout vec4 color)
{
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= passColor.a * opacity;
}

void paintClouds(inout vec4 color)
{
    if (useFalloutCloudShader)
    {
        // Exact SKYTEX.pso (FNV shaderpackage013) channel contract. The shader treats an all-black texture sample
        // as an absent cloud during WTHR transitions, blends alpha separately, then applies the interpolated
        // SKYCLOUDS vertex color. Params.y modulates RGB only; it does not crush authored texture/vertex alpha.
        vec4 currentSample = texture2D(diffuseMap, diffuseMapUV);
        vec4 blendSample = texture2D(falloutCloudBlendMap, diffuseMapUV);
        float blend = clamp(falloutCloudBlendFactor, 0.0, 1.0);

        vec4 currentOnly = vec4(currentSample.rgb, currentSample.a * (1.0 - blend));
        vec4 crossFaded = mix(currentSample, blendSample, blend);
        bool blendHasColor = dot(blendSample.rgb, vec3(1.0)) != 0.0;
        vec4 selected = blendHasColor ? crossFaded : currentOnly;

        blendSample.a *= blend;
        bool currentHasColor = dot(currentSample.rgb, vec3(1.0)) != 0.0;
        color = currentHasColor ? selected : blendSample;
        color *= passColor;
        color.rgb *= opacity;
    }
    else
    {
        color = texture2D(diffuseMap, diffuseMapUV);
        color.a *= passColor.a * opacity;
        color.xyz = clamp(color.xyz * gl_FrontMaterial.emission.xyz, 0.0, 1.0);
        color = mix(vec4(gl_Fog.color.xyz, color.a), color, passColor.a);
    }
}

void paintMoon(inout vec4 color)
{
    vec4 phase = texture2D(diffuseMap, diffuseMapUV);
    vec4 mask = texture2D(maskMap, diffuseMapUV);

    // Morrowind does this in two passes

    // First pass: moon shadow, normal blending (src alpha, 1 - src alpha)
    // dst.rgb = mask.rgb * mask.a + dst.rgb * (1 - mask.a)
    // Second pass: moon phase, additive blending (src alpha, 1)
    // dst.rgb += phase.rgb * phase.a

    // The same is doable in a single pass through premultiplied alpha blending
    // color.rgb = mask.rgb * mask.a + phase.rgb * phase.a
    // color.a = mask.a
    // dst.rgb = color.rgb + dst.rgb * (1 - color.a)

    vec3 maskTinted = mask.rgb * atmosphereFade.rgb;
    float maskAlpha = mask.a * atmosphereFade.a;
    vec3 phaseTinted = phase.rgb * moonBlend.rgb;
    float phaseAlpha = phase.a * atmosphereFade.a;

    color.rgb = maskTinted * maskAlpha + phaseTinted * phaseAlpha;
    color.a = maskAlpha;
}

void paintSun(inout vec4 color)
{
    color = texture2D(diffuseMap, diffuseMapUV);
    color.rgb = clamp(color.rgb * gl_FrontMaterial.emission.rgb, 0.0, 1.0);
    color.a *= gl_FrontMaterial.diffuse.a;
}

void paintSunglare(inout vec4 color)
{
    color = gl_FrontMaterial.emission;
    color.a = gl_FrontMaterial.diffuse.a;
}

void processSunflashQuery()
{
    const float threshold = 0.8;

    if (texture2D(diffuseMap, diffuseMapUV).a <= threshold)
        discard;
}

void main()
{
    vec4 color = vec4(0.0);

    if (pass == PASS_ATMOSPHERE)
        paintAtmosphere(color);
    else if (pass == PASS_ATMOSPHERE_NIGHT)
        paintAtmosphereNight(color);
    else if (pass == PASS_CLOUDS)
        paintClouds(color);
    else if (pass == PASS_MOON)
        paintMoon(color);
    else if (pass == PASS_SUN)
        paintSun(color);
    else if (pass == PASS_SUNGLARE)
        paintSunglare(color);
    else if (pass == PASS_SUNFLASH_QUERY)
    {
        processSunflashQuery();
        return;
    }

    gl_FragData[0] = color;
}

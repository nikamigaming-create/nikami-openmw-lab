#if @skyBlending
#include "lib/core/fragment.h.glsl"

uniform float skyBlendingStart;
#endif

uniform bool falloutFogEnabled;
uniform float falloutFogPower;
uniform bool falloutFogStep;

vec4 applyFogAtDist(vec4 color, float euclideanDist, float linearDist, float far)
{
#if @radialFog
    float dist = euclideanDist;
#else
    float dist = abs(linearDist);
#endif

    if (falloutFogEnabled)
        dist = euclideanDist;

    float fogValue;
    if (falloutFogEnabled)
    {
        if (falloutFogStep)
            fogValue = dist > gl_Fog.start ? 1.0 : 0.0;
        else
        {
            float normalizedDistance = clamp((dist - gl_Fog.start) / (gl_Fog.end - gl_Fog.start), 0.0, 1.0);
            fogValue = pow(normalizedDistance, falloutFogPower);
        }
    }
    else
    {
#if @exponentialFog
        fogValue = 1.0 - exp(-2.0 * max(0.0, dist - gl_Fog.start/2.0) / (gl_Fog.end - gl_Fog.start/2.0));
#else
        fogValue = clamp((dist - gl_Fog.start) * gl_Fog.scale, 0.0, 1.0);
#endif
    }
#ifdef ADDITIVE_BLENDING
    color.xyz *= 1.0 - fogValue;
#else
    color.xyz = mix(color.xyz, gl_Fog.color.xyz, fogValue);
#endif

#if @skyBlending
    float fadeValue = clamp((far - dist) / (far - skyBlendingStart), 0.0, 1.0);
    fadeValue *= fadeValue;
#ifdef ADDITIVE_BLENDING
    color.xyz *= fadeValue;
#else
    color.xyz = mix(sampleSkyColor(gl_FragCoord.xy / screenRes), color.xyz, fadeValue);
#endif
#endif

    return color;
}

vec4 applyFogAtPos(vec4 color, vec3 pos, float far)
{
    return applyFogAtDist(color, length(pos), pos.z, far);
}

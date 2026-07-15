#version 120

#include "lib/core/vertex.h.glsl"

#include "lib/sky/passes.glsl"

uniform int pass;
uniform bool useFalloutCloudShader; // PASS_CLOUDS
uniform vec4 falloutCloudColor; // PASS_CLOUDS
uniform vec4 falloutCloudSkyLowerColor; // PASS_CLOUDS
uniform vec4 falloutCloudSkyUpperColor; // PASS_CLOUDS
uniform int useFalloutAtmosphereZGradient; // PASS_ATMOSPHERE
uniform vec2 falloutAtmosphereZRange;      // PASS_ATMOSPHERE

varying vec4 passColor;
varying vec2 diffuseMapUV;

void main()
{
    gl_Position = modelToClip(gl_Vertex);
    passColor = gl_Color;
    if (pass == PASS_ATMOSPHERE && useFalloutAtmosphereZGradient != 0)
    {
        float zRange = max(falloutAtmosphereZRange.y - falloutAtmosphereZRange.x, 0.0001);
        passColor.a = clamp((gl_Vertex.z - falloutAtmosphereZRange.x) / zRange, 0.0, 1.0);
    }

    if (pass == PASS_CLOUDS)
    {
        diffuseMapUV = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
        if (useFalloutCloudShader)
        {
            // Byte-equivalent to SKYCLOUDS.vso BlendColor[0..2]. clouds.nif stores the three blend weights in
            // vertex RGB and the authored radial/horizon fade in vertex alpha.
            passColor.rgb = falloutCloudColor.rgb * gl_Color.r
                + falloutCloudSkyLowerColor.rgb * gl_Color.g
                + falloutCloudSkyUpperColor.rgb * gl_Color.b;
            passColor.a = gl_Color.a * falloutCloudColor.a;
        }
    }
    else
        diffuseMapUV = gl_MultiTexCoord0.xy;
}

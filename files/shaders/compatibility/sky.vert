#version 120

#include "lib/core/vertex.h.glsl"

#include "lib/sky/passes.glsl"

uniform int pass;
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
        diffuseMapUV = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
    else
        diffuseMapUV = gl_MultiTexCoord0.xy;
}

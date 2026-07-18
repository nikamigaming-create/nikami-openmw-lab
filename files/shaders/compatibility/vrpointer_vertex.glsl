#version 120

#include "lib/core/vertex.h.glsl"

varying vec4 pointerColor;

void main()
{
    gl_Position = modelToClip(gl_Vertex);
    pointerColor = gl_Color;
}

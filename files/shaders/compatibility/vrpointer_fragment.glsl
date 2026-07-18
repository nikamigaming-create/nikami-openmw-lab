#version 120

varying vec4 pointerColor;

void main()
{
    gl_FragColor = pointerColor;
    if (gl_FragColor.a == 0.0)
        discard;
}

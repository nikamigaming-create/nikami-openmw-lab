#version 120

#if @useOVR_multiview
#extension GL_OVR_multiview : require
#extension GL_OVR_multiview2 : require
#endif

uniform sampler2D diffuseMap;
#if @useOVR_multiview
uniform sampler2D diffuseMapRight;
#endif

varying vec2 diffuseMapUV;

void main()
{
#if @useOVR_multiview
    if (gl_ViewID_OVR == 0)
        gl_FragColor = texture2D(diffuseMap, diffuseMapUV);
    else
        gl_FragColor = texture2D(diffuseMapRight, diffuseMapUV);
#else
    gl_FragColor = texture2D(diffuseMap, diffuseMapUV);
#endif
    if(gl_FragColor.a == 0.0) discard;
}

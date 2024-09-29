#version 120

uniform sampler2D albedo;

varying vec2 v_texcoord;

void main()
{
    gl_FragColor = texture2D(albedo, v_texcoord);
    gl_FragDepth = gl_FragCoord.z;
}
#version 130

uniform vec4 u_code;

void main()
{
    gl_FragColor = u_code;
    gl_FragDepth = gl_FragCoord.z;
}
#version 120

void main()
{
    gl_FragColor = vec4(1);
    gl_FragDepth = gl_FragCoord.z;
}
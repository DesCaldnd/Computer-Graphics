#version 120

void main()
{
    gl_FragColor = vec4(gl_Color.xyz * gl_FragCoord.z, gl_Color.z);
    gl_FragDepth = gl_FragCoord.z;
}
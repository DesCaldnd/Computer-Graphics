#version 120

uniform bool u_use_diffuse_map;
uniform vec3 u_diffuse_color;
uniform sampler2D u_diffuse_map;

varying vec2 v_texcoord;

void main()
{
    if (u_use_diffuse_map)
        gl_FragColor = texture2D(u_diffuse_map, v_texcoord);
    else
        gl_FragColor = vec4(u_diffuse_color, 1);
    gl_FragDepth = gl_FragCoord.z;
}
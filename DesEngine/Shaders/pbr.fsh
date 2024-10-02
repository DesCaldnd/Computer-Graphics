#version 120

uniform bool u_use_diffuse_map;
uniform vec3 u_diffuse_color;
uniform sampler2D u_diffuse_map;

uniform bool u_use_ambient_map;
uniform vec3 u_ambient_color;
uniform sampler2D u_ambient_map;

uniform bool u_use_specular_map;
uniform vec3 u_specular_color;
uniform sampler2D u_specular_map;

uniform bool u_use_specexp_map;
uniform vec3 u_specexp_color;
uniform sampler2D u_specexp_map;

uniform bool u_use_normal_map;
uniform sampler2D u_normal_map;

uniform float u_d;

varying vec2 v_texcoord;

void main()
{
    if (u_use_diffuse_map)
        gl_FragColor = texture2D(u_diffuse_map, v_texcoord);
    else
        gl_FragColor = vec4(u_diffuse_color, u_d);
    gl_FragDepth = gl_FragCoord.z;
}
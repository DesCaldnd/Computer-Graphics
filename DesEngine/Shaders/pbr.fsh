#version 120

uniform highp mat4 view;

//Light

uniform vec3 u_light_pos;
uniform float u_light_power;

//In screen coordinates
uniform vec3 u_eye_pos = vec3(0, 0, 0);

//Textures

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
uniform float u_specexp_value;
uniform sampler2D u_specexp_map;

uniform bool u_use_normal_map;
uniform sampler2D u_normal_map;

uniform float u_d;

varying vec2 v_texcoord;
varying vec3 v_normal;
varying vec3 v_position;

varying mat3 v_tbn_mat;

vec4 dumb_multiply(vec4 a, vec4 b)
{
    return vec4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}


void main()
{
    vec4 result_color = vec4(0, 0, 0, 0);
    vec4 diffuse_mat_color;

    if (u_use_diffuse_map)
        diffuse_mat_color = texture2D(u_diffuse_map, v_texcoord);
    else
        diffuse_mat_color = vec4(u_diffuse_color, u_d);


    vec3 using_normal = v_normal;

    if (u_use_normal_map)
        using_normal = normalize(texture2D(u_normal_map, v_texcoord).rgb * 2.0 - 1);

    vec3 light_pos = vec3(view * vec4(u_light_pos, 1));

    vec3 eye_vec = normalize(v_position - u_eye_pos);
    vec3 light_vec = normalize(v_position - light_pos);

    if (u_use_normal_map)
    {
        eye_vec = normalize(v_tbn_mat * eye_vec);
        light_vec = normalize(v_tbn_mat * light_vec);
    }

    vec3 reflect_light = normalize(reflect(light_vec, using_normal));
    float len = length(v_position - u_eye_pos) + length(v_position - light_pos);
    len /= 5;

    float spec_exp;

    if (u_use_specexp_map)
        spec_exp = texture2D(u_specexp_map, v_texcoord).x;
    else
        spec_exp = u_specexp_value;

    vec4 ambient;

    if (u_use_ambient_map)
        ambient = texture2D(u_ambient_map, v_texcoord);
    else
        ambient = vec4(u_ambient_color, u_d);

    vec4 specular_color;

    if (u_use_specular_map)
        specular_color = texture2D(u_specular_map, v_texcoord);
    else
        specular_color = vec4(u_specular_color, u_d);


    vec4 diffuse_color = diffuse_mat_color * u_light_power * max(0.0, dot(using_normal, -light_vec)) / (1 + 0.25 * pow(len, 2));

    result_color += diffuse_color;


    vec4 ambient_color = dumb_multiply(ambient, diffuse_mat_color);
    result_color += ambient_color;

    specular_color *= u_light_power * pow(max(0.0, dot(reflect_light, -eye_vec)), spec_exp) / (1 + 0.25 * pow(len, 2));
    result_color += specular_color;

    gl_FragColor = result_color;

    gl_FragDepth = gl_FragCoord.z;

    //----------------------------------------------
}
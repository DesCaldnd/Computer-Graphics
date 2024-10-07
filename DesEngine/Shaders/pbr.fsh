#version 120

uniform highp mat4 view;

//Light

uniform float u_light_power;
uniform sampler2D u_shadow_map;

//In screen coordinates
//uniform vec3 u_eye_pos = vec3(0, 0, 0);

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

varying vec4 v_light_direction;
varying vec4 v_position_light;

varying mat3 v_tbn_mat;

vec4 dumb_multiply(vec4 a, vec4 b)
{
    return vec4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

float sample_shadow_map(sampler2D map, vec2 coords, float compare)
{
    vec4 v = texture2D(map, coords);

    float value = v.x * 255.0 + (v.y * 255.0 + (v.z * 255.0 + v.w) / 255.0) / 255.0;
    return step(compare, value);
}

float sample_shadow_map_linear(sampler2D map, vec2 coords, float compare, vec2 texel_size)
{
    vec2 pixel_pos = coords / texel_size + 0.5;
    vec2 fract_part = fract(pixel_pos);
    vec2 start_texel = (pixel_pos - fract_part) * texel_size;

    float bl_texel = sample_shadow_map(map, start_texel, compare);
    float br_texel = sample_shadow_map(map, start_texel + vec2(texel_size.x, 0), compare);
    float tl_texel = sample_shadow_map(map, start_texel + vec2(0, texel_size.y), compare);
    float tr_texel = sample_shadow_map(map, start_texel + texel_size, compare);

    float mix_a = mix(bl_texel, tl_texel, fract_part.y);
    float mix_b = mix(br_texel, tr_texel, fract_part.y);

    return mix(mix_a, mix_b, fract_part.x);
}

float sample_shadow_map_pcf(sampler2D map, vec2 coords, float compare, vec2 texel_size)
{
    float result = 0;

    for(float y = -1; y <= 1; y += 1)
    {
        for (float x = -1; x <= 1; x += 1)
        {
            vec2 offset = vec2(x, y) * texel_size;
            result += sample_shadow_map_linear(map, coords + offset, compare, texel_size);
        }
    }

    return result / 9;
}

float calc_shadow_amount(sampler2D map, vec4 initial_shadow_coords)
{
    vec3 tmp = initial_shadow_coords.xyz / initial_shadow_coords.w;
    tmp = tmp * vec3(0.5) + vec3(0.5);

    float offset = dot(v_normal, v_light_direction.xyz);

    if (offset <= 0)
        return sample_shadow_map_pcf(u_shadow_map, tmp.xy, tmp.z * 255.0 - 0.6, vec2(1.0 / 4096));
    else
        return 0;
}

void main()
{
    vec3 eye_pos = vec3(0, 0, 0);

    float shadow_coeff = calc_shadow_amount(u_shadow_map, v_position_light);

    vec4 result_color = vec4(0, 0, 0, 0);
    vec4 diffuse_mat_color;

    if (u_use_diffuse_map)
        diffuse_mat_color = texture2D(u_diffuse_map, v_texcoord);
    else
        diffuse_mat_color = vec4(u_diffuse_color, u_d);


    vec3 using_normal = v_normal;

    if (u_use_normal_map)
        using_normal = normalize(texture2D(u_normal_map, v_texcoord).rgb * 2.0 - 1);

    vec3 eye_vec = normalize(v_position - eye_pos);
    vec3 light_vec = normalize(v_light_direction.xyz);

    if (u_use_normal_map)
    {
        eye_vec = normalize(v_tbn_mat * eye_vec);
        light_vec = normalize(v_tbn_mat * light_vec);
    }

    vec3 reflect_light = normalize(reflect(light_vec, using_normal));
    float len = length(v_position - eye_pos);

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


    vec4 diffuse_color = diffuse_mat_color * u_light_power * max(0.0, dot(using_normal, -light_vec)) /*/ (1 + 0.25 * pow(len, 2))*/;

    result_color += diffuse_color * shadow_coeff;


    vec4 ambient_color = dumb_multiply(ambient, diffuse_mat_color);
    result_color += ambient_color;

    specular_color *= u_light_power * pow(max(0.0, dot(reflect_light, -eye_vec)), spec_exp) /*/ (1 + 0.25 * pow(len, 2))*/;
    result_color += specular_color * shadow_coeff;


    gl_FragColor = result_color;

    gl_FragDepth = gl_FragCoord.z;

    //----------------------------------------------
}
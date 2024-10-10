#version 130
#define Directional 1
#define Point 2
#define Spot 3

#define PI 3.1415926535

uniform highp mat4 view;

//Light

uniform int u_light_count;
vec2 u_shadow_map_size = vec2(4096, 4096);

struct light
{
    float light_power;
    float cutoff;
    float soft;
    sampler2D shadow_map;
    samplerCube shadow_map_cube;
    int type;
    vec3 diffuse_color;
    vec3 specular_color;
};

uniform light u_lights[5];

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

in vec2 v_texcoord;
in vec3 v_normal;
in vec3 v_position;
in vec3 v_position_no_view;

in vec4 v_light_direction[5];
in vec4 v_position_light[5];
in vec4 v_light_position[5];
in vec4 v_light_position_no_view[5];

in mat3 v_tbn_mat;

vec4 dumb_multiply(vec4 a, vec4 b)
{
    return vec4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

float sample_shadow_map(sampler2D map, samplerCube map_c, int type, vec2 coords, vec3 coords_3, float compare)
{
    vec4 v;

    if (type != Point)
    {
        v = texture2D(map, coords);
    } else
    {
        v = texture(map_c, coords_3);
    }

    float value = v.x * 255.0 + (v.y * 255.0 + (v.z * 255.0 + v.w) / 255.0) / 255.0;
    return step(compare, value);
//    return length(coords_3);
}

float sample_shadow_map_linear(sampler2D map, samplerCube map_c, int type, vec2 coords, vec3 coords_3, float compare, vec2 texel_size)
{
    vec2 pixel_pos = coords / texel_size + 0.5;
    vec2 fract_part = fract(pixel_pos);
    vec2 start_texel = (pixel_pos - fract_part) * texel_size;

    float bl_texel = sample_shadow_map(map, map_c, type, start_texel, coords_3, compare);
    float br_texel = sample_shadow_map(map, map_c, type, start_texel + vec2(texel_size.x, 0), coords_3, compare);
    float tl_texel = sample_shadow_map(map, map_c, type, start_texel + vec2(0, texel_size.y), coords_3, compare);
    float tr_texel = sample_shadow_map(map, map_c, type, start_texel + texel_size, coords_3, compare);

    float mix_a = mix(bl_texel, tl_texel, fract_part.y);
    float mix_b = mix(br_texel, tr_texel, fract_part.y);

    return mix(mix_a, mix_b, fract_part.x);
}

float sample_shadow_map_pcf(sampler2D map, samplerCube map_c, int type, vec2 coords, vec3 coords_3, float compare, vec2 texel_size)
{
    float result = 0;

    for(float y = -1; y <= 1; y += 1)
    {
        for (float x = -1; x <= 1; x += 1)
        {
            vec2 offset = vec2(x, y) * texel_size;
            result += sample_shadow_map_linear(map, map_c, type, coords + offset, coords_3 + vec3(offset, 0), compare, texel_size);
        }
    }

    return result / 9;
}

float calc_shadow_amount(sampler2D map, samplerCube map_c, int type, vec4 initial_shadow_coords, vec3 coords_3, vec3 light_vec)
{
    vec3 tmp = initial_shadow_coords.xyz / initial_shadow_coords.w;
    tmp = tmp * vec3(0.5) + vec3(0.5);

    float offset = dot(v_normal, light_vec);

    if (offset <= 0)
        return sample_shadow_map_pcf(map, map_c, type, tmp.xy, coords_3, tmp.z * 255.0 - 0.6, vec2(1.0 / u_shadow_map_size));
    else
        return 0;
}

void main()
{
    vec3 eye_pos = vec3(0, 0, 0);

    // Get colors or maps
    vec4 diffuse_mat_color;

    if (u_use_diffuse_map)
        diffuse_mat_color = texture2D(u_diffuse_map, v_texcoord);
    else
        diffuse_mat_color = vec4(u_diffuse_color, u_d);

    vec3 using_normal = v_normal;

    if (u_use_normal_map)
        using_normal = normalize(texture2D(u_normal_map, v_texcoord).rgb * 2.0 - 1);

    vec3 eye_vec = normalize(v_position - eye_pos);

    if (u_use_normal_map)
    {
        eye_vec = normalize(v_tbn_mat * eye_vec);
    }

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

    vec4 result_color = vec4(0, 0, 0, 0);


//    float v;

    for (int i = 0; i < u_light_count; ++i)
    {
        if (u_lights[i].type == Point)
            continue;

        vec3 light_vec = normalize(v_light_direction[i].xyz);

        float shadow_mult = 1;

        if (u_lights[i].type != Directional)
        {
            light_vec = normalize(v_position - v_light_position[i].xyz);

            if (u_lights[i].type == Spot)
            {
                float angle = acos(dot(v_light_direction[i].xyz, light_vec)) * 180.0 / PI;
                if (angle > u_lights[i].cutoff)
                    shadow_mult = 0;
                else if (angle > u_lights[i].soft)
                {
                    shadow_mult = pow(1 - (angle - u_lights[i].soft) / (u_lights[i].cutoff - u_lights[i].soft), 2);
                }
            }
        }

        float shadow_coeff = calc_shadow_amount(u_lights[i].shadow_map, u_lights[i].shadow_map_cube, u_lights[i].type, v_position_light[i], v_position_no_view - v_light_position_no_view[i].xyz, light_vec) * shadow_mult;

        if (u_use_normal_map)
        {
            light_vec = normalize(v_tbn_mat * light_vec);
        }

        vec3 reflect_light = normalize(reflect(light_vec, using_normal));
        float len = length(v_light_position[i].xyz - v_position);

        vec4 diffuse_color = diffuse_mat_color * u_lights[i].light_power * max(0.0, dot(using_normal, -light_vec));
        diffuse_color = dumb_multiply(diffuse_color, vec4(u_lights[i].diffuse_color, 1));

        if (u_lights[i].type != Directional)
        {
            diffuse_color /= (1 + 0.25 * pow(len, 2));
        }

//      (1 + 0.25 * pow(len, 2))
        result_color += diffuse_color * shadow_coeff;

        vec4 _specular_color = specular_color * u_lights[i].light_power * pow(max(0.0, dot(reflect_light, -eye_vec)), spec_exp);
        _specular_color = dumb_multiply(_specular_color, vec4(u_lights[i].specular_color, 1));

        if (u_lights[i].type != Directional)
        {
            _specular_color /= (1 + 0.25 * pow(len, 2));
        }

        result_color += _specular_color * shadow_coeff;

        result_color = max(vec4(0, 0, 0, 0), result_color);

//        v = length(v_position_no_view - v_light_position_no_view[i].xyz) / 20;
    }


    vec4 ambient_color = dumb_multiply(ambient, diffuse_mat_color);
    result_color += ambient_color;

    gl_FragColor = result_color;
//    gl_FragColor = vec4(v, v, v, 1);

    gl_FragDepth = gl_FragCoord.z;

    //----------------------------------------------
}
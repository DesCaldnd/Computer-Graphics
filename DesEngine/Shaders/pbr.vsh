#version 120

attribute highp vec3 a_position;
attribute highp vec2 a_uv;
attribute highp vec3 a_normal;
attribute highp vec3 a_tangent;
attribute highp vec3 a_bitangent;
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;


uniform highp mat4 proj_light;
uniform highp mat4 shadow_light;
uniform highp mat4 light;

uniform vec3 u_light_direction;

varying vec2 v_texcoord;
varying vec3 v_normal;
varying vec3 v_position;

varying mat3 v_tbn_mat;
varying vec4 v_light_direction;
varying vec4 v_position_light;

mat3 transpose(in mat3 mat)
{
    vec3 i0 = mat[0];
    vec3 i1 = mat[1];
    vec3 i2 = mat[2];

    mat3 out_mat = mat3(
        vec3(i0.x, i1.x, i2.x),
        vec3(i0.y, i1.y, i2.y),
        vec3(i0.z, i1.z, i2.z)
    );

    return out_mat;
}

void main()
{
    mat4 mv_mat = view * model;

    gl_Position = proj * mv_mat * vec4(a_position, 1);
    v_texcoord = a_uv;

    v_normal = normalize(vec3(mv_mat * vec4(a_normal, 0)));

    v_position = vec3(mv_mat * vec4(a_position, 1));

    vec3 tangent = normalize(vec3(mv_mat * vec4(a_tangent, 0)));
    vec3 bitangent = normalize(vec3(mv_mat * vec4(a_bitangent, 0)));
    vec3 normal = normalize(vec3(mv_mat * vec4(a_normal, 0)));

    v_tbn_mat = transpose(mat3(tangent, bitangent, normal));

    v_light_direction = view * light * vec4(u_light_direction, 0);
    v_position_light = proj_light * shadow_light * model * vec4(a_position, 1);
 }
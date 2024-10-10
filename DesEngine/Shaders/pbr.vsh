#version 130

// Attributes
in highp vec3 a_position;
in highp vec2 a_uv;
in highp vec3 a_normal;
in highp vec3 a_tangent;
in highp vec3 a_bitangent;

// MVP
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;

// Lights
uniform int u_light_count;

uniform highp mat4 proj_light[5];
uniform highp mat4 shadow_light[5];
uniform highp mat4 light[5];

uniform vec3 u_light_direction = vec3(0, 0, -1);

// UV, Normal, Position
out vec2 v_texcoord;
out vec3 v_normal;
out vec3 v_position;

out mat3 v_tbn_mat;
out vec4 v_light_direction[5];
out vec4 v_position_light[5];
out vec4 v_light_position[5];

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


    for(int i = 0; i < u_light_count; ++i)
    {
        v_light_direction[i] = normalize(view * light[i] * vec4(u_light_direction, 0));
        v_position_light[i] = proj_light[i] * shadow_light[i] * model * vec4(a_position, 1);
        v_light_position[i] = view * light[i] * vec4(0, 0, 0, 1);
    }

 }
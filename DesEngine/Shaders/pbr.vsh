#version 120

attribute highp vec3 a_position;
attribute highp vec2 a_uv;
attribute highp vec3 a_normal;
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;


varying vec2 v_texcoord;
varying vec3 v_normal;
varying vec3 v_position;

void main()
{
    mat4 mv_mat = view * model;

    gl_Position = proj * mv_mat * vec4(a_position, 1);
    v_texcoord = a_uv;

    v_normal = normalize(vec3(mv_mat * vec4(a_normal, 0)));

    v_position = vec3(mv_mat * vec4(a_position, 1));
}
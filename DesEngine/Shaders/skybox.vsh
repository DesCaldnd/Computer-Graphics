#version 120

attribute highp vec3 a_position;
attribute highp vec2 a_uv;
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;

varying vec2 v_texcoord;

void main()
{
    mat4 mview = view;
    mview[3][0] = 0;
    mview[3][1] = 0;
    mview[3][2] = 0;
    gl_Position = proj * mview * model * vec4(a_position, 1);
    v_texcoord = a_uv;
}
#version 120

attribute highp vec3 a_position;
uniform highp mat4 proj_light;
uniform highp mat4 shadow_light;
uniform highp mat4 model;

varying vec4 v_position;

void main()
{

    v_position = proj_light * shadow_light * model * vec4(a_position, 1);
    gl_Position = v_position;
}
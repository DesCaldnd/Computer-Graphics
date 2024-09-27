#version 120

attribute highp vec3 a_position;
attribute highp vec2 a_uv;
attribute highp vec3 a_normal;
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;

void main()
{
//    gl_Position = proj * view * model * vec4(a_position, 1);
//    gl_FrontColor = vec4(1);
}
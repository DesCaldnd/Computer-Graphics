#version 120

attribute highp vec3 a_position;
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;

void main()
{
    gl_Position = proj * view * model * vec4(a_position, 1);
//    gl_Position = vec4(a_position, 1);
}
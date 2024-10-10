#version 330 core

in highp vec3 a_position;

uniform highp mat4 model;

void main()
{
    gl_Position = model * vec4(a_position, 1);
}
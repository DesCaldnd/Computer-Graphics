#version 130

// MVP
uniform highp mat4 proj;
uniform highp mat4 view;
uniform highp mat4 model;

// Attributes
in highp vec3 a_position;

void main()
{
    gl_Position = proj * view * model * vec4(a_position, 1);
}
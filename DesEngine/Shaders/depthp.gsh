#version 330 core
layout (triangles) in;
layout (triangle_strip, max_vertices=18) out;

//uniform mat4 shadowMatrices[6];
uniform highp mat4 proj_light;
uniform highp mat4 shadow_light[6];

out vec4 v_position; // FragPos from GS (output per emitvertex)

void main()
{
    for(int face = 0; face < 6; ++face)
    {
        gl_Layer = face; // built-in variable that specifies to which face we render.
        for(int i = 0; i < 3; ++i) // for each triangle vertex
        {
            v_position = proj_light * shadow_light[face] * gl_in[i].gl_Position;
            gl_Position = v_position;
            EmitVertex();
        }
        EndPrimitive();
    }
}
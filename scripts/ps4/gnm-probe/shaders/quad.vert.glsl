// Textured quad: vertices are pulled from a storage buffer by gl_VertexIndex
// (opengnm-psbc can't fetch vertex attributes without a fetch shader),
// position is transformed with values from a uniform buffer.
#version 450

layout(binding = 0) uniform Transform {
	vec4 scaleoffset;
} t;

layout(std430, binding = 1) readonly buffer Vertices {
	vec4 v[]; // xy - position, zw - texture coordinates
} verts;

layout(location = 0) out vec2 uv;

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	vec4 d = verts.v[gl_VertexIndex];
	gl_Position = vec4(d.xy * t.scaleoffset.xy + t.scaleoffset.zw, 0.0, 1.0);
	uv = d.zw;
}

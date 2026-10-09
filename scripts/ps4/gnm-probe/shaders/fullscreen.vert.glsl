// Fullscreen triangle generated from vertex index, replaces firmware embedded
// fullscreen VS: opengnm references its code at a firmware address taken from
// FW 9.00, which is different on newer firmware and makes the GPU fault.
#version 450

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}

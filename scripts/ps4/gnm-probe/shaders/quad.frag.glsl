// Texture modulated by a color from a uniform buffer
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outcol;

layout(binding = 0) uniform Constants {
	vec4 color;
} c;

layout(binding = 1) uniform sampler2D tex;

void main() {
	outcol = texture(tex, uv) * c.color;
}

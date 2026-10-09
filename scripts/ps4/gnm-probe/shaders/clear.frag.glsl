// Constant color clear. Shaders that read resources fault on the GPU for now:
// opengnm-psbc compiles with an empty pipeline layout, so descriptor loads
// become loads through a null descriptor (address 0).
#version 450

layout(location = 0) out vec4 outcol;

void main() {
	outcol = vec4(0.1, 0.2, 0.4, 1.0);
}

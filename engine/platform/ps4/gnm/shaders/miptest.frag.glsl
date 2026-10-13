// -gnmmiptest: reads texels of every mip level with texelFetch, the texture
// memory holds dword indices, so the render target shows where the texture
// unit reads each level: row = level, columns = texels (0,0) (1,0) (0,1) (0,2)
#version 450

layout(binding = 1) uniform sampler2D uTex;

layout(location = 0) out vec4 oFragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	ivec2 c = ivec2( 0, 0 );

	if( p.x == 1 )
		c = ivec2( 1, 0 );
	else if( p.x == 2 )
		c = ivec2( 0, 1 );
	else if( p.x == 3 )
		c = ivec2( 0, 2 );

	oFragColor = texelFetch( uTex, c, p.y );
}

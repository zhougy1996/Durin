module VolumetricCloudComposite;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

[[vk::binding(0, 0)]] Texture2D<float4> SceneColorTexture;
[[vk::binding(1, 0)]] Texture2D<float4> CloudTexture;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

// Ping-pong composition preserves scene alpha and applies the frozen
// radiance/transmittance algebra in linear space.
[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target0
{
	int3 pixel = int3(int2(input.pos.xy), 0);
	float4 scene = SceneColorTexture.Load(pixel);
	float4 cloud = CloudTexture.Load(pixel);
	return float4(cloud.rgb + cloud.a * scene.rgb, scene.a);
}

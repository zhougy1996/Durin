struct SkyBoxVSOutput
{
    float4 Position : SV_Position;
    float2 ClipPosition : TEXCOORD;
};

struct SkyBoxUniform
{
    float4x4 ClipToWorld;
    float4x4 WorldToSky;
    float4 ViewPosition;
    float4 TintIntensity;
};

[[vk::binding(0, 0)]] TextureCube<float4> SkyTexture;
[[vk::binding(1, 0)]] SamplerState SkySampler;
[[vk::binding(2, 0)]] ConstantBuffer<SkyBoxUniform> Sky;

[shader("vertex")]
SkyBoxVSOutput VertexMain(uint vertexId : SV_VulkanVertexID)
{
    static const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    SkyBoxVSOutput output;
    output.Position = float4(positions[vertexId], 0.0, 1.0);
    output.ClipPosition = positions[vertexId];
    return output;
}

[shader("fragment")]
float4 FragmentMain(SkyBoxVSOutput input) : SV_Target
{
    float4 worldPositionH = mul(Sky.ClipToWorld, float4(input.ClipPosition, 1.0, 1.0));
    float3 worldPosition = worldPositionH.xyz / worldPositionH.w;
    // Subtracting the view position removes camera translation while retaining
    // camera rotation and the current projection's field of view.
    float3 worldDirection = normalize(worldPosition - Sky.ViewPosition.xyz);
    float3 sampleDirection = normalize(mul(Sky.WorldToSky, float4(worldDirection, 0.0)).xyz);
    float3 linearColor = SkyTexture.Sample(SkySampler, sampleDirection).rgb;
    return float4(linearColor * Sky.TintIntensity.rgb * Sky.TintIntensity.a, 1.0);
}

// Minimal StaticMesh shader.

struct VSInput
{
    float3 pos : POSITION;
};

struct VSOutput
{
    float4 pos : SV_Position;
};

struct TransformUniform
{
    float4x4 LocalToClip;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(input.pos, 1.0));
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return float4(0.95, 0.62, 0.22, 1.0);
}

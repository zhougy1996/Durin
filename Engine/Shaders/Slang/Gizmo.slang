struct VSInput
{
    float3 pos : POSITION;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

struct TransformUniform
{
    float4x4 LocalToClip;
    float4 Color;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(input.pos, 1.0));
    output.color = Transform.Color;
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return input.color;
}

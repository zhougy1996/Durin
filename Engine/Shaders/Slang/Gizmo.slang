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

struct LineVSInput
{
    float4 pos : POSITION;
    float4 color : COLOR;
};

struct LineVSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

struct LineStyleUniform
{
    float AlphaScale;
    float3 Padding;
};

// Module-level resources need distinct bindings so per-entry-point reflection can identify the one actually used.
[[vk::binding(1, 0)]] ConstantBuffer<LineStyleUniform> Style;

[shader("vertex")]
LineVSOutput LineVertexMain(LineVSInput input)
{
    LineVSOutput output;
    output.pos = input.pos;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 LineFragmentMain(LineVSOutput input) : SV_Target
{
    float4 color = input.color;
    color.a *= Style.AlphaScale;
    return color;
}

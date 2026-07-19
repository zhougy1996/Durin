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
    float2 pattern : TEXCOORD;
};

struct LineVSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
    float2 pattern : TEXCOORD;
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
    output.pattern = input.pattern;
    return output;
}

[shader("fragment")]
float4 LineFragmentMain(LineVSOutput input) : SV_Target
{
    if (input.pattern.y > 0.0 && fmod(input.pattern.x, input.pattern.y) > input.pattern.y * 0.5)
        discard;
    float4 color = input.color;
    color.a *= Style.AlphaScale;
    return color;
}

struct IconVSInput
{
    float4 pos : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct IconVSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct IconStyleUniform
{
    float AlphaScale;
    float3 Padding;
};

[[vk::binding(2, 0)]] Texture2D<float4> Atlas;
[[vk::binding(3, 0)]] SamplerState AtlasSampler;
[[vk::binding(4, 0)]] ConstantBuffer<IconStyleUniform> IconStyle;

[shader("vertex")]
IconVSOutput IconVertexMain(IconVSInput input)
{
    IconVSOutput output;
    output.pos = input.pos;
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 IconFragmentMain(IconVSOutput input) : SV_Target
{
    float coverage = Atlas.Sample(AtlasSampler, input.uv).a;
    if (coverage <= 0.01)
        discard;
    float4 color = input.color;
    color.a *= coverage * IconStyle.AlphaScale;
    return color;
}

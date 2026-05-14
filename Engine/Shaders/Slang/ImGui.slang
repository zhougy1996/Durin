// ImGui UI shader

struct VSInput
{
    float2 pos : POSITION;  // location = 0
    float2 uv  : TEXCOORD;  // location = 1
    float4 col : COLOR;     // location = 2
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
    float4 col : COLOR;
};

struct PushConstant {
    float2 uScale;
    float2 uTranslate;
};

[[vk::push_constant]] ConstantBuffer<PushConstant> pc;
[[vk::binding(0, 0)]] Texture2D<float4> fontTexture;
[[vk::binding(0, 1)]] SamplerState fontSampler;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos * pc.uScale + pc.uTranslate, 0.0, 1.0);
    output.uv  = input.uv;
    output.col = input.col;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    return input.col * fontTexture.Sample(fontSampler, input.uv);
}
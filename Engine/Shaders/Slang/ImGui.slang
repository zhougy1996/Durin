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

struct ProjectionUniform {
    float2 uScale;
    float2 uTranslate;
};

Texture2D<float4> FontTexture;
SamplerState FontSampler;
ConstantBuffer<ProjectionUniform> Projection;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos * Projection.uScale + Projection.uTranslate, 0.0, 1.0);
    output.uv  = input.uv;
    output.col = input.col;
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return input.col * FontTexture.Sample(FontSampler, input.uv);
}

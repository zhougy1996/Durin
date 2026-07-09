// Minimal fullscreen post-process shader.

struct VSInput
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

struct ViewUniform
{
    float2 InvRenderTargetSize;
    float2 Padding;
};

[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] SamplerState SceneColorSampler;
[[vk::binding(2, 0)]] ConstantBuffer<ViewUniform> View;

static float Luma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 CopyFragmentMain(VSOutput input) : SV_Target
{
    return SceneColor.Sample(SceneColorSampler, input.uv);
}

[shader("fragment")]
float4 FXAAFragmentMain(VSOutput input) : SV_Target
{
    float2 texel = View.InvRenderTargetSize;
    float3 rgbNW = SceneColor.Sample(SceneColorSampler, input.uv + texel * float2(-1.0, -1.0)).rgb;
    float3 rgbNE = SceneColor.Sample(SceneColorSampler, input.uv + texel * float2( 1.0, -1.0)).rgb;
    float3 rgbSW = SceneColor.Sample(SceneColorSampler, input.uv + texel * float2(-1.0,  1.0)).rgb;
    float3 rgbSE = SceneColor.Sample(SceneColorSampler, input.uv + texel * float2( 1.0,  1.0)).rgb;
    float4 center = SceneColor.Sample(SceneColorSampler, input.uv);

    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);
    float lumaM = Luma(center.rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float directionReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.03125), 0.0078125);
    float inverseDirectionAdjustment = 1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirectionAdjustment, float2(-8.0, -8.0), float2(8.0, 8.0)) * texel;

    float3 rgbA = 0.5 * (
        SceneColor.Sample(SceneColorSampler, input.uv + direction * (1.0 / 3.0 - 0.5)).rgb +
        SceneColor.Sample(SceneColorSampler, input.uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        SceneColor.Sample(SceneColorSampler, input.uv + direction * -0.5).rgb +
        SceneColor.Sample(SceneColorSampler, input.uv + direction * 0.5).rgb);

    float lumaB = Luma(rgbB);
    float3 resolved = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(resolved, center.a);
}

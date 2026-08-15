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
    float ExposureScale;
    float Padding;
};

[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] SamplerState SceneColorSampler;
[[vk::binding(2, 0)]] ConstantBuffer<ViewUniform> View;

static float Luma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

static float DisplayMapChannel(float sceneLinear)
{
    const float exposed = max(sceneLinear * View.ExposureScale, 0.0);
    const float mapped =
        (exposed * (2.51 * exposed + 0.03)) /
        (exposed * (2.43 * exposed + 0.59) + 0.14);
    return isfinite(mapped) ? saturate(mapped) : 0.0;
}

static float3 DisplayMap(float3 sceneLinear)
{
    return float3(
        DisplayMapChannel(sceneLinear.r),
        DisplayMapChannel(sceneLinear.g),
        DisplayMapChannel(sceneLinear.b));
}

static float DisplayAlpha(float sceneAlpha)
{
    return isfinite(sceneAlpha) ? saturate(sceneAlpha) : 0.0;
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
    const float4 center = SceneColor.Sample(SceneColorSampler, input.uv);
    return float4(DisplayMap(center.rgb), DisplayAlpha(center.a));
}

[shader("fragment")]
float4 FXAAFragmentMain(VSOutput input) : SV_Target
{
    float2 texel = View.InvRenderTargetSize;
    float3 rgbNW = DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + texel * float2(-1.0, -1.0)).rgb);
    float3 rgbNE = DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + texel * float2( 1.0, -1.0)).rgb);
    float3 rgbSW = DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + texel * float2(-1.0,  1.0)).rgb);
    float3 rgbSE = DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + texel * float2( 1.0,  1.0)).rgb);
    float4 center = SceneColor.Sample(SceneColorSampler, input.uv);
    float3 centerDisplay = DisplayMap(center.rgb);

    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);
    float lumaM = Luma(centerDisplay);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float directionReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.03125), 0.0078125);
    float inverseDirectionAdjustment = 1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirectionAdjustment, float2(-8.0, -8.0), float2(8.0, 8.0)) * texel;

    float3 rgbA = 0.5 * (
        DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + direction * (1.0 / 3.0 - 0.5)).rgb) +
        DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + direction * (2.0 / 3.0 - 0.5)).rgb));
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + direction * -0.5).rgb) +
        DisplayMap(SceneColor.Sample(SceneColorSampler, input.uv + direction * 0.5).rgb));

    float lumaB = Luma(rgbB);
    float3 resolved = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(resolved, DisplayAlpha(center.a));
}

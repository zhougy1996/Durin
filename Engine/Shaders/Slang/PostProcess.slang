// Minimal fullscreen post-process shader.

import Material.GBufferDecode;
import Material.SingleLayerWaterLighting;
import Lighting.DirectionalShadow;
import Lighting.ViewLightingUniform;

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
    float BloomIntensity;
    float4 BloomRect;
};

Texture2D<float4> SceneColor;
Texture2D<float4> Bloom;
Texture2D<float> WaterSnapshotDepth;
SamplerState SceneColorSampler;
ConstantBuffer<ViewUniform> View;

struct BloomPassUniform
{
    float4 SourceRect;
    float2 InvSourceSize;
    float ExposureScale;
    uint Stage;
};

Texture2D<float4> BloomSource;
Texture2D<float4> BloomDetail;
ConstantBuffer<BloomPassUniform> BloomPass;

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

static float3 DisplaySample(float3 sceneLinear, float2 uv)
{
    if (View.BloomIntensity > 0.0)
    {
        float2 bloomUV = (uv - View.BloomRect.xy) / View.BloomRect.zw;
        if (all(bloomUV >= 0.0) && all(bloomUV < 1.0))
            sceneLinear += Bloom.SampleLevel(SceneColorSampler, bloomUV, 0).rgb * View.BloomIntensity;
    }
    return DisplayMap(sceneLinear);
}

static float3 SampleDisplay(float2 uv)
{
    return DisplaySample(SceneColor.Sample(SceneColorSampler, uv).rgb, uv);
}

static float3 BloomTap(float2 uv, bool extract)
{
    const float2 halfTexel = BloomPass.InvSourceSize * 0.5;
    uv = clamp(uv, BloomPass.SourceRect.xy + halfTexel,
        max(BloomPass.SourceRect.xy + halfTexel, BloomPass.SourceRect.xy + BloomPass.SourceRect.zw - halfTexel));
    float3 color = BloomSource.SampleLevel(SceneColorSampler, uv, 0).rgb;
    if (!extract) return color;

    color = all(isfinite(color)) ? max(color, 0.0) : float3(0.0);
    const float brightness = dot(color, float3(0.2126, 0.7152, 0.0722)) * BloomPass.ExposureScale;
    const float knee = saturate(brightness - 0.5);
    const float contribution = max(brightness - 1.0, 0.5 * knee * knee);
    return color * (contribution / max(brightness, 1e-6));
}

[shader("fragment")]
float4 BloomFragmentMain(VSOutput input) : SV_Target
{
    const bool extractQuarter = BloomPass.Stage == 3;
    const bool extract = BloomPass.Stage == 0 || extractQuarter;
    const bool upsample = BloomPass.Stage == 2;
    const float2 uv = BloomPass.SourceRect.xy + input.uv * BloomPass.SourceRect.zw;
    // At 4:1, four bilinear taps cover the aligned 4x4 source footprint.
    const float tapRadius = extractQuarter ? 1.0 : (upsample ? 0.75 : 0.5);
    const float2 offset = BloomPass.InvSourceSize * tapRadius;
    float3 color = 0.25 * BloomTap(uv + float2(-offset.x, -offset.y), extract)
        + 0.25 * BloomTap(uv + float2(offset.x, -offset.y), extract)
        + 0.25 * BloomTap(uv + float2(-offset.x, offset.y), extract)
        + 0.25 * BloomTap(uv + float2(offset.x, offset.y), extract);
    if (upsample)
        color = lerp(BloomDetail.SampleLevel(SceneColorSampler, input.uv, 0).rgb, color, 0.65);
    return float4(color, 0.0);
}

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

struct FWaterSnapshotOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
};

[shader("fragment")]
FWaterSnapshotOutput WaterSnapshotFragmentMain(VSOutput input)
{
    int3 pixel = int3(int2(input.pos.xy), 0);
    FWaterSnapshotOutput result;
    result.color = SceneColor.Load(pixel);
    result.depth = WaterSnapshotDepth.Load(pixel);
    return result;
}

struct FWaterReflectionView
{
    float4x4 ViewToWorld;
    FWaterViewUniform Water;
    float4 Viewport;
};

Texture2D<float> WaterReflectionCoverage;
Texture2D<float> WaterReflectionDepth;
Texture2D<float4> WaterReflectionNormalRoughness;
Texture2D<float4> WaterReflectionSpecularF0;
TextureCube<float4> WaterReflectionEnvironment;
Texture2D<float4> WaterReflectionBrdfLut;
SamplerState WaterReflectionSampler;
ConstantBuffer<FWaterReflectionView> WaterReflectionView;
ConstantBuffer<FViewLightingUniform> WaterReflectionLighting;

[shader("fragment")]
float4 WaterReflectionFragmentMain(VSOutput input) : SV_Target0
{
    int3 pixel = int3(int2(input.pos.xy), 0);
    if (WaterReflectionCoverage.Load(pixel) < 0.5)
        discard;
    float2 ndc = (input.pos.xy - WaterReflectionView.Viewport.xy) / WaterReflectionView.Viewport.zw * 2.0 - 1.0;
    float3 viewPosition;
    if (!ReconstructViewPositionAnalytic(WaterReflectionView.Water.Projection[0], WaterReflectionView.Water.Projection[1],
            WaterReflectionView.Water.Projection[2], WaterReflectionView.Water.Projection[3], ndc,
            WaterReflectionDepth.Load(pixel), viewPosition))
        discard;
    float3 incident = WaterIncidentDirection(WaterReflectionView.Water, viewPosition, WaterViewForward(WaterReflectionView.Water));
    float3 toView = normalize(mul((float3x3)WaterReflectionView.ViewToWorld, -incident));
    float4 record = WaterReflectionNormalRoughness.Load(pixel);
    float3 normal = DecodeOctahedralNormal(record.xy);
    float roughness = record.z;
    float3 reflection = reflect(-toView, normal);
    float4 rotation = WaterReflectionLighting.EnvironmentRotation;
    reflection += 2.0 * cross(rotation.xyz, cross(rotation.xyz, reflection) + rotation.w * reflection);
    float2 brdf = WaterReflectionBrdfLut.SampleLevel(WaterReflectionSampler,
        float2(saturate(dot(normal, toView)), roughness), 0).rg;
    float3 sky = WaterReflectionEnvironment.SampleLevel(WaterReflectionSampler,
        reflection, roughness * WaterReflectionLighting.EnvironmentControl.y).rgb;
    float3 f0 = WaterReflectionSpecularF0.Load(pixel).rgb;
    return float4(sky * WaterReflectionLighting.EnvironmentControl.x * (f0 * brdf.x + brdf.y), 0.0);
}

[shader("fragment")]
float4 CopyFragmentMain(VSOutput input) : SV_Target
{
    const float4 center = SceneColor.Sample(SceneColorSampler, input.uv);
    return float4(DisplaySample(center.rgb, input.uv), DisplayAlpha(center.a));
}

[shader("fragment")]
float4 FXAAFragmentMain(VSOutput input) : SV_Target
{
    float2 texel = View.InvRenderTargetSize;
    float3 rgbNW = SampleDisplay(input.uv + texel * float2(-1.0, -1.0));
    float3 rgbNE = SampleDisplay(input.uv + texel * float2( 1.0, -1.0));
    float3 rgbSW = SampleDisplay(input.uv + texel * float2(-1.0,  1.0));
    float3 rgbSE = SampleDisplay(input.uv + texel * float2( 1.0,  1.0));
    float4 center = SceneColor.Sample(SceneColorSampler, input.uv);
    float3 centerDisplay = DisplaySample(center.rgb, input.uv);
    const float alpha = DisplayAlpha(center.a);

    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);
    float lumaM = Luma(centerDisplay);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    const float contrastThreshold = max(1.0 / 32.0, lumaMax * (1.0 / 8.0));
    if (lumaMax - lumaMin < contrastThreshold)
        return float4(centerDisplay, alpha);

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float directionReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.03125), 0.0078125);
    float inverseDirectionAdjustment = 1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirectionAdjustment, float2(-8.0, -8.0), float2(8.0, 8.0)) * texel;

    float3 rgbA = 0.5 * (
        SampleDisplay(input.uv + direction * (1.0 / 3.0 - 0.5)) +
        SampleDisplay(input.uv + direction * (2.0 / 3.0 - 0.5)));
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        SampleDisplay(input.uv + direction * -0.5) +
        SampleDisplay(input.uv + direction * 0.5));

    float lumaB = Luma(rgbB);
    float3 resolved = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(resolved, alpha);
}

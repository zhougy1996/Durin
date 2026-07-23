struct GridVSInput
{
    float2 pos : POSITION;
};

struct GridVSOutput
{
    float4 pos : SV_Position;
    float3 worldPosition : TEXCOORD;
};

struct GridFragmentOutput
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

struct GridUniform
{
    float4x4 WorldToClip;
    float4x4 ClipToWorld;
    // x is the horizontal grid plane's world-space height; yzw are reserved.
    float4 GridPlane;
    float4 ViewPositionFadeDistance;
    float4 MinorColor;
    float4 MajorColor;
    float4 AxisXColor;
    float4 AxisYColor;
};

[[vk::binding(0, 0)]] ConstantBuffer<GridUniform> Grid;

[shader("vertex")]
GridVSOutput VertexMain(GridVSInput input)
{
    GridVSOutput output;
    output.worldPosition = float3(
        Grid.ViewPositionFadeDistance.xy + input.pos * Grid.ViewPositionFadeDistance.w * 1.1,
        Grid.GridPlane.x
    );
    output.pos = mul(Grid.WorldToClip, float4(output.worldPosition, 1.0));
    return output;
}

float GridLine(float2 worldPosition, float spacing, float2 worldPositionWidth)
{
    float2 coordinate = worldPosition / spacing;
    float2 width = max(worldPositionWidth / spacing, float2(1.0e-5));
    float2 distanceToLine = abs(frac(coordinate - 0.5) - 0.5) / width;
    return 1.0 - min(min(distanceToLine.x, distanceToLine.y), 1.0);
}

float AxisLine(float coordinate)
{
    return 1.0 - min(abs(coordinate) / max(fwidth(coordinate) * 1.5, 1.0e-5), 1.0);
}

[shader("fragment")]
GridFragmentOutput FragmentMain(GridVSOutput input)
{
    float3 toView = Grid.ViewPositionFadeDistance.xyz - input.worldPosition;
    float distanceToView = length(toView);
    float fadeDistance = max(Grid.ViewPositionFadeDistance.w, 1.0);
    float distanceFade = 1.0 - smoothstep(fadeDistance * 0.55, fadeDistance, distanceToView);
    // A grid viewed edge-on aliases heavily and no longer conveys useful scale, so let it
    // disappear before the projected cells collapse into a noisy horizon band.
    float angleFade = smoothstep(0.025, 0.16, abs(toView.z) / max(distanceToView, 1.0e-5));

    float2 worldPositionDx = ddx(input.worldPosition.xy);
    float2 worldPositionDy = ddy(input.worldPosition.xy);
    // Evaluate derivatives before selecting the discrete decade spacing. Taking fwidth()
    // after that selection lets a 2x2 pixel quad straddle a 10x LOD jump and produces a
    // false sawtooth line along the transition boundary.
    float2 worldPositionWidth = max(abs(worldPositionDx) + abs(worldPositionDy), float2(1.0e-5));
    float worldUnitsPerPixel = max(length(worldPositionDx), length(worldPositionDy));
    float logarithmicSpacing = log10(max(worldUnitsPerPixel * 24.0, 1.0e-4));
    float lowerSpacing = pow(10.0, floor(logarithmicSpacing));
    float transition = smoothstep(0.15, 0.85, frac(logarithmicSpacing));

    float lowerLine = GridLine(input.worldPosition.xy, lowerSpacing, worldPositionWidth);
    float upperLine = GridLine(input.worldPosition.xy, lowerSpacing * 10.0, worldPositionWidth);
    float nextMajorLine = GridLine(input.worldPosition.xy, lowerSpacing * 100.0, worldPositionWidth);
    float minorAmount = lowerLine * (1.0 - transition);
    float upperMinorAmount = upperLine * transition;
    float majorAmount = upperLine * (1.0 - transition) + nextMajorLine * transition;

    float4 color = Grid.MinorColor;
    float neutralAlpha = max(
        (minorAmount + upperMinorAmount) * Grid.MinorColor.a,
        majorAmount * Grid.MajorColor.a
    );
    color.rgb = lerp(Grid.MinorColor.rgb, Grid.MajorColor.rgb, saturate(majorAmount));
    color.a = neutralAlpha;

    // The red X axis lies on y=0; the green Y axis lies on x=0.
    float xAxis = AxisLine(input.worldPosition.y);
    float yAxis = AxisLine(input.worldPosition.x);
    color.rgb = lerp(color.rgb, Grid.AxisXColor.rgb, xAxis);
    color.rgb = lerp(color.rgb, Grid.AxisYColor.rgb, yAxis);
    color.a = max(color.a, max(xAxis * Grid.AxisXColor.a, yAxis * Grid.AxisYColor.a));
    color.a *= distanceFade * angleFade;
    if (color.a <= 0.002)
        discard;

    GridFragmentOutput output;
    output.color = color;
    // Stage 0 contract spike: preserve the rasterized V1 grid depth while proving that
    // the real grid pipeline accepts a color output paired with fragment depth.
    output.depth = input.pos.z;
    return output;
}

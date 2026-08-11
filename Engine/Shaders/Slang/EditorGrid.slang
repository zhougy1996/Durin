struct GridVSInput
{
    float2 pos : POSITION;
};

struct GridVSOutput
{
    float4 pos : SV_Position;
    float2 clipPosition : TEXCOORD;
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
    // x is the horizontal grid plane's world-space height; y is the normalized
    // depth bias that keeps coplanar scene geometry in front; zw are reserved.
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
    output.pos = float4(input.pos, 0.0, 1.0);
    output.clipPosition = input.pos;
    return output;
}

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsFinite(float3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool IsFinite(float4 value)
{
    return !any(isnan(value)) && !any(isinf(value));
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
    float4 nearHomogeneous = mul(Grid.ClipToWorld, float4(input.clipPosition, 0.0, 1.0));
    float4 farHomogeneous = mul(Grid.ClipToWorld, float4(input.clipPosition, 1.0, 1.0));
    if (!IsFinite(nearHomogeneous) || !IsFinite(farHomogeneous)
        || abs(nearHomogeneous.w) <= 1.0e-6 || abs(farHomogeneous.w) <= 1.0e-6)
        discard;

    float3 nearPosition = nearHomogeneous.xyz / nearHomogeneous.w;
    float3 farPosition = farHomogeneous.xyz / farHomogeneous.w;
    float3 rayDelta = farPosition - nearPosition;
    float rayLength = length(rayDelta);
    if (!IsFinite(nearPosition) || !IsFinite(farPosition) || !IsFinite(rayLength) || rayLength <= 1.0e-6)
        discard;

    float3 rayDirection = rayDelta / rayLength;
    // The visible grazing fade reaches zero well before this numerical guard, so the
    // hard rejection cannot expose a horizon edge or a derivative-quad seam.
    if (!IsFinite(rayDirection) || abs(rayDirection.z) <= 1.0e-6)
        discard;

    float rayDistance = (Grid.GridPlane.x - nearPosition.z) / rayDirection.z;
    float fadeDistance = max(Grid.ViewPositionFadeDistance.w, 1.0);
    if (!IsFinite(rayDistance) || rayDistance < 0.0)
        discard;

    float3 worldPosition = nearPosition + rayDirection * rayDistance;
    float4 hitClip = mul(Grid.WorldToClip, float4(worldPosition, 1.0));
    if (!IsFinite(worldPosition) || !IsFinite(hitClip) || abs(hitClip.w) <= 1.0e-6)
        discard;

    float hitDepth = hitClip.z / hitClip.w;
    if (!IsFinite(hitDepth) || hitDepth < 0.0 || hitDepth > 1.0)
        discard;

    float3 toView = Grid.ViewPositionFadeDistance.xyz - worldPosition;
    float distanceToView = length(toView);
    // Reject only after the smooth distance fade has reached zero. Keeping this hard
    // bound outside the visible interval prevents an early clip edge near the far range.
    if (!IsFinite(distanceToView) || distanceToView >= fadeDistance)
        discard;

    float distanceFade = 1.0 - smoothstep(fadeDistance * 0.55, fadeDistance, distanceToView);
    // A grid viewed edge-on aliases heavily and no longer conveys useful scale, so let it
    // disappear before the projected cells collapse into a noisy horizon band.
    float angleFade = smoothstep(0.025, 0.16, abs(rayDirection.z));

    float2 worldPositionDx = ddx(worldPosition.xy);
    float2 worldPositionDy = ddy(worldPosition.xy);
    // Evaluate derivatives before selecting the discrete decade spacing. Taking fwidth()
    // after that selection lets a 2x2 pixel quad straddle a 10x LOD jump and produces a
    // false sawtooth line along the transition boundary.
    float2 worldPositionWidth = max(abs(worldPositionDx) + abs(worldPositionDy), float2(1.0e-5));
    float worldUnitsPerPixel = max(length(worldPositionDx), length(worldPositionDy));
    float logarithmicSpacing = log10(max(worldUnitsPerPixel * 24.0, 1.0e-4));
    float lowerSpacing = pow(10.0, floor(logarithmicSpacing));
    float transition = smoothstep(0.15, 0.85, frac(logarithmicSpacing));

    float lowerLine = GridLine(worldPosition.xy, lowerSpacing, worldPositionWidth);
    float upperLine = GridLine(worldPosition.xy, lowerSpacing * 10.0, worldPositionWidth);
    float nextMajorLine = GridLine(worldPosition.xy, lowerSpacing * 100.0, worldPositionWidth);
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
    float xAxis = AxisLine(worldPosition.y);
    float yAxis = AxisLine(worldPosition.x);
    color.rgb = lerp(color.rgb, Grid.AxisXColor.rgb, xAxis);
    color.rgb = lerp(color.rgb, Grid.AxisYColor.rgb, yAxis);
    color.a = max(color.a, max(xAxis * Grid.AxisXColor.a, yAxis * Grid.AxisYColor.a));
    color.a *= distanceFade * angleFade;
    if (color.a <= 0.002)
        discard;

    GridFragmentOutput output;
    output.color = color;
    output.depth = saturate(hitDepth + Grid.GridPlane.y);
    return output;
}

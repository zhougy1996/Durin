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
    float4x4 RelativeWorldToClip;
    float4x4 ClipToRelativeWorld;
    // x is the horizontal grid plane's height relative to the view origin; z selects
    // reversed-Z endpoint ordering; yw are reserved.
    float4 GridPlane;
    float4 ViewPositionFadeDistance;
    float4 GridPhases[13];
    float4 MinorColor;
    float4 MajorColor;
    float4 AxisXColor;
    float4 AxisYColor;
    // The world grid plane transformed into homogeneous clip space on the CPU.
    float4 ClipPlane;
};

[[vk::binding(0, 0)]] ConstantBuffer<GridUniform> Grid;

// Bounded to roughly 1.2e-4 relative depth for normalized positive floats. This
// absorbs independent plane/raster evaluation without turning the grid into X-ray.
static const uint CoplanarDepthPrioritySteps = 1024u;

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

float GridLine(
    float2 relativeWorldPosition,
    float spacing,
    float2 worldPositionWidth,
    float2 cameraPhase)
{
    float2 coordinate = relativeWorldPosition / spacing + cameraPhase;
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
    float nearDepth = Grid.GridPlane.z > 0.5 ? 1.0 : 0.0;
    // Any two device-depth samples define the same view ray. Avoid the far endpoint:
    // with a very long or infinite projection its unprojected magnitude loses enough
    // float precision to make a grazing plane intersection unstable while rotating.
    float raySampleDepth = 0.5;
    float4 nearHomogeneous = mul(
        Grid.ClipToRelativeWorld, float4(input.clipPosition, nearDepth, 1.0));
    float4 raySampleHomogeneous = mul(
        Grid.ClipToRelativeWorld, float4(input.clipPosition, raySampleDepth, 1.0));
    if (!IsFinite(nearHomogeneous) || !IsFinite(raySampleHomogeneous)
        || abs(nearHomogeneous.w) <= 1.0e-6 || abs(raySampleHomogeneous.w) <= 1.0e-6)
        discard;

    float3 nearPosition = nearHomogeneous.xyz / nearHomogeneous.w;
    float3 raySamplePosition = raySampleHomogeneous.xyz / raySampleHomogeneous.w;
    float3 rayDelta = raySamplePosition - nearPosition;
    float rayLength = length(rayDelta);
    if (!IsFinite(nearPosition) || !IsFinite(raySamplePosition)
        || !IsFinite(rayLength) || rayLength <= 1.0e-6)
        discard;

    float3 rayDirection = rayDelta / rayLength;
    // The visible grazing fade reaches zero well before this numerical guard, so the
    // hard rejection cannot expose a horizon edge or a derivative-quad seam.
    if (!IsFinite(rayDirection) || abs(rayDirection.z) <= 1.0e-6)
        discard;

    float fadeDistance = max(Grid.ViewPositionFadeDistance.w, 1.0);

    // Depth over a projected plane is affine in screen space. Solve the homogeneous
    // clip-plane equation directly so this value follows the same planar interpolation
    // as scene triangles instead of accumulating ray/intersection/reprojection error.
    if (!IsFinite(Grid.ClipPlane) || abs(Grid.ClipPlane.z) <= 1.0e-8)
        discard;
    float hitDepth = -(
        Grid.ClipPlane.x * input.clipPosition.x
        + Grid.ClipPlane.y * input.clipPosition.y
        + Grid.ClipPlane.w) / Grid.ClipPlane.z;
    if (!IsFinite(hitDepth) || hitDepth < 0.0 || hitDepth > 1.0)
        discard;

    float4 hitHomogeneous = mul(
        Grid.ClipToRelativeWorld, float4(input.clipPosition, hitDepth, 1.0));
    if (!IsFinite(hitHomogeneous) || abs(hitHomogeneous.w) <= 1.0e-6)
        discard;
    float3 relativeWorldPosition = hitHomogeneous.xyz / hitHomogeneous.w;
    if (!IsFinite(relativeWorldPosition))
        discard;

    float distanceToView = length(relativeWorldPosition);
    // Reject only after the smooth distance fade has reached zero. Keeping this hard
    // bound outside the visible interval prevents an early clip edge near the far range.
    if (!IsFinite(distanceToView) || distanceToView >= fadeDistance)
        discard;

    float distanceFade = 1.0 - smoothstep(fadeDistance * 0.55, fadeDistance, distanceToView);
    // A grid viewed edge-on aliases heavily and no longer conveys useful scale, so let it
    // disappear before the projected cells collapse into a noisy horizon band.
    float angleFade = smoothstep(0.025, 0.16, abs(rayDirection.z));

    float2 worldPositionDx = ddx(relativeWorldPosition.xy);
    float2 worldPositionDy = ddy(relativeWorldPosition.xy);
    // Evaluate derivatives before selecting the discrete decade spacing. Taking fwidth()
    // after that selection lets a 2x2 pixel quad straddle a 10x LOD jump and produces a
    // false sawtooth line along the transition boundary.
    float2 worldPositionWidth = max(abs(worldPositionDx) + abs(worldPositionDy), float2(1.0e-5));
    float worldUnitsPerPixel = max(length(worldPositionDx), length(worldPositionDy));
    float logarithmicSpacing = log10(max(worldUnitsPerPixel * 24.0, 1.0e-4));
    int lowerExponent = clamp((int)floor(logarithmicSpacing), -4, 6);
    float lowerSpacing = pow(10.0, (float)lowerExponent);
    float transition = smoothstep(0.15, 0.85, frac(logarithmicSpacing));

    int phaseIndex = lowerExponent + 4;
    float lowerLine = GridLine(
        relativeWorldPosition.xy, lowerSpacing, worldPositionWidth, Grid.GridPhases[phaseIndex].xy);
    float upperLine = GridLine(
        relativeWorldPosition.xy, lowerSpacing * 10.0, worldPositionWidth, Grid.GridPhases[phaseIndex + 1].xy);
    float nextMajorLine = GridLine(
        relativeWorldPosition.xy, lowerSpacing * 100.0, worldPositionWidth, Grid.GridPhases[phaseIndex + 2].xy);
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
    float2 worldPositionForAxes =
        relativeWorldPosition.xy + Grid.ViewPositionFadeDistance.xy;
    float xAxis = AxisLine(worldPositionForAxes.y);
    float yAxis = AxisLine(worldPositionForAxes.x);
    color.rgb = lerp(color.rgb, Grid.AxisXColor.rgb, xAxis);
    color.rgb = lerp(color.rgb, Grid.AxisYColor.rgb, yAxis);
    color.a = max(color.a, max(xAxis * Grid.AxisXColor.a, yAxis * Grid.AxisYColor.a));
    color.a *= distanceFade * angleFade;
    if (color.a <= 0.002)
        discard;

    GridFragmentOutput output;
    output.color = color;
    // Give the editor reference plane bounded coplanar priority without moving it in
    // world space. Reversed-Z grows toward the camera; forward-Z shrinks toward it.
    uint depthBits = asuint(hitDepth);
    if (Grid.GridPlane.z > 0.5)
    {
        output.depth = asfloat(min(
            depthBits + CoplanarDepthPrioritySteps, asuint(1.0)));
    }
    else
    {
        output.depth = asfloat(
            depthBits > CoplanarDepthPrioritySteps
                ? depthBits - CoplanarDepthPrioritySteps
                : 0u);
    }
    return output;
}

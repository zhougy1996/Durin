module SingleLayerWaterLighting;

import Material.GBufferDecode;

public struct FWaterViewUniform
{
    public float4 Projection[4];
    public float4 DepthParameters;
};

public struct FWaterBackground
{
    public float3 radiance;
    public float depth;
    public bool valid;
    public bool open;
};

public float3 WaterViewForward(FWaterViewUniform view)
{
    float3 perspectiveAxis = view.Projection[3].xyz;
    float3 orthographicAxis = view.Projection[2].xyz * (view.DepthParameters.x == 0.0 ? -1.0 : 1.0);
    return normalize(dot(perspectiveAxis, perspectiveAxis) > 1.0e-12 ? perspectiveAxis : orthographicAxis);
}

float2 WaterNdc(float2 pixel, float4 viewport)
{
    return (pixel - viewport.xy) / viewport.zw * 2.0 - 1.0;
}

public float2 ClampWaterPixel(float2 pixel, float4 viewport)
{
    float2 middle = viewport.xy + 0.5 * viewport.zw;
    return clamp(pixel, min(viewport.xy + 0.55, middle),
        max(viewport.xy + viewport.zw - 0.55, middle));
}

float WaterBackgroundLayerTolerance(float slopeTolerance)
{
    const float minLayerDepthTolerance = 0.001;
    const float layerDepthSlopeScale = 2.0;
    return max(minLayerDepthTolerance, layerDepthSlopeScale * slopeTolerance);
}

public FWaterBackground SampleWaterBackground(
    Texture2D<float4> sceneColor, Texture2D<float> sceneDepth, Texture2D<float> waterCoverage,
    FWaterViewUniform view, float4 viewport, float2 samplePixel,
    float3 viewForward, float surfaceDepth, float slopeTolerance, float selectedDepth = 0.0)
{
    float2 texel = ClampWaterPixel(samplePixel, viewport) - 0.5;
    int2 corner = int2(floor(texel));
    float2 fraction = frac(texel);
    float finiteWeight = 0.0;
    float clearWeight = 0.0;
    float3 bedColor = 0.0;
    float3 skyColor = 0.0;
    float depth = 0.0;
    float layerDepthTolerance = WaterBackgroundLayerTolerance(slopeTolerance);
    [unroll]
    for (uint tap = 0; tap < 4; ++tap)
    {
        int2 offset = int2(tap & 1u, tap >> 1u);
        float2 bilinear = lerp(1.0 - fraction, fraction, float2(offset));
        float weight = bilinear.x * bilinear.y;
        if (weight <= 0.0) continue;
        int3 pixel = int3(corner + offset, 0);
        float deviceDepth = sceneDepth.Load(pixel);
        if (!isfinite(deviceDepth)) continue;
        // Coverage records where water passed the opaque depth test at this tap.
        // On oblique views its water depth can differ greatly from the source pixel's.
        if (waterCoverage.Load(pixel) < 0.5) continue;
        if (deviceDepth == view.DepthParameters.x)
        {
            clearWeight += weight;
            skyColor += sceneColor.Load(pixel).rgb * weight;
            continue;
        }
        float3 viewPosition;
        if (!ReconstructViewPositionAnalytic(view.Projection[0], view.Projection[1],
                view.Projection[2], view.Projection[3], WaterNdc(float2(pixel.xy) + 0.5, viewport),
                deviceDepth, viewPosition))
            continue;
        float tapDepth = dot(viewPosition, viewForward);
        if (tapDepth + slopeTolerance < surfaceDepth) continue;
        if (selectedDepth > 0.0 && abs(tapDepth - selectedDepth) > layerDepthTolerance) continue;
        finiteWeight += weight;
        depth += tapDepth * weight;
        bedColor += sceneColor.Load(pixel).rgb * weight;
    }
    FWaterBackground result = (FWaterBackground)0;
    if (finiteWeight > 1.0e-5)
    {
        result.radiance = bedColor / finiteWeight;
        result.depth = depth / finiteWeight;
        result.valid = true;
    }
    else if (clearWeight > 1.0e-5)
    {
        result.radiance = skyColor / clearWeight;
        result.valid = true;
        result.open = true;
    }
    return result;
}

// Recover the position on the original view ray at the selected linear depth.
public float3 ReconstructWaterBed(FWaterViewUniform view, float4 viewport,
    float2 pixel, float3 viewForward, float depth)
{
    float2 ndc = WaterNdc(pixel, viewport);
    float4 rowX = view.Projection[0] - view.Projection[3] * ndc.x;
    float4 rowY = view.Projection[1] - view.Projection[3] * ndc.y;
    float3 ray = cross(rowX.xyz, rowY.xyz);
    float determinant = dot(viewForward, ray);
    if (abs(determinant) <= 1.0e-20) return viewForward * depth;
    return (depth * ray - rowX.w * cross(rowY.xyz, viewForward)
        - rowY.w * cross(viewForward, rowX.xyz)) / determinant;
}

public float WaterIor(float specular)
{
    float f0 = 0.08 * specular;
    return 2.0 / (1.0 - sqrt(min(f0, 0.99))) - 1.0;
}

struct FWaterRefractionRay
{
    float2 pixelDirection;
    float surfaceW;
    float directionW;
};

FWaterRefractionRay BuildWaterRefractionRay(FWaterViewUniform view, float4 viewport, float2 pixel,
    float3 surfaceInView, float3 normal, float3 viewForward, float ior)
{
    FWaterRefractionRay result = (FWaterRefractionRay)0;
    if (ior <= 0.0 || ior == 1.0) return result;
    bool perspective = dot(view.Projection[3].xyz, view.Projection[3].xyz) > 1.0e-12;
    float3 incident = perspective ? normalize(surfaceInView) : viewForward;
    float3 ray = refract(incident, dot(incident, normal) > 0.0 ? -normal : normal, 1.0 / ior);
    float forward = dot(ray, viewForward);
    if (!all(isfinite(ray)) || forward <= 1.0e-5) return result;

    float3 direction = ray / forward;
    result.surfaceW = dot(view.Projection[3], float4(surfaceInView, 1.0));
    result.directionW = dot(view.Projection[3].xyz, direction);
    float2 clipDirection = float2(dot(view.Projection[0].xyz, direction), dot(view.Projection[1].xyz, direction));
    result.pixelDirection = (clipDirection - WaterNdc(pixel, viewport) * result.directionW) * (0.5 * viewport.zw);
    return result;
}

float2 ProjectWaterRefraction(FWaterRefractionRay ray, float separation, float depthBiasCm)
{
    float w = ray.surfaceW + separation * ray.directionW;
    if (w <= 1.0e-6 || separation <= 0.0) return 0.0;
    float separationCm = separation * 100.0;
    float bias = -depthBiasCm;
    float range = max(1.0, clamp(abs(bias) * 0.5, 0.0, 50.0));
    float fade = saturate(separationCm / range + bias) * saturate(separationCm / 30.0);
    return ray.pixelDirection * (separation * fade / w);
}

// A silhouette has two distinct depths. Interpolating them would create a
// receiver between surfaces. Filter color only after the ray has picked a layer.
float WaterReceiverDepth(Texture2D<float> sceneDepth, Texture2D<float> coverage,
    FWaterViewUniform view, float4 viewport, float2 samplePixel, float3 forward)
{
    int3 texel = int3(int2(ClampWaterPixel(samplePixel, viewport)), 0);
    float deviceDepth = sceneDepth.Load(texel);
    float3 position;
    if (coverage.Load(texel) < 0.5 || deviceDepth == view.DepthParameters.x ||
        !ReconstructViewPositionAnalytic(view.Projection[0], view.Projection[1], view.Projection[2],
            view.Projection[3], WaterNdc(float2(texel.xy) + 0.5, viewport), deviceDepth, position))
        return -1.0;
    return dot(position, forward);
}

public struct FWaterRefractionCandidate
{
    public float2 offset;
    // Zero means no candidate.
    public float depth;
    public bool converged;
};

FWaterRefractionCandidate FindCoveredReceiver(Texture2D<float> sceneDepth, Texture2D<float> coverage, FWaterViewUniform view,
    float4 viewport, float2 anchorPixel, float2 targetPixel, float3 viewForward, float surfaceDepth,
    float layerTolerance, float anchorDepth, float2 sourcePixel)
{
    const uint maxRetreatSteps = 24;
    float2 anchor = floor(ClampWaterPixel(anchorPixel, viewport)) + 0.5;
    float2 start = ClampWaterPixel(targetPixel, viewport);
    float2 end = ClampWaterPixel(anchorPixel, viewport);
    float2 travel = start - end;
    float span = max(abs(travel.x), abs(travel.y));
    float clipWSlope = dot(view.Projection[3].xyz, viewForward);
    float clipWConstant = view.Projection[3].w;
    bool perspective = clipWSlope > 1.0e-6;
    float anchorValue = perspective ? rcp(clipWSlope * anchorDepth + clipWConstant) : anchorDepth;
    float2 gradient = 0.0;
    // Reciprocal clip W varies linearly across the projection of a planar receiver.
    // Establish its local slope using covered taps on the original depth layer.
    [unroll]
    for (uint axis = 0; axis < 2; ++axis)
    {
        if (travel[axis] == 0.0) continue;
        float2 step = axis == 0 ? float2(1, 0) : float2(0, 1);
        float depth = WaterReceiverDepth(sceneDepth, coverage, view, viewport, anchor + step, viewForward);
        if (depth <= 0.0 || abs(depth - anchorDepth) > layerTolerance)
        {
            step = -step;
            depth = WaterReceiverDepth(sceneDepth, coverage, view, viewport, anchor + step, viewForward);
            if (depth <= 0.0 || abs(depth - anchorDepth) > layerTolerance) continue;
        }
        float value = perspective ? rcp(clipWSlope * depth + clipWConstant) : depth;
        gradient[axis] = (value - anchorValue) * step[axis];
    }

    // Search back toward the observed receiver even when the target has water
    // coverage: the reprojected pixel can land on a different depth layer.
    // Step one pixel at a time and bound the extra depth reads.
    [loop]
    for (uint step = 0; step <= maxRetreatSteps; ++step)
    {
        float fraction = saturate(float(step) / max(span, 1.0e-6));
        float2 samplePixel = lerp(start, end, fraction);
        float2 texelCenter = floor(samplePixel) + 0.5;
        float value = anchorValue + dot(texelCenter - anchor, gradient);
        if (value > 0.0)
        {
            float expectedDepth = perspective ? (rcp(value) - clipWConstant) / clipWSlope : value;
            float depth = WaterReceiverDepth(sceneDepth, coverage, view, viewport, samplePixel, viewForward);
            if (depth >= surfaceDepth && abs(depth - expectedDepth) <= layerTolerance)
            {
                FWaterRefractionCandidate result = (FWaterRefractionCandidate)0;
                result.depth = depth;
                result.offset = samplePixel - sourcePixel;
                return result;
            }
        }
        if (fraction == 1.0) break;
    }
    return (FWaterRefractionCandidate)0;
}

public FWaterRefractionCandidate FindWaterRefractionCandidate(Texture2D<float> sceneDepth,
    Texture2D<float> coverage, FWaterViewUniform view, float4 viewport, float2 pixel, float3 surfaceInView,
    float3 normal, float3 viewForward, float surfaceDepth, float slopeTolerance, float ior, float depthBias,
    float sourceDepth)
{
    const uint maxSearchSteps = 16;
    const float initialSpanSegments = 6.0;
    const float minStridePixels = 4.0;
    const float crossingConvergencePixels = 0.25;
    const float minRayDepthTolerance = 0.001;
    const float rayDepthSlopeScale = 2.0;
    FWaterRefractionCandidate candidate = (FWaterRefractionCandidate)0;
    if (sourceDepth <= surfaceDepth) return candidate;
    FWaterRefractionRay ray = BuildWaterRefractionRay(view, viewport, pixel, surfaceInView, normal, viewForward, ior);
    float distance = sourceDepth - surfaceDepth;
    float2 probe = ProjectWaterRefraction(ray, distance, depthBias);
    float span = max(abs(probe.x), abs(probe.y));
    if (span == 0.0) return candidate;
    float stride = max(minStridePixels, span / initialSpanSegments);
    float marchStep = distance * min(1.0, stride / span);
    distance = marchStep;
    probe = ProjectWaterRefraction(ray, distance, depthBias);
    float nearDistance = 0.0;
    float farDistance = -1.0;
    float rayDepthTolerance = max(minRayDepthTolerance, rayDepthSlopeScale * slopeTolerance);
    float bestError = 1.0e20;
    // The source texel supplies only a first estimate. At a silhouette it can
    // belong to the bed while the refracted ray hits the pier, or vice versa.
    [loop]
    for (uint step = 0; step < maxSearchSteps; ++step)
    {
        float depth = WaterReceiverDepth(sceneDepth, coverage, view, viewport,
            pixel + probe, viewForward);
        bool validReceiver = depth > 0.0 && depth + slopeTolerance >= surfaceDepth;
        float receiver = max(depth - surfaceDepth, 0.0);
        float error = receiver - distance;
        if (validReceiver && error >= -rayDepthTolerance && abs(error) < bestError)
        {
            candidate.depth = depth;
            bestError = abs(error);
            candidate.offset = probe;
            if (bestError <= rayDepthTolerance)
            {
                candidate.converged = true;
                break;
            }
        }
        // An opaque silhouette can hide the receiver without intersecting the
        // transmitted ray. Advance beyond holes in the visible water coverage.
        if (!validReceiver)
        {
            distance = max(distance, farDistance) + marchStep;
            nearDistance = distance - marchStep;
            farDistance = -1.0;
            probe = ProjectWaterRefraction(ray, distance, depthBias);
            continue;
        }
        if (error <= 0.0) farDistance = distance;
        else nearDistance = distance;
        float nextDistance = receiver > nearDistance && (farDistance < 0.0 || receiver < farDistance)
            ? receiver : 0.5 * (nearDistance + farDistance);
        float2 next = ProjectWaterRefraction(ray, nextDistance, depthBias);
        float2 delta = abs(next - probe);
        if (all(delta < crossingConvergencePixels) && farDistance >= 0.0)
        {
            // A crossing that cannot converge in depth is a jump between
            // surfaces in the depth buffer. Continue looking for a receiver.
            nearDistance = farDistance;
            farDistance = -1.0;
            distance = nearDistance + marchStep;
            probe = ProjectWaterRefraction(ray, distance, depthBias);
            continue;
        }
        if (farDistance < 0.0 && max(delta.x, delta.y) > stride)
        {
            nextDistance = lerp(distance, nextDistance, stride / max(delta.x, delta.y));
        }
        distance = nextDistance;
        probe = ProjectWaterRefraction(ray, distance, depthBias);
    }
    if (candidate.depth > 0.0 && !candidate.converged)
    {
        // The march probe may be far from the ray at the selected depth.
        // Project to that depth and let the final gather revalidate the layer.
        float2 projected = ProjectWaterRefraction(ray, candidate.depth - surfaceDepth, depthBias);
        return FindCoveredReceiver(sceneDepth, coverage, view, viewport, pixel + candidate.offset, pixel + projected,
            viewForward, surfaceDepth, WaterBackgroundLayerTolerance(slopeTolerance), candidate.depth, pixel);
    }
    return candidate;
}

public float WaterSchlickPhase(float g, float rayLightCosine)
{
    // The cubic overshoots unity near either endpoint. Bound the directional peak as well.
    float k = clamp(1.55 * g - 0.55 * g * g * g, -0.99, 0.99);
    float denominator = max(abs(1.0 + k * rayLightCosine), 1.0e-4);
    return (1.0 - k * k) / (12.5663706144 * denominator * denominator);
}

public float3 WaterTransmission(float3 extinction, float distanceCm, bool open)
{
    if (open) return select(extinction > 0.0, float3(0.0), float3(1.0));
    return exp(-extinction * distanceCm);
}

public float3 WaterIntegratedScattering(float3 scattering, float3 extinction, float3 transmission)
{
    return saturate(scattering * (1.0 - transmission) / max(extinction, 1.0e-5));
}

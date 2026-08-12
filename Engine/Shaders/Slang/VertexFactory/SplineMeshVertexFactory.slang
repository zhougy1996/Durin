module SplineMeshVertexFactory;

import VertexFactory.LocalVertexFactory;

public struct FSplineMeshUniform
{
    public float4 StartPosition;
    public float4 StartTangent;
    public float4 EndPosition;
    public float4 EndTangent;
    public float4 StartEndScale;
    public float4 StartEndOffset;
    public float4 RollUpAxis;
    public float4 SourceRangePolicy;
};

internal float3 SafeNormalize(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * rsqrt(lengthSquared) : fallback;
}

internal float3 LeastAlignedCardinal(float3 direction)
{
    float3 absolute = abs(direction);
    if (absolute.x <= absolute.y && absolute.x <= absolute.z) return float3(1.0, 0.0, 0.0);
    if (absolute.y <= absolute.z) return float3(0.0, 1.0, 0.0);
    return float3(0.0, 0.0, 1.0);
}

internal float3 SourceCoordinates(float3 value, uint axis)
{
    if (axis == 1u) return float3(value.y, value.z, value.x);
    if (axis == 2u) return float3(value.z, value.x, value.y);
    return value;
}

internal void EvaluateSpline(FSplineMeshUniform data, float t,
    out float3 position, out float3 forward, out float3 side, out float3 up,
    out float2 scale, out float2 offset)
{
    float t2 = t * t;
    float t3 = t2 * t;
    position = data.StartPosition.xyz * (2.0 * t3 - 3.0 * t2 + 1.0)
        + data.StartTangent.xyz * (t3 - 2.0 * t2 + t)
        + data.EndPosition.xyz * (-2.0 * t3 + 3.0 * t2)
        + data.EndTangent.xyz * (t3 - t2);
    float3 derivative = data.StartPosition.xyz * (6.0 * t2 - 6.0 * t)
        + data.StartTangent.xyz * (3.0 * t2 - 4.0 * t + 1.0)
        + data.EndPosition.xyz * (-6.0 * t2 + 6.0 * t)
        + data.EndTangent.xyz * (3.0 * t2 - 2.0 * t);
    uint axis = (uint)data.RollUpAxis.w;
    float3 axisFallback = axis == 1u ? float3(0.0, 1.0, 0.0)
        : axis == 2u ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    forward = SafeNormalize(derivative,
        SafeNormalize(data.EndPosition.xyz - data.StartPosition.xyz, axisFallback));
    float3 upCandidate = data.RollUpAxis.xyz - forward * dot(data.RollUpAxis.xyz, forward);
    if (dot(upCandidate, upCandidate) <= 1.0e-12)
    {
        float3 cardinal = LeastAlignedCardinal(forward);
        upCandidate = cardinal - forward * dot(cardinal, forward);
    }
    upCandidate = SafeNormalize(upCandidate, float3(0.0, 0.0, 1.0));
    side = SafeNormalize(cross(upCandidate, forward), float3(0.0, 1.0, 0.0));
    up = SafeNormalize(cross(forward, side), upCandidate);
    float alpha = data.SourceRangePolicy.z > 0.5 ? t * t * (3.0 - 2.0 * t) : t;
    scale = lerp(data.StartEndScale.xy, data.StartEndScale.zw, alpha);
    offset = lerp(data.StartEndOffset.xy, data.StartEndOffset.zw, alpha);
    float roll = lerp(data.StartPosition.w, data.StartTangent.w, alpha);
    float sine;
    float cosine;
    sincos(roll, sine, cosine);
    float3 rolledSide = side * cosine + up * sine;
    up = up * cosine - side * sine;
    side = rolledSide;
}

public FLocalVertexFactoryIntermediates GetSplineMeshVertexFactoryIntermediates(
    FLocalVertexFactoryInput input, FSplineMeshUniform data)
{
    FLocalVertexFactoryIntermediates result = GetLocalVertexFactoryIntermediates(input);
    uint axis = (uint)data.RollUpAxis.w;
    float3 source = SourceCoordinates(input.position, axis);
    float extent = max(data.SourceRangePolicy.y - data.SourceRangePolicy.x, 1.0e-12);
    float t = clamp((source.x - data.SourceRangePolicy.x) / extent, 0.0, 1.0);
    float3 curvePosition;
    float3 forward;
    float3 side;
    float3 up;
    float2 scale;
    float2 offset;
    EvaluateSpline(data, t, curvePosition, forward, side, up, scale, offset);
    result.localPosition = curvePosition
        + side * (source.y * scale.x + offset.x)
        + up * (source.z * scale.y + offset.y);
    float3 normal = SourceCoordinates(input.packedNormal.xyz, axis);
    result.localNormal = SafeNormalize(forward * normal.x
        + side * (abs(scale.x) > 1.0e-12 ? normal.y / scale.x : 0.0)
        + up * (abs(scale.y) > 1.0e-12 ? normal.z / scale.y : 0.0), up);
    float3 tangent = SourceCoordinates(input.packedTangent.xyz, axis);
    tangent = SafeNormalize(forward * tangent.x + side * tangent.y * scale.x
        + up * tangent.z * scale.y, forward);
    tangent = SafeNormalize(tangent - result.localNormal * dot(result.localNormal, tangent), forward);
    result.localTangent = float4(tangent, input.packedTangent.w);
    return result;
}

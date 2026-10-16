module MeshUniforms;

public struct MaterialViewUniform
{
    public float4 Parameters;
    public float4 CameraWorldPosition;
    public float4 Viewport;
    public float4 ReciprocalViewport;
    public float4x4 WorldToView;
    public float4x4 ViewToWorld;
    public float4x4 WorldToClip;
    public float4 WorldOriginHigh;
    public float4 WorldOriginLow;
};

public struct MaterialPrimitiveUniform
{
    public float4x4 LocalToWorld;
    public float4x4 NormalToWorld;
    public float4x4 WorldToLocal;
    public float4 BoundsCenter;
    public float4 TransformParams;
    public float4 WorldOriginHigh;
    public float4 WorldOriginLow;
    public float4 CustomPrimitiveData[8];
    public float4 CustomInstanceData[8];
};

// Keep the stable primitive origin independent of every camera and cascade.
// Subtract the high parts before adding the low parts to preserve local detail.
public MaterialPrimitiveUniform TranslateMaterialPrimitive(
    MaterialPrimitiveUniform primitive, MaterialViewUniform view)
{
    float3 origin = (primitive.WorldOriginHigh.xyz - view.WorldOriginHigh.xyz)
        + (primitive.WorldOriginLow.xyz - view.WorldOriginLow.xyz);
    primitive.LocalToWorld[0][3] = origin.x;
    primitive.LocalToWorld[1][3] = origin.y;
    primitive.LocalToWorld[2][3] = origin.z;
    float3 inverseOrigin = -mul((float3x3)primitive.WorldToLocal, origin);
    primitive.WorldToLocal[0][3] = inverseOrigin.x;
    primitive.WorldToLocal[1][3] = inverseOrigin.y;
    primitive.WorldToLocal[2][3] = inverseOrigin.z;
    primitive.BoundsCenter.xyz += origin;
    return primitive;
}

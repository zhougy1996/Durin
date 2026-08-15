module ForwardLightingUniform;

import Lighting.DirectionalShadow;

public struct FForwardLocalLightUniform
{
    public float4 PositionInverseRange;
    public float4 DirectionType;
    public float4 ColorIntensity;
    public float4 SpotCone;
};

public struct FForwardLightingUniform
{
    public float4 ViewPosition;
    public uint4 Counts;
    public float4 DirectionalDirection;
    public float4 DirectionalColorIntensity;
    public float4 DirectionalShadowControl;
    public float4 DirectionalShadowViewDepthTransform;
    public float4 DirectionalShadowSplitDepths;
    public float4 DirectionalShadowLightTransition;
    public FDirectionalShadowCascadeUniform DirectionalShadowCascades[3];
    public FForwardLocalLightUniform Local[4];
};

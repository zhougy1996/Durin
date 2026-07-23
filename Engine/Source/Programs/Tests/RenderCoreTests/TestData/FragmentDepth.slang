struct FragmentOutput
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("fragment")]
FragmentOutput fragmentMain(float4 position : SV_Position)
{
    FragmentOutput output;
    output.color = float4(0.25, 0.5, 0.75, 1.0);
    output.depth = position.z;
    return output;
}

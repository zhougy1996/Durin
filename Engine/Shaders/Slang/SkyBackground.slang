struct SkyBackgroundVSOutput
{
    float4 Position : SV_Position;
    float2 ClipPosition : TEXCOORD;
};

[shader("vertex")]
SkyBackgroundVSOutput VertexMain(uint vertexId : SV_VulkanVertexID)
{
    static const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    SkyBackgroundVSOutput output;
    output.Position = float4(positions[vertexId], 0.0, 1.0);
    output.ClipPosition = positions[vertexId];
    return output;
}

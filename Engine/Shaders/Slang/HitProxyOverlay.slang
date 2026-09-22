struct VSInput { float4 Position : POSITION; float Distance : TEXCOORD0; };
struct VSOutput { float4 Position : SV_Position; float Distance : TEXCOORD0; };
struct IdUniform { uint4 Value; };
[[vk::binding(0, 0)]] ConstantBuffer<IdUniform> Id;
[shader("vertex")]
VSOutput VertexMain(VSInput Input)
{
    VSOutput Output;
    Output.Position = Input.Position;
    Output.Distance = Input.Distance;
    return Output;
}
[shader("fragment")]
uint2 FragmentMain(VSOutput Input) : SV_Target0
{
    return uint2(Id.Value.x, asuint(Input.Distance));
}

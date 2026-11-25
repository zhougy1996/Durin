struct DomeVertex
{
    float3 direction : POSITION;
    float2 uv : TEXCOORD0;
};

struct DomeProjection
{
    float4 clipX;
    float4 clipY;
    float4 clipW;
};
[[vk::push_constant]] ConstantBuffer<DomeProjection> Projection;

struct DomeVaryings
{
    float4 position : SV_Position;
    float3 direction : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

[shader("vertex")]
DomeVaryings VertexMain(DomeVertex input)
{
    DomeVaryings output;
    float w = dot(Projection.clipW.xyz, input.direction);
    output.position = float4(dot(Projection.clipX.xyz, input.direction),
        dot(Projection.clipY.xyz, input.direction), Projection.clipW.w * w, w);
    output.direction = input.direction;
    output.uv = input.uv;
    return output;
}

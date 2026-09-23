struct FUniform { uint4 Value; };
[[vk::binding(0, 0)]] ConstantBuffer<FUniform> Uniform;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> Values;
[[vk::binding(2, 0)]] RWTexture2D<float4> Output;
struct FPush { uint Pixel; };
[[vk::push_constant]] ConstantBuffer<FPush> Push;

float4 Color() { return float4(Uniform.Value.x, Values[0].y, Values[0].z, 255) / 255.0; }

[numthreads(1, 1, 1)]
void ComputeMain() { Output[int2(Push.Pixel, 0)] = Color(); }

[shader("vertex")]
float4 VertexMain(uint index : SV_VertexID) : SV_Position
{
    static const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(positions[index], 0, 1);
}

[shader("fragment")]
float4 FragmentMain() : SV_Target { return Color(); }

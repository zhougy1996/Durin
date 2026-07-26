// Editor-only reflective-sphere preview for TextureCube thumbnails.

struct VSInput
{
    float3 pos : POSITION;
    float4 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

struct TransformUniform
{
    float4x4 LocalToClip;
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
    float4 ViewPosition;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;
[[vk::binding(1, 0)]] TextureCube<float4> CubeTexture;
[[vk::binding(2, 0)]] SamplerState CubeSampler;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(input.pos, 1.0));
    output.worldPosition = mul(Transform.LocalToWorld, float4(input.pos, 1.0)).xyz;
    output.worldNormal = normalize(mul(Transform.NormalToWorld, float4(input.normal.xyz, 0.0)).xyz);
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    float3 incident = normalize(input.worldPosition - Transform.ViewPosition.xyz);
    float3 reflectionDirection = normalize(reflect(incident, normalize(input.worldNormal)));
    return float4(CubeTexture.Sample(CubeSampler, reflectionDirection).rgb, 1.0);
}

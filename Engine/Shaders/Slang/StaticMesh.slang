// Minimal StaticMesh shader.

struct VSInput
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

struct TransformUniform
{
    float4x4 LocalToClip;
    float4 Color;
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
};

struct LightingUniform
{
    float4 LightDirection;
    float4 LightColorIntensity;
    float4 ViewPositionAmbient;
    float4 MaterialParams;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;
[[vk::binding(1, 0)]] ConstantBuffer<LightingUniform> Lighting;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(input.pos, 1.0));
    output.color = Transform.Color;
    output.worldPosition = mul(Transform.LocalToWorld, float4(input.pos, 1.0)).xyz;
    output.worldNormal = normalize(mul(Transform.NormalToWorld, float4(input.normal, 0.0)).xyz);
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    if (Lighting.MaterialParams.z < 0.5)
        return input.color;

    float3 normal = normalize(input.worldNormal);
    float3 toLight = normalize(-Lighting.LightDirection.xyz);
    float3 toView = normalize(Lighting.ViewPositionAmbient.xyz - input.worldPosition);
    float diffuse = max(dot(normal, toLight), 0.0);
    float3 halfVector = normalize(toLight + toView);
    float specular = diffuse > 0.0 ? pow(max(dot(normal, halfVector), 0.0), Lighting.MaterialParams.y) * Lighting.MaterialParams.x : 0.0;
    float3 lightColor = Lighting.LightColorIntensity.rgb * Lighting.LightColorIntensity.a;
    float3 litColor = input.color.rgb * (Lighting.ViewPositionAmbient.a + diffuse * lightColor) + specular * lightColor;
    return float4(litColor, input.color.a);
}

// Define a structure for data passing between Vertex and Fragment stages
struct Coords
{
    float4 pos : SV_Position; // Built-in semantic for clip-space position
    float3 color : COLOR;     // Custom semantic for interpolated color
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
};
ConstantBuffer<UniformBuffer> ubo;

// --- Vertex Shader ---
[shader("vertex")]
Coords vertexMain(
    float3 inPosition : POSITION, 
    float3 inColor : COLOR)
{
    Coords output;

    // gl_Position equivalent
    float3 transformedPosition = inPosition * 0.1; // Scale down the position
    output.pos = mul(ubo.model, float4(transformedPosition.x, -transformedPosition.y, transformedPosition.z + 0.5, 1.0));
    
    // Assign color based on vertex input
    output.color = inColor;

    return output;
}

// --- Fragment Shader ---
[shader("fragment")]
float4 fragmentMain(Coords input) : SV_Target
{
    // Return the interpolated color as RGBA
    return float4(input.color, 1.0);
}
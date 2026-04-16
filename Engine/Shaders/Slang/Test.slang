// Define a structure for data passing between Vertex and Fragment stages
struct Coords
{
    float4 pos : SV_Position; // Built-in semantic for clip-space position
    float3 color : COLOR;     // Custom semantic for interpolated color
};

// Global constant array
static const float3 kColors[4] = 
{
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0),
    float3(1.0, 1.0, 1.0)
};

// --- Vertex Shader ---
[shader("vertex")]
Coords vertexMain(
    float3 inPosition : POSITION, 
    uint vertexIndex : SV_VertexID)
{
    Coords output;

    // gl_Position equivalent
    output.pos = float4(inPosition, 1.0);
    
    // Assign color based on vertex index
    output.color = kColors[vertexIndex];

    return output;
}

// --- Fragment Shader ---
[shader("fragment")]
float4 fragmentMain(Coords input) : SV_Target
{
    // Return the interpolated color as RGBA
    return float4(input.color, 1.0);
}
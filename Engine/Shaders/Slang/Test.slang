// Define a structure for data passing between Vertex and Fragment stages
struct Coords
{
    float4 pos : SV_Position; // Built-in semantic for clip-space position
    float3 color : COLOR;     // Custom semantic for interpolated color
};

// --- Vertex Shader ---
[shader("vertex")]
Coords vertexMain(
    float3 inPosition : POSITION, 
    float3 inColor : COLOR)
{
    Coords output;

    // gl_Position equivalent
    output.pos = float4(inPosition, 1.0);
    
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
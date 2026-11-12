[shader("vertex")]
float4 VertexMain(uint id : SV_VertexID) : SV_Position { return float4(float(id), 0, 0, 1); }

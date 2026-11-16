#if DURIN_GENERATED_MATERIAL_POSTPROCESS
// A material fragment entry uses the shared fullscreen vertex implementation.
[shader("fragment")]
float4 PostProcessMaterialMain(float4 position : SV_Position) : SV_Target0
{
    VSOutput input = (VSOutput)0;
    input.pos = position;
    FMaterialSurface material = EvaluateGeneratedMaterial(input);
    float3 color = material.emissive;
    color = float3(isfinite(color.x) ? clamp(color.x, 0, 65504) : 0,
        isfinite(color.y) ? clamp(color.y, 0, 65504) : 0,
        isfinite(color.z) ? clamp(color.z, 0, 65504) : 0);
    float2 uv = (position.xy - MaterialView.Viewport.xy) * MaterialView.ReciprocalViewport.xy;
    return float4(color, ReadMaterialInput0Linear(uv).a);
}

#endif

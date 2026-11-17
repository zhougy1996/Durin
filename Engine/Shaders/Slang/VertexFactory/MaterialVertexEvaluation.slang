#if DURIN_GENERATED_MATERIAL && DURIN_MATERIAL_VERTEX_STAGE
// Common material evaluation over factory-provided geometry.
VSOutput EvaluateGeneratedGeometry(FLocalVertexFactoryIntermediates vertex, uint sceneInstanceId)
{
    VSOutput input = (VSOutput)0;
    MaterialPrimitiveUniform primitive = TranslateMaterialPrimitive(GetMaterialPrimitiveData(sceneInstanceId), MaterialView);
#if DURIN_MATERIAL_PIXEL_PRIMITIVE_DATA
    input.sceneInstanceId = sceneInstanceId;
#endif
    input.pos = mul(MaterialView.WorldToClip, mul(primitive.LocalToWorld, float4(vertex.localPosition, 1.0)));
    input.color = vertex.color;
    CopyPrimitiveCustomData(input, primitive);
    input.worldPosition = mul(primitive.LocalToWorld, float4(vertex.localPosition, 1.0)).xyz;
    input.worldNormal = mul(primitive.NormalToWorld, float4(vertex.localNormal, 0.0)).xyz;
    input.worldTangent = float4(mul(primitive.LocalToWorld, float4(vertex.localTangent.xyz, 0.0)).xyz,
        vertex.localTangent.w * primitive.TransformParams.x);
    input.uv0 = vertex.texCoords[0]; input.uv1 = vertex.texCoords[1];
    input.uv2 = vertex.texCoords[2]; input.uv3 = vertex.texCoords[3];
    float3 offset;
    VSOutput output = EvaluateGeneratedVertex(input, primitive, offset);
    output.pos += mul(MaterialView.WorldToClip, float4(offset, 0.0));
    return output;
}

#endif

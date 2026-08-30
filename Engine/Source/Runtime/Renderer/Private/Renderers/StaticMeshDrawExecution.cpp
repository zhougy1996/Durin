#include "Renderers/StaticMeshDrawExecution.h"

namespace Durin::RendererPrivate
{
	auto FStaticMeshSurfaceMaterialPreparer::Prepare(
		ESurfaceMaterialPass Pass,
		bool bEnableLighting,
		bool bEnableSpecularAA,
		FRHITexture* DirectionalShadowTexture,
		FRHISampler* DirectionalShadowSampler,
		FPreparedStaticMeshSurfaceMaterial& OutMaterial
	) const -> bool
	{
		if (MaterialBinding == nullptr || !SurfaceMaterials.Resolve_RenderThread(
			*MaterialBinding, Pass, bEnableLighting, bEnableSpecularAA,
			DirectionalShadowTexture, DirectionalShadowSampler,
			OutMaterial.Surface))
		{
			return false;
		}
		OutMaterial.Uniform = CommandList.AllocateDynamicUniformBuffer(
			&OutMaterial.Surface.Uniform,
			sizeof(OutMaterial.Surface.Uniform)
		);
		return true;
	}

	auto FStaticMeshPrimitiveUniformPreparer::Prepare(
		const FPreparedStaticMeshPrimitive& Primitive
	) const -> FStaticMeshPrimitiveUniformBindings
	{
		FStaticMeshTransformUniform TransformUniform;
		TransformUniform.LocalToClip = Math::TransposeToFloat(
			View.ViewProjectionMatrix * Primitive.LocalToWorld
		);
		TransformUniform.LocalToWorld =
			Math::TransposeToFloat(Primitive.LocalToWorld);
		TransformUniform.NormalToWorld = Math::TransposeToFloat(
			Math::Transpose(Math::Inverse(Primitive.LocalToWorld))
		);
		TransformUniform.TransformParams.x = Math::LinearDeterminant(
			FMatrix4f(Primitive.LocalToWorld)
		) < 0.0f ? -1.0f : 1.0f;

		const FSplineMeshUniform SplineUniform = MakeSplineMeshUniform(
			Primitive.VertexDomain == EVertexDeformationDomain::Spline
				? Primitive.SplineDynamicData.Params : FSplineMeshParams{}
		);
		return {
			.Transform = CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform, sizeof(TransformUniform)
			),
			.SplineMesh = CommandList.AllocateDynamicUniformBuffer(
				&SplineUniform, sizeof(SplineUniform)
			)
		};
	}

	auto FStaticMeshGeometryBinding::IsValid() const -> bool
	{
		return Primitive.LOD != nullptr && Primitive.VertexFactory != nullptr
			&& Draw.Section != nullptr;
	}

	auto FStaticMeshGeometryBinding::GetVertexDeclaration() const
		-> FVertexDeclarationRHIRef
	{
		check(IsValid());
		return FVertexDeclarationRHIRef(Primitive.VertexFactory->GetDeclaration());
	}

	auto FStaticMeshGeometryBinding::Bind(
		FRHICommandListImmediate& CommandList
	) const -> void
	{
		check(IsValid());
		Primitive.VertexFactory->BindStreams(CommandList);
		CommandList.BindIndexBuffer(Primitive.LOD->IndexBuffer.GetRHI(), 0);
	}

	auto FStaticMeshGeometryBinding::DrawIndexed(
		FRHICommandListImmediate& CommandList
	) const -> void
	{
		check(IsValid());
		CommandList.DrawIndexed(
			Draw.Section->IndexCount, Draw.Section->FirstIndex, 0
		);
	}
} // namespace Durin::RendererPrivate

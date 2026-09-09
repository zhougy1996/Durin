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
		if (OutMaterial.Surface.bCompiledLayout)
		{
			OutMaterial.Uniform = CommandList.AllocateDynamicUniformBuffer(
				OutMaterial.Surface.CompiledUniformPayload.data(),
				static_cast<uint32>(OutMaterial.Surface.CompiledUniformPayload.size()));
			return true;
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
		TransformUniform.NormalToWorld = Primitive.NormalToWorld;
		TransformUniform.TransformParams.x = Math::LinearDeterminant(
			FMatrix4f(Primitive.LocalToWorld)
		) < 0.0f ? -1.0f : 1.0f;

		return {
			.Transform = CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform, sizeof(TransformUniform)
			)
		};
	}

	auto FStaticMeshGeometryBinding::IsValid() const -> bool
	{
		return Primitive.CollectedBinding && Primitive.CollectedBinding->Declaration
			&& Draw.Geometry.Validate(Draw.Vertices.Range, Draw.Indices.Range) == EGeometrySubmissionOutcome::Submitted;
	}

	auto FStaticMeshGeometryBinding::GetVertexDeclaration() const
		-> FVertexDeclarationRHIRef
	{
		check(IsValid());
		return FVertexDeclarationRHIRef(Primitive.CollectedBinding->Declaration);
	}

	auto FStaticMeshGeometryBinding::Bind(
		FRHICommandListImmediate& CommandList
	) const -> void
	{
		check(IsValid());
		for (const auto& Stream : Primitive.CollectedBinding->Streams)
			CommandList.BindVertexBuffer(Stream.StreamIndex, Stream.VertexBuffer, Stream.Offset);
		if (Draw.Geometry.bIndexed)
			CommandList.BindIndexBuffer(Draw.Indices.Buffer, static_cast<uint32>(Draw.Indices.Range.ByteOffset));
	}

	auto FStaticMeshGeometryBinding::DrawIndexed(
		FRHICommandListImmediate& CommandList
	) const -> void
	{
		check(IsValid());
		if (Draw.Geometry.bIndexed)
			CommandList.DrawIndexed(Draw.Geometry.GetIndexedDrawArguments());
		else
			CommandList.Draw(Draw.Geometry.GetDrawArguments());
	}
} // namespace Durin::RendererPrivate

#include "Profiling/Profiling.h"
#include "Renderers/StaticMeshDrawExecution.h"


namespace Durin::RendererPrivate
{
	auto PrepareMeshViewUniform(FRHICommandListImmediate& CommandList, const FSceneView& View,
		bool bLighting) -> FRHIUniformBufferRange
	{
		const FVector4f Parameters(static_cast<float>(View.MaterialTimeSeconds), 0.0f,
			bLighting ? 1.0f : 0.0f, bLighting && View.Settings.Mode.bEnableSpecularAA ? 1.0f : 0.0f);
		return CommandList.AllocateDynamicUniformBuffer(&Parameters, sizeof(Parameters));
	}
	auto PrepareStaticMeshPrimitiveUniforms(FRHICommandListImmediate& CommandList,
		const FSceneView& View, const FPreparedStaticMeshView& Prepared,
		FResolvedStaticMeshView& Resolved) -> bool
	{
		Resolved.PrimitiveUniforms.clear();
		Resolved.PrimitiveUniforms.reserve(Prepared.Primitives.size());
		for (const auto& Primitive : Prepared.Primitives)
		{
			const auto Uniform = FStaticMeshPrimitiveUniformPreparer(CommandList, View).Prepare(Primitive).Transform;
			if (!Uniform.Buffer) return false;
			Resolved.PrimitiveUniforms.push_back(Uniform);
			++Resolved.Observations.PrimitiveUniformUploads;
		}
		return true;
	}

	auto FStaticMeshSurfaceMaterialPreparer::Prepare(
		ESurfaceMaterialPass Pass,
		FRHITexture* DirectionalShadowTexture,
		FRHISampler* DirectionalShadowSampler,
		FPreparedStaticMeshSurfaceMaterial& OutMaterial,
		const FRHIUniformBufferRange& SharedUniform
	) const -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareSurfaceMaterial");
		if (MaterialBinding == nullptr || !SurfaceMaterials.Resolve_RenderThread(
			*MaterialBinding, Pass,
			DirectionalShadowTexture, DirectionalShadowSampler,
			OutMaterial.Surface))
		{
			return false;
		}
		if (OutMaterial.Surface.bCompiledLayout)
		{
			OutMaterial.Uniform = SharedUniform.Buffer ? SharedUniform : CommandList.AllocateDynamicUniformBuffer(
				OutMaterial.Surface.CompiledUniformPayload.data(),
				static_cast<uint32>(OutMaterial.Surface.CompiledUniformPayload.size()));
			return OutMaterial.Uniform.Buffer != nullptr;
		}
		return false;
	}

	auto FStaticMeshPrimitiveUniformPreparer::Prepare(
		const FPreparedStaticMeshPrimitive& Primitive
	) const -> FStaticMeshPrimitiveUniformBindings
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PreparePrimitiveUniform");
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
			&& Draw.Command && Draw.Command->Geometry.Validate(Draw.Command->Vertices.Range, Draw.Command->Indices.Range) == EGeometrySubmissionOutcome::Submitted;
	}

	auto FStaticMeshGeometryBinding::GetVertexDeclaration() const
		-> FVertexDeclarationRHIRef
	{
		check(IsValid());
		return FVertexDeclarationRHIRef(Primitive.CollectedBinding->Declaration);
	}

	auto FStaticMeshGeometryBinding::Bind(
		FRHICommandList& CommandList
	) const -> void
	{
		check(IsValid());
		for (const auto& Stream : Primitive.CollectedBinding->Streams)
			CommandList.BindVertexBuffer(Stream.StreamIndex, Stream.VertexBuffer, Stream.Offset);
		if (Draw.Command->Geometry.bIndexed)
			CommandList.BindIndexBuffer(Draw.Command->Indices.Buffer, static_cast<uint32>(Draw.Command->Indices.Range.ByteOffset));
	}

	auto FStaticMeshGeometryBinding::DrawIndexed(
		FRHICommandList& CommandList
	) const -> void
	{
		check(IsValid());
		if (Draw.Command->Geometry.bIndexed)
			CommandList.DrawIndexed(Draw.Command->Geometry.GetIndexedDrawArguments());
		else
			CommandList.Draw(Draw.Command->Geometry.GetDrawArguments());
	}
} // namespace Durin::RendererPrivate

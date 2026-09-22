#pragma once

#include "Renderers/MeshRendererShared.h"

namespace Durin::RendererPrivate
{
	// Uniform ranges shared by every pass that draws one prepared primitive.
	struct FStaticMeshPrimitiveUniformBindings
	{
		FRHIUniformBufferRange Transform;
	};

	// Owns resolved material state and the uniform allocation shared by a draw group.
	struct FPreparedStaticMeshSurfaceMaterial
	{
		FResolvedSurfaceMaterial Surface;
		FRHIUniformBufferRange Uniform;
	};

	// Resolves a logical material binding into resources ready for one surface pass.
	class FStaticMeshSurfaceMaterialPreparer
	{
	public:
		FStaticMeshSurfaceMaterialPreparer(
			FRHICommandListImmediate& InCommandList,
			FSurfaceMaterialResources& InSurfaceMaterials,
			const FMaterialRenderBinding* InMaterialBinding
		)
			: CommandList(InCommandList)
			, SurfaceMaterials(InSurfaceMaterials)
			, MaterialBinding(InMaterialBinding)
		{
		}

		auto IsValid() const -> bool { return MaterialBinding != nullptr; }
		auto Prepare(
			ESurfaceMaterialPass Pass,
			FRHITexture* DirectionalShadowTexture,
			FRHISampler* DirectionalShadowSampler,
			FPreparedStaticMeshSurfaceMaterial& OutMaterial,
			const FRHIUniformBufferRange& SharedUniform = {}
		) const -> bool;

	private:
		FRHICommandListImmediate& CommandList;
		FSurfaceMaterialResources& SurfaceMaterials;
		const FMaterialRenderBinding* MaterialBinding = nullptr;
	};

	// Builds the pass-independent primitive uniforms consumed by StaticMesh vertex shaders.
	class FStaticMeshPrimitiveUniformPreparer
	{
	public:
		FStaticMeshPrimitiveUniformPreparer(
			FRHICommandListImmediate& InCommandList,
			const FSceneView& InView
		)
			: CommandList(InCommandList), View(InView)
		{
		}

		auto Prepare(const FPreparedStaticMeshPrimitive& Primitive) const
			-> FStaticMeshPrimitiveUniformBindings;

	private:
		FRHICommandListImmediate& CommandList;
		const FSceneView& View;
	};

	auto PrepareMeshViewUniform(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bLighting) -> FRHIUniformBufferRange;

	RENDERER_API auto PrepareStaticMeshPrimitiveUniforms(FRHICommandListImmediate& CommandList,
		const FSceneView& View, const FPreparedStaticMeshView& Prepared,
		FResolvedStaticMeshView& Resolved) -> bool;

	// Validates, binds, and submits the geometry referenced by one prepared draw.
	class FStaticMeshGeometryBinding
	{
	public:
		FStaticMeshGeometryBinding(
			const FPreparedStaticMeshPrimitive& InPrimitive,
			const FPreparedStaticMeshDraw& InDraw
		)
			: Primitive(InPrimitive), Draw(InDraw)
		{
		}

		auto IsValid() const -> bool;
		auto GetVertexDeclaration() const -> FVertexDeclarationRHIRef;
		auto Bind(FRHICommandList& CommandList) const -> void;
		auto DrawIndexed(FRHICommandList& CommandList) const -> void;

	private:
		const FPreparedStaticMeshPrimitive& Primitive;
		const FPreparedStaticMeshDraw& Draw;
	};
} // namespace Durin::RendererPrivate

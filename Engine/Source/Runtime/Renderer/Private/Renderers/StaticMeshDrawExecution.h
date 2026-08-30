#pragma once

#include "Renderers/MeshRendererShared.h"

namespace Durin::RendererPrivate
{
	// Uniform ranges shared by every pass that draws one prepared primitive.
	struct FStaticMeshPrimitiveUniformBindings
	{
		FRHIUniformBufferRange Transform;
		FRHIUniformBufferRange SplineMesh;
	};

	// Owns resolved material state and its per-draw uniform allocation.
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
			bool bEnableLighting,
			bool bEnableSpecularAA,
			FRHITexture* DirectionalShadowTexture,
			FRHISampler* DirectionalShadowSampler,
			FPreparedStaticMeshSurfaceMaterial& OutMaterial
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
		auto Bind(FRHICommandListImmediate& CommandList) const -> void;
		auto DrawIndexed(FRHICommandListImmediate& CommandList) const -> void;

	private:
		const FPreparedStaticMeshPrimitive& Primitive;
		const FPreparedStaticMeshDraw& Draw;
	};
} // namespace Durin::RendererPrivate

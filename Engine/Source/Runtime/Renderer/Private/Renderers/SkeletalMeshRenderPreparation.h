#pragma once

#include "Renderers/StaticMeshRenderPreparation.h"

#include "Animation/SkeletalAnimation.h"
#include "Materials/MaterialRenderProxy.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace Durin
{
	struct FPreparedSkeletalMeshPrimitive
	{
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		const FSkeletalMeshRenderData* RenderData = nullptr;
		const FSkeletalMeshVertexFactory* VertexFactory = nullptr;
		std::shared_ptr<const FSkeletalPosePalette> Pose;
		FMatrix LocalToWorld{1.0};
		FRHIStorageBufferRange PaletteRange;
	};

	struct FPreparedSkeletalMeshDraw
	{
		uint32 PrimitiveIndex = 0;
		uint32 SectionIndex = 0;
		const FSkeletalMeshRenderSection* Section = nullptr;
		FVector3 SortCenter{0.0};
		double TranslucentDistanceSquared = 0.0;
		FMaterialRenderData Material;
		FMaterialRenderV3Binding MaterialBinding;
		EStaticMeshBasePass Pass = EStaticMeshBasePass::Opaque;
		FMaterialShaderMapIdentity ShaderMapIdentity;
		FEffectiveStaticMeshPipelineKey PipelineKey;
		FStaticMeshDrawSortKey SortKey;
		bool bCastsShadow = false;
		bool bResourcesReady = false;
	};

	enum class EPreparedSkeletalMeshPhase : uint8
	{
		Prepared,
		ResourcesPrepared,
		Executed,
	};

	struct FPreparedSkeletalMeshView
	{
		std::vector<FPreparedSkeletalMeshPrimitive> Primitives;
		std::vector<FPreparedSkeletalMeshDraw> Opaque;
		std::vector<FPreparedSkeletalMeshDraw> Masked;
		std::vector<FPreparedSkeletalMeshDraw> Translucent;
		size_t VisibleCandidates = 0;
		size_t RejectedPrimitives = 0;
		size_t SelectedSections = 0;
		size_t SelectedTriangles = 0;
		size_t OpaqueSections = 0;
		size_t MaskedSections = 0;
		size_t TranslucentSections = 0;
		size_t OpaqueTriangles = 0;
		size_t MaskedTriangles = 0;
		size_t TranslucentTriangles = 0;
		size_t OpaqueStateGroups = 0;
		size_t MaskedStateGroups = 0;
		size_t PipelineTransitions = 0;
		size_t MaterialTransitions = 0;
		size_t VertexFactoryTransitions = 0;
		size_t GeometryTransitions = 0;
		size_t ResourcePreparationAttemptedDraws = 0;
		size_t ResourcePreparationSuccessfulDraws = 0;
		size_t ResourcePreparationRejectedDraws = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t RequestedPaletteUploads = 0;
		size_t UploadedPalettes = 0;
		size_t ReusedPalettes = 0;
		size_t RejectedPalettes = 0;
		size_t UploadedPaletteMatrices = 0;
		size_t UploadedPaletteBytes = 0;
		EPreparedSkeletalMeshPhase Phase = EPreparedSkeletalMeshPhase::Prepared;

		auto GetNumSections() const -> size_t
		{
			return Opaque.size() + Masked.size() + Translucent.size();
		}
		auto GetPrimitive(const FPreparedSkeletalMeshDraw& Draw) const
			-> const FPreparedSkeletalMeshPrimitive*
		{
			return Draw.PrimitiveIndex < Primitives.size()
				? &Primitives[Draw.PrimitiveIndex] : nullptr;
		}
	};

	auto PrepareSkeletalMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode) -> FPreparedSkeletalMeshView;
}

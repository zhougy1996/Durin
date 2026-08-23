#pragma once

#include "Renderers/MeshRenderPreparationCommon.h"

#include "Animation/SkeletalAnimation.h"
#include "Materials/MaterialRenderProxy.h"
#include "Scene.h"
#include "SceneView.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

#include <unordered_map>

namespace Durin
{
	struct FPreparedSkeletalMeshPrimitive
	{
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		const FSkeletalMeshRenderData* RenderData = nullptr;
		const FSkeletalMeshVertexFactory* VertexFactory = nullptr;
		std::shared_ptr<const FSkeletalPosePalette> Pose;
		FMatrix LocalToWorld{1.0};
	};

	// One render-submission-local logical registry shared by the receiver view
	// and every directional-shadow cascade. Entries contain no upload state.
	struct FPreparedSkeletalPaletteTable
	{
		struct FEntry
		{
			std::shared_ptr<const FSkeletalPosePalette> Pose;
		};

		std::vector<FEntry> Entries;
		std::unordered_map<FPrimitiveSceneId, uint32, FSceneIdHash>
			PrimitiveToEntry;
	};

	// Owns the fallible upload result for the corresponding logical registry.
	struct FResolvedSkeletalPaletteTable
	{
		struct FEntry
		{
			FRHIStorageBufferRange Range;
			bool bUploadAttempted = false;
		};

		std::vector<FEntry> Entries;
		uint64 UploadedBytes = 0;
		size_t RequestedPalettes = 0;
		size_t UploadedPalettes = 0;
		size_t ReusedPalettes = 0;
		size_t RejectedPalettes = 0;
		size_t UploadedMatrices = 0;
	};

	struct FPreparedSkeletalMeshDraw
	{
		uint32 ResolvedIndex = 0;
		uint32 PrimitiveIndex = 0;
		uint32 SectionIndex = 0;
		const FSkeletalMeshRenderSection* Section = nullptr;
		FVector3 SortCenter{0.0};
		double TranslucentDistanceSquared = 0.0;
		FMaterialRenderData Material;
		EMeshBasePass Pass = EMeshBasePass::Opaque;
		FMaterialShaderMapIdentity ShaderMapIdentity;
		FEffectiveMeshPipelineKey PipelineKey;
		FMeshDrawSortKey SortKey;
		bool bCastsShadow = false;
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
		uint64 SortingNanoseconds = 0;
		size_t SharedPrimitiveFactBuilds = 0;
		size_t SharedPrimitiveFactReuses = 0;
		size_t SharedSectionFactBuilds = 0;
		size_t SharedSectionFactReuses = 0;
		size_t RequestedPaletteUploads = 0;
		size_t RejectedPalettes = 0;

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

	struct FSkeletalMeshRenderObservations
	{
		size_t ResourcePreparationAttemptedDraws = 0;
		size_t ResourcePreparationSuccessfulDraws = 0;
		size_t ResourcePreparationRejectedDraws = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t GBufferAttemptedDraws = 0;
		size_t GBufferSuccessfulDraws = 0;
		size_t GBufferRejectedDraws = 0;
		size_t GBufferSkippedDraws = 0;
		size_t UploadedPalettes = 0;
		size_t ReusedPalettes = 0;
		size_t RejectedPalettes = 0;
		size_t UploadedPaletteMatrices = 0;
		size_t UploadedPaletteBytes = 0;
	};

	// Owns fallible SkeletalMesh resources separately from observations.
	struct FResolvedSkeletalMeshView
	{
		std::vector<FResolvedMeshDrawRecord> Draws;
		std::vector<FRHIStorageBufferRange> PaletteRanges;
		FRHITexture* DirectionalShadowTexture = nullptr;
		FRHISampler* DirectionalShadowSampler = nullptr;
		FSkeletalMeshRenderObservations Observations;

		auto IsReady(const FPreparedSkeletalMeshDraw& Draw) const -> bool
		{
			return Draw.ResolvedIndex < Draws.size()
				&& Draws[Draw.ResolvedIndex].bReady;
		}
		auto GetMaterialBinding(const FPreparedSkeletalMeshDraw& Draw) const
			-> const FMaterialRenderBinding*
		{
			return Draw.ResolvedIndex < Draws.size()
				&& Draws[Draw.ResolvedIndex].MaterialBinding
				? &*Draws[Draw.ResolvedIndex].MaterialBinding : nullptr;
		}
		auto GetPaletteRange(const FPreparedSkeletalMeshDraw& Draw) const
			-> FRHIStorageBufferRange
		{
			return Draw.PrimitiveIndex < PaletteRanges.size()
				? PaletteRanges[Draw.PrimitiveIndex] : FRHIStorageBufferRange{};
		}
	};

	auto PrepareSkeletalMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		FPreparedSkeletalPaletteTable& PaletteTable,
		ERenderPreparationMode Mode = ERenderPreparationMode::Full
	) -> FPreparedSkeletalMeshView;
}

#pragma once

#include "SkeletalMeshEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"

namespace Durin::Editor::SkeletalMesh
{
	// Freezes reference-pose LOD 0 skeletal thumbnail identity and output policy.
	struct FSkeletalMeshThumbnailContract
	{
		static constexpr std::string_view RendererName = "Durin.SkeletalMeshThumbnail";
		static constexpr uint32 GeneratorSchemaVersion = 1;
		static constexpr std::string_view PreviewFixtureIdentity =
			"/Engine/Editor/SkeletalMeshPreview/ReferencePoseLOD0DefaultMaterials";
		static constexpr uint32 PreviewFixtureVersion = 1;
		static constexpr uint32 ShaderContractVersion = 1;
	};

	class FSkeletalMeshThumbnailGenerationInput final
		: public ::Durin::Editor::IAssetThumbnailGenerationInput
	{
	public:
		FSkeletalMeshThumbnailGenerationInput(
			FTopLevelAssetPath InAssetPath, ::Durin::Editor::FThumbnailVisualContract InVisual)
			: AssetPath(std::move(InAssetPath)), Visual(std::move(InVisual)) {}
		FTopLevelAssetPath AssetPath;
		::Durin::Editor::FThumbnailVisualContract Visual;
	};

	// Captures the mesh, Skeleton, material, and texture package closure before generation.
	class DSkeletalMeshThumbnailRenderer final
		: public ::Durin::Editor::DDefaultSizedThumbnailRenderer
	{
	public:
		SKELETALMESHEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FThumbnailRenderingInfo override;
		SKELETALMESHEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request, uint64 RendererGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		SKELETALMESHEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession> override;
	};
}

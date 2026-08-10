#pragma once

#include "SkeletalMeshEditorAPI.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin
{
	// Freezes reference-pose LOD 0 skeletal thumbnail identity and output policy.
	struct FSkeletalMeshAssetThumbnailContract
	{
		static constexpr std::string_view ProviderName = "Durin.SkeletalMeshThumbnail";
		static constexpr uint32 GeneratorSchemaVersion = 1;
		static constexpr std::string_view PreviewFixtureIdentity =
			"/Engine/Editor/SkeletalMeshPreview/ReferencePoseLOD0DefaultMaterials";
		static constexpr uint32 PreviewFixtureVersion = 1;
		static constexpr uint32 ShaderContractVersion = 1;
	};

	class FSkeletalMeshAssetThumbnailGenerationInput final
		: public IAssetThumbnailGenerationInput
	{
	public:
		FSkeletalMeshAssetThumbnailGenerationInput(
			FAssetPath InAssetPath, FRenderedAssetThumbnailVisualContract InVisual)
			: AssetPath(std::move(InAssetPath)), Visual(std::move(InVisual)) {}
		FAssetPath AssetPath;
		FRenderedAssetThumbnailVisualContract Visual;
	};

	// Captures the mesh, Skeleton, material, and texture package closure before generation.
	class FSkeletalMeshAssetThumbnailProvider final
		: public IRenderedAssetThumbnailExtension
	{
	public:
		SKELETALMESHEDITOR_API auto GetRegistration() const
			-> FAssetThumbnailProviderRegistration override;
		SKELETALMESHEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request, uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		SKELETALMESHEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<IRenderedAssetThumbnailGenerationSession> override;
	};
}

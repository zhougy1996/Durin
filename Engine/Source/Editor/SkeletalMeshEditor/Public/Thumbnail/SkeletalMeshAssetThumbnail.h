#pragma once

#include "SkeletalMeshEditorAPI.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin::Editor::SkeletalMesh
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
		: public ::Durin::Editor::IAssetThumbnailGenerationInput
	{
	public:
		FSkeletalMeshAssetThumbnailGenerationInput(
			FAssetPath InAssetPath, ::Durin::Editor::FRenderedAssetThumbnailVisualContract InVisual)
			: AssetPath(std::move(InAssetPath)), Visual(std::move(InVisual)) {}
		FAssetPath AssetPath;
		::Durin::Editor::FRenderedAssetThumbnailVisualContract Visual;
	};

	// Captures the mesh, Skeleton, material, and texture package closure before generation.
	class FSkeletalMeshAssetThumbnailProvider final
		: public ::Durin::Editor::IRenderedAssetThumbnailExtension
	{
	public:
		SKELETALMESHEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FAssetThumbnailProviderRegistration override;
		SKELETALMESHEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request, uint64 ProviderGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		SKELETALMESHEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<::Durin::Editor::IRenderedAssetThumbnailGenerationSession> override;
	};
}

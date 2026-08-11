#pragma once

#include "Math/Box.h"
#include "Math/Transform.h"
#include "StaticMeshEditorAPI.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin
{
	// Freezes the initial StaticMesh thumbnail identity and visual policy.
	struct FStaticMeshAssetThumbnailContract
	{
		static constexpr std::string_view AssetClassName = "DStaticMesh";
		static constexpr std::string_view ProviderName = "Durin.StaticMeshThumbnail";
		static constexpr uint32 GeneratorSchemaVersion = 2;
		static constexpr std::string_view PreviewFixtureIdentity =
			"/Engine/Editor/StaticMeshPreview/LOD0DefaultMaterials";
		static constexpr uint32 PreviewFixtureVersion = 2;
		static constexpr uint32 ShaderContractVersion = 1;
		static constexpr double ImageMargin = 0.04;
		static constexpr uint32 LODIndex = 0;
		static constexpr bool bOutputOpaque = false;
	};

	// Supplies card-size-independent inputs for deterministic StaticMesh framing.
	struct FStaticMeshAssetThumbnailViewInput
	{
		FBox LocalBounds;
		double OutputAspectRatio = 1.0;
		double VerticalFieldOfViewDegrees = 42.0;
		FVector3 CameraDirection{2.4, -3.2, 2.4};
		double ImageMargin = FStaticMeshAssetThumbnailContract::ImageMargin;
	};

	// Centers the mesh and describes the complete view derived from its local bounds.
	struct FStaticMeshAssetThumbnailView
	{
		FTransform MeshTransform;
		FVector3 CameraPosition{0.0};
		FVector3 CameraTarget{0.0};
		FVector3 CameraForward{0.0};
		FVector3 CameraRight{0.0};
		FVector3 CameraUp{0.0};
		double CameraDistance = 0.0;
		double NearClipDistance = 0.0;
		double FarClipDistance = 0.0;
	};

	// Carries only the mounted identity and frozen visual inputs across the shared pipeline.
	class FStaticMeshAssetThumbnailGenerationInput final
		: public Editor::IAssetThumbnailGenerationInput
	{
	public:
		FStaticMeshAssetThumbnailGenerationInput(
			FAssetPath InAssetPath,
			Editor::FRenderedAssetThumbnailVisualContract InVisualContract)
			: AssetPath(std::move(InAssetPath))
			, VisualContract(std::move(InVisualContract))
		{
		}

		FAssetPath AssetPath;
		Editor::FRenderedAssetThumbnailVisualContract VisualContract;
	};

	// Captures the exact StaticMesh fingerprint and sorted transitive dependency closure.
	class FStaticMeshAssetThumbnailProvider final : public Editor::IRenderedAssetThumbnailExtension
	{
	public:
		STATICMESHEDITOR_API auto GetRegistration() const
			-> Editor::FAssetThumbnailProviderRegistration override;
		STATICMESHEDITOR_API auto CaptureGenerationRequest(
			const Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		STATICMESHEDITOR_API auto CreateGenerationSession(
			const Editor::FAssetThumbnailGenerationRequest& Request,
			const Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<Editor::IRenderedAssetThumbnailGenerationSession> override;
	};

	// Fits all eight finite, non-degenerate bounds corners inside the requested image margin.
	STATICMESHEDITOR_API auto CalculateStaticMeshAssetThumbnailView(
		const FStaticMeshAssetThumbnailViewInput& Input,
		FStaticMeshAssetThumbnailView& OutView,
		std::string& OutError) -> bool;
} // namespace Durin

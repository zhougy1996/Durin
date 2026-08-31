#pragma once

#include "Math/Box.h"
#include "Math/Transform.h"
#include "StaticMeshEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"

namespace Durin::Editor::StaticMesh
{
	// Freezes the initial StaticMesh thumbnail identity and visual policy.
	struct FStaticMeshThumbnailRendererContract
	{
		static constexpr std::string_view AssetClassName = "DStaticMesh";
		static constexpr std::string_view RendererName = "Durin.StaticMeshThumbnail";
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
	struct FStaticMeshThumbnailRendererViewInput
	{
		FBox LocalBounds;
		double OutputAspectRatio = 1.0;
		double VerticalFieldOfViewDegrees = 42.0;
		FVector3 CameraDirection{2.4, -3.2, 2.4};
		double ImageMargin = FStaticMeshThumbnailRendererContract::ImageMargin;
	};

	// Centers the mesh and describes the complete view derived from its local bounds.
	struct FStaticMeshThumbnailRendererView
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
	class FStaticMeshThumbnailRendererGenerationInput final
		: public ::Durin::Editor::IAssetThumbnailGenerationInput
	{
	public:
		FStaticMeshThumbnailRendererGenerationInput(
			FPackagePath InAssetPath,
			::Durin::Editor::FThumbnailVisualContract InVisualContract)
			: AssetPath(std::move(InAssetPath))
			, VisualContract(std::move(InVisualContract))
		{
		}

		FPackagePath AssetPath;
		::Durin::Editor::FThumbnailVisualContract VisualContract;
	};

	// Captures the exact StaticMesh fingerprint and sorted transitive dependency closure.
	class DStaticMeshThumbnailRenderer final : public ::Durin::Editor::DDefaultSizedThumbnailRenderer
	{
	public:
		STATICMESHEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FThumbnailRenderingInfo override;
		STATICMESHEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 RendererGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		STATICMESHEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession> override;
	};

	// Fits all finite, valid bounds corners, including zero-thickness bounds, inside the image margin.
	STATICMESHEDITOR_API auto CalculateStaticMeshThumbnailRendererView(
		const FStaticMeshThumbnailRendererViewInput& Input,
		FStaticMeshThumbnailRendererView& OutView,
		std::string& OutError) -> bool;
} // namespace Durin::Editor::StaticMesh

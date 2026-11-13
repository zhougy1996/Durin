#pragma once

#include "CoreMinimal.h"

#include "Math/Box.h"
#include "Math/Transform.h"
#include "StaticMeshEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"


namespace Durin::StaticMeshEditor
{
	// Freezes the initial StaticMesh thumbnail identity and visual policy.
	struct FStaticMeshThumbnailRendererContract
	{
		static constexpr std::string_view AssetClassName = "DStaticMesh";
		static constexpr std::string_view RendererName = "Durin.StaticMeshThumbnail";
		static constexpr uint32 GeneratorSchemaVersion = 2;
		static constexpr std::string_view PreviewFixtureIdentity =
			"/Engine/Editor/StaticMeshPreview/LOD0DefaultMaterials";
		static constexpr uint32 PreviewFixtureVersion = 3;
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
		: public IAssetThumbnailGenerationInput
	{
	public:
		FStaticMeshThumbnailRendererGenerationInput(
			FTopLevelAssetPath InAssetPath,
			FThumbnailVisualContract InVisualContract)
			: AssetPath(std::move(InAssetPath))
			, VisualContract(std::move(InVisualContract))
		{
		}

		FTopLevelAssetPath AssetPath;
		FThumbnailVisualContract VisualContract;
	};

	// Fits all finite, valid bounds corners, including zero-thickness bounds, inside the image margin.
	STATICMESHEDITOR_API auto CalculateStaticMeshThumbnailRendererView(
		const FStaticMeshThumbnailRendererViewInput& Input)
		-> std::expected<FStaticMeshThumbnailRendererView, std::string>;
} // namespace Durin::StaticMeshEditor

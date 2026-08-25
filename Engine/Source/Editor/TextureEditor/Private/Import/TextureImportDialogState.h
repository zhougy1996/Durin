#pragma once

#include "Editor/Import/ImportDialogSupport.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	enum class ETextureUsage : uint8;
	enum class EVolumeTextureSourceChannels : uint8;
}

namespace Durin::Editor::Texture
{
	using namespace ::Durin::Editor::Import;

	// Selects the concrete texture asset created by the unified import workflow.
	enum class ETextureImportAssetType : uint8
	{
		Texture2D,
		TextureCube,
		VolumeTexture
	};

	struct FTexture2DImportFormState
	{
		auto Reset() -> void;

		FMountedSourceImportFormModel Source;
		ETextureUsage Usage = static_cast<ETextureUsage>(0);
	};

	struct FVolumeTextureImportFormState
	{
		auto Reset() -> void;

		FMountedSourceImportFormModel Source;
		EVolumeTextureSourceChannels Channels =
			static_cast<EVolumeTextureSourceChannels>(0);
		uint32 SliceWidth = 128;
		uint32 SliceHeight = 128;
		uint32 Depth = 128;
		uint32 TilesX = 12;
		uint32 TilesY = 12;
	};

	struct FTextureCubeImportFormState
	{
		auto Reset() -> void;

		std::array<std::array<char, 512>, TextureCubeFaceCount> FacePathBuffers{};
		std::array<std::array<char, 512>, TextureCubeFaceCount> FaceDestinationBuffers{};
		std::array<std::string, TextureCubeFaceCount> LastSuggestedFaceDestinations;
		std::array<char, 512> PanoramaPathBuffer{};
		std::array<char, 512> PanoramaDestinationBuffer{};
		std::string LastSuggestedPanoramaDestination;
		std::string SourceValidationMessage;
		ETextureCubeSourceLayout SourceLayout =
			ETextureCubeSourceLayout::EquirectangularPanorama;
		uint32 PanoramaFaceDimension = 0;
		uint32 PanoramaCustomFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		uint32 ValidatedSourceWidth = 0;
		uint32 ValidatedSourceHeight = 0;
		uint32 ValidatedDimension = 0;
		uint32 ValidatedMipCount = 0;
		EPixelFormat ValidatedPixelFormat = EPixelFormat::Unknown;
		bool bValidatedHDR = false;
		bool bSourcesValid = false;
	};

	// Owns reset and inactive-form retention for one unified texture import modal.
	class FTextureImportDialogState
	{
	public:
		auto Reset() -> void;

		auto GetAssetType() const -> ETextureImportAssetType { return AssetType; }
		auto SetAssetType(ETextureImportAssetType InAssetType) -> void
		{
			AssetType = InAssetType;
		}
		auto GetSourceMode() const -> EMountedSourceImportMode { return SourceMode; }
		auto SetSourceMode(EMountedSourceImportMode InSourceMode) -> void
		{
			SourceMode = InSourceMode;
		}
		auto GetTexture2D() -> FTexture2DImportFormState& { return Texture2D; }
		auto GetTexture2D() const -> const FTexture2DImportFormState& { return Texture2D; }
		auto GetTextureCube() -> FTextureCubeImportFormState& { return TextureCube; }
		auto GetTextureCube() const -> const FTextureCubeImportFormState& { return TextureCube; }
		auto GetVolumeTexture() -> FVolumeTextureImportFormState& { return VolumeTexture; }
		auto GetVolumeTexture() const -> const FVolumeTextureImportFormState& { return VolumeTexture; }

	private:
		ETextureImportAssetType AssetType = ETextureImportAssetType::Texture2D;
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
		FTexture2DImportFormState Texture2D;
		FTextureCubeImportFormState TextureCube;
		FVolumeTextureImportFormState VolumeTexture;
	};
} // namespace Durin::Editor::Texture

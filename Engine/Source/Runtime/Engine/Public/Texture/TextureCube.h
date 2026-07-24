#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "RHIDefinitions.h"
#include "Texture/Texture2D.h"

#include "TextureCube.gen.h"

namespace Durin
{
	class FTextureCubeRenderResource;

	struct FTextureCubeSourceData
	{
		std::array<FTextureSourceData, TextureCubeFaceCount> Faces;

		ENGINE_API auto IsValid() const -> bool;
	};

	struct FTextureCubePlatformData
	{
		std::array<FTexturePlatformData, TextureCubeFaceCount> Faces;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
	};

	struct FTextureCubeImportSettings
	{
		bool bSRGB = true;
	};

	struct FTextureCubeImportResult;

	DCLASS()
	class DTextureCube : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTextureCube(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTextureCube() override;

		ENGINE_API auto GetSourceFile(ETextureCubeFace Face) const -> const std::string&;
		auto GetSourceData() const -> const FTextureCubeSourceData* { return SourceData.get(); }
		auto GetPlatformData() const -> const FTextureCubePlatformData* { return PlatformData.get(); }
		auto GetRenderResource() const -> const std::shared_ptr<FTextureCubeRenderResource>& { return RenderResource; }
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }

		ENGINE_API auto RebuildPlatformData(std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto RefreshBuildStatus() -> void;

		ENGINE_API static auto ImportAsset(
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			std::string_view AssetPath,
			const FTextureCubeImportSettings& Settings = {}) -> FTextureCubeImportResult;

	private:
		auto GetMutableSourceFile(ETextureCubeFace Face) -> std::string&;
		auto InvalidatePlatformData() -> void;
		auto QueueRenderResourceBuild() -> void;

		DPROPERTY(DisplayName = "Positive X Source")
		std::string PositiveXSourceFile;

		DPROPERTY(DisplayName = "Negative X Source")
		std::string NegativeXSourceFile;

		DPROPERTY(DisplayName = "Positive Y Source")
		std::string PositiveYSourceFile;

		DPROPERTY(DisplayName = "Negative Y Source")
		std::string NegativeYSourceFile;

		DPROPERTY(DisplayName = "Positive Z Source")
		std::string PositiveZSourceFile;

		DPROPERTY(DisplayName = "Negative Z Source")
		std::string NegativeZSourceFile;

		DPROPERTY()
		bool bSRGB = true;

		std::unique_ptr<FTextureCubeSourceData> SourceData;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		std::shared_ptr<FTextureCubeRenderResource> RenderResource;
		uint64 BuildRevision = 0;
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;
	};

	struct FTextureCubeImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTextureCube* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}

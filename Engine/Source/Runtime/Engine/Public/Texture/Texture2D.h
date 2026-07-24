#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "PixelFormat.h"

#include "Texture2D.gen.h"

namespace Durin
{
	class FTexture2DRenderResource;

	enum class ETextureSourceFormat : uint8
	{
		Invalid,
		RGBA8
	};

	DENUM()
	enum class ETextureUsage : uint8
	{
		Color,
		Normal,
		DataMask
	};

	struct ENGINE_API FTextureSourceData
	{
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;
		bool bHasTransparency = false;

		auto IsValid() const -> bool;
	};

	struct ENGINE_API FTexture2DMipData
	{
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 RowPitch = 0;

		auto IsValid(EPixelFormat PixelFormat) const -> bool;
	};

	struct ENGINE_API FTexturePlatformData
	{
		std::vector<FTexture2DMipData> Mips;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		auto IsValid() const -> bool;
	};

	class DTexture2D;

	struct FTexture2DImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTexture2D* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FTexture2DImportSettings
	{
		ETextureUsage Usage = ETextureUsage::Color;

		// Empty selects the preset default. Keeping this override explicit prevents a
		// usage change from silently preserving an incompatible color space.
		std::optional<bool> bSRGB;
	};

	DCLASS()
	class ENGINE_API DTexture2D : public DObject
	{
		GENERATED_BODY()
	public:
		enum class ETextureBuildStatus : uint8
		{
			Unbuilt,           // PostLoad has not run or platform data was invalidated
			Ready,             // Fully built and render resource queued
			MissingSource,     // SourceFile is empty or the file does not exist
			DecodeFailure,     // Image decoding failed (unsupported/corrupt file)
			BuildFailure,      // FTexturePlatformData construction failed
			UploadFailure,     // RHI texture creation or mip upload failed
			UnsupportedFormat, // Selected pixel format unsupported by current RHI
		};

		explicit DTexture2D(const FObjectInitializer& ObjectInitializer);
		~DTexture2D() override;

		auto GetSourceFile() const -> const std::string& { return SourceFile; }
		auto GetSourceData() const -> const FTextureSourceData* { return SourceData.get(); }
		auto GetPlatformData() const -> const FTexturePlatformData* { return PlatformData.get(); }
		auto GetRenderResource() const -> const std::shared_ptr<FTexture2DRenderResource>& { return RenderResource; }
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }
		auto GetUsage() const -> ETextureUsage { return Usage; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto GetBuildStatus() const -> ETextureBuildStatus { return BuildStatus; }
		auto GetLastBuildError() const -> const std::string& { return LastBuildError; }
		auto SetUsage(ETextureUsage InUsage, std::string& OutError) -> bool;
		auto SetSRGB(bool bInSRGB, std::string& OutError) -> bool;

		auto RebuildPlatformData(std::string& OutError) -> bool;
		auto PostLoad(std::string& OutError) -> bool override;
		auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		auto RefreshBuildStatus() -> void;

		static auto ImportAsset(std::string_view FilePath, std::string_view AssetPath, const FTexture2DImportSettings& Settings = {}) -> FTexture2DImportResult;

	private:
		auto BuildSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto BuildPlatformData(ETextureUsage InUsage, bool bInSRGB,
			std::unique_ptr<FTexturePlatformData>& OutPlatformData, std::string& OutError) const -> bool;
		auto InvalidatePlatformData() -> void;
		auto QueueRenderResourceBuild() -> void;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		ETextureUsage Usage = ETextureUsage::Color;

		DPROPERTY()
		bool bSRGB = true;

		// Both representations are derived from the imported source file. Keeping them
		// separate lets platform builds replace format/mips without mutating edit data.
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;

		// The shared proxy can outlive this UObject while queued render commands drain.
		// Its RHI member is intentionally never accessed through DTexture2D.
		std::shared_ptr<FTexture2DRenderResource> RenderResource;
		uint64 BuildRevision = 0;

		// Persistent failure state. Set by the build pipeline and cleared on success.
		ETextureBuildStatus BuildStatus = ETextureBuildStatus::Unbuilt;
		std::string LastBuildError;

		// Reflected edits validate and build detached settings before live storage
		// changes. PostEditChangeProperty consumes this exact candidate atomically.
		std::unique_ptr<FTexturePlatformData> PendingEditPlatformData;
		ETextureUsage PendingEditUsage = ETextureUsage::Color;
		bool bPendingEditSRGB = true;
	};
}

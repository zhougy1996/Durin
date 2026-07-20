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
		// Color textures normally contain sRGB-encoded bytes. Data textures must opt
		// into linear sampling so the GPU does not apply an sRGB decode.
		bool bSRGB = true;
	};

	DCLASS()
	class ENGINE_API DTexture2D : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DTexture2D(const FObjectInitializer& ObjectInitializer);
		~DTexture2D() override;

		auto GetSourceFile() const -> const std::string& { return SourceFile; }
		auto GetSourceData() const -> const FTextureSourceData* { return SourceData.get(); }
		auto GetPlatformData() const -> const FTexturePlatformData* { return PlatformData.get(); }
		auto GetRenderResource() const -> const std::shared_ptr<FTexture2DRenderResource>& { return RenderResource; }
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }
		auto IsSRGB() const -> bool { return bSRGB; }
		auto SetSRGB(bool bInSRGB, std::string& OutError) -> bool;

		auto RebuildPlatformData(std::string& OutError) -> bool;
		auto PostLoad(std::string& OutError) -> bool override;

		static auto ImportAsset(std::string_view FilePath, std::string_view AssetPath, const FTexture2DImportSettings& Settings = {}) -> FTexture2DImportResult;

	private:
		auto BuildSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto InvalidatePlatformData() -> void;
		auto QueueRenderResourceBuild() -> void;

		DPROPERTY()
		std::string SourceFile;

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
	};
}

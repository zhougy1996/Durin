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

		auto RebuildPlatformData(std::string& OutError) -> bool;
		auto PostLoad(std::string& OutError) -> bool override;

		static auto ImportAsset(std::string_view FilePath, std::string_view AssetPath) -> FTexture2DImportResult;

	private:
		auto BuildSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto InvalidatePlatformData() -> void;
		auto QueueRenderResourceBuild() -> void;

		DPROPERTY()
		std::string SourceFile;

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

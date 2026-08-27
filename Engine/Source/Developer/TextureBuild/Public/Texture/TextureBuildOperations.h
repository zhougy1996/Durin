#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2D.h"

namespace Durin::Asset
{
	struct FTexture2DBuildSettings
	{
		ETextureUsage Usage = ETextureUsage::Color;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		uint32 MaxResolution = 0;
		std::optional<bool> bSRGB;
	};

	// Source-format-neutral, owned worker input. Translation providers supply the
	// decoded pixels and the captured source-content identity.
	struct FTexture2DBuildRequest
	{
		FTextureSourceData SourceData;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		FTexture2DBuildSettings Settings;
		bool bPersistDerivedData = true;
	};

	struct FTexture2DRecipeMetrics
	{
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PersistenceNanoseconds = 0;
		uint64 PeakIntermediateBytes = 0;
	};

	struct FTexture2DBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
		std::function<void()> OnPersisting;
		FTexture2DRecipeMetrics* Metrics = nullptr;
	};

	// Detached worker output. Publishing this value is a separate main-thread
	// operation and the worker never observes an asset object.
	struct FTexture2DBuildProduct
	{
		FTextureSourceData SourceData;
		FTexturePlatformData PlatformData;
		std::string DerivedDataKey;
		std::string PersistenceDiagnostic;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		FTexture2DBuildSettings Settings;
		bool bSRGB = true;
	};

	struct FTexture2DPublicationContext
	{
		bool bMarkPackageDirty = true;
		bool bReportLoadMutation = false;
		bool bSourceDecoderInvoked = true;
	};

	TEXTUREBUILD_API auto BuildTexture2D(
		FTexture2DBuildRequest Request,
		FTexture2DBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr) -> bool;

	TEXTUREBUILD_API auto PublishTexture2DProduct(
		DTexture2D& Texture,
		FTexture2DBuildProduct Product,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool;

	// Synchronous creation-side entrypoint. Detached product details remain an
	// implementation concern for callers that only need to populate a new asset.
	TEXTUREBUILD_API auto BuildTexture2DInto(
		DTexture2D& Texture,
		FTexture2DBuildRequest Request,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool;

	// Build-owned recipe identity and DDC value loading used by uncooked source processing
	// loads. Source translation remains outside this module.
	TEXTUREBUILD_API auto MakeTexture2DDerivedDataKey(
		const DTexture2D& Texture,
		std::string& OutKey,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto LoadTexture2DDerivedData(
		std::string_view Key,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool;

}

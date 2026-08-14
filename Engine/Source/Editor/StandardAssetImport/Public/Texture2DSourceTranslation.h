#pragma once

#include "StandardAssetImportAPI.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoringService.h"

namespace Durin::Asset::Import::Standard
{
	STANDARDASSETIMPORT_API auto IsTexture2DSourceExtension(
		std::string_view Extension) -> bool;
	// Translates one concrete encoded image into Engine's normalized RGBA8 source value.
	STANDARDASSETIMPORT_API auto TranslateTexture2DSource(
		std::span<const uint8> EncodedBytes,
		FTextureSourceData& OutSourceData,
		std::string& OutError) -> bool;

	// Standard image-provider adapter: translate, build a detached product, then
	// publish it to a main-thread candidate object.

	STANDARDASSETIMPORT_API auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FTexture2DImportResult;

	STANDARDASSETIMPORT_API auto ReimportTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto ChangeTexture2DSourceReference(
		DTexture2D& Texture,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto IngestAndChangeTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto RepairTexture2DSourcePath(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto ChangeTexture2DSourceLocation(
		DTexture2D& Texture,
		std::string_view SourceDestination,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool;
}

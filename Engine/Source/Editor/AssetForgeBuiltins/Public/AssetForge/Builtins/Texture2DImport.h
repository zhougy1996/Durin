#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto IsTexture2DSourceExtension(
		std::string_view Extension) -> bool;
	// Translates one concrete encoded image into Engine's normalized RGBA8 source value.
	ASSETFORGEBUILTINS_API auto TranslateTexture2DSource(
		FByteView EncodedBytes,
		FTextureSourceData& OutSourceData,
		std::string& OutError) -> bool;

	// Reimports from the retained optional source hint. Completion runs on the
	// game thread after the detached candidate is either published or rejected.
	ASSETFORGEBUILTINS_API auto ReimportTexture2D(
		DTexture2D& Texture,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion = {}) -> bool;
	// Selects and captures a new source, then atomically publishes canonical
	// imported data and the new hint only after the detached build succeeds.
	ASSETFORGEBUILTINS_API auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion = {}) -> bool;
	// Rebuilds one packaged texture from its resident canonical imported data.
	ASSETFORGEBUILTINS_API auto RebuildTexture2DFromImportedData(
		DTexture2D& Texture,
		const FTexture2DBuildSettings& Settings,
		std::string& OutError,
		ETexture2DCompilationPriority Priority =
			ETexture2DCompilationPriority::Interactive,
		FTexture2DCompilationCompletion Completion = {}) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool;
}

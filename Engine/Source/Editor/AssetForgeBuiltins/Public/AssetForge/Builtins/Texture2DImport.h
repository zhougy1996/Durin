#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Texture/Texture2D.h"
#include "Hash/XxHash.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::AssetForge::Builtins
{
	// Detached source capture. Safe to prepare on a worker; no object or mount access.
	struct FPreparedTexture2DImport
	{
		std::string Filename;
		FTextureSource Source;
		FXxHash128 ContentHash{};
		uint64 ByteCount = 0;
		FTexture2DImportSettings InferredSettings;
	};
	ASSETFORGEBUILTINS_API auto PrepareTexture2DImport(std::string_view Filename,
		FPreparedTexture2DImport& OutPrepared, std::string& OutError) -> bool;

	// Conservative first-import defaults from the filename's final semantic token,
	// then optional decoded-source normal detection. Flat colors stay ambiguous.
	// Reimport preserves the asset's settings instead of inferring them again.
	ASSETFORGEBUILTINS_API auto InferTexture2DImportSettings(
		std::string_view Filename, const FTextureSource* Source = nullptr)
		-> FTexture2DImportSettings;

	ASSETFORGEBUILTINS_API auto IsTexture2DSourceExtension(
		std::string_view Extension) -> bool;
	// Decodes one image into detached authored source, retaining original channel metadata.
	ASSETFORGEBUILTINS_API auto TranslateTexture2DSource(
		FByteView EncodedBytes,
		FTextureSource& OutSourceData,
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
	ASSETFORGEBUILTINS_API auto RebuildTexture2DFromSource(
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

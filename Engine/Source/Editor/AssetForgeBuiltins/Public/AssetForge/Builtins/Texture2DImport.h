#pragma once

#include "CoreMinimal.h"

#include "AssetForgeBuiltinsAPI.h"
#include "Texture/Texture2D.h"
#include "Image/ImageDecoder.h"
#include "Asset/SourceHint.h"
#include "Misc/MountPaths.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/TextureBuildOperation.h"

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
	[[nodiscard]] ASSETFORGEBUILTINS_API auto PrepareTexture2DImport(std::string_view Filename)
		-> std::expected<FPreparedTexture2DImport, std::string>;

	// Conservative first-import defaults from the filename's final semantic token,
	// then optional decoded-source normal detection. Flat colors stay ambiguous.
	// Reimport preserves the asset's settings instead of inferring them again.
	ASSETFORGEBUILTINS_API auto InferTexture2DImportSettings(
		std::string_view Filename, const FTextureSource* Source = nullptr)
		-> FTexture2DImportSettings;

	ASSETFORGEBUILTINS_API auto IsTexture2DSourceExtension(
		std::string_view Extension) -> bool;
	using FTexture2DTranslationResult = std::expected<FTextureSource, std::string>;
	using FTexture2DPreparationResult = std::expected<FPreparedTexture2DImport, std::string>;
	using FTexture2DSubmissionResult = std::expected<void, std::string>;

	// Import completion includes source metadata publication and optional persistence.

	using FTexture2DImportCompletion = std::function<void(FTextureOperationResult)>;

	// Decodes one image into detached authored source, retaining original channel metadata.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto TranslateTexture2DSource(
		FByteView EncodedBytes) -> FTexture2DTranslationResult;

	// Reimports from the retained optional source hint. Completion runs on the
	// game thread after the detached candidate is either published or rejected.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportTexture2D(
		DTexture2D& Texture,
		FTexture2DImportCompletion Completion = {}) -> FTexture2DSubmissionResult;
	// Selects and captures a new source, then atomically publishes canonical
	// imported data and the new hint only after the detached build succeeds.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath,
		FTexture2DImportCompletion Completion = {}) -> FTexture2DSubmissionResult;
	// Rebuilds one packaged texture from its resident canonical imported data.
	ASSETFORGEBUILTINS_API auto RebuildTexture2DFromSource(
		DTexture2D& Texture,
		const FTexture2DBuildSettings& Settings,
		ETexture2DCompilationPriority Priority =
			ETexture2DCompilationPriority::Interactive,
		FTexture2DCompilationCompletion Completion = {}) -> std::expected<void, FTexture2DCompilationError>;
	ASSETFORGEBUILTINS_API auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage) -> std::expected<void, FTexture2DCompilationError>;
	ASSETFORGEBUILTINS_API auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB) -> std::expected<void, FTexture2DCompilationError>;
	ASSETFORGEBUILTINS_API auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution) -> std::expected<void, FTexture2DCompilationError>;
	ASSETFORGEBUILTINS_API auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality) -> std::expected<void, FTexture2DCompilationError>;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode) -> std::expected<void, FTexture2DCompilationError>;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold) -> std::expected<void, FTexture2DCompilationError>;
}

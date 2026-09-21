#pragma once

#include <expected>

#include "AssetForgeBuiltinsAPI.h"
#include "Texture/Texture2D.h"
#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Asset/SourceHint.h"
#include "Misc/MountPaths.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::AssetForge::Builtins
{
	struct FTexture2DPreparationError;
	struct FEncodedSourceError;

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
		-> std::expected<FPreparedTexture2DImport, FTexture2DPreparationError>;

	// Conservative first-import defaults from the filename's final semantic token,
	// then optional decoded-source normal detection. Flat colors stay ambiguous.
	// Reimport preserves the asset's settings instead of inferring them again.
	ASSETFORGEBUILTINS_API auto InferTexture2DImportSettings(
		std::string_view Filename, const FTextureSource* Source = nullptr)
		-> FTexture2DImportSettings;

	ASSETFORGEBUILTINS_API auto IsTexture2DSourceExtension(
		std::string_view Extension) -> bool;
	enum class ETexture2DTranslationError : uint8 { None, Decode, Dimensions, Image, Source };
	struct FTexture2DTranslationError
	{
		ETexture2DTranslationError Code = ETexture2DTranslationError::None;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		std::optional<Image::FImageDecodeError> DecodeCause;
	};
	using FTexture2DTranslationResult = std::expected<FTextureSource, FTexture2DTranslationError>;
	ASSETFORGEBUILTINS_API auto FormatTexture2DTranslationError(const FTexture2DTranslationError& Error) -> std::string;

	enum class ETexture2DPreparationError : uint8 { None, Path, Format, Capture, Translation };
	struct FTexture2DPreparationError
	{
		ETexture2DPreparationError Code = ETexture2DPreparationError::None;
		std::string Filename;
		std::error_code SystemError;
		std::shared_ptr<const FEncodedSourceError> CaptureCause;
		std::optional<FTexture2DTranslationError> TranslationCause;
	};
	using FTexture2DPreparationResult = std::expected<FPreparedTexture2DImport, FTexture2DPreparationError>;
	ASSETFORGEBUILTINS_API auto FormatTexture2DPreparationError(const FTexture2DPreparationError& Error) -> std::string;

	// Decodes one image into detached authored source, retaining original channel metadata.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto TranslateTexture2DSource(
		FByteView EncodedBytes) -> FTexture2DTranslationResult;

	enum class ETexture2DSubmissionError : uint8
	{
		None, Package, Mount, SourceHint, SourceFile, Capture, Translation, Compilation, MissingSource
	};
	struct FTexture2DSubmissionError
	{
		ETexture2DSubmissionError Code = ETexture2DSubmissionError::None;
		std::string ObjectPath;
		std::string Filename;
		std::string PackagePath;
		EMountPathError MountCause = EMountPathError::None;
		std::optional<FSourceHintError> SourceHintCause;
		std::shared_ptr<const FEncodedSourceError> CaptureCause;
		std::optional<FTexture2DTranslationError> TranslationCause;
		std::optional<FTexture2DCompilationError> CompilationCause;
	};
	using FTexture2DSubmissionResult = std::expected<void, FTexture2DSubmissionError>;
	ASSETFORGEBUILTINS_API auto FormatTexture2DSubmissionError(const FTexture2DSubmissionError& Error) -> std::string;

	// Reimports from the retained optional source hint. Completion runs on the
	// game thread after the detached candidate is either published or rejected.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportTexture2D(
		DTexture2D& Texture,
		FTexture2DCompilationCompletion Completion = {}) -> FTexture2DSubmissionResult;
	// Selects and captures a new source, then atomically publishes canonical
	// imported data and the new hint only after the detached build succeeds.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath,
		FTexture2DCompilationCompletion Completion = {}) -> FTexture2DSubmissionResult;
	// Rebuilds one packaged texture from its resident canonical imported data.
	ASSETFORGEBUILTINS_API auto RebuildTexture2DFromSource(
		DTexture2D& Texture,
		const FTexture2DBuildSettings& Settings,
		ETexture2DCompilationPriority Priority =
			ETexture2DCompilationPriority::Interactive,
		FTexture2DCompilationCompletion Completion = {}) -> FTexture2DCompilationOperationResult;
	ASSETFORGEBUILTINS_API auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage) -> FTexture2DCompilationOperationResult;
	ASSETFORGEBUILTINS_API auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB) -> FTexture2DCompilationOperationResult;
	ASSETFORGEBUILTINS_API auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution) -> FTexture2DCompilationOperationResult;
	ASSETFORGEBUILTINS_API auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality) -> FTexture2DCompilationOperationResult;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode) -> FTexture2DCompilationOperationResult;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold) -> FTexture2DCompilationOperationResult;
}

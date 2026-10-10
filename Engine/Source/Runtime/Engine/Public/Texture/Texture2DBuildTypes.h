#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "EngineAPI.h"
#include "Texture/Texture2DData.h"

namespace Durin
{
	// Value-owned settings frozen before a Texture2D build runs.
	struct FTexture2DBuildSettings
	{
		ETextureUsage Usage = ETextureUsage::Color;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;
		uint32 MaxResolution = 0;
		std::optional<bool> bSRGB;

		auto operator==(const FTexture2DBuildSettings&) const -> bool = default;
	};

	enum class ETexture2DInputError : uint8
	{
		None, EmptyMips, InvalidImage, UnsupportedFormat, UnsupportedShape, ExcessiveResolution,
		InvalidMipDimensions, GammaMismatch, SourceBudgetExceeded, MipAfterTerminal,
		InvalidUsage, InvalidCompressionQuality, InvalidAlphaMipMode, InvalidAlphaCoverageThreshold,
	};
	struct FTexture2DInputError
	{
		ETexture2DInputError Code = ETexture2DInputError::None;
		uint64 Index = 0;
		uint64 Bytes = 0;
		Image::FImageInfo Actual;
		Image::FImageInfo Base;
		FTexture2DBuildSettings Settings;
	};
	ENGINE_API auto FormatTexture2DInputError(const FTexture2DInputError& Error) -> std::string;

	// Recipe-local timing and generated storage; excludes shared source/output bytes.
	struct FTexture2DBuildTimings
	{
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PeakIntermediateBytes = 0;
	};

	struct FTexture2DBuildInput
	{
		std::span<const Image::FImage> SourceMips;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	struct FTexture2DBuildOutput
	{
		FTexturePlatformData PlatformData;
		FTexture2DBuildTimings Metrics;
	};

	enum class ETaskState : uint8;
	// Engine orchestration diagnostics; recipes report failures through module logs.
	enum class ETexture2DBuildError : uint8
	{
		None, InvalidInput,
		MissingSourceIdentity, AuthoredBuildUnavailable, InvalidBuilderVersion, Cancelled,
		InvalidBuilderProduct, ModuleUnavailable,
	};
	struct FTexture2DBuildError
	{
		ETexture2DBuildError Code = ETexture2DBuildError::None;
		std::optional<FTexture2DInputError> InputCause;
		std::string Description; // Already formatted at the generic build boundary.
	};
	ENGINE_API auto FormatTexture2DBuildError(const FTexture2DBuildError& Error) -> std::string;

	ENGINE_API auto ValidateTexture2DSourceMips(
		std::span<const Image::FImage> Mips) -> std::expected<void, FTexture2DInputError>;

	ENGINE_API auto ValidateTexture2DBuildSettings(
		const FTexture2DBuildSettings& Settings) -> std::expected<void, FTexture2DInputError>;

	ENGINE_API auto ResolveTexture2DSRGB(
		const FTexture2DBuildSettings& Settings) -> bool;

}

#endif

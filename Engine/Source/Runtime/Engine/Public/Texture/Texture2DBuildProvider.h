#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Texture/Texture2DData.h"

namespace Durin
{
	// Value-owned settings frozen before a Texture2D build enters provider code.
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
	struct FTexture2DInputResult
	{
		FTexture2DInputError Error;
		auto Succeeded() const -> bool { return Error.Code == ETexture2DInputError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	ENGINE_API auto FormatTexture2DInputError(const FTexture2DInputError& Error) -> std::string;

	struct FTexture2DBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 BuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && BuilderVersion != 0;
		}

		auto operator==(const FTexture2DBuildProviderDescriptor&) const -> bool = default;
	};

	struct FTexture2DRecipeMetrics
	{
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PeakIntermediateBytes = 0;
	};

	struct FTexture2DRecipeBuildRequest
	{
		std::span<const Image::FImage> SourceMips;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	struct FTexture2DRecipeBuildProduct
	{
		FTexturePlatformData PlatformData;
		FTexture2DRecipeMetrics Metrics;
	};

	enum class ETexture2DBuildStatus : uint8
	{
		Succeeded,
		Failed,
		Cancelled
	};

	enum class ETaskState : uint8;
	enum class ETexture2DBuildError : uint8
	{
		None, InvalidInput, CompressionTaskFailed,
		MissingSourceIdentity, AuthoredBuildUnavailable, InvalidProviderDescriptor, Cancelled,
		InvalidProviderProduct, ProviderUnavailable, AmbiguousProvider, ProviderInvocationFailed,
		ProviderFailed, UnsupportedTarget, CompressedLayoutOverflow, InvalidCompressionQuality,
		InvalidUsage, InvalidAlphaMipMode, InvalidAlphaCoverageThreshold, UnsupportedPixelFormat,
		InvalidMipLayout, InvalidPlatformData,
	};
	struct FTexture2DBuildError
	{
		ETexture2DBuildError Code = ETexture2DBuildError::None;
		std::optional<FTexture2DInputError> InputCause;
		std::optional<ETaskState> TaskState;
	};
	ENGINE_API auto FormatTexture2DBuildError(const FTexture2DBuildError& Error) -> std::string;

	struct FTexture2DBuildResult
	{
		ETexture2DBuildStatus Status = ETexture2DBuildStatus::Failed;
		FTexture2DBuildError Error;

		explicit operator bool() const
		{
			return Status == ETexture2DBuildStatus::Succeeded;
		}
	};

	struct FTexture2DRecipeExecutionControl
	{
		std::function<bool()> ShouldCancel;
		FTexture2DRecipeMetrics* Metrics = nullptr;
	};

	ENGINE_API auto ValidateTexture2DSourceMips(
		std::span<const Image::FImage> Mips) -> FTexture2DInputResult;

	ENGINE_API auto ValidateTexture2DBuildSettings(
		const FTexture2DBuildSettings& Settings) -> FTexture2DInputResult;
	ENGINE_API auto ResolveTexture2DSRGB(
		const FTexture2DBuildSettings& Settings) -> bool;

	// Synchronous pure-recipe seam invoked by an Engine-owned worker.
	class ITexture2DBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.Texture2DBuildProvider";
		static constexpr uint32 FeatureVersion = 5;

		virtual auto GetDescriptor() const -> FTexture2DBuildProviderDescriptor = 0;
		virtual auto Build(
			const FTexture2DRecipeBuildRequest& Request,
			FTexture2DRecipeBuildProduct& OutProduct,
			const FTexture2DRecipeExecutionControl* ExecutionControl = nullptr) -> FTexture2DBuildResult = 0;
	};

}

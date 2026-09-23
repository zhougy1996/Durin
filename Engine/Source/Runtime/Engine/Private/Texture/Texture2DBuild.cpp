#include "Texture/Texture2DBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"

namespace Durin
{
	auto FormatTexture2DBuildError(const FTexture2DBuildError& Error) -> std::string
	{
		if (Error.InputCause) return FormatTexture2DInputError(*Error.InputCause);
		switch (Error.Code)
		{
		case ETexture2DBuildError::None: return {};
		case ETexture2DBuildError::InvalidInput: return "Texture build input is invalid.";
		case ETexture2DBuildError::CompressionTaskFailed: return "Texture compression task failed.";
		case ETexture2DBuildError::MissingSourceIdentity: return "Texture2D source identity is missing.";
		case ETexture2DBuildError::AuthoredBuildUnavailable: return "Texture2D authored build orchestration is unavailable outside editor builds.";
		case ETexture2DBuildError::InvalidBuilderDescriptor: return "The Texture2D builder descriptor is invalid.";
		case ETexture2DBuildError::Cancelled: return "Texture2D build was cancelled.";
		case ETexture2DBuildError::InvalidBuilderProduct: return "Texture2D builder returned invalid platform data.";
		case ETexture2DBuildError::ModuleUnavailable: return "The TextureBuild module is unavailable.";
		case ETexture2DBuildError::UnsupportedTarget: return "Texture2D build target is unsupported.";
		case ETexture2DBuildError::CompressedLayoutOverflow: return "Compressed texture mip layout exceeds supported limits.";
		case ETexture2DBuildError::UnsupportedPixelFormat: return "Selected pixel format is not supported by the current RHI backend.";
		case ETexture2DBuildError::InvalidMipLayout: return "Generated texture mip layout is invalid.";
		case ETexture2DBuildError::InvalidPlatformData: return "Failed to build texture platform data.";
		}
		return {};
	}

	auto FormatTexture2DInputError(const FTexture2DInputError& Error) -> std::string
	{
		if (Error.Code == ETexture2DInputError::None) return {};
		if (Error.Code == ETexture2DInputError::EmptyMips) return "Texture2D source mip chain is empty.";
		if (Error.Code >= ETexture2DInputError::InvalidUsage) return "Texture2D build settings are invalid.";
		return "Texture2D source mip chain is invalid.";
	}

	auto ValidateTexture2DSourceMips(std::span<const Image::FImage> Mips) -> std::expected<void, FTexture2DInputError>
	{
		if (Mips.empty()) return std::unexpected(FTexture2DInputError{.Code = ETexture2DInputError::EmptyMips});
		const auto& Base = Mips.front().GetInfo();
		uint64 Bytes = 0;
		for (size_t Index = 0; Index < Mips.size(); ++Index)
		{
			const auto& Info = Mips[Index].GetInfo();
			Bytes += Mips[Index].GetPixels().size();
			ETexture2DInputError Code = ETexture2DInputError::None;
			if (!Mips[Index].IsValid()) Code = ETexture2DInputError::InvalidImage;
			else if (Info.Format != Image::ERawImageFormat::RGBA8) Code = ETexture2DInputError::UnsupportedFormat;
			else if (Info.Depth != 1 || Info.SliceCount != 1) Code = ETexture2DInputError::UnsupportedShape;
			else if (Base.Width > 16384 || Base.Height > 16384) Code = ETexture2DInputError::ExcessiveResolution;
			else if (Info.Width != std::max(1u, Base.Width >> std::min<size_t>(Index, 31))
				|| Info.Height != std::max(1u, Base.Height >> std::min<size_t>(Index, 31))) Code = ETexture2DInputError::InvalidMipDimensions;
			else if (Info.GammaSpace != Base.GammaSpace) Code = ETexture2DInputError::GammaMismatch;
			else if (Bytes > MaximumTextureSourceBytes) Code = ETexture2DInputError::SourceBudgetExceeded;
			else if (Index > 0 && Mips[Index - 1].GetInfo().Width == 1
				&& Mips[Index - 1].GetInfo().Height == 1) Code = ETexture2DInputError::MipAfterTerminal;
			if (Code != ETexture2DInputError::None)
				return std::unexpected(FTexture2DInputError{.Code = Code, .Index = Index, .Bytes = Bytes, .Actual = Info, .Base = Base});
		}
		return {};
	}

	auto MakeTexture2DBuildRequest(const FTextureSource& Source,
		const FTexture2DBuildSettings& Settings) -> FTexture2DBuildRequest
	{
		FTexture2DBuildRequest Result{.Settings = Settings};
		if (!Source.IsValid() || Source.GetKind() != ETextureSourceKind::Texture2D
			|| Source.GetBlocks().size() != 1 || Source.GetLayers().size() != 1) return Result;
		const auto Mips = Source.GetMipData();
		if (!Mips.IsValid()) return Result;
		for (uint32 Index = 0; Index < Source.GetLayers()[0].NumMips; ++Index)
		{
			const auto View = Mips.GetMipImage(0, 0, Index);
			if (!View.IsValid()) return {.Settings = Settings};
			auto ImageResult = Image::FImage::TryCreate(View.GetInfo(), Mips.GetMipData(0, 0, Index));
			if (!ImageResult) return {.Settings = Settings};
			Result.SourceMips.push_back(std::move(*ImageResult));
		}
		if (!ValidateTexture2DSourceMips(Result.SourceMips)) return {.Settings = Settings};
		Result.SourceIdentity = Source.GetIdentity();
		return Result;
	}

	auto ValidateTexture2DBuildSettings(const FTexture2DBuildSettings& Settings) -> std::expected<void, FTexture2DInputError>
	{
		ETexture2DInputError Code = ETexture2DInputError::None;
		if (!IsValidTextureUsage(Settings.Usage)) Code = ETexture2DInputError::InvalidUsage;
		else if (!IsValidTextureCompressionQuality(Settings.CompressionQuality)) Code = ETexture2DInputError::InvalidCompressionQuality;
		else if (!IsValidTextureAlphaMipMode(Settings.AlphaMipMode)) Code = ETexture2DInputError::InvalidAlphaMipMode;
		else if (!IsValidTextureAlphaCoverageThreshold(Settings.AlphaCoverageThreshold)) Code = ETexture2DInputError::InvalidAlphaCoverageThreshold;
		if (Code != ETexture2DInputError::None)
			return std::unexpected(FTexture2DInputError{.Code = Code, .Settings = Settings});
		return {};
	}

	auto ResolveTexture2DSRGB(const FTexture2DBuildSettings& Settings) -> bool
	{
		return Settings.bSRGB.value_or(GetDefaultTextureSRGB(Settings.Usage));
	}

	auto BuildTexture2DPlatformData(const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> std::expected<void, FTexture2DBuildError>
	{
		OutProduct = {};
		OutIdentity = {};
		if (Request.DeferredSource && (!Request.SourceMips.empty()
			|| !Request.DeferredSource->IsValid() || Request.DeferredSource->GetOwner()
			|| Request.DeferredSource->GetKind() != ETextureSourceKind::Texture2D
			|| Request.DeferredSource->GetIdentity() != Request.SourceIdentity))
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::MissingSourceIdentity});
		if (const auto Validation = ValidateTexture2DSourceMips(Request.SourceMips); !Request.DeferredSource && !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		if (Request.SourceIdentity.IsZero())
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::MissingSourceIdentity});
		OutIdentity = {
			.SourceIdentity = Request.SourceIdentity,
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		OutIdentity.Settings.bSRGB = ResolveTexture2DSRGB(Request.Settings);
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::AuthoredBuildUnavailable});
#else
		auto* Module = ITextureBuildModule::Get();
		if (!Module) return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::ModuleUnavailable});
		OutIdentity.Builder = Module->GetTexture2DDescriptor();
		if (!OutIdentity.Builder.IsValid())
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidBuilderDescriptor});
		}
		const FTexture2DBuildKeyInput KeyInput{
			.SourceIdentity = OutIdentity.SourceIdentity,
			.Usage = Request.Settings.Usage,
			.bSRGB = ResolveTexture2DSRGB(Request.Settings),
			.CompressionQuality = Request.Settings.CompressionQuality,
			.AlphaMipMode = Request.Settings.AlphaMipMode,
			.MaximumResolution = Request.Settings.MaxResolution,
			.AlphaCoverageThreshold = Request.Settings.AlphaCoverageThreshold,
			.BuilderVersion = OutIdentity.Builder.BuilderVersion,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		const FCacheKeyProxy Key = BuildTexture2DDerivedDataKey(KeyInput);
		TextureDerivedDataCache::FOperationDiagnostic CacheDiagnostic;
		FTexturePlatformData PlatformData;
		if (TextureDerivedDataCache::Load(
			Key,
			Request.TargetPlatform, Request.TargetProfile,
			PlatformData, CacheDiagnostic) == TextureDerivedDataCache::ELoadResult::Hit)
		{
			OutProduct = {.PlatformData = std::move(PlatformData),
				.DerivedDataKey = Key,
				.Builder = OutIdentity.Builder,
				.Origin = ETexture2DBuildProductOrigin::CacheHit};
			return std::expected<void, FTexture2DBuildError>{};
		}
		if (ExecutionControl && ExecutionControl->ShouldCancel
			&& ExecutionControl->ShouldCancel())
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::Cancelled});
		}
		FTexture2DBuildRequest Decoded;
		if (Request.DeferredSource)
		{
			Decoded = MakeTexture2DBuildRequest(*Request.DeferredSource, Request.Settings);
			if (const auto Validation = ValidateTexture2DSourceMips(Decoded.SourceMips); !Validation)
				return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		}
		const FTexture2DRecipeExecutionControl RecipeControl{
			.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
				: std::function<bool()>{}};
		auto RecipeResult = Module->BuildTexture2D({
			.SourceMips = Request.DeferredSource ? Decoded.SourceMips : Request.SourceMips,
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile},
			&RecipeControl);
		if (!RecipeResult) return std::unexpected(std::move(RecipeResult.error()));
		auto RecipeProduct = std::move(*RecipeResult);
		FTexture2DBuildMetrics RecipeMetrics{RecipeProduct.Metrics};
		if (!RecipeProduct.PlatformData.IsValid())
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidBuilderProduct});
		}
		if (ExecutionControl && ExecutionControl->ShouldCancel
			&& ExecutionControl->ShouldCancel())
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::Cancelled});
		}

		TextureDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
		if (Request.bPersistDerivedData)
		{
			if (ExecutionControl && ExecutionControl->OnPersisting)
				ExecutionControl->OnPersisting();
			TextureDerivedDataCache::Store(
				Key,
				Request.TargetPlatform, Request.TargetProfile,
				RecipeProduct.PlatformData, StoreDiagnostic);
			RecipeMetrics.PersistenceNanoseconds =
				StoreDiagnostic.DurationNanoseconds;
		}
		if (ExecutionControl && ExecutionControl->Metrics)
			*ExecutionControl->Metrics = RecipeMetrics;
		OutProduct = {.PlatformData = std::move(RecipeProduct.PlatformData),
			.DerivedDataKey = Key,
			.PersistenceDiagnostic = {std::move(CacheDiagnostic), std::move(StoreDiagnostic)},
			.Builder = OutIdentity.Builder,
			.Metrics = RecipeMetrics,
			.Origin = ETexture2DBuildProductOrigin::Rebuilt};
		return std::expected<void, FTexture2DBuildError>{};
#endif
	}

}

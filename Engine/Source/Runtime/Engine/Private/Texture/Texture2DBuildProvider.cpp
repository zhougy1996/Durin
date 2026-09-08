#include "Texture/Texture2DBuild.h"

#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataCache.h"
#include "TextureDerivedDataKey.h"

namespace Durin
{
	auto ValidateTexture2DSourceMips(std::span<const Image::FImage> Mips,
		std::string& OutError) -> bool
	{
		if (Mips.empty())
		{
			OutError = "Texture2D source mip chain is empty.";
			return false;
		}
		const auto& Base = Mips.front().GetInfo();
		uint64 Bytes = 0;
		for (size_t Index = 0; Index < Mips.size(); ++Index)
		{
			const auto& Info = Mips[Index].GetInfo();
			Bytes += Mips[Index].GetPixels().size();
			if (!Mips[Index].IsValid() || Info.Format != Image::ERawImageFormat::RGBA8
				|| Info.Depth != 1 || Info.SliceCount != 1
				|| Base.Width > 16384 || Base.Height > 16384
				|| Info.Width != std::max(1u, Base.Width >> std::min<size_t>(Index, 31))
				|| Info.Height != std::max(1u, Base.Height >> std::min<size_t>(Index, 31))
				|| Info.GammaSpace != Base.GammaSpace || Bytes > MaximumTextureSourceBytes
				|| (Index > 0 && Mips[Index - 1].GetInfo().Width == 1
					&& Mips[Index - 1].GetInfo().Height == 1))
			{
				OutError = "Texture2D source mip chain is invalid.";
				return false;
			}
		}
		OutError.clear();
		return true;
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
			Image::FImage Image;
			if (!View.IsValid() || !Image::FImage::TryCreate(View.GetInfo(),
				Mips.GetMipData(0, 0, Index), Image)) return {.Settings = Settings};
			Result.SourceMips.push_back(std::move(Image));
		}
		std::string Error;
		if (!ValidateTexture2DSourceMips(Result.SourceMips, Error)) return {.Settings = Settings};
		Result.SourceIdentity = Source.GetIdentity();
		return Result;
	}

	auto ValidateTexture2DBuildSettings(
		const FTexture2DBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!IsValidTextureUsage(Settings.Usage)
			|| !IsValidTextureCompressionQuality(Settings.CompressionQuality)
			|| !IsValidTextureAlphaMipMode(Settings.AlphaMipMode)
			|| !IsValidTextureAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D build settings are invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto ResolveTexture2DSRGB(const FTexture2DBuildSettings& Settings) -> bool
	{
		return Settings.bSRGB.value_or(GetDefaultTextureSRGB(Settings.Usage));
	}

	auto InvokeTexture2DBuildProvider(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> FTexture2DBuildResult
	{
		OutProduct = {};
		OutIdentity = {};
		std::string Error;
		if (!ValidateTexture2DSourceMips(Request.SourceMips, Error)
			|| !ValidateTexture2DBuildSettings(Request.Settings, Error))
			return {ETexture2DBuildStatus::Failed, std::move(Error)};
		if (Request.SourceIdentity.IsZero())
			return {ETexture2DBuildStatus::Failed, "Texture2D source identity is missing."};
		OutIdentity = {
			.SourceIdentity = Request.SourceIdentity,
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		OutIdentity.Settings.bSRGB = ResolveTexture2DSRGB(Request.Settings);
#if !DURIN_WITH_EDITOR
		return {ETexture2DBuildStatus::Failed,
			"Texture2D authored build orchestration is unavailable outside editor builds."};
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITexture2DBuildProvider>([&](ITexture2DBuildProvider& Provider) {
				OutIdentity.Provider = Provider.GetDescriptor();
				if (!OutIdentity.Provider.IsValid())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Failed,
						"The Texture2D build provider descriptor is invalid."};
				}
				const FTexture2DBuildKeyInput KeyInput{
					.SourceIdentity = OutIdentity.SourceIdentity,
					.Usage = Request.Settings.Usage,
					.bSRGB = ResolveTexture2DSRGB(Request.Settings),
					.CompressionQuality = Request.Settings.CompressionQuality,
					.AlphaMipMode = Request.Settings.AlphaMipMode,
					.MaximumResolution = Request.Settings.MaxResolution,
					.AlphaCoverageThreshold = Request.Settings.AlphaCoverageThreshold,
					.BuilderVersion = OutIdentity.Provider.BuilderVersion,
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
						.Provider = OutIdentity.Provider,
						.Origin = ETexture2DBuildProductOrigin::CacheHit};
					return FTexture2DBuildResult{ETexture2DBuildStatus::Succeeded, {}};
				}
				if (ExecutionControl && ExecutionControl->ShouldCancel
					&& ExecutionControl->ShouldCancel())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Cancelled,
						"Texture2D build was cancelled."};
				}
				FTexture2DRecipeBuildProduct RecipeProduct;
				FTexture2DBuildMetrics RecipeMetrics;
				const FTexture2DRecipeExecutionControl RecipeControl{
					.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
						: std::function<bool()>{},
					.Metrics = &RecipeMetrics};
				const FTexture2DBuildResult RecipeResult = Provider.Build({
					.SourceMips = Request.SourceMips,
					.Settings = Request.Settings,
					.TargetPlatform = Request.TargetPlatform,
					.TargetProfile = Request.TargetProfile},
					RecipeProduct, &RecipeControl);
				if (!RecipeResult) return RecipeResult;
				if (!RecipeProduct.PlatformData.IsValid())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Failed,
						"Texture2D provider returned invalid platform data."};
				}
				if (ExecutionControl && ExecutionControl->ShouldCancel
					&& ExecutionControl->ShouldCancel())
				{
					return FTexture2DBuildResult{ETexture2DBuildStatus::Cancelled,
						"Texture2D build was cancelled."};
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
					.PersistenceDiagnostic = AssetDerivedDataCache::CombineDiagnostics(
						CacheDiagnostic, StoreDiagnostic),
					.Provider = OutIdentity.Provider,
					.Metrics = RecipeMetrics,
					.Origin = ETexture2DBuildProductOrigin::Rebuilt};
				return FTexture2DBuildResult{ETexture2DBuildStatus::Succeeded, {}};
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value())
		{
			if (!*Invocation.Value) OutProduct = {};
			return std::move(*Invocation.Value);
		}
		OutProduct = {};
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
			return {ETexture2DBuildStatus::Failed,
				"The Texture2D build provider is unavailable."};
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			return {ETexture2DBuildStatus::Failed,
				"Multiple Texture2D build providers are registered."};
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
			return {ETexture2DBuildStatus::Failed,
				"The Texture2D build provider invocation failed."};
		return {ETexture2DBuildStatus::Failed,
			"The Texture2D build provider failed without a diagnostic."};
#endif
	}
}

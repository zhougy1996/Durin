#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/Texture2DBuild.h"
#include "Texture/ITextureCompressorModule.h"

#include "Texture/TextureDerivedData.h"
#include "TextureBuildSession.h"
#include "Texture2DBuildFunction.h"
#include "TextureDerivedDataKey.h"
#include "Texture2DSharedOutput.h"

namespace Durin
{
	auto FormatTexture2DBuildError(const FTexture2DBuildError& Error) -> std::string
	{
		if (!Error.Description.empty()) return Error.Description;
		if (Error.InputCause) return FormatTexture2DInputError(*Error.InputCause);
		switch (Error.Code)
		{
		case ETexture2DBuildError::None: return {};
		case ETexture2DBuildError::InvalidInput: return "Texture build input is invalid.";
		case ETexture2DBuildError::MissingSourceIdentity: return "Texture2D source identity is missing.";
		case ETexture2DBuildError::AuthoredBuildUnavailable: return "Texture2D authored build orchestration is unavailable outside editor builds.";
		case ETexture2DBuildError::InvalidBuilderVersion: return "The Texture2D builder descriptor is invalid.";
		case ETexture2DBuildError::Cancelled: return "Texture2D build was cancelled.";
		case ETexture2DBuildError::InvalidBuilderProduct: return "Texture2D builder returned invalid platform data.";
		case ETexture2DBuildError::ModuleUnavailable: return "The TextureCompressor module is unavailable.";
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

	auto ValidateTexture2DBuildSource(const FTextureSource& Source)
		-> std::expected<void, FTexture2DInputError>
	{
		if (!Source.IsValid())
			return std::unexpected(FTexture2DInputError{.Code = ETexture2DInputError::EmptyMips});
		if (Source.GetKind() != ETextureSourceKind::Texture2D
			|| Source.GetBlocks().size() != 1 || Source.GetLayers().size() != 1)
			return std::unexpected(FTexture2DInputError{.Code = ETexture2DInputError::UnsupportedShape});
		if (Source.GetFormat() != ETextureSourceFormat::RGBA8)
			return std::unexpected(FTexture2DInputError{.Code = ETexture2DInputError::UnsupportedFormat});
		if (Source.GetWidth() > 16384 || Source.GetHeight() > 16384)
			return std::unexpected(FTexture2DInputError{.Code = ETexture2DInputError::ExcessiveResolution});
		if (Source.GetDecodedPayloadSize() > MaximumTextureSourceBytes)
			return std::unexpected(FTexture2DInputError{.Code = ETexture2DInputError::SourceBudgetExceeded});
		return {};
	}

	auto MakeTexture2DBuildRequest(const FTextureSource& Source,
		const FTexture2DBuildSettings& Settings)
		-> std::expected<FTexture2DBuildRequest, FTexture2DInputError>
	{
		if (const auto Valid = ValidateTexture2DBuildSource(Source); !Valid)
			return std::unexpected(Valid.error());
		return FTexture2DBuildRequest{.Source = Source.CopyTornOff(), .Settings = Settings};
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
		FTexturePlatformData& OutPlatformData,
		FTexture2DBuildInputIdentity& OutIdentity,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> std::expected<void, FTexture2DBuildError>
	{
		OutPlatformData = {};
		OutIdentity = {};
		if (const auto Validation = ValidateTexture2DBuildSource(Request.Source); !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		if (Request.Source.GetOwner() || Request.Source.GetIdentity().IsZero())
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::MissingSourceIdentity});
		OutIdentity = {.SourceIdentity = Request.Source.GetIdentity(), .Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile};
		OutIdentity.Settings.bSRGB = ResolveTexture2DSRGB(Request.Settings);
#if !DURIN_WITH_EDITOR
		return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::AuthoredBuildUnavailable});
#else
		auto* Module = ITextureCompressorModule::Get();
		if (!Module) return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::ModuleUnavailable});
		OutIdentity.BuilderVersion = Module->GetTexture2DBuilderVersion();
		if (!OutIdentity.BuilderVersion)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidBuilderVersion});
		auto Definition = TexturePrivate::MakeTexture2DSessionDefinition(Request);
		if (!Definition) return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput});
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = Request.bPersistDerivedData;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumTextureSourceBytes;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumTexturePayloadBytes;
		Options.Policy.MaximumEncodedBytes = MaximumTexturePayloadBytes;
		Options.Cancellation = DerivedData::FBuildCancellation(ExecutionControl ? ExecutionControl->ShouldCancel : std::function<bool()>{});
		auto Built = TexturePrivate::Build(std::move(*Definition),
			TexturePrivate::MakeTexture2DInputResolver(Request.Source), std::move(Options));
		if (!Built) return std::unexpected(FTexture2DBuildError{
			.Code = ETexture2DBuildError::ModuleUnavailable});
		auto& Result = *Built;
		if (Result.GetStatus() == DerivedData::EStatus::Canceled)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::Cancelled});
		if (Result.GetStatus() == DerivedData::EStatus::Error)
		{
			FTexture2DBuildError Error{.Code = Result.GetOutput() ? ETexture2DBuildError::InvalidBuilderProduct : ETexture2DBuildError::ModuleUnavailable};
			if (const auto* Output = Result.GetOutput(); Output && !Output->GetMessages().empty()) Error.Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Result.GetOutput(); Output && !Output->GetLogs().empty()) Error.Description = Output->GetLogs().back().Text;
			return std::unexpected(std::move(Error));
		}
		auto Product = TexturePrivate::AssembleTexture2DSharedOutput(*Result.GetOutput(), Request.TargetPlatform, Request.TargetProfile);
		if (!Product) return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidBuilderProduct, .Description = std::move(Product.error())});
		OutPlatformData = std::move(*Product);
		return {};
#endif
	}
}

#endif

#include "Texture/Texture2DBuild.h"
#include "Texture/ITextureBuildModule.h"

#include "Texture/TextureDerivedData.h"
#include "Asset/AssetDerivedDataBuild.h"
#include "TexturePlatformCodec.h"
#include "TextureDerivedDataKey.h"

namespace Durin
{
	auto FormatTexture2DBuildError(const FTexture2DBuildError& Error) -> std::string
	{
		if (Error.ArchiveCause) return std::format("Archive code {} at {}: {}", static_cast<int>(Error.ArchiveCause->Code), Error.ArchiveCause->Path, Error.ArchiveCause->Message);
		if (Error.InputCause) return FormatTexture2DInputError(*Error.InputCause);
		switch (Error.Code)
		{
		case ETexture2DBuildError::None: return {};
		case ETexture2DBuildError::InvalidInput: return "Texture build input is invalid.";
		case ETexture2DBuildError::CompressionTaskFailed: return "Texture compression task failed.";
		case ETexture2DBuildError::MissingSourceIdentity: return "Texture2D source identity is missing.";
		case ETexture2DBuildError::AuthoredBuildUnavailable: return "Texture2D authored build orchestration is unavailable outside editor builds.";
		case ETexture2DBuildError::InvalidBuilderVersion: return "The Texture2D builder descriptor is invalid.";
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

#if DURIN_WITH_EDITOR
	namespace
	{
		struct FTexture2DBuildAdapter
		{
			using FProduct = FTexture2DBuildOutput;
			using FError = FTexture2DBuildError;
			const FTexture2DBuildRequest& Request;
			ITextureBuildModule& Module;
			const DerivedData::FBuildDefinition& Definition;
			FTexture2DBuildSettings Settings;
			FTexture2DBuildControl Control;
			std::array<DerivedData::FBuildInputReference, 1> Inputs;

			auto GetFunction() const -> DerivedData::FBuildFunctionDescriptor
			{
				auto Descriptor = Definition.GetFunction();
				Descriptor.Version = Module.GetTexture2DBuilderVersion();
				return Descriptor;
			}
			auto GetInputs() const -> std::span<const DerivedData::FBuildInputReference> { return Inputs; }
			auto MakeError(DerivedData::EBuildFailure Code) const -> FError
			{
				return {.Code = Code == DerivedData::EBuildFailure::Cancelled ? ETexture2DBuildError::Cancelled
					: Code == DerivedData::EBuildFailure::FunctionMismatch ? ETexture2DBuildError::InvalidBuilderVersion
					: ETexture2DBuildError::MissingSourceIdentity};
			}
			auto IsCancelled(const FError& Error) const -> bool { return Error.Code == ETexture2DBuildError::Cancelled; }
			auto ValidateBindings(const DerivedData::FBuildDefinition&) const -> std::expected<void, FError>
			{
				if (Request.Source.GetOwner() || Request.Source.GetIdentity() != Inputs[0].Identity)
					return std::unexpected(MakeError(DerivedData::EBuildFailure::InputMismatch));
				return {};
			}
			auto Resolve() const -> std::expected<std::vector<Image::FImage>, FError>
			{
				const auto Mips = Request.Source.GetMipData();
				if (!Mips.IsValid()) return std::unexpected(FError{.Code = ETexture2DBuildError::InvalidInput,
					.InputCause = FTexture2DInputError{.Code = ETexture2DInputError::InvalidImage}});
				std::vector<Image::FImage> Result;
				for (uint32 Index = 0; Index < Request.Source.GetLayers()[0].NumMips; ++Index)
				{
					if (Control.ShouldCancel && Control.ShouldCancel())
						return std::unexpected(MakeError(DerivedData::EBuildFailure::Cancelled));
					const auto View = Mips.GetMipImage(0, 0, Index);
					auto Image = Image::FImage::TryCreate(View.GetInfo(), Mips.GetMipData(0, 0, Index));
					if (!Image) return std::unexpected(FError{.Code = ETexture2DBuildError::InvalidInput,
						.InputCause = FTexture2DInputError{.Code = ETexture2DInputError::InvalidImage, .Index = Index}});
					Result.push_back(std::move(*Image));
				}
				if (auto Valid = ValidateTexture2DSourceMips(Result); !Valid)
					return std::unexpected(FError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Valid.error()});
				return Result;
			}
			auto Build(std::vector<Image::FImage>& Mips) const -> std::expected<FProduct, FError>
			{
				return Module.BuildTexture2D({.SourceMips = Mips, .Settings = Settings,
					.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile}, &Control);
			}
			auto Validate(const FProduct& Product) const -> std::expected<void, FError>
			{
				if (!Product.PlatformData.IsValid()) return std::unexpected(FError{.Code = ETexture2DBuildError::InvalidBuilderProduct});
				return {};
			}
			auto Decode(const FSharedByteBuffer& Bytes) -> std::expected<FProduct, FError>
			{
				FProduct Product;
				if (auto Decoded = TexturePrivate::DecodePlatformData(Bytes.GetBytes(), Request.TargetProfile, Product.PlatformData); !Decoded)
				{
					return std::unexpected(FError{.Code = ETexture2DBuildError::InvalidPlatformData, .ArchiveCause = std::move(Decoded.error())});
				}
				return Product;
			}
			auto Encode(FProduct& Product) -> std::expected<FByteBuffer, FError>
			{
				auto Encoded = TexturePrivate::EncodePlatformData(Product.PlatformData, Request.TargetProfile);
				if (!Encoded)
				{
					return std::unexpected(FError{.Code = ETexture2DBuildError::InvalidPlatformData, .ArchiveCause = std::move(Encoded.error())});
				}
				return std::move(*Encoded);
			}
		};
	}
#endif

	auto BuildTexture2DPlatformData(const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> std::expected<void, FTexture2DBuildError>
	{
		OutProduct = {};
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
		auto* Module = ITextureBuildModule::Get();
		if (!Module) return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::ModuleUnavailable});
		OutIdentity.BuilderVersion = Module->GetTexture2DBuilderVersion();
		if (!OutIdentity.BuilderVersion)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidBuilderVersion});
		auto Definition = MakeTexture2DBuildDefinition({
			.SourceIdentity = OutIdentity.SourceIdentity, .Usage = Request.Settings.Usage,
			.bSRGB = *OutIdentity.Settings.bSRGB, .CompressionQuality = Request.Settings.CompressionQuality,
			.AlphaMipMode = Request.Settings.AlphaMipMode, .MaximumResolution = Request.Settings.MaxResolution,
			.AlphaCoverageThreshold = Request.Settings.AlphaCoverageThreshold,
			.BuilderVersion = OutIdentity.BuilderVersion,
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
		if (!Definition) return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::UnsupportedTarget});
		FTexture2DBuildAdapter Adapter{.Request = Request, .Module = *Module, .Definition = *Definition,
			.Settings = OutIdentity.Settings,
			.Control = {.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel : std::function<bool()>{}},
			.Inputs = {DerivedData::FBuildInputReference{"Source", Request.Source.GetIdentity(),
				"TextureSource", TextureSourceSchemaVersion, "Texture2D.RGBA8", 1}}};
		DerivedData::TBuildObservations<FTexture2DBuildError> Observations;
		const DerivedData::FBuildExecutionContext Context{
			.ShouldCancel = Adapter.Control.ShouldCancel,
			.OnPhase = [ExecutionControl](DerivedData::EBuildPhase Phase) {
				if (Phase == DerivedData::EBuildPhase::Store && ExecutionControl && ExecutionControl->OnPersisting)
					ExecutionControl->OnPersisting();
			}};
		auto Built = DerivedData::ExecuteBuild(*Definition, Adapter,
			{.bWriteCache = Request.bPersistDerivedData, .MaximumValueBytes = MaximumTexturePayloadBytes}, Context, Observations);
		AssetDerivedDataBuild::ReportCacheIssues(*Definition, Observations, FormatTexture2DBuildError);
		if (!Built) return std::unexpected(std::move(Built.error()));
		FTexture2DBuildMetrics Metrics{Built->Metrics};
		Metrics.PersistenceNanoseconds = Observations.Nanoseconds[static_cast<size_t>(DerivedData::EBuildPhase::Store)];
		if (ExecutionControl && ExecutionControl->Metrics) *ExecutionControl->Metrics = Metrics;
		OutProduct = {.PlatformData = std::move(Built->PlatformData),
			.DerivedDataKey = FCacheKeyProxy(Definition->GetKey()),
			.BuilderVersion = OutIdentity.BuilderVersion, .Metrics = Metrics,
			.Origin = Observations.Origin == DerivedData::EBuildOrigin::CacheHit
				? ETexture2DBuildProductOrigin::CacheHit : ETexture2DBuildProductOrigin::Rebuilt};
		return {};
#endif
	}
}

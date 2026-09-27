#include "Texture2DBuildFunction.h"
#if DURIN_WITH_EDITOR
#include "Texture/ITextureBuildModule.h"
#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataKey.h"
#include "Texture2DSharedOutput.h"

namespace Durin::TexturePrivate
{
	using namespace DerivedData;
	namespace
	{
		auto Error(ETexture2DBuildError Code, std::string Message) -> FBuildError
		{
			return {.Category = Code == ETexture2DBuildError::Cancelled ? EBuildErrorCategory::Cancelled
				: Code == ETexture2DBuildError::InvalidInput ? EBuildErrorCategory::InvalidInput
				: Code == ETexture2DBuildError::InvalidBuilderProduct ? EBuildErrorCategory::InvalidOutput
				: EBuildErrorCategory::ProducerFailure,
				.Description = std::move(Message), .ProducerCode = static_cast<uint32>(Code)};
		}
		auto Identity(const FTextureSource& Source) -> FBuildInputReference
		{ return {"Source", Source.GetIdentity(), "TextureSource", TextureSourceSchemaVersion, "Texture2D.RGBA8", 1}; }

		template<typename T>
		auto Constant(const FBuildAction& Action, std::string_view Name) -> const T*
		{
			for (const auto& Entry : Action.GetConstants())
				if (Entry.Name == Name) return std::get_if<T>(&Entry.Value);
			return nullptr;
		}
		auto ReadSettings(const FBuildAction& Action) -> std::expected<FTexture2DBuildInput, FBuildError>
		{
			const auto* Usage = Constant<uint64>(Action, "Usage");
			const auto* Quality = Constant<uint64>(Action, "Quality");
			const auto* Alpha = Constant<uint64>(Action, "AlphaMode");
			const auto* Resolution = Constant<uint64>(Action, "MaximumResolution");
			const auto* Threshold = Constant<float>(Action, "AlphaThreshold");
			const auto* SRGB = Constant<bool>(Action, "SRGB");
			const auto* Platform = Constant<uint64>(Action, "TargetPlatform");
			const auto* Profile = Constant<uint64>(Action, "TargetProfile");
			if (Action.GetConstants().size() != 8 || !Usage || !Quality || !Alpha || !Resolution
				|| !Threshold || !SRGB || !Platform || !Profile || *Usage > 255 || *Quality > 255
				|| *Alpha > 255 || *Resolution > UINT32_MAX)
				return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D constants are invalid."));
			if (*Platform != static_cast<uint64>(ECookTargetPlatform::Win64)
				|| *Profile != static_cast<uint64>(ECookTargetProfile::Game))
				return std::unexpected(Error(ETexture2DBuildError::UnsupportedTarget, "Texture2D target is unsupported."));
			FTexture2DBuildInput Input{.Settings = {.Usage = static_cast<ETextureUsage>(*Usage),
				.CompressionQuality = static_cast<ETextureCompressionQuality>(*Quality),
				.AlphaMipMode = static_cast<ETextureAlphaMipMode>(*Alpha), .AlphaCoverageThreshold = *Threshold,
				.MaxResolution = static_cast<uint32>(*Resolution), .bSRGB = *SRGB},
				.TargetPlatform = static_cast<ECookTargetPlatform>(*Platform), .TargetProfile = static_cast<ECookTargetProfile>(*Profile)};
			if (auto Valid = ValidateTexture2DBuildSettings(Input.Settings); !Valid)
				return std::unexpected(Error(ETexture2DBuildError::InvalidInput, FormatTexture2DInputError(Valid.error())));
			return Input;
		}

		class FTexture2DResolver final : public IBuildInputResolver
		{
		public:
			explicit FTexture2DResolver(const FTextureSource& Source) : Source(Source.CopyTornOff()) {}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildError> override
			{
				if (Sources.size() != 1 || Sources[0].Name != "Source" || Sources[0].Source != "CapturedSource"
					|| !ValidateTexture2DBuildSource(Source) || Source.GetIdentity().IsZero())
					return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D captured source is invalid."));
				return std::vector{Identity(Source)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildError> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity(Source))
					return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D source identity changed."));
				// GetMipData verifies the decoded payload hash against captured semantic metadata.
				const auto Mips = Source.GetMipData();
				if (!Mips.IsValid()) return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D source bytes are unavailable or corrupt."));
				const auto Count = Source.GetLayers()[0].NumMips;
				FBinaryWriter Metadata({.MaximumTotalBytes = 4 + MaximumTextureMipCount * 12});
				Metadata.WriteU32(Count);
				FBuildInput Result{.Identity = Inputs[0]};
				for (uint32 Index = 0; Index < Count; ++Index)
				{
					if (Cancel.IsCancelled()) return std::unexpected(Error(ETexture2DBuildError::Cancelled, {}));
					const auto Info = Mips.GetMipImage(0, 0, Index).GetInfo();
					Metadata.WriteU32(Info.Width); Metadata.WriteU32(Info.Height);
					Metadata.WriteU32(Info.GammaSpace == Image::EImageGammaSpace::SRGB ? 2 : Info.GammaSpace == Image::EImageGammaSpace::Linear ? 1 : 0);
					Result.Values.push_back({std::format("Mip/{}", Index), Mips.GetMipData(0, 0, Index)});
				}
				if (Metadata.HasError()) return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D source metadata exceeds its bound."));
				Result.Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes());
				return std::vector<FBuildInput>{std::move(Result)};
			}
		private:
			FTextureSource Source;
		};

		class FTexture2DFunction final : public IBuildFunction
		{
		public:
			explicit FTexture2DFunction(ITextureBuildModule& Module) : Module(Module), Version(Module.GetTexture2DBuilderVersion()) {}
			auto GetDescriptor() const -> FBuildFunctionDescriptor override
			{ return {"Durin.Texture2D", Version, 1, "Texture2D.Output", 1, FCacheBucket::FromString(Texture2DCacheBucket)}; }
			auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildError> override
			{
				auto Input = ReadSettings(Context.GetAction());
				if (!Input) return std::unexpected(std::move(Input.error()));
				const auto Inputs = Context.GetInputs();
				if (Inputs.size() != 1 || Inputs[0].Identity.Name != "Source"
					|| Inputs[0].Identity.IdentityScheme != "TextureSource"
					|| Inputs[0].Identity.IdentityVersion != TextureSourceSchemaVersion
					|| Inputs[0].Identity.Representation != "Texture2D.RGBA8" || Inputs[0].Identity.RepresentationVersion != 1)
					return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D input representation is invalid."));
				FBinaryReader Reader(Inputs[0].Metadata.GetBytes(), {.MaximumTotalBytes = 4 + MaximumTextureMipCount * 12});
				uint32 Count = 0;
				if (!Reader.ReadU32(Count) || !Count || Count > MaximumTextureMipCount || Inputs[0].Values.size() != Count)
					return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D input mip count is invalid."));
				std::vector<Image::FImage> Mips;
				for (uint32 Index = 0; Index < Count; ++Index)
				{
					Image::FImageInfo Info{.Format = Image::ERawImageFormat::RGBA8};
					uint32 Gamma = 0;
					if (!Reader.ReadU32(Info.Width) || !Reader.ReadU32(Info.Height) || !Reader.ReadU32(Gamma) || Gamma > 2)
						return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D input mip descriptor is invalid."));
					Info.GammaSpace = Gamma == 2 ? Image::EImageGammaSpace::SRGB : Gamma == 1 ? Image::EImageGammaSpace::Linear : Image::EImageGammaSpace::Unknown;
					const auto Name = std::format("Mip/{}", Index);
					const auto Value = std::find_if(Inputs[0].Values.begin(), Inputs[0].Values.end(), [&](const FBuildValue& Value) { return Value.Id == Name; });
					if (Value == Inputs[0].Values.end()) return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D input mip is missing."));
					auto Mip = Image::FImage::TryCreate(Info, Value->Data);
					if (!Mip) return std::unexpected(Error(ETexture2DBuildError::InvalidInput, Mip.error().ToString()));
					Mips.push_back(std::move(*Mip));
				}
				if (!Reader.IsAtEnd()) return std::unexpected(Error(ETexture2DBuildError::InvalidInput, "Texture2D input has trailing metadata."));
				if (auto Valid = ValidateTexture2DSourceMips(Mips); !Valid)
					return std::unexpected(Error(ETexture2DBuildError::InvalidInput, FormatTexture2DInputError(Valid.error())));
				Input->SourceMips = Mips;
				FTexture2DBuildControl Control{.ShouldCancel = [&] { return Context.IsCancelled(); }};
				auto Built = Module.BuildTexture2D(*Input, &Control);
				if (!Built) return std::unexpected(Error(Built.error().Code, FormatTexture2DBuildError(Built.error())));
				Context.ReportMetric("Texture2D.MipGenerationNanoseconds", Built->Metrics.MipGenerationNanoseconds);
				Context.ReportMetric("Texture2D.CompressionNanoseconds", Built->Metrics.CompressionNanoseconds);
				Context.ReportMetric("Texture2D.PeakIntermediateBytes", Built->Metrics.PeakIntermediateBytes);
				auto Output = MakeTexture2DSharedOutput(Built->PlatformData, Input->TargetPlatform, Input->TargetProfile);
				if (!Output) return std::unexpected(Error(ETexture2DBuildError::InvalidBuilderProduct, std::move(Output.error())));
				return std::move(*Output);
			}
			auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation&) const
				-> std::expected<void, FBuildError> override
			{
				auto Settings = ReadSettings(Action);
				if (!Settings) return std::unexpected(std::move(Settings.error()));
				if (auto Layout = ReadTexture2DOutputLayout(Output, Settings->TargetPlatform, Settings->TargetProfile); !Layout)
					return std::unexpected(Error(ETexture2DBuildError::InvalidBuilderProduct, std::move(Layout.error())));
				return {};
			}
		private:
			ITextureBuildModule& Module;
			uint32 Version;
		};
	}
	auto MakeTexture2DBuildFunction(ITextureBuildModule& Module) -> std::shared_ptr<const IBuildFunction>
	{ return std::make_shared<FTexture2DFunction>(Module); }
	auto MakeTexture2DInputResolver(const FTextureSource& Source) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FTexture2DResolver>(Source); }
	auto MakeTexture2DSessionDefinition(const FTexture2DBuildRequest& Request)
		-> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		const auto& S = Request.Settings;
		return FBuildDefinition::TryCreate("Durin.Texture2D",
			{{"Usage", uint64(S.Usage)}, {"SRGB", ResolveTexture2DSRGB(S)}, {"Quality", uint64(S.CompressionQuality)},
			 {"AlphaMode", uint64(S.AlphaMipMode)}, {"MaximumResolution", uint64(S.MaxResolution)},
			 {"AlphaThreshold", S.AlphaCoverageThreshold}, {"TargetPlatform", uint64(Request.TargetPlatform)},
			 {"TargetProfile", uint64(Request.TargetProfile)}}, {{"Source", "CapturedSource"}});
	}
}
#endif

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
		auto Error(std::string Message) -> FBuildInputError { return {std::move(Message)}; }
		auto Identity(const FTextureSource& Source) -> FBuildInputReference
		{ return {"Source", Source.GetIdentity(), "TextureSource", TextureSourceSchemaVersion, "Texture2D.RGBA8", 1}; }

		auto ReadSettings(const FBuildContext& Context) -> std::expected<FTexture2DBuildInput, std::string>
		{
			const auto* Usage = Context.FindConstant<uint64>("Usage");
			const auto* Quality = Context.FindConstant<uint64>("Quality");
			const auto* Alpha = Context.FindConstant<uint64>("AlphaMode");
			const auto* Resolution = Context.FindConstant<uint64>("MaximumResolution");
			const auto* Threshold = Context.FindConstant<float>("AlphaThreshold");
			const auto* SRGB = Context.FindConstant<bool>("SRGB");
			const auto* Platform = Context.FindConstant<uint64>("TargetPlatform");
			const auto* Profile = Context.FindConstant<uint64>("TargetProfile");
			if (!Usage || !Quality || !Alpha || !Resolution
				|| !Threshold || !SRGB || !Platform || !Profile || *Usage > 255 || *Quality > 255
				|| *Alpha > 255 || *Resolution > UINT32_MAX)
				return std::unexpected("Texture2D constants are invalid.");
			if (*Platform != static_cast<uint64>(ECookTargetPlatform::Win64)
				|| *Profile != static_cast<uint64>(ECookTargetProfile::Game))
				return std::unexpected("Texture2D target is unsupported.");
			FTexture2DBuildInput Input{.Settings = {.Usage = static_cast<ETextureUsage>(*Usage),
				.CompressionQuality = static_cast<ETextureCompressionQuality>(*Quality),
				.AlphaMipMode = static_cast<ETextureAlphaMipMode>(*Alpha), .AlphaCoverageThreshold = *Threshold,
				.MaxResolution = static_cast<uint32>(*Resolution), .bSRGB = *SRGB},
				.TargetPlatform = static_cast<ECookTargetPlatform>(*Platform), .TargetProfile = static_cast<ECookTargetProfile>(*Profile)};
			if (auto Valid = ValidateTexture2DBuildSettings(Input.Settings); !Valid)
				return std::unexpected(FormatTexture2DInputError(Valid.error()));
			return Input;
		}

		class FTexture2DResolver final : public IBuildInputResolver
		{
		public:
			explicit FTexture2DResolver(const FTextureSource& Source) : Source(Source.CopyTornOff()) {}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
			{
				if (Sources.size() != 1 || Sources[0].Name != "Source" || Sources[0].Source != "CapturedSource"
					|| !ValidateTexture2DBuildSource(Source) || Source.GetIdentity().IsZero())
					return std::unexpected(Error("Texture2D captured source is invalid."));
				return std::vector{Identity(Source)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity(Source))
					return std::unexpected(Error("Texture2D source identity changed."));
				// GetMipData verifies the decoded payload hash against captured semantic metadata.
				const auto Mips = Source.GetMipData();
				if (!Mips.IsValid()) return std::unexpected(Error("Texture2D source bytes are unavailable or corrupt."));
				const auto Count = Source.GetLayers()[0].NumMips;
				FBinaryWriter Metadata({.MaximumTotalBytes = 4 + MaximumTextureMipCount * 12});
				Metadata.WriteU32(Count);
				FBuildInput Result{.Identity = Inputs[0]};
				for (uint32 Index = 0; Index < Count; ++Index)
				{
					if (Cancel.IsCancelled()) return std::unexpected(Error("Texture2D source resolution was canceled."));
					const auto Info = Mips.GetMipImage(0, 0, Index).GetInfo();
					Metadata.WriteU32(Info.Width); Metadata.WriteU32(Info.Height);
					Metadata.WriteU32(Info.GammaSpace == Image::EImageGammaSpace::SRGB ? 2 : Info.GammaSpace == Image::EImageGammaSpace::Linear ? 1 : 0);
					Result.Values.push_back({std::format("Mip/{}", Index), Mips.GetMipData(0, 0, Index)});
				}
				if (Metadata.HasError()) return std::unexpected(Error("Texture2D source metadata exceeds its bound."));
				Result.Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes());
				return std::vector<FBuildInput>{std::move(Result)};
			}
		private:
			FTextureSource Source;
		};

		class FTexture2DFunction final : public IBuildFunction
		{
		public:
			explicit FTexture2DFunction(ITextureBuildModule& Module) : Version(Module.GetTexture2DBuilderVersion()) {}
			auto GetName() const -> std::string_view override { return "Durin.Texture2D"; }
			auto GetVersion() const -> uint32 override { return Version; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ Context.SetConstantsSchema(1); Context.SetOutput("Texture2D.Output", 2); Context.SetCacheBucket(FCacheBucket::FromString(Texture2DCacheBucket)); }
			auto Build(FBuildContext& Context) const -> void override
			{
				auto Fail = [&](std::string Text) { Context.AddError(std::move(Text)); };
				auto Input = ReadSettings(Context); if (!Input) return Fail(std::move(Input.error()));
				const auto* Source = Context.FindInput("Source");
				if (!Source || Source->Identity.IdentityScheme != "TextureSource" || Source->Identity.IdentityVersion != TextureSourceSchemaVersion
					|| Source->Identity.Representation != "Texture2D.RGBA8" || Source->Identity.RepresentationVersion != 1) return Fail("Texture2D input representation is invalid.");
				FBinaryReader Reader(Source->Metadata.GetBytes(), {.MaximumTotalBytes = 4 + MaximumTextureMipCount * 12});
				uint32 Count = 0;
				if (!Reader.ReadU32(Count) || !Count || Count > MaximumTextureMipCount || Source->Values.size() != Count) return Fail("Texture2D input mip count is invalid.");
				std::vector<Image::FImage> Mips;
				for (uint32 Index = 0; Index < Count; ++Index)
				{
					Image::FImageInfo Info{.Format = Image::ERawImageFormat::RGBA8};
					uint32 Gamma = 0;
					if (!Reader.ReadU32(Info.Width) || !Reader.ReadU32(Info.Height) || !Reader.ReadU32(Gamma) || Gamma > 2)
						return Fail("Texture2D input mip descriptor is invalid.");
					Info.GammaSpace = Gamma == 2 ? Image::EImageGammaSpace::SRGB : Gamma == 1 ? Image::EImageGammaSpace::Linear : Image::EImageGammaSpace::Unknown;
					const auto Name = std::format("Mip/{}", Index);
					const auto Value = std::find_if(Source->Values.begin(), Source->Values.end(), [&](const FBuildInputValue& Value) { return Value.Name == Name; });
					if (Value == Source->Values.end()) return Fail("Texture2D input mip is missing.");
					auto Mip = Image::FImage::TryCreate(Info, Value->Data);
					if (!Mip) return Fail(Mip.error().ToString());
					Mips.push_back(std::move(*Mip));
				}
				if (!Reader.IsAtEnd()) return Fail("Texture2D input has trailing metadata.");
				if (auto Valid = ValidateTexture2DSourceMips(Mips); !Valid)
					return Fail(FormatTexture2DInputError(Valid.error()));
				Input->SourceMips = Mips;
				FTexture2DBuildControl Control{.ShouldCancel = [&] { return Context.IsCancelled(); }};
				auto* Module = ITextureBuildModule::Get();
				if (!Module) return Fail("The TextureBuild module is unavailable.");
				auto Built = Module->BuildTexture2D(*Input, &Control);
				if (!Built) return Fail(FormatTexture2DBuildError(Built.error()));
				auto Output = MakeTexture2DSharedOutput(Built->PlatformData,
					Input->TargetPlatform, Input->TargetProfile);
				if (!Output) return Fail(std::move(Output.error()));
				for (const auto& Value : Output->GetValues()) Context.AddValue(Value.Id, Value.Value.GetData());
				for (const auto& Meta : Output->GetMetadata()) Context.AddMeta(Meta.Id, Meta.Object);
			}
		private:
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
		return MakeTexture2DSessionDefinition(FTexture2DBuildKeyInput{
			.Usage = S.Usage, .bSRGB = ResolveTexture2DSRGB(S),
			.CompressionQuality = S.CompressionQuality, .AlphaMipMode = S.AlphaMipMode,
			.MaximumResolution = S.MaxResolution, .AlphaCoverageThreshold = S.AlphaCoverageThreshold,
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
	}
	auto MakeTexture2DSessionDefinition(const FTexture2DBuildKeyInput& Input)
		-> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		FBuildDefinitionBuilder Builder("Durin.Texture2D");
		Builder.AddConstant("Usage", uint64(Input.Usage)).AddConstant("SRGB", Input.bSRGB)
			.AddConstant("Quality", uint64(Input.CompressionQuality)).AddConstant("AlphaMode", uint64(Input.AlphaMipMode))
			.AddConstant("MaximumResolution", uint64(Input.MaximumResolution)).AddConstant("AlphaThreshold", Input.AlphaCoverageThreshold)
			.AddConstant("TargetPlatform", uint64(Input.TargetPlatform)).AddConstant("TargetProfile", uint64(Input.TargetProfile))
			.AddInput("Source", "CapturedSource");
		return std::move(Builder).Build();
	}
}
#endif

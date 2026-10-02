#include "VolumeTextureBuildFunction.h"
#if DURIN_WITH_EDITOR
#include "Texture/ITextureBuildModule.h"
#include "TexturePlatformSharedOutput.h"
#include "TextureDerivedDataKey.h"

namespace Durin::TexturePrivate
{
	using namespace DerivedData;
	namespace
	{
		auto Error(std::string Description) -> FBuildInputError { return {std::move(Description)}; }
		auto Identity(const FTextureSource& Source) -> FBuildInputReference
		{ return {"Source", Source.GetIdentity(), "TextureSource", TextureSourceSchemaVersion, "Volume.Voxels", 1}; }
		auto ReadConstants(const FBuildContext& Context) -> std::expected<FVolumeTextureBuildKeyInput, std::string>
		{
			FVolumeTextureBuildKeyInput Result;
			auto Read = [&](std::string_view Name, uint32& Value) {
				const auto* Integer = Context.FindConstant<uint64>(Name); if (!Integer || *Integer > UINT32_MAX) return false; Value = static_cast<uint32>(*Integer); return true;
			};
			uint32 Format = 0, Filter = 0, Platform = 0, Profile = 0;
			if (!Read("Width", Result.Width) || !Read("Height", Result.Height)
				|| !Read("Depth", Result.Depth) || !Read("Format", Format) || !Read("MipFilter", Filter)
				|| !Read("SourceSchema", Result.SourcePayloadSchemaVersion) || !Read("TargetPlatform", Platform)
				|| !Read("TargetProfile", Profile) || Format > static_cast<uint32>(EVolumeTextureFormat::RGBA16_FLOAT)
				|| Filter != static_cast<uint32>(EVolumeTextureMipFilter::Box)
				|| Platform != static_cast<uint32>(ECookTargetPlatform::Win64)
				|| Profile != static_cast<uint32>(ECookTargetProfile::Game))
				return std::unexpected("Volume build constants are invalid.");
			Result.Settings = {.OutputFormat = static_cast<EVolumeTextureFormat>(Format), .MipFilter = static_cast<EVolumeTextureMipFilter>(Filter)};
			Result.TargetPlatform = static_cast<ECookTargetPlatform>(Platform); Result.TargetProfile = static_cast<ECookTargetProfile>(Profile);
			if (!Result.IsValid()) return std::unexpected("Volume build dimensions exceed their bound.");
			return Result;
		}
		class FVolumeResolver final : public IBuildInputResolver
		{
		public:
			explicit FVolumeResolver(const FTextureSource& Source) : Source(Source.CopyTornOff()) {}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
			{
				if (Sources.size() != 1 || Sources[0].Name != "Source" || Sources[0].Source != "CapturedSource"
					|| !Source.IsValid() || Source.GetKind() != ETextureSourceKind::Volume)
					return std::unexpected(Error("Volume captured source is invalid."));
				return std::vector{Identity(Source)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity(Source))
					return std::unexpected(Error("Volume source identity changed."));
				const auto Mips = Source.GetMipData(); // Verifies the captured decoded payload hash.
				if (!Mips.IsValid()) return std::unexpected(Error("Volume captured bytes are unavailable or corrupt."));
				FBinaryWriter Metadata({.MaximumTotalBytes = 16});
				Metadata.WriteU32(Source.GetWidth()); Metadata.WriteU32(Source.GetHeight()); Metadata.WriteU32(Source.GetDepth());
				Metadata.WriteU32(static_cast<uint32>(Source.GetFormat()));
				return std::vector<FBuildInput>{{.Identity = Inputs[0], .Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes()), .Values = {{"Voxels", Mips.GetData()}}}};
			}
		private:
			FTextureSource Source;
		};
		class FVolumeFunction final : public IBuildFunction
		{
		public:
			explicit FVolumeFunction(ITextureBuildModule& Module) : Version(Module.GetVolumeTextureBuilderVersion()) {}
			auto GetName() const -> std::string_view override { return "Durin.VolumeTexture"; }
			auto GetVersion() const -> uint32 override { return Version; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ Context.SetConstantsSchema(1); Context.SetOutput("VolumeTexture.Output", 2); Context.SetCacheBucket(FCacheBucket::FromString(VolumeTextureCacheBucket)); }
			auto Build(FBuildContext& Context) const -> void override
			{
				auto Fail = [&](std::string Text) { Context.AddError(std::move(Text)); };
				auto Constants = ReadConstants(Context); if (!Constants) return Fail(std::move(Constants.error()));
				const auto* SourceInput = Context.FindInput("Source");
				if (!SourceInput || SourceInput->Identity.IdentityScheme != "TextureSource" || SourceInput->Identity.IdentityVersion != TextureSourceSchemaVersion || SourceInput->Identity.Representation != "Volume.Voxels"
					|| SourceInput->Identity.RepresentationVersion != 1 || SourceInput->Values.size() != 1 || SourceInput->Values[0].Name != "Voxels") return Fail("Volume input representation is invalid.");
				FBinaryReader Metadata(SourceInput->Metadata.GetBytes(), {.MaximumTotalBytes = 16});
				uint32 Width = 0, Height = 0, Depth = 0, Format = 0;
				constexpr ETextureSourceFormat Formats[] = {ETextureSourceFormat::R8_UNORM, ETextureSourceFormat::RG8_UNORM,
					ETextureSourceFormat::RGBA8, ETextureSourceFormat::R16_FLOAT, ETextureSourceFormat::RGBA16_FLOAT};
				if (!Metadata.ReadU32(Width) || !Metadata.ReadU32(Height) || !Metadata.ReadU32(Depth) || !Metadata.ReadU32(Format)
					|| !Metadata.IsAtEnd() || Width != Constants->Width || Height != Constants->Height || Depth != Constants->Depth
					|| Format != static_cast<uint32>(Formats[static_cast<size_t>(Constants->Settings.OutputFormat)]))
					return Fail("Volume input metadata disagrees with its constants.");
				FVolumeTextureSourceData Source{.Width = Width, .Height = Height, .Depth = Depth, .Format = Constants->Settings.OutputFormat};
				Source.CanonicalSourceIdentity = SourceInput->Identity.Identity;
				if (!Source.Voxels.UpdatePayload(SourceInput->Values[0].Data) || !Source.IsValid()) return Fail("Volume voxel byte count is invalid.");
				auto* Module = ITextureBuildModule::Get();
				if (!Module) return Fail("The TextureBuild module is unavailable.");
				auto Built = Module->BuildVolumeTexture({.SourceData = std::cref(Source), .Settings = Constants->Settings,
					.TargetPlatform = Constants->TargetPlatform, .TargetProfile = Constants->TargetProfile});
				if (!Built) return Fail(Built.error().Diagnostic);
				if (!*Built) return Fail("Volume recipe returned no product.");
				auto Output = MakeVolumeTextureSharedOutput(**Built, Constants->TargetPlatform, Constants->TargetProfile);
				if (!Output) return Fail(std::move(Output.error()));
				for (const auto& Value : Output->GetValues()) Context.AddValue(Value.Id, Value.Value.GetData());
				for (const auto& Meta : Output->GetMetadata()) Context.AddMeta(Meta.Id, Meta.Object);
			}
		private:
			uint32 Version;
		};
	}
	auto MakeVolumeTextureBuildFunction(ITextureBuildModule& Module) -> std::shared_ptr<const IBuildFunction>
	{ return std::make_shared<FVolumeFunction>(Module); }
	auto MakeVolumeTextureInputResolver(const FTextureSource& Source) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FVolumeResolver>(Source); }
	auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildRequest& Request) -> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		return MakeVolumeTextureSessionDefinition(FVolumeTextureBuildKeyInput{
			.Width = Request.Source.GetWidth(), .Height = Request.Source.GetHeight(),
			.Depth = Request.Source.GetDepth(), .Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile});
	}
	auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildKeyInput& Input)
		-> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		FBuildDefinitionBuilder Builder("Durin.VolumeTexture");
		Builder.AddConstant("Width", uint64(Input.Width)).AddConstant("Height", uint64(Input.Height))
			.AddConstant("Depth", uint64(Input.Depth)).AddConstant("Format", uint64(Input.Settings.OutputFormat))
			.AddConstant("MipFilter", uint64(Input.Settings.MipFilter)).AddConstant("SourceSchema", uint64(Input.SourcePayloadSchemaVersion))
			.AddConstant("TargetPlatform", uint64(Input.TargetPlatform)).AddConstant("TargetProfile", uint64(Input.TargetProfile))
			.AddInput("Source", "CapturedSource");
		return std::move(Builder).Build();
	}
}
#endif

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
		auto Error(EBuildFailureReason Category, std::string Description) -> FBuildFailure
		{ return {.Reason = Category, .Description = std::move(Description)}; }
		auto Identity(const FTextureSource& Source) -> FBuildInputReference
		{ return {"Source", Source.GetIdentity(), "TextureSource", TextureSourceSchemaVersion, "Volume.Voxels", 1}; }
		auto ReadConstants(const FBuildAction& Action) -> std::expected<FVolumeTextureBuildKeyInput, FBuildFailure>
		{
			FVolumeTextureBuildKeyInput Result;
			auto Read = [&](std::string_view Name, uint32& Value) {
				for (const auto& Entry : Action.GetConstants()) if (Entry.Name == Name)
				{
					const auto* Integer = std::get_if<uint64>(&Entry.Value);
					if (!Integer || *Integer > UINT32_MAX) return false;
					Value = static_cast<uint32>(*Integer); return true;
				}
				return false;
			};
			uint32 Format = 0, Filter = 0, Platform = 0, Profile = 0;
			if (Action.GetConstants().size() != 8 || !Read("Width", Result.Width) || !Read("Height", Result.Height)
				|| !Read("Depth", Result.Depth) || !Read("Format", Format) || !Read("MipFilter", Filter)
				|| !Read("SourceSchema", Result.SourcePayloadSchemaVersion) || !Read("TargetPlatform", Platform)
				|| !Read("TargetProfile", Profile) || Format > static_cast<uint32>(EVolumeTextureFormat::RGBA16_FLOAT)
				|| Filter != static_cast<uint32>(EVolumeTextureMipFilter::Box)
				|| Platform != static_cast<uint32>(ECookTargetPlatform::Win64)
				|| Profile != static_cast<uint32>(ECookTargetProfile::Game))
				return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume build constants are invalid."));
			Result.Settings = {.OutputFormat = static_cast<EVolumeTextureFormat>(Format), .MipFilter = static_cast<EVolumeTextureMipFilter>(Filter)};
			Result.TargetPlatform = static_cast<ECookTargetPlatform>(Platform); Result.TargetProfile = static_cast<ECookTargetProfile>(Profile);
			if (!Result.IsValid()) return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume build dimensions exceed their bound."));
			return Result;
		}
		class FVolumeResolver final : public IBuildInputResolver
		{
		public:
			explicit FVolumeResolver(const FTextureSource& Source) : Source(Source.CopyTornOff()) {}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
			{
				if (Sources.size() != 1 || Sources[0].Name != "Source" || Sources[0].Source != "CapturedSource"
					|| !Source.IsValid() || Source.GetKind() != ETextureSourceKind::Volume)
					return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume captured source is invalid."));
				return std::vector{Identity(Source)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInput>, FBuildFailure> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity(Source))
					return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume source identity changed."));
				const auto Mips = Source.GetMipData(); // Verifies the captured decoded payload hash.
				if (!Mips.IsValid()) return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume captured bytes are unavailable or corrupt."));
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
			explicit FVolumeFunction(ITextureBuildModule& Module) : Module(Module), Version(Module.GetVolumeTextureBuilderVersion()) {}
			auto GetDescriptor() const -> FBuildFunctionDescriptor override
			{ return {"Durin.VolumeTexture", Version, 1, "VolumeTexture.Output", 1, FCacheBucket::FromString(VolumeTextureCacheBucket)}; }
			auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildFailure> override
			{
				auto Constants = ReadConstants(Context.GetAction());
				if (!Constants) return std::unexpected(std::move(Constants.error()));
				const auto Inputs = Context.GetInputs();
				if (Inputs.size() != 1 || Inputs[0].Identity.Name != "Source" || Inputs[0].Identity.IdentityScheme != "TextureSource"
					|| Inputs[0].Identity.IdentityVersion != TextureSourceSchemaVersion || Inputs[0].Identity.Representation != "Volume.Voxels"
					|| Inputs[0].Identity.RepresentationVersion != 1 || Inputs[0].Values.size() != 1 || Inputs[0].Values[0].Id != "Voxels")
					return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume input representation is invalid."));
				FBinaryReader Metadata(Inputs[0].Metadata.GetBytes(), {.MaximumTotalBytes = 16});
				uint32 Width = 0, Height = 0, Depth = 0, Format = 0;
				constexpr ETextureSourceFormat Formats[] = {ETextureSourceFormat::R8_UNORM, ETextureSourceFormat::RG8_UNORM,
					ETextureSourceFormat::RGBA8, ETextureSourceFormat::R16_FLOAT, ETextureSourceFormat::RGBA16_FLOAT};
				if (!Metadata.ReadU32(Width) || !Metadata.ReadU32(Height) || !Metadata.ReadU32(Depth) || !Metadata.ReadU32(Format)
					|| !Metadata.IsAtEnd() || Width != Constants->Width || Height != Constants->Height || Depth != Constants->Depth
					|| Format != static_cast<uint32>(Formats[static_cast<size_t>(Constants->Settings.OutputFormat)]))
					return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume input metadata disagrees with its constants."));
				FVolumeTextureSourceData Source{.Width = Width, .Height = Height, .Depth = Depth, .Format = Constants->Settings.OutputFormat};
				Source.CanonicalSourceIdentity = Inputs[0].Identity.Identity;
				if (!Source.Voxels.UpdatePayload(Inputs[0].Values[0].Data) || !Source.IsValid())
					return std::unexpected(Error(EBuildFailureReason::InvalidInput, "Volume voxel byte count is invalid."));
				auto Built = Module.BuildVolumeTexture({.SourceData = std::cref(Source), .Settings = Constants->Settings,
					.TargetPlatform = Constants->TargetPlatform, .TargetProfile = Constants->TargetProfile});
				if (!Built) return std::unexpected(Error(EBuildFailureReason::ProducerFailure, Built.error().Diagnostic));
				if (!*Built) return std::unexpected(Error(EBuildFailureReason::InvalidOutput, "Volume recipe returned no product."));
				auto Output = MakeVolumeTextureSharedOutput(**Built, Constants->TargetPlatform, Constants->TargetProfile);
				if (!Output) return std::unexpected(Error(EBuildFailureReason::InvalidOutput, std::move(Output.error())));
				return std::move(*Output);
			}
			auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation&) const -> std::expected<void, FBuildFailure> override
			{
				auto Constants = ReadConstants(Action);
				if (!Constants) return std::unexpected(std::move(Constants.error()));
				auto Layout = ReadTexturePlatformOutputLayout(Output, false, Constants->TargetPlatform, Constants->TargetProfile);
				if (!Layout) return std::unexpected(Error(EBuildFailureReason::InvalidOutput, std::move(Layout.error())));
				constexpr EPixelFormat Formats[] = {EPixelFormat::R8_UNORM, EPixelFormat::RG8_UNORM, EPixelFormat::RGBA8_UNORM,
					EPixelFormat::R16_FLOAT, EPixelFormat::RGBA16_FLOAT};
				if (Layout->Format != Formats[static_cast<size_t>(Constants->Settings.OutputFormat)] || Layout->Blocks[0].Width != Constants->Width || Layout->Blocks[0].Height != Constants->Height || Layout->Blocks[0].Depth != Constants->Depth)
					return std::unexpected(Error(EBuildFailureReason::InvalidOutput, "Volume output dimensions disagree with its action."));
				return {};
			}
		private:
			ITextureBuildModule& Module;
			uint32 Version;
		};
	}
	auto MakeVolumeTextureBuildFunction(ITextureBuildModule& Module) -> std::shared_ptr<const IBuildFunction>
	{ return std::make_shared<FVolumeFunction>(Module); }
	auto MakeVolumeTextureInputResolver(const FTextureSource& Source) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FVolumeResolver>(Source); }
	auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildRequest& Request) -> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		return FBuildDefinition::TryCreate("Durin.VolumeTexture", {{"Width", uint64(Request.Source.GetWidth())},
			{"Height", uint64(Request.Source.GetHeight())}, {"Depth", uint64(Request.Source.GetDepth())},
			{"Format", uint64(Request.Settings.OutputFormat)}, {"MipFilter", uint64(Request.Settings.MipFilter)},
			{"SourceSchema", uint64(VolumeTextureSourcePayloadSchemaVersion)}, {"TargetPlatform", uint64(Request.TargetPlatform)},
			{"TargetProfile", uint64(Request.TargetProfile)}}, {{"Source", "CapturedSource"}});
	}
}
#endif

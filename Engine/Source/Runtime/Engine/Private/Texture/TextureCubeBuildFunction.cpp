#include "TextureCubeBuildFunction.h"
#if DURIN_WITH_EDITOR
#include "Texture/ITextureBuildModule.h"
#include "Texture/TextureCube.h"
#include "TexturePlatformSharedOutput.h"

namespace Durin::TexturePrivate
{
	using namespace DerivedData;
	namespace
	{
		auto Error(std::string Message) -> FBuildInputError { return {std::move(Message)}; }
		auto Identity(const FTextureSource& Source) -> FBuildInputReference
		{ return {"Source", Source.GetIdentity(), "TextureSource", TextureSourceSchemaVersion,
			Source.GetKind() == ETextureSourceKind::LongLatCube ? "Panorama.RGBA32F" : "Cube.RGBA8", 1}; }
		auto Settings(const FBuildContext& Context) -> std::expected<FTextureCubeBuildKeyInput, std::string>
		{
			const auto* Layout = Context.FindConstant<uint64>("Layout");
			const auto* Dimension = Context.FindConstant<uint64>("FaceDimension");
			const auto* Exposure = Context.FindConstant<float>("ExposureEV");
			const auto* SRGB = Context.FindConstant<bool>("SRGB");
			const auto* Projection = Context.FindConstant<uint64>("ProjectionVersion");
			const auto* Platform = Context.FindConstant<uint64>("TargetPlatform");
			const auto* Profile = Context.FindConstant<uint64>("TargetProfile");
			if (!Layout || !Dimension || !Exposure || !SRGB || !Projection || !Platform || !Profile
				|| *Layout > 1 || *Dimension > MaximumTextureCubeDimension || !*Projection || *Projection > UINT32_MAX
				|| *Platform != static_cast<uint64>(ECookTargetPlatform::Win64) || *Profile != static_cast<uint64>(ECookTargetProfile::Game))
				return std::unexpected("Cube build constants are invalid.");
			FTextureCubeBuildKeyInput Result{.SourceLayout = static_cast<ETextureCubeBuildSourceLayout>(*Layout),
				.FaceDimension = static_cast<uint32>(*Dimension), .ExposureEV = *Exposure, .bSRGB = *SRGB,
				.ProjectionVersion = static_cast<uint32>(*Projection), .TargetPlatform = static_cast<ECookTargetPlatform>(*Platform),
				.TargetProfile = static_cast<ECookTargetProfile>(*Profile)};
			if (!Result.IsValid() || (*Layout == 0 && (*Dimension != 0 || *Exposure != 0)) || (*Layout == 1 && *SRGB))
				return std::unexpected("Cube normalization constants are invalid.");
			return Result;
		}

		class FCubeResolver final : public IBuildInputResolver
		{
		public:
			FCubeResolver(const FTextureSource& Source, const FTextureCubeCanonicalBuildInput* Prepared) : Source(Source.CopyTornOff())
			{
				if (!Prepared) return;
				const bool HDR = Source.GetKind() == ETextureSourceKind::LongLatCube;
				PreparedValid = HDR ? Prepared->AuthoredPanorama.IsValid() : Prepared->DecodedFaces.IsValid();
				if (!HDR) PreparedValid = PreparedValid && Prepared->DecodedFaces.TransparencyMask == Source.GetTransparencyMask()
					&& Prepared->DecodedFaces.SourceChannelCounts[0] == Source.GetSourceChannelCount();
				Captured.emplace();
				for (uint32 Index = 0; Index < (HDR ? 1u : TextureCubeFaceCount); ++Index)
				{
					const auto& Image = HDR ? Prepared->AuthoredPanorama : Prepared->DecodedFaces.Faces[Index];
					const auto& Info = Image.GetInfo();
					PreparedValid = PreparedValid && Info.Width == Source.GetWidth() && Info.Height == Source.GetHeight()
						&& Info.Depth == 1 && Info.SliceCount == 1 && Info.Format == (HDR ? Image::ERawImageFormat::RGBA32F : Image::ERawImageFormat::RGBA8);
					Captured->push_back({HDR ? "Panorama" : std::format("Face/{}", Index), Image.GetView().GetBuffer()});
				}
			}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
			{
				if (Sources.size() != 1 || Sources[0].Name != "Source" || Sources[0].Source != "CapturedSource" || !Source.IsValid()
					|| (Source.GetKind() != ETextureSourceKind::TextureCube && Source.GetKind() != ETextureSourceKind::LongLatCube))
					return std::unexpected(Error("Cube captured source is invalid."));
				return std::vector{Identity(Source)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity(Source) || !PreparedValid)
					return std::unexpected(Error("Cube captured input does not match its identity."));
				const bool HDR = Source.GetKind() == ETextureSourceKind::LongLatCube;
				FBuildInput Result{.Identity = Inputs[0]};
				if (Captured) Result.Values = *Captured;
				else
				{
					const auto Mips = Source.GetMipData();
					if (!Mips.IsValid()) return std::unexpected(Error("Cube source bytes are unavailable or corrupt."));
					if (HDR) Result.Values.push_back({"Panorama", Mips.GetMipData(0, 0, 0)});
					else
					{
						const uint64 FaceSize = uint64(Source.GetWidth()) * Source.GetHeight() * 4;
						if (Mips.GetData().GetSize() != FaceSize * TextureCubeFaceCount)
							return std::unexpected(Error("Cube face byte count is invalid."));
						for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
							Result.Values.push_back({std::format("Face/{}", Index), Mips.GetData().MakeView(Index * FaceSize, FaceSize)});
					}
				}
				FXxHash128Builder Hash;
				uint64 Size = 0;
				for (const auto& Value : Result.Values)
				{
					if (Cancel.IsCancelled()) return std::unexpected(Error("Cube source resolution was canceled."));
					if (Value.Data.GetSize() > MaximumTextureSourceBytes - Size) return std::unexpected(Error("Cube source exceeds its byte bound."));
					Size += Value.Data.GetSize(); Hash.Update(Value.Data.GetBytes());
				}
				if (Size != Source.GetDecodedPayloadSize() || Hash.Finalize() != Source.GetDecodedPayloadHash())
					return std::unexpected(Error("Cube canonical source checksum changed."));
				FBinaryWriter Metadata({.MaximumTotalBytes = 16});
				Metadata.WriteU32(Source.GetWidth()); Metadata.WriteU32(Source.GetHeight());
				Metadata.WriteU32(Source.GetSourceChannelCount()); Metadata.WriteU32(Source.GetTransparencyMask());
				Result.Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes());
				return std::vector<FBuildInput>{std::move(Result)};
			}
		private:
			FTextureSource Source;
			std::optional<std::vector<FBuildInputValue>> Captured;
			bool PreparedValid = true;
		};

		class FCubeFunction final : public IBuildFunction
		{
		public:
			explicit FCubeFunction(ITextureBuildModule& Module) : Module(Module), Version(Module.GetTextureCubeBuilderVersion()), Projection(Module.GetTextureCubeProjectionVersion()) {}
			auto GetName() const -> std::string_view override { return "Durin.TextureCube"; }
			auto GetVersion() const -> uint32 override { return Version; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ Context.SetConstantsSchema(2); Context.SetOutput("TextureCube.Output", 2); Context.SetCacheBucket(FCacheBucket::FromString(TextureCubeCacheBucket)); }
			auto Build(FBuildContext& Context) const -> void override
			{
				auto Fail = [&](std::string Text) { Context.AddError(std::move(Text)); };
				auto Options = Settings(Context); if (!Options) return Fail(std::move(Options.error()));
				const bool HDR = Options->SourceLayout == ETextureCubeBuildSourceLayout::EquirectangularPanorama;
				const auto* Source = Context.FindInput("Source");
				if (Options->ProjectionVersion != Projection || !Source || Source->Identity.IdentityScheme != "TextureSource" || Source->Identity.IdentityVersion != TextureSourceSchemaVersion
					|| Source->Identity.Representation != (HDR ? "Panorama.RGBA32F" : "Cube.RGBA8") || Source->Identity.RepresentationVersion != 1
					|| Source->Values.size() != (HDR ? 1u : TextureCubeFaceCount)) return Fail("Cube input representation is invalid.");
				FBinaryReader Metadata(Source->Metadata.GetBytes(), {.MaximumTotalBytes = 16});
				uint32 Width = 0, Height = 0, Channels = 0, Transparency = 0;
				if (!Metadata.ReadU32(Width) || !Metadata.ReadU32(Height) || !Metadata.ReadU32(Channels) || !Metadata.ReadU32(Transparency)
					|| !Metadata.IsAtEnd() || !Width || !Height || Width > MaximumTextureCubePanoramaDimension || Height > MaximumTextureCubePanoramaDimension
					|| !Channels || Channels > 4 || Transparency > 63 || (HDR && (Channels != 4 || Transparency != 0)))
					return Fail("Cube input metadata is invalid.");
				FTextureCubeDecodedFaces Faces;
				Image::FImage Panorama;
				for (uint32 Index = 0; Index < (HDR ? 1u : TextureCubeFaceCount); ++Index)
				{
					const auto Id = HDR ? "Panorama" : std::format("Face/{}", Index);
					const auto Value = std::find_if(Source->Values.begin(), Source->Values.end(), [&](const auto& Value) { return Value.Name == Id; });
					if (Value == Source->Values.end()) return Fail("Cube input block is missing.");
					auto Image = Image::FImage::TryCreate({.Width = Width, .Height = Height,
						.Format = HDR ? Image::ERawImageFormat::RGBA32F : Image::ERawImageFormat::RGBA8,
						.GammaSpace = HDR ? Image::EImageGammaSpace::Linear : Image::EImageGammaSpace::Unknown}, Value->Data);
					if (!Image) return Fail(Image.error().ToString());
					if (HDR) Panorama = std::move(*Image); else Faces.Faces[Index] = std::move(*Image);
				}
				Faces.SourceChannelCounts.fill(static_cast<uint8>(Channels)); Faces.TransparencyMask = static_cast<uint8>(Transparency);
				auto Built = Module.BuildTextureCube({.DecodedFaces = std::cref(Faces), .bSRGB = Options->bSRGB,
					.TargetPlatform = Options->TargetPlatform, .TargetProfile = Options->TargetProfile, .HDRPanorama = HDR ? &Panorama : nullptr,
					.PanoramaSettings = {.FaceDimension = Options->FaceDimension, .ExposureEV = Options->ExposureEV,
						.Output = HDR ? ETextureCubeOutput::HDR : ETextureCubeOutput::LDR}});
				if (!Built) return Fail(Built.error().Diagnostic);
				if (!*Built) return Fail("Cube recipe returned no product.");
				auto Output = MakeTextureCubeSharedOutput(**Built, Options->TargetPlatform, Options->TargetProfile);
				if (!Output) return Fail(std::move(Output.error()));
				for (const auto& Value : Output->GetValues()) Context.AddValue(Value.Id, Value.Value.GetData());
				for (const auto& Meta : Output->GetMetadata()) Context.AddMeta(Meta.Id, Meta.Object);
			}
		private:
			ITextureBuildModule& Module;
			uint32 Version, Projection;
		};
	}
	auto MakeTextureCubeBuildFunction(ITextureBuildModule& Module) -> std::shared_ptr<const IBuildFunction>
	{ return std::make_shared<FCubeFunction>(Module); }
	auto MakeTextureCubeInputResolver(const FTextureSource& Source, const FTextureCubeCanonicalBuildInput* Prepared) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FCubeResolver>(Source, Prepared); }
	auto MakeTextureCubeSessionDefinition(const FTextureCubeBuildKeyInput& Input) -> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		if (!Input.IsValid()) return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidConstant, "TextureCube"});
		const bool HDR = Input.SourceLayout == ETextureCubeBuildSourceLayout::EquirectangularPanorama;
		FBuildDefinitionBuilder Builder("Durin.TextureCube");
		Builder.AddConstant("Layout", uint64(Input.SourceLayout)).AddConstant("SRGB", Input.bSRGB)
			.AddConstant("FaceDimension", uint64(HDR ? Input.FaceDimension : 0)).AddConstant("ExposureEV", HDR ? Input.ExposureEV : 0.0f)
			.AddConstant("ProjectionVersion", uint64(Input.ProjectionVersion)).AddConstant("TargetPlatform", uint64(Input.TargetPlatform))
			.AddConstant("TargetProfile", uint64(Input.TargetProfile)).AddInput("Source", "CapturedSource");
		return std::move(Builder).Build();
	}
}
#endif

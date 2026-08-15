#include "Texture/TextureCubeBuildOperations.h"

#include "AssetBuild/BuildSession.h"
#include "Hash/XxHash.h"
#include "Serialization/Archive.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureCubeDerivedData.h"

namespace Durin::Asset::Build
{
	namespace
	{
		inline constexpr uint64 TextureCubeDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		inline constexpr uint32 TextureCubeDerivedDataCleanupDeleteLimit = 16;
		const FBuildFunctionIdentity TextureCubeFunctionIdentity{
			"Durin.TextureBuild.TextureCube", 1};
		constexpr std::string_view TextureCubeInputName = "TextureCubeBuildInput";
		constexpr std::string_view TextureCubeValueName = "TextureCubePayload";

		auto ParseU32(std::string_view Text, uint32& OutValue) -> bool
		{
			if (Text.empty()) return false;
			uint64 Value = 0;
			for (const char Character : Text)
			{
				if (Character < '0' || Character > '9') return false;
				Value = Value * 10 + static_cast<uint32>(Character - '0');
				if (Value > std::numeric_limits<uint32>::max()) return false;
			}
			OutValue = static_cast<uint32>(Value);
			return true;
		}
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

		auto MakeSourceFile(std::string_view Path, const FXxHash128& Hash)
			-> FTextureSourceFile
		{
			return {{.Path = std::string(Path)}, Hash.HashLow, Hash.HashHigh};
		}

		auto ValidateCubeSourceData(
			const FTextureCubeSourceData& SourceData,
			std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceData& Face = SourceData.Faces[Index];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[Index]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[Index], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height
					|| Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format("{} face source layout must be identical to PositiveX.",
						FaceNames[Index]);
					return false;
				}
			}
			return true;
		}

		auto BuildCubePlatformData(
			const FTextureCubeSourceData& SourceData,
			bool bSRGB,
			FTextureCubePlatformData& OutPlatformData,
			std::string& OutError) -> bool
		{
			if (!ValidateCubeSourceData(SourceData, OutError)) return false;
			const bool bHasTransparency = std::ranges::any_of(
				SourceData.Faces, [](const FTextureSourceData& Face) {
					return Face.bHasTransparency;
				});
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				FTextureSourceData BuildSource = SourceData.Faces[Index];
				BuildSource.bHasTransparency = bHasTransparency;
				if (!TextureBuilder::BuildMipChain(
					BuildSource, ETextureUsage::Color, bSRGB,
					OutPlatformData.Faces[Index], OutError))
				{
					OutError = std::format(
						"{} face platform build failed: {}", FaceNames[Index], OutError);
					return false;
				}
			}
			OutPlatformData.PixelFormat = OutPlatformData.Faces[0].PixelFormat;
			if (OutPlatformData.IsValid()) return true;
			OutError = "Cube texture platform data is inconsistent.";
			return false;
		}

		auto EncodePlatformData(const FTextureCubePlatformData& PlatformData,
			FBuildValue& OutValue, std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTextureCubePlatformData&>(PlatformData).Serialize(Ar, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				return false;
			}
			OutValue = FBuildValue::FromOwned(std::string(TextureCubeValueName), std::move(Bytes));
			OutError.clear();
			return true;
		}

		auto DecodePlatformData(const FBuildValue& Value,
			FTextureCubePlatformData& OutPlatformData, std::string& OutError) -> bool
		{
			if (Value.GetName() != TextureCubeValueName)
			{
				OutError = "TextureCube build value name is incompatible.";
				return false;
			}
			FTextureCubePlatformData Candidate;
			FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			Candidate.Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate.IsValid())
			{
				OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
					: "TextureCube payload is invalid or has trailing bytes.";
				return false;
			}
			OutPlatformData = std::move(Candidate);
			OutError.clear();
			return true;
		}

		auto AppendU32(std::vector<uint8>& Bytes, uint32 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 4; ++Byte) Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto AppendU64(std::vector<uint8>& Bytes, uint64 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 8; ++Byte) Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto ReadU32(std::span<const uint8> Bytes, size_t& Offset, uint32& Value) -> bool
		{
			if (Offset + 4 > Bytes.size()) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 4; ++Byte) Value |= uint32(Bytes[Offset++]) << (Byte * 8);
			return true;
		}
		auto ReadU64(std::span<const uint8> Bytes, size_t& Offset, uint64& Value) -> bool
		{
			if (Offset + 8 > Bytes.size()) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 8; ++Byte) Value |= uint64(Bytes[Offset++]) << (Byte * 8);
			return true;
		}

		auto EncodeSourceData(const FTextureCubeSourceData& SourceData) -> std::vector<uint8>
		{
			std::vector<uint8> Bytes;
			for (const FTextureSourceData& Face : SourceData.Faces)
			{
				AppendU32(Bytes, Face.Width);
				AppendU32(Bytes, Face.Height);
				AppendU32(Bytes, Face.SourceChannelCount);
				AppendU32(Bytes, static_cast<uint32>(Face.Format));
				AppendU32(Bytes, Face.bHasTransparency ? 1u : 0u);
				AppendU64(Bytes, Face.Pixels.size());
				Bytes.insert(Bytes.end(), Face.Pixels.begin(), Face.Pixels.end());
			}
			return Bytes;
		}

		auto DecodeSourceData(std::span<const uint8> Bytes,
			FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool
		{
			size_t Offset = 0;
			FTextureCubeSourceData Candidate;
			for (FTextureSourceData& Face : Candidate.Faces)
			{
				uint32 Channels = 0, Format = 0, Transparency = 0;
				uint64 ByteCount = 0;
				if (!ReadU32(Bytes, Offset, Face.Width) || !ReadU32(Bytes, Offset, Face.Height)
					|| !ReadU32(Bytes, Offset, Channels) || !ReadU32(Bytes, Offset, Format)
					|| !ReadU32(Bytes, Offset, Transparency) || !ReadU64(Bytes, Offset, ByteCount)
					|| Channels > std::numeric_limits<uint8>::max()
					|| ByteCount > Bytes.size() - Offset)
				{
					OutError = "TextureCube local build input is malformed.";
					return false;
				}
				Face.SourceChannelCount = static_cast<uint8>(Channels);
				Face.Format = static_cast<ETextureSourceFormat>(Format);
				Face.bHasTransparency = Transparency != 0;
				Face.Pixels.assign(Bytes.begin() + Offset, Bytes.begin() + Offset + ByteCount);
				Offset += static_cast<size_t>(ByteCount);
			}
			if (Offset != Bytes.size() || !ValidateCubeSourceData(Candidate, OutError))
			{
				if (OutError.empty()) OutError = "TextureCube local build input has trailing bytes.";
				return false;
			}
			OutSourceData = std::move(Candidate);
			return true;
		}

		class FTextureCubeBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheRoot = "TextureCube/Objects",
					.ExpectedValueName = std::string(TextureCubeValueName),
					.MaximumValueBytes = MaximumTexturePayloadBytes,
					.CleanupBudgetBytes = TextureCubeDerivedDataBudgetBytes,
					.CleanupDeleteLimit = TextureCubeDerivedDataCleanupDeleteLimit};
			}
			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				if (Definition.GetTargetFact("Platform") != std::optional<std::string_view>("Win64")
					|| Definition.GetTargetFact("Profile") != std::optional<std::string_view>("Game"))
				{
					OutError = "TextureCube target facts are missing or incompatible.";
					return false;
				}
				FTextureCubePlatformData PlatformData;
				if (!DecodePlatformData(Value, PlatformData, OutError)) return false;
				const auto DimensionFact = Definition.GetTargetFact("Dimension");
				uint32 Dimension = 0;
				if (DimensionFact && (!ParseU32(*DimensionFact, Dimension)
					|| PlatformData.Faces[0].Mips[0].Width != Dimension))
				{
					OutError = "TextureCube payload dimension does not match the definition.";
					return false;
				}
				return true;
			}
			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(TextureCubeInputName);
				FTextureCubeSourceData SourceData;
				if (!Input || !DecodeSourceData(Input->GetBytes(), SourceData, OutError)) return false;
				if (Context.IsCanceled()) { OutError = "TextureCube build was canceled."; return false; }
				const auto SRGB = Context.GetDefinition().GetTargetFact("SRGB");
				if (!SRGB || (*SRGB != "0" && *SRGB != "1"))
				{
					OutError = "TextureCube sRGB target fact is missing.";
					return false;
				}
				FTextureCubePlatformData PlatformData;
				if (!BuildCubePlatformData(SourceData, *SRGB == "1", PlatformData, OutError)) return false;
				if (Context.IsCanceled()) { OutError = "TextureCube build was canceled."; return false; }
				return EncodePlatformData(PlatformData, OutValue, OutError);
			}
		};

		std::mutex GTextureCubeFunctionMutex;
		FBuildFunctionRegistration GTextureCubeFunctionRegistration;
		auto EnsureTextureCubeBuildFunction(std::string* OutError,
			FModuleOwnedCallbackGate Gate = {}) -> bool
		{
			std::lock_guard Lock(GTextureCubeFunctionMutex);
			if (GTextureCubeFunctionRegistration.IsValid()) return true;
			GTextureCubeFunctionRegistration = RegisterBuildFunction(TextureCubeFunctionIdentity,
				std::make_shared<FTextureCubeBuildFunction>(), std::move(Gate), OutError);
			return GTextureCubeFunctionRegistration.IsValid();
		}

		auto ExecuteCubeBuild(FTextureCubeSourceData& SourceData,
			const FTextureCubeBuildKeyInput& KeyInput, std::string& OutKey,
			std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
			std::string& OutError) -> bool
		{
			if (!EnsureTextureCubeBuildFunction(&OutError)) return false;
			const std::vector<uint8> KeyBytes = BuildTextureCubeDerivedDataKeyBytes(KeyInput, OutError);
			OutKey = KeyBytes.empty() ? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
			if (OutKey.empty()) return false;
			FBuildDefinition Definition;
			FBuildDefinitionBuilder Builder(TextureCubeFunctionIdentity, std::string(TextureCubeValueName));
			Builder.SetKey(FBuildKey::FromString(OutKey), KeyBytes)
				.AddTargetFact("Platform", "Win64").AddTargetFact("Profile", "Game")
				.AddTargetFact("SRGB", KeyInput.bSRGB ? "1" : "0")
				.AddTargetFact("Dimension", std::to_string(SourceData.Faces[0].Width))
				.AddInput(FBuildValue::FromOwned(std::string(TextureCubeInputName), EncodeSourceData(SourceData)));
			if (!Builder.Build(Definition, &OutError)) return false;
			const FBuildOutput Output = FBuildSession().Build(Definition, {
				.bQueryCache = true, .bAllowLocalBuild = true, .bStoreBuildResult = true,
				.bRequireStoreSuccess = true, .bReturnData = true});
			if (!Output.Succeeded()) { OutError = Output.Diagnostic; return false; }
			auto Candidate = std::make_unique<FTextureCubePlatformData>();
			if (!DecodePlatformData(Output.Value, *Candidate, OutError)) return false;
			OutPlatformData = std::move(Candidate);
			return true;
		}

		auto FinishPanoramaProduct(
			FTextureCubeSourceData SourceData,
			uint32 SourceWidth,
			uint32 SourceHeight,
			const FXxHash128& Hash,
			const FTextureCubePanoramaBuildSettings& Settings,
			FTextureCubeBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			const FTextureCubeBuildKeyInput KeyInput{
				.SourceLayout = ETextureCubeBuildSourceLayout::EquirectangularPanorama,
				.PanoramaContentHash = Hash,
				.FaceDimension = Settings.FaceDimension,
				.ExposureEV = Settings.ExposureEV,
				.bSRGB = true,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game};
			std::string Key;
			std::unique_ptr<FTextureCubePlatformData> PlatformData;
			if (!ExecuteCubeBuild(SourceData, KeyInput, Key, PlatformData, OutError)) return false;
			OutProduct = {
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.SourceData = std::move(SourceData),
				.PlatformData = std::move(PlatformData),
				.DerivedDataKey = std::move(Key),
				.SourceWidth = SourceWidth,
				.SourceHeight = SourceHeight,
				.PanoramaFaceDimension = Settings.FaceDimension,
				.PanoramaExposureEV = Settings.ExposureEV,
				.bSRGB = true};
			OutError.clear();
			return true;
		}
	}

	auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		FTextureCubeSourceData SourceData;
		if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
			Panorama, {Settings.FaceDimension, Settings.ExposureEV}, SourceData, OutError))
			return false;
		return FinishPanoramaProduct(std::move(SourceData), Panorama.Width,
			Panorama.Height, SourceHash, Settings, OutProduct, OutError);
	}

	auto MakeTextureCubeDerivedDataKey(
		const DTextureCube& Texture,
		std::string& OutError) -> std::string
	{
		const FTextureCubeSourceImportData& Source = Texture.GetSourceImportData();
		if (!Source.HasSource())
		{
			OutError = "TextureCube has no persisted source identity.";
			return {};
		}
		FTextureCubeBuildKeyInput Input{
			.SourceLayout = Source.SourceLayout == ETextureCubeSourceLayout::SixFaces
				? ETextureCubeBuildSourceLayout::SixFaces
				: ETextureCubeBuildSourceLayout::EquirectangularPanorama,
			.FaceDimension = Texture.GetPanoramaFaceDimension(),
			.ExposureEV = Texture.GetPanoramaExposureEV(),
			.bSRGB = Texture.IsSRGB(),
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		if (Source.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceFile& Face =
					Source.GetFace(static_cast<ETextureCubeFace>(Index));
				if (!Face.HasSource() || !Face.HasContentHash())
				{
					OutError = "TextureCube face source provenance is incomplete.";
					return {};
				}
				Input.FaceContentHashes[Index] = {
					.HashLow = Face.SourceContentHashLow,
					.HashHigh = Face.SourceContentHashHigh};
			}
		}
		else
		{
			if (!Source.Panorama.HasSource() || !Source.Panorama.HasContentHash())
			{
				OutError = "TextureCube panorama source provenance is incomplete.";
				return {};
			}
			Input.PanoramaContentHash = {
				.HashLow = Source.Panorama.SourceContentHashLow,
				.HashHigh = Source.Panorama.SourceContentHashHigh};
		}
		return BuildTextureCubeDerivedDataKey(Input, OutError);
	}

	auto LoadTextureCubeDerivedData(
		std::string_view Key,
		std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool
	{
		std::string Error;
		if (!EnsureTextureCubeBuildFunction(&Error))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			OutMessage = Error;
			return false;
		}
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(TextureCubeFunctionIdentity, std::string(TextureCubeValueName));
		Builder.SetKey(FBuildKey::FromString(Key)).AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game");
		if (!Builder.Build(Definition, &Error))
		{
			OutStatus = ETextureDerivedDataStatus::Incompatible;
			OutMessage = Error;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false, .bReturnData = true});
		if (!Output.Succeeded())
		{
			OutStatus = Output.Status == EBuildStatus::CacheMiss
				? ETextureDerivedDataStatus::Missing : ETextureDerivedDataStatus::Corrupt;
			OutMessage = Output.Diagnostic;
			return false;
		}
		auto Candidate = std::make_unique<FTextureCubePlatformData>();
		if (!DecodePlatformData(Output.Value, *Candidate, OutMessage))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutStatus = ETextureDerivedDataStatus::Hit;
		OutMessage.clear();
		return true;
	}

	auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		FTextureCubeSourceData SourceData;
		if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
			Panorama, {Settings.FaceDimension, Settings.ExposureEV}, SourceData, OutError))
			return false;
		return FinishPanoramaProduct(std::move(SourceData), Panorama.Width,
			Panorama.Height, SourceHash, Settings, OutProduct, OutError);
	}

	auto BuildTextureCubeFaces(
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& Hashes,
		const FTextureCubeFacesBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		const FTextureCubeBuildKeyInput KeyInput{
			.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
			.FaceContentHashes = Hashes,
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		std::string Key;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		if (!ExecuteCubeBuild(SourceData, KeyInput, Key, PlatformData, OutError)) return false;
		const uint32 SourceWidth = SourceData.Faces[0].Width;
		const uint32 SourceHeight = SourceData.Faces[0].Height;
		OutProduct = {
			.SourceLayout = ETextureCubeSourceLayout::SixFaces,
			.SourceData = std::move(SourceData),
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = std::move(Key),
			.SourceWidth = SourceWidth,
			.SourceHeight = SourceHeight,
			.bSRGB = Settings.bSRGB};
		OutError.clear();
		return true;
	}

	auto InitializeTextureCubeBuildFunction(FModuleOwnedCallbackGate Gate,
		std::string* OutError) -> bool
	{
		return EnsureTextureCubeBuildFunction(OutError, std::move(Gate));
	}

	auto ShutdownTextureCubeBuildFunction() -> void
	{
		std::lock_guard Lock(GTextureCubeFunctionMutex);
		GTextureCubeFunctionRegistration.Reset();
	}

	auto PublishTextureCubeProduct(
		DTextureCube& Texture,
		FTextureCubeBuildProduct Product,
		const FTextureCubePublicationContext& Context,
		std::string& OutError) -> bool
	{
		if (!Product.PlatformData || !Product.PlatformData->IsValid()
			|| !Product.SourceData.Faces[0].IsValid() || Product.DerivedDataKey.empty())
		{
			OutError = "TextureCube publication product is incomplete.";
			return false;
		}
		FTextureCubeSourceImportData Provenance;
		Provenance.SourceLayout = Product.SourceLayout;
		Provenance.DecoderId = Context.DecoderId;
		Provenance.DecoderVersion = Context.DecoderVersion;
		Provenance.ProjectionVersion = TextureCubeProjectionVersion;
		if (Product.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				Provenance.GetMutableFace(static_cast<ETextureCubeFace>(Index)) =
					MakeSourceFile(Context.FacePaths[Index].Path, Context.FaceHashes[Index]);
		}
		else
		{
			Provenance.Panorama = MakeSourceFile(
				Context.PanoramaPath.Path, Context.PanoramaHash);
		}
		const std::string DiagnosticKey = Product.DerivedDataKey;
		const bool bPanorama = Product.SourceLayout
			== ETextureCubeSourceLayout::EquirectangularPanorama;
		Texture.PublishAuthoringCandidate(
			Product.SourceLayout, std::move(Provenance), Product.PanoramaFaceDimension,
			Product.PanoramaExposureEV, Product.SourceWidth, Product.SourceHeight,
			Product.bSRGB,
			std::make_unique<FTextureCubeSourceData>(std::move(Product.SourceData)),
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			{.Status = ETextureDerivedDataStatus::Rebuilt,
				.Key = DiagnosticKey,
				.Message = bPanorama
					? "Built TextureCube panorama candidate from normalized pixels."
					: "Built six-face TextureCube candidate from normalized pixels.",
				.bSourceDecoderInvoked = true});
		OutError.clear();
		return true;
	}

}

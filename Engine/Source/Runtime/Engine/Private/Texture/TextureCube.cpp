#include "Texture/TextureCube.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "Texture/EquirectangularTextureCube.h"
#include "Texture/TextureBuild.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceSuffixes = {
			"px", "nx", "py", "ny", "pz", "nz"};
		constexpr uint64 TextureCubeDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureCubeDerivedDataCleanupDeleteLimit = 16;
		constexpr std::string_view TextureSourceRoot = "Textures";
		constexpr std::string_view TextureDecoderId = "DurinImage";
		constexpr uint32 TextureDecoderVersion = 1;

		auto GetTextureCubeObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore("TextureCube/Objects", MaximumTexturePayloadBytes);
		}

		auto FaceToIndex(ETextureCubeFace Face) -> size_t
		{
			const size_t Index = static_cast<size_t>(Face);
			check(Index < TextureCubeFaceCount);
			return Index;
		}

		auto FindOwningMount(std::string_view VirtualPath) -> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Suffix,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::filesystem::path& OutPhysicalPath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format("TextureCube asset {} is not beneath a registered package mount.",
					AssetPath.ToString());
				return false;
			}
			if (RequestedSourcePath.empty())
			{
				std::filesystem::path RelativeAssetPath(
					std::string(AssetPath.ToString().substr(Mount->VirtualRoot.size())));
				const std::string FileName = std::format("{}{}{}",
					RelativeAssetPath.stem().generic_string(), Suffix, Extension);
				RelativeAssetPath.replace_filename(FileName);
				const std::filesystem::path StoredPath =
					std::filesystem::path(TextureSourceRoot) / RelativeAssetPath;
				OutStoredPath = Mount->VirtualRoot + StoredPath.lexically_normal().generic_string();
			}
			else
			{
				OutStoredPath = RequestedSourcePath;
			}
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					OutStoredPath, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPhysicalPath = Resolved.PhysicalPath;
			return true;
		}

		auto HashTextureSource(const std::filesystem::path& Path,
			FXxHash128& OutHash, std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutError = std::format("Failed to read TextureCube source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes);
			OutError.clear();
			return true;
		}

		auto MakeSourceFile(const std::string& StoredPath, const FXxHash128& Hash) -> FTextureSourceFile
		{
			return {
				.SourcePath = {.Path = StoredPath},
				.SourceContentHashLow = Hash.HashLow,
				.SourceContentHashHigh = Hash.HashHigh};
		}

		auto ResolveCubeSource(const DTextureCube& Texture, ETextureCubeFace Face) -> std::filesystem::path
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces
				&& Provenance.GetFace(Face).HasSource())
			{
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.GetFace(Face).SourcePath.Path,
						PathUtilities::EPathExistence::RequireFile);
				if (Resolved) return Resolved.PhysicalPath;
			}
			return {};
		}

		auto ResolvePanoramaSource(const DTextureCube& Texture) -> std::filesystem::path
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama
				&& Provenance.Panorama.HasSource())
			{
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.Panorama.SourcePath.Path,
						PathUtilities::EPathExistence::RequireFile);
				if (Resolved) return Resolved.PhysicalPath;
			}
			return {};
		}

		auto ValidateCubeProvenance(const DTextureCube& Texture, std::string& OutError) -> bool
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (!Provenance.HasSource()) return true;
			if (!Texture.GetPackage())
			{
				OutError = "TextureCube source cannot be validated without an owning package.";
				return false;
			}
			auto ValidateSourcePath = [&](std::string_view SourcePath) -> bool {
				const PathUtilities::FMountPolicyResult Dependency =
					PathUtilities::CheckMountDependency(
						Texture.GetPackage()->GetPackagePath(), SourcePath);
				if (!Dependency)
				{
					OutError = Dependency.Message;
					return false;
				}
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						SourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Resolved
					&& Resolved.Error != PathUtilities::EMountPathError::UnavailableRoot)
				{
					OutError = Resolved.Message;
					return false;
				}
				return true;
			};
			if (Provenance.DecoderId != TextureDecoderId
				|| Provenance.DecoderVersion != TextureDecoderVersion
				|| Provenance.ProjectionVersion != TextureCubeProjectionVersion)
			{
				OutError = "TextureCube source decoder or projection version is unsupported.";
				return false;
			}
			if (Provenance.SourceLayout != Texture.GetSourceLayout())
			{
				OutError = "TextureCube source provenance layout does not match its authored layout.";
				return false;
			}
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				if (Provenance.Panorama.HasSource())
				{
					OutError = "Six-face TextureCube provenance contains an inactive panorama source.";
					return false;
				}
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
				{
					const FTextureSourceFile& Source =
						Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex));
					if (!Source.HasSource() || !Source.HasContentHash())
					{
						OutError = "TextureCube face source provenance is incomplete.";
						return false;
					}
					if (!ValidateSourcePath(Source.SourcePath.Path)) return false;
				}
				return true;
			}
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				if (!Provenance.Panorama.HasSource() || !Provenance.Panorama.HasContentHash())
				{
					OutError = "TextureCube panorama source provenance is incomplete.";
					return false;
				}
				if (!ValidateSourcePath(Provenance.Panorama.SourcePath.Path)) return false;
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
					if (Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex)).HasSource())
					{
						OutError = "Panorama TextureCube provenance contains inactive face sources.";
						return false;
					}
				return true;
			}
			OutError = "TextureCube source provenance layout is invalid.";
			return false;
		}

		auto MakeTextureCubeDerivedDataKey(
			const DTextureCube& Texture, std::string& OutKey, std::string& OutError) -> bool
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (!Provenance.HasSource())
			{
				OutError = "TextureCube has no persisted source identity.";
				return false;
			}
			if (!ValidateCubeProvenance(Texture, OutError)) return false;
			FTextureCubeDerivedDataKeyInput Input{
				.SourceLayout = Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces
					? ETextureCubeDerivedDataSourceLayout::SixFaces
					: ETextureCubeDerivedDataSourceLayout::EquirectangularPanorama,
				.FaceDimension = Texture.GetPanoramaFaceDimension(),
				.ExposureEV = Texture.GetPanoramaExposureEV(),
				.bSRGB = Texture.IsSRGB(),
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game};
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
				{
					const FTextureSourceFile& Source =
						Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex));
					Input.FaceContentHashes[FaceIndex] = {
						Source.SourceContentHashLow, Source.SourceContentHashHigh};
				}
			}
			else
			{
				Input.PanoramaContentHash = {
					Provenance.Panorama.SourceContentHashLow,
					Provenance.Panorama.SourceContentHashHigh};
			}
			return BuildTextureCubeDerivedDataKey(Input, OutKey, OutError);
		}

		auto LoadTextureCubeDerivedData(
			std::string_view Key,
			std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
			ETextureDerivedDataStatus& OutStatus,
			std::string& OutMessage) -> bool
		{
			std::vector<uint8> Bytes;
			const Asset::FDerivedDataObjectReadResult Read = GetTextureCubeObjectStore().Read(Key, Bytes);
			if (!Read)
			{
				OutStatus = Read.Status == Asset::EDerivedDataObjectReadStatus::Missing
					? ETextureDerivedDataStatus::Missing : ETextureDerivedDataStatus::Corrupt;
				OutMessage = Read.Message;
				return false;
			}
			FPayloadDecodeResult DecodeResult = DecodeTextureCubePayload(
				Bytes, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
				OutPlatformData);
			if (!DecodeResult)
			{
				OutStatus = DecodeResult.Code == EPayloadDecodeError::Incompatible
					? ETextureDerivedDataStatus::Incompatible : ETextureDerivedDataStatus::Corrupt;
				OutMessage = std::move(DecodeResult.Message);
				return false;
			}
			OutStatus = ETextureDerivedDataStatus::Hit;
			OutMessage.clear();
			return true;
		}

		auto StoreTextureCubeDerivedData(
			std::string_view Key, const FTextureCubePlatformData& PlatformData,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!EncodeTextureCubePayload(
				PlatformData, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
				Bytes, OutError)
				|| !GetTextureCubeObjectStore().Write(Key, Bytes, &OutError)) return false;
			const Asset::FDerivedDataObjectCleanupResult Cleanup =
				GetTextureCubeObjectStore().CleanupToBudget(
					TextureCubeDerivedDataBudgetBytes, TextureCubeDerivedDataCleanupDeleteLimit);
			if (!Cleanup.Message.empty()) DURIN_WARN("TextureCube DDC cleanup: {}", Cleanup.Message);
			return true;
		}

		auto MakePanoramaSourceFileName(std::string_view AssetName, std::string_view Extension) -> std::string
		{
			std::string NormalizedExtension(Extension);
			std::ranges::transform(NormalizedExtension, NormalizedExtension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return std::format("{}_panorama{}", AssetName, NormalizedExtension);
		}

		auto MakeSourceFileName(std::string_view AssetName, size_t FaceIndex, std::string_view Extension) -> std::string
		{
			return std::format("{}_{}{}", AssetName, FaceSuffixes[FaceIndex], Extension);
		}

		auto ValidateCubeSourceData(const FTextureCubeSourceData& SourceData, std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				const FTextureSourceData& Face = SourceData.Faces[FaceIndex];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[FaceIndex]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[FaceIndex], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height)
				{
					OutError = std::format("{} face dimensions {}x{} do not match PositiveX {}x{}; all faces must be identical.",
						FaceNames[FaceIndex], Face.Width, Face.Height, Reference.Width, Reference.Height);
					return false;
				}
				if (Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format("{} face source channel count {} does not match PositiveX {}; all faces must use an identical source format.",
						FaceNames[FaceIndex], Face.SourceChannelCount, Reference.SourceChannelCount);
					return false;
				}
			}
			return true;
		}

		auto BuildCubePlatformData(const FTextureCubeSourceData& SourceData, bool bSRGB,
			FTextureCubePlatformData& OutPlatformData, std::string& OutError) -> bool
		{
			OutPlatformData = {};
			if (!ValidateCubeSourceData(SourceData, OutError)) return false;
			const bool bCubeHasTransparency = std::ranges::any_of(SourceData.Faces,
				[](const FTextureSourceData& Face) { return Face.bHasTransparency; });
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				FTextureSourceData BuildSource = SourceData.Faces[FaceIndex];
				// All physical layers of one cube image must use one pixel format.
				BuildSource.bHasTransparency = bCubeHasTransparency;
				if (!TextureBuild::BuildMipChain(BuildSource, ETextureUsage::Color, bSRGB,
					OutPlatformData.Faces[FaceIndex], OutError))
				{
					OutError = std::format("{} face platform build failed: {}", FaceNames[FaceIndex], OutError);
					return false;
				}
			}
			OutPlatformData.PixelFormat = OutPlatformData.Faces[0].PixelFormat;
			if (OutPlatformData.IsValid()) return true;
			OutPlatformData = {};
			OutError = "Cube texture platform data is inconsistent.";
			return false;
		}

		auto DecodeCubeInputs(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			std::array<std::filesystem::path, TextureCubeFaceCount>& OutInputs,
			FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool
		{
#if DURIN_WITH_EDITOR
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				if (FaceFiles[FaceIndex].empty())
				{
					OutError = std::format("{} face source is missing.", FaceNames[FaceIndex]);
					return false;
				}
				OutInputs[FaceIndex] = std::filesystem::absolute(FaceFiles[FaceIndex]).lexically_normal();
				if (!std::filesystem::is_regular_file(OutInputs[FaceIndex]))
				{
					OutError = std::format("{} face source file does not exist.", FaceNames[FaceIndex]);
					return false;
				}
				if (!Asset::IsSupportedImageExtension(OutInputs[FaceIndex].extension().generic_string()))
				{
					OutError = std::format("{} face uses an unsupported texture source format.", FaceNames[FaceIndex]);
					return false;
				}
				std::string DecodeError;
				if (!TextureBuild::DecodeRGBA8(OutInputs[FaceIndex].generic_string(),
					OutSourceData.Faces[FaceIndex], DecodeError))
				{
					OutError = std::format("{} face decode failed: {}", FaceNames[FaceIndex], DecodeError);
					return false;
				}
			}
			return ValidateCubeSourceData(OutSourceData, OutError);
#else
			(void)FaceFiles;
			(void)OutInputs;
			OutSourceData = {};
			OutError = "TextureCube source decoding is unavailable in runtime-only builds.";
			return false;
#endif
		}

		enum class EPanoramaDecodeError
		{
			None,
			Decode,
			Build
		};

		auto DecodePanoramaInput(const std::filesystem::path& Input,
			const FTextureCubePanoramaImportSettings& Settings, FTextureCubeSourceData& OutSourceData,
			uint32& OutSourceWidth, uint32& OutSourceHeight, bool& bOutHDR,
			EPanoramaDecodeError& OutCode, std::string& OutError) -> bool
		{
#if DURIN_WITH_EDITOR
			OutSourceData = {};
			OutSourceWidth = 0;
			OutSourceHeight = 0;
			bOutHDR = false;
			OutCode = EPanoramaDecodeError::Build;
			if (!std::filesystem::is_regular_file(Input))
			{
				OutError = "Panorama source file does not exist.";
				return false;
			}

			const std::string Extension = Input.extension().generic_string();
			const TextureBuild::FEquirectangularTextureCubeProjectionSettings ProjectionSettings{
				.FaceDimension = Settings.FaceDimension,
				.ExposureEV = Settings.ExposureEV,
			};
			if (Asset::IsRadianceHDRExtension(Extension))
			{
				bOutHDR = true;
				Asset::FDecodedFloatImage Panorama;
				if (!Asset::DecodeRadianceHDRFromFile(Input.generic_string(), Panorama, OutError))
				{
					OutCode = EPanoramaDecodeError::Decode;
					OutError = std::format("Panorama HDR decode failed: {}", OutError);
					return false;
				}
				OutSourceWidth = Panorama.Width;
				OutSourceHeight = Panorama.Height;
				if (!TextureBuild::ProjectEquirectangularTextureCube(
					Panorama, ProjectionSettings, OutSourceData, OutError))
				{
					OutError = std::format("Panorama projection failed: {}", OutError);
					return false;
				}
				OutCode = EPanoramaDecodeError::None;
				return true;
			}
			if (!Asset::IsSupportedImageExtension(Extension))
			{
				OutError = "Panorama uses an unsupported source format.";
				return false;
			}

			Asset::FDecodedImage Panorama;
			Asset::FImageDecodeLimits Limits;
			Limits.MaximumDecodedPixels = TextureBuild::MaximumPanoramaPixels;
			if (!Asset::DecodeImageFromFile(Input.generic_string(), Panorama, OutError, Limits))
			{
				OutCode = EPanoramaDecodeError::Decode;
				OutError = std::format("Panorama LDR decode failed: {}", OutError);
				return false;
			}
			OutSourceWidth = Panorama.Width;
			OutSourceHeight = Panorama.Height;
			if (!TextureBuild::ProjectEquirectangularTextureCube(
				Panorama, ProjectionSettings, OutSourceData, OutError))
			{
				OutError = std::format("Panorama projection failed: {}", OutError);
				return false;
			}
			OutCode = EPanoramaDecodeError::None;
			return true;
#else
			(void)Input;
			(void)Settings;
			OutSourceData = {};
			OutSourceWidth = 0;
			OutSourceHeight = 0;
			bOutHDR = false;
			OutCode = EPanoramaDecodeError::Build;
			OutError = "TextureCube panorama decoding and projection are unavailable in runtime-only builds.";
			return false;
#endif
		}

		auto ValidatePanorama(const std::filesystem::path& Input,
			const FTextureCubePanoramaImportSettings& Settings, FTextureCubeSourceData& OutSourceData,
			FTextureCubePlatformData& OutPlatformData, uint32& OutSourceWidth, uint32& OutSourceHeight,
			bool& bOutHDR, std::string& OutError) -> bool
		{
			EPanoramaDecodeError DecodeCode = EPanoramaDecodeError::None;
			if (!DecodePanoramaInput(
				Input, Settings, OutSourceData, OutSourceWidth, OutSourceHeight,
				bOutHDR, DecodeCode, OutError))
				return false;
			return BuildCubePlatformData(OutSourceData, true, OutPlatformData, OutError);
		}
	}

	auto FTextureCubeSourceImportData::GetFace(ETextureCubeFace Face) const -> const FTextureSourceFile&
	{
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: return PositiveX;
		case ETextureCubeFace::NegativeX: return NegativeX;
		case ETextureCubeFace::PositiveY: return PositiveY;
		case ETextureCubeFace::NegativeY: return NegativeY;
		case ETextureCubeFace::PositiveZ: return PositiveZ;
		case ETextureCubeFace::NegativeZ: return NegativeZ;
		}
		checkf(false, "Invalid cube face");
		return PositiveX;
	}

	auto FTextureCubeSourceImportData::GetMutableFace(ETextureCubeFace Face) -> FTextureSourceFile&
	{
		return const_cast<FTextureSourceFile&>(std::as_const(*this).GetFace(Face));
	}

	auto FTextureCubeSourceImportData::HasSource() const -> bool
	{
		if (SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			return Panorama.HasSource();
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			if (GetFace(static_cast<ETextureCubeFace>(FaceIndex)).HasSource()) return true;
		return false;
	}

	auto FTextureCubeSourceData::IsValid() const -> bool
	{
		std::string Error;
		return ValidateCubeSourceData(*this, Error);
	}

	auto FTextureCubePlatformData::IsValid() const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown) return false;
		const FTexturePlatformData& Reference = Faces[0];
		if (!Reference.IsValid() || Reference.PixelFormat != PixelFormat || Reference.Mips.front().Width != Reference.Mips.front().Height)
			return false;
		for (const FTexturePlatformData& Face : Faces)
		{
			if (!Face.IsValid() || Face.PixelFormat != PixelFormat || Face.Mips.size() != Reference.Mips.size()) return false;
			for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
			{
				if (Face.Mips[MipIndex].Width != Reference.Mips[MipIndex].Width
					|| Face.Mips[MipIndex].Height != Reference.Mips[MipIndex].Height
					|| Face.Mips[MipIndex].RowPitch != Reference.Mips[MipIndex].RowPitch) return false;
			}
		}
		return true;
	}

	DTextureCube::DTextureCube(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DTextureCube::~DTextureCube() = default;

	auto DTextureCube::GetSourceFile(ETextureCubeFace Face) const -> const std::string&
	{
		if (SourceImportData.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			const FTextureSourceFile& Source = SourceImportData.GetFace(Face);
			if (Source.HasSource()) return Source.SourcePath.Path;
		}
		static const std::string EmptySource;
		return EmptySource;
	}

	auto DTextureCube::ResolvePanoramaSource() const -> std::filesystem::path
	{
		return Durin::ResolvePanoramaSource(*this);
	}

	auto DTextureCube::GetBuiltFaceDimension() const -> uint32
	{
		if (PlatformData && !PlatformData->Faces[0].Mips.empty())
			return PlatformData->Faces[0].Mips[0].Width;
		return SourceData && SourceData->IsValid() ? SourceData->Faces[0].Width : 0;
	}

	auto DTextureCube::GetBuiltMipCount() const -> uint32
	{
		return PlatformData && PlatformData->IsValid()
			? static_cast<uint32>(PlatformData->Faces[0].Mips.size()) : 0;
	}

	auto DTextureCube::GetBuiltPixelFormat() const -> EPixelFormat
	{
		return PlatformData && PlatformData->IsValid() ? PlatformData->PixelFormat : EPixelFormat::Unknown;
	}

	auto DTextureCube::InvalidatePlatformData() -> void
	{
		PlatformData.reset();
		InvalidateRenderResource();
	}

	auto DTextureCube::CreateRenderResourceCandidate(
		FTextureReference* TextureReference,
		uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTextureCubeResource>(
			TextureReference,
			std::make_shared<const FTextureCubePlatformData>(*PlatformData),
			Revision,
			Completion);
	}

	auto DTextureCube::RebuildPlatformData(std::string& OutError) -> bool
	{
		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		if (!SourceData || !SourceData->IsValid() || !BuildCubePlatformData(*SourceData, bSRGB, *NewPlatformData, OutError))
		{
			if (OutError.empty()) OutError = "Cube texture source data is unavailable or invalid.";
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			InvalidatePlatformData();
			return false;
		}
		PlatformData = std::move(NewPlatformData);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		return true;
	}

	auto DTextureCube::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedPlatformData(OutError);
		if (Asset::IsAssetMigrationLoad())
		{
			OutError.clear();
			return true;
		}

		DerivedDataKey.clear();
		DerivedDataDiagnostic = {};
		bLoadedFromDerivedDataCache = false;
		if (!SourceImportData.HasSource())
		{
			OutError = "TextureCube has no normalized mounted-source provenance; legacy source metadata is unsupported.";
			BuildStatus = ETextureBuildStatus::MissingSource;
			LastBuildError = OutError;
			DerivedDataDiagnostic = {
				.Status = ETextureDerivedDataStatus::Incompatible,
				.Message = OutError};
			return false;
		}
		if (SourceImportData.HasSource())
		{
			if (!MakeTextureCubeDerivedDataKey(*this, DerivedDataKey, OutError))
			{
				DerivedDataDiagnostic = {
					.Status = ETextureDerivedDataStatus::Incompatible,
					.Message = OutError};
				LastBuildError = OutError;
				return false;
			}
			std::unique_ptr<FTextureCubePlatformData> CachedPlatformData;
			ETextureDerivedDataStatus CacheStatus = ETextureDerivedDataStatus::Missing;
			std::string CacheMessage;
			if (LoadTextureCubeDerivedData(
				DerivedDataKey, CachedPlatformData, CacheStatus, CacheMessage))
			{
				SourceData.reset();
				PlatformData = std::move(CachedPlatformData);
				BuildStatus = ETextureBuildStatus::Ready;
				LastBuildError.clear();
				bLoadedFromDerivedDataCache = true;
				DerivedDataDiagnostic = {
					.Status = ETextureDerivedDataStatus::Hit,
					.Key = DerivedDataKey,
					.Message = std::format("TextureCube DDC hit for key {}.", DerivedDataKey)};
				QueueRenderResourceBuild();
				OutError.clear();
				return true;
			}
			DerivedDataDiagnostic = {
				.Status = CacheStatus,
				.Key = DerivedDataKey,
				.Message = std::format("TextureCube DDC miss for key {}: {}",
					DerivedDataKey, CacheMessage)};
		}

		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		if (SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
		{
			if (GetPanoramaSourceFile().empty())
			{
				OutError = "Panorama layout has no source file.";
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
			const std::filesystem::path SourcePath = ResolvePanoramaSource();
			if (!std::filesystem::is_regular_file(SourcePath))
			{
				OutError = std::format("Panorama source file does not exist: {}", GetPanoramaSourceFile());
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
			bool bHDR = false;
			EPanoramaDecodeError DecodeCode = EPanoramaDecodeError::None;
			DerivedDataDiagnostic.bSourceDecoderInvoked = true;
			if (!DecodePanoramaInput(SourcePath,
				{.FaceDimension = PanoramaFaceDimension, .ExposureEV = PanoramaExposureEV},
				*NewSourceData, OriginalSourceWidth, OriginalSourceHeight,
				bHDR, DecodeCode, OutError))
			{
				BuildStatus = DecodeCode == EPanoramaDecodeError::Decode
					? ETextureBuildStatus::DecodeFailure : ETextureBuildStatus::BuildFailure;
				LastBuildError = OutError;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
		}
		else if (SourceLayout != ETextureCubeSourceLayout::SixFaces)
		{
			OutError = "Texture cube source layout is invalid.";
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			DerivedDataDiagnostic.Message = OutError;
			return false;
		}
		else for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			const ETextureCubeFace Face = static_cast<ETextureCubeFace>(FaceIndex);
			if (GetSourceFile(Face).empty())
			{
				OutError = std::format("{} face has no source file.", FaceNames[FaceIndex]);
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
			const std::filesystem::path SourcePath = ResolveCubeSource(*this, Face);
			if (!std::filesystem::is_regular_file(SourcePath))
			{
				OutError = std::format("{} face source file does not exist: {}", FaceNames[FaceIndex], GetSourceFile(Face));
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
			DerivedDataDiagnostic.bSourceDecoderInvoked = true;
			if (!TextureBuild::DecodeRGBA8(SourcePath.generic_string(), NewSourceData->Faces[FaceIndex], OutError))
			{
				OutError = std::format("{} face decode failed: {}", FaceNames[FaceIndex], OutError);
				BuildStatus = ETextureBuildStatus::DecodeFailure;
				LastBuildError = OutError;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
		}
		if (!ValidateCubeSourceData(*NewSourceData, OutError))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			DerivedDataDiagnostic.Message = OutError;
			return false;
		}

		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		if (!BuildCubePlatformData(*NewSourceData, bSRGB, *NewPlatformData, OutError))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			DerivedDataDiagnostic.Message = OutError;
			return false;
		}
		if (SourceImportData.HasSource())
		{
			if (DerivedDataKey.empty()
				&& !MakeTextureCubeDerivedDataKey(*this, DerivedDataKey, OutError))
				return false;
			if (!StoreTextureCubeDerivedData(DerivedDataKey, *NewPlatformData, OutError))
			{
				DerivedDataDiagnostic = {
					.Status = ETextureDerivedDataStatus::WriteFailure,
					.Key = DerivedDataKey,
					.Message = OutError,
					.bSourceDecoderInvoked = true};
				LastBuildError = OutError;
				return false;
			}
		}
		SourceData = std::move(NewSourceData);
		PlatformData = std::move(NewPlatformData);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = std::format("Rebuilt TextureCube and stored DDC key {}.", DerivedDataKey),
			.bSourceDecoderInvoked = true};
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DTextureCube::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked TextureCube '{}': {}", GetObjectPath(), Message);
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = DerivedDataDiagnostic.Message;
			OutError = LastBuildError;
			return false;
		};
		if (CookedPayload.PayloadId != TextureCubePrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != TexturePayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required TXPL descriptor is missing or incompatible.");

		const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				LoadContext.CookRoot, PackagePath, CompanionPath, &OutError))
			return FailCooked(OutError.empty()
				? "package companion path could not be resolved." : OutError);

		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, Container, &OutError))
			return FailCooked(OutError);
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);
		std::unique_ptr<FTextureCubePlatformData> CandidatePlatformData;
		const FPayloadDecodeResult DecodeResult = DecodeTextureCubePayload(
			Bytes, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
			CandidatePlatformData);
		if (!DecodeResult)
			return FailCooked(DecodeResult.Message);

		SourceData.reset();
		PlatformData = std::move(CandidatePlatformData);
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::CookedLoaded,
			.Message = std::format("Loaded cooked TextureCube payload for '{}'.", GetObjectPath())};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DTextureCube::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticSourceMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"TextureCube '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		std::string ExpectedKey;
		if (!MakeTextureCubeDerivedDataKey(*this, ExpectedKey, OutError))
		{
			OutError = std::format("Failed to cook TextureCube '{}': {}", GetObjectPath(), OutError);
			return false;
		}

		std::vector<uint8> PayloadBytes;
		std::unique_ptr<FTextureCubePlatformData> ValidatedPlatformData;
		const Asset::FDerivedDataObjectReadResult Read =
			GetTextureCubeObjectStore().Read(ExpectedKey, PayloadBytes);
		const FPayloadDecodeResult DecodeResult = Read
			? DecodeTextureCubePayload(
				PayloadBytes, Asset::ECookTargetPlatform::Win64,
				Asset::ECookTargetProfile::Game, ValidatedPlatformData)
			: FPayloadDecodeResult{
				.Code = EPayloadDecodeError::Corrupt,
				.Message = Read.Message};
		if (!DecodeResult)
		{
			if (!PlatformData && !PostLoad(OutError))
			{
				OutError = std::format("Failed to cook TextureCube '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			if (!PlatformData || !EncodeTextureCubePayload(
				*PlatformData, Asset::ECookTargetPlatform::Win64,
				Asset::ECookTargetProfile::Game, PayloadBytes, OutError))
			{
				OutError = std::format("Failed to cook TextureCube '{}': {}", GetObjectPath(), OutError);
				return false;
			}
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = TextureCubePrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = TexturePayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TexturePayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this, bRetainDiagnosticSourceMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes, std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != TextureCubePrimaryCookedPayloadId)
				{
					if (Error) *Error = "TextureCube cook did not produce its required descriptor.";
					return false;
				}
				const FTextureCubeSourceImportData SavedSourceImportData = SourceImportData;
				const Asset::FCookedPayloadDescriptor SavedCookedPayload = CookedPayload;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticSourceMetadata)
				{
					SourceImportData = {};
				}
				Asset::FAssetPackageSerializationOptions Options;
				if (!bRetainDiagnosticSourceMetadata)
				{
					Options.PropertyFilter = [this](const DObject* Object, const FProperty* Property) {
						if (Object != this) return true;
						const FName Name = Property->NamePrivate;
						return Name != FName("SourceImportData");
					};
				}
				const Asset::FAssetResult Result =
					Asset::SerializeAssetPackageBytes(GetPackage(), OutPackageBytes, Options);
				SourceImportData = SavedSourceImportData;
				CookedPayload = SavedCookedPayload;
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			}, &OutError);
	}

	auto DTextureCube::RefreshBuildStatus() -> void
	{
		const std::shared_ptr<FTextureResourceCompletion>& Completion =
			GetRenderCompletion();
		if (Completion->GetFailedRevision() == GetBuildRevision())
		{
			if (Completion->GetFailureReason()
				== ETextureRenderFailure::UnsupportedFormat)
			{
				BuildStatus = ETextureBuildStatus::UnsupportedFormat;
				LastBuildError = "The current RHI does not support this cube texture format and usage.";
			}
			else
			{
				BuildStatus = ETextureBuildStatus::UploadFailure;
				LastBuildError = "GPU cube texture creation or upload failed.";
			}
		}
	}

	auto DTextureCube::ValidateImportSources(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings) -> FTextureCubeImportValidation
	{
		std::array<std::filesystem::path, TextureCubeFaceCount> Inputs;
		FTextureCubeSourceData SourceData;
		std::string Error;
		if (!DecodeCubeInputs(FaceFiles, Inputs, SourceData, Error))
			return {false, std::move(Error)};

		FTextureCubePlatformData PlatformData;
		if (!BuildCubePlatformData(SourceData, Settings.bSRGB, PlatformData, Error))
			return {false, std::move(Error)};
		return {
			.bValid = true,
			.SourceLayout = ETextureCubeSourceLayout::SixFaces,
			.SourceWidth = SourceData.Faces[0].Width,
			.SourceHeight = SourceData.Faces[0].Height,
			.Dimension = SourceData.Faces[0].Width,
			.MipCount = static_cast<uint32>(PlatformData.Faces[0].Mips.size()),
			.PixelFormat = PlatformData.PixelFormat,
		};
	}

	auto DTextureCube::ValidatePanoramaImportSource(std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportValidation
	{
		if (PanoramaFile.empty()) return {false, "Panorama source is missing."};
		const std::filesystem::path Input = std::filesystem::absolute(PanoramaFile).lexically_normal();
		FTextureCubeSourceData SourceData;
		FTextureCubePlatformData PlatformData;
		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		bool bHDR = false;
		std::string Error;
		if (!ValidatePanorama(Input, Settings, SourceData, PlatformData,
			SourceWidth, SourceHeight, bHDR, Error))
		{
			return {false, std::move(Error)};
		}
		return {
			.bValid = true,
			.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
			.SourceWidth = SourceWidth,
			.SourceHeight = SourceHeight,
			.Dimension = SourceData.Faces[0].Width,
			.MipCount = static_cast<uint32>(PlatformData.Faces[0].Mips.size()),
			.PixelFormat = PlatformData.PixelFormat,
			.bHDR = bHDR,
		};
	}

	auto DTextureCube::ImportPanoramaAsset(std::string_view PanoramaFile, std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string_view SourceDestination,
		bool bEngineAuthoringContext) -> FTextureCubeImportResult
	{
		if (PanoramaFile.empty()) return {false, "Panorama source is missing.", nullptr};
		const std::filesystem::path Input = std::filesystem::absolute(PanoramaFile).lexically_normal();
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		bool bHDR = false;
		std::string Error;
		if (!ValidatePanorama(Input, Settings, *NewSourceData, *NewPlatformData,
			SourceWidth, SourceHeight, bHDR, Error))
		{
			return {false, std::move(Error), nullptr};
		}

		FAssetPath ParsedAssetPath;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		std::filesystem::path Destination;
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedAssetPath, "_panorama", Input.extension().generic_string(),
			SourceDestination,
			Destination, StoredSourcePath, Error))
			return {false, std::move(Error), nullptr};
		FMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input, ParsedAssetPath.ToString(), StoredSourcePath, MountedSource, Error,
			bEngineAuthoringContext))
			return {false, std::move(Error), nullptr};
		Destination = MountedSource.PhysicalPath;
		StoredSourcePath = MountedSource.SourcePath.Path;
		FXxHash128 SourceHash;
		if (!HashTextureSource(Destination, SourceHash, Error))
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, std::move(Error), nullptr};
		}

		std::string DerivedKey;
		if (!BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeDerivedDataSourceLayout::EquirectangularPanorama,
			.PanoramaContentHash = SourceHash,
			.FaceDimension = Settings.FaceDimension,
			.ExposureEV = Settings.ExposureEV,
			.bSRGB = true,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, DerivedKey, Error)
			|| !StoreTextureCubeDerivedData(DerivedKey, *NewPlatformData, Error))
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, std::move(Error), nullptr};
		}

		DTextureCube* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, CreateResult.Message, nullptr};
		}

		Texture->SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		Texture->SourceImportData = {
			.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
			.Panorama = MakeSourceFile(StoredSourcePath, SourceHash),
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion,
			.ProjectionVersion = TextureCubeProjectionVersion};
		Texture->PanoramaFaceDimension = Settings.FaceDimension;
		Texture->PanoramaExposureEV = Settings.ExposureEV;
		Texture->OriginalSourceWidth = SourceWidth;
		Texture->OriginalSourceHeight = SourceHeight;
		Texture->bSRGB = true;
		Texture->SourceData = std::move(NewSourceData);
		Texture->PlatformData = std::move(NewPlatformData);
		Texture->DerivedDataKey = std::move(DerivedKey);
		Texture->DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = Texture->DerivedDataKey,
			.Message = "Imported panorama TextureCube and populated the DDC.",
			.bSourceDecoderInvoked = true};
		Texture->BuildStatus = ETextureBuildStatus::Ready;

		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		CommitMountedSourceFile(MountedSource);
		Texture->QueueRenderResourceBuild();
		return {true, {}, Texture};
	}

	auto DTextureCube::ReimportPanorama(std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings, std::string& OutError) -> bool
	{
		OutError.clear();
		if (SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
		{
			OutError = "Only panorama-backed texture cubes can be reimported through this API.";
			return false;
		}
		if (PanoramaFile.empty())
		{
			OutError = "Panorama source is missing.";
			return false;
		}

		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		FMountedSourceFile MountedSource;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(),
			SourceImportData.Panorama.SourcePath.Path,
			MountedSource, OutError)) return false;
		const std::filesystem::path Input = MountedSource.PhysicalPath;
		std::error_code EquivalentError;
		const std::filesystem::path Requested =
			std::filesystem::absolute(PanoramaFile).lexically_normal();
		if (!std::filesystem::equivalent(Input, Requested, EquivalentError)
			|| EquivalentError)
		{
			OutError =
				"Reimport is read-only and must use the persisted mounted panorama source.";
			return false;
		}
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		uint32 NewSourceWidth = 0;
		uint32 NewSourceHeight = 0;
		bool bHDR = false;
		if (!ValidatePanorama(Input, Settings, *NewSourceData, *NewPlatformData,
			NewSourceWidth, NewSourceHeight, bHDR, OutError))
		{
			return false;
		}

		FXxHash128 SourceHash;
		if (!HashTextureSource(Input, SourceHash, OutError)) return false;
		const std::filesystem::path OldSource = ResolvePanoramaSource();
		const std::filesystem::path NewSource = MountedSource.PhysicalPath;
		const std::string NewStoredSourcePath = MountedSource.SourcePath.Path;
		std::string NewDerivedKey;
		if (!BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeDerivedDataSourceLayout::EquirectangularPanorama,
			.PanoramaContentHash = SourceHash,
			.FaceDimension = Settings.FaceDimension,
			.ExposureEV = Settings.ExposureEV,
			.bSRGB = true,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, NewDerivedKey, OutError)
			|| !StoreTextureCubeDerivedData(NewDerivedKey, *NewPlatformData, OutError)) return false;
		std::error_code ErrorCode;
		const bool bInputIsDestination = Input.lexically_normal() == NewSource.lexically_normal();
		const std::filesystem::path TemporarySource =
			NewSource.generic_string() + ".reimport.tmp";
		const std::filesystem::path BackupSource =
			NewSource.generic_string() + ".reimport.backup";
		if (!bInputIsDestination)
		{
			std::filesystem::create_directories(NewSource.parent_path(), ErrorCode);
			if (NewSource != OldSource && std::filesystem::exists(NewSource))
			{
				OutError = std::format("Panorama replacement destination already exists: {}",
					NewSource.generic_string());
				return false;
			}
			std::filesystem::remove(TemporarySource, ErrorCode);
			ErrorCode.clear();
			if (!std::filesystem::copy_file(Input, TemporarySource,
				std::filesystem::copy_options::none, ErrorCode))
			{
				OutError = std::format("Failed to stage panorama replacement: {}", ErrorCode.message());
				return false;
			}
			if (NewSource == OldSource && std::filesystem::exists(NewSource))
			{
				std::filesystem::remove(BackupSource, ErrorCode);
				ErrorCode.clear();
				std::filesystem::rename(NewSource, BackupSource, ErrorCode);
				if (ErrorCode)
				{
					const std::string MoveError = ErrorCode.message();
					std::error_code CleanupError;
					std::filesystem::remove(TemporarySource, CleanupError);
					OutError = std::format("Failed to preserve the current panorama source: {}", MoveError);
					return false;
				}
			}
			ErrorCode.clear();
			std::filesystem::rename(TemporarySource, NewSource, ErrorCode);
			if (ErrorCode)
			{
				const std::string InstallError = ErrorCode.message();
				if (std::filesystem::exists(BackupSource))
				{
					std::error_code RestoreError;
					std::filesystem::rename(BackupSource, NewSource, RestoreError);
				}
				std::error_code CleanupError;
				std::filesystem::remove(TemporarySource, CleanupError);
				OutError = std::format("Failed to install panorama replacement: {}", InstallError);
				return false;
			}
		}

		const FTextureCubeSourceImportData OldSourceImportData = SourceImportData;
		const bool bOldSourceWasPortable = OldSourceImportData.HasSource();
		const uint32 OldFaceDimension = PanoramaFaceDimension;
		const float OldExposureEV = PanoramaExposureEV;
		const uint32 OldSourceWidth = OriginalSourceWidth;
		const uint32 OldSourceHeight = OriginalSourceHeight;
		auto OldSourceData = std::move(SourceData);
		auto OldPlatformData = std::move(PlatformData);
		const std::string OldDerivedDataKey = DerivedDataKey;
		const FTextureDerivedDataDiagnostic OldDiagnostic = DerivedDataDiagnostic;
		SourceImportData = {
			.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
			.Panorama = MakeSourceFile(NewStoredSourcePath, SourceHash),
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion,
			.ProjectionVersion = TextureCubeProjectionVersion};
		PanoramaFaceDimension = Settings.FaceDimension;
		PanoramaExposureEV = Settings.ExposureEV;
		OriginalSourceWidth = NewSourceWidth;
		OriginalSourceHeight = NewSourceHeight;
		SourceData = std::move(NewSourceData);
		PlatformData = std::move(NewPlatformData);
		DerivedDataKey = NewDerivedKey;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Reimported panorama TextureCube and populated the DDC.",
			.bSourceDecoderInvoked = true};

		const Asset::FAssetResult SaveResult = Asset::SavePackage(GetPackage());
		if (!SaveResult)
		{
			SourceImportData = OldSourceImportData;
			PanoramaFaceDimension = OldFaceDimension;
			PanoramaExposureEV = OldExposureEV;
			OriginalSourceWidth = OldSourceWidth;
			OriginalSourceHeight = OldSourceHeight;
			SourceData = std::move(OldSourceData);
			PlatformData = std::move(OldPlatformData);
			DerivedDataKey = OldDerivedDataKey;
			DerivedDataDiagnostic = OldDiagnostic;
			if (!bInputIsDestination)
			{
				std::filesystem::remove(NewSource, ErrorCode);
				if (std::filesystem::exists(BackupSource))
					std::filesystem::rename(BackupSource, OldSource, ErrorCode);
			}
			OutError = SaveResult.Message;
			return false;
		}

		if (!bInputIsDestination)
		{
			std::filesystem::remove(BackupSource, ErrorCode);
			if (!bOldSourceWasPortable && OldSource != NewSource)
				std::filesystem::remove(OldSource, ErrorCode);
		}
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		return true;
	}

	auto DTextureCube::ReimportSources(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (SourceLayout != ETextureCubeSourceLayout::SixFaces)
		{
			OutError = "Only six-face texture cubes can be reimported through this API.";
			return false;
		}
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<std::string, TextureCubeFaceCount> MountedFaceFiles;
		std::array<FMountedSourceFile, TextureCubeFaceCount> MountedSources;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			const ETextureCubeFace Face = static_cast<ETextureCubeFace>(FaceIndex);
			if (!ResolveMountedSourceReference(
				GetPackage()->GetPackagePath(),
				SourceImportData.GetFace(Face).SourcePath.Path,
				MountedSources[FaceIndex], OutError)) return false;
			std::error_code EquivalentError;
			if (!std::filesystem::equivalent(
				MountedSources[FaceIndex].PhysicalPath,
				std::filesystem::absolute(FaceFiles[FaceIndex]).lexically_normal(),
				EquivalentError) || EquivalentError)
			{
				OutError =
					"Reimport is read-only and every face must use its persisted mounted source.";
				return false;
			}
			MountedFaceFiles[FaceIndex] =
				MountedSources[FaceIndex].PhysicalPath.generic_string();
		}
		std::array<std::filesystem::path, TextureCubeFaceCount> Inputs;
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		if (!DecodeCubeInputs(MountedFaceFiles, Inputs, *NewSourceData, OutError)) return false;
		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		if (!BuildCubePlatformData(
			*NewSourceData, Settings.bSRGB, *NewPlatformData, OutError)) return false;

		std::array<FXxHash128, TextureCubeFaceCount> Hashes;
		std::array<std::filesystem::path, TextureCubeFaceCount> Destinations;
		std::array<std::string, TextureCubeFaceCount> StoredPaths;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!HashTextureSource(Inputs[FaceIndex], Hashes[FaceIndex], OutError))
				return false;
			Destinations[FaceIndex] = MountedSources[FaceIndex].PhysicalPath;
			StoredPaths[FaceIndex] = MountedSources[FaceIndex].SourcePath.Path;
		}
		std::string NewDerivedKey;
		if (!BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeDerivedDataSourceLayout::SixFaces,
			.FaceContentHashes = Hashes,
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, NewDerivedKey, OutError)
			|| !StoreTextureCubeDerivedData(NewDerivedKey, *NewPlatformData, OutError))
			return false;

		std::array<std::filesystem::path, TextureCubeFaceCount> Temporaries;
		std::array<std::filesystem::path, TextureCubeFaceCount> Backups;
		std::array<bool, TextureCubeFaceCount> Replaced{};
		std::array<bool, TextureCubeFaceCount> Installed{};
		std::error_code ErrorCode;
		auto RollbackFiles = [&] {
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				if (!Temporaries[FaceIndex].empty())
					std::filesystem::remove(Temporaries[FaceIndex], ErrorCode);
				if (Installed[FaceIndex])
				{
					std::filesystem::remove(Destinations[FaceIndex], ErrorCode);
					if (Replaced[FaceIndex] && std::filesystem::exists(Backups[FaceIndex]))
						std::filesystem::rename(
							Backups[FaceIndex], Destinations[FaceIndex], ErrorCode);
				}
			}
		};
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (Inputs[FaceIndex] == Destinations[FaceIndex]) continue;
			std::filesystem::create_directories(Destinations[FaceIndex].parent_path(), ErrorCode);
			Temporaries[FaceIndex] = Destinations[FaceIndex].generic_string() + ".reimport.tmp";
			Backups[FaceIndex] = Destinations[FaceIndex].generic_string() + ".reimport.backup";
			std::filesystem::remove(Temporaries[FaceIndex], ErrorCode);
			std::filesystem::remove(Backups[FaceIndex], ErrorCode);
			ErrorCode.clear();
			if (!std::filesystem::copy_file(
				Inputs[FaceIndex], Temporaries[FaceIndex],
				std::filesystem::copy_options::none, ErrorCode))
			{
				OutError = std::format("Failed to stage {} face replacement: {}",
					FaceNames[FaceIndex], ErrorCode.message());
				RollbackFiles();
				return false;
			}
			if (std::filesystem::exists(Destinations[FaceIndex]))
			{
				ErrorCode.clear();
				std::filesystem::rename(
					Destinations[FaceIndex], Backups[FaceIndex], ErrorCode);
				if (ErrorCode)
				{
					OutError = std::format("Failed to preserve {} face source: {}",
						FaceNames[FaceIndex], ErrorCode.message());
					RollbackFiles();
					return false;
				}
				Replaced[FaceIndex] = true;
			}
			ErrorCode.clear();
			std::filesystem::rename(
				Temporaries[FaceIndex], Destinations[FaceIndex], ErrorCode);
			if (ErrorCode)
			{
				OutError = std::format("Failed to install {} face replacement: {}",
					FaceNames[FaceIndex], ErrorCode.message());
				RollbackFiles();
				return false;
			}
			Installed[FaceIndex] = true;
		}

		const FTextureCubeSourceImportData OldSourceImportData = SourceImportData;
		const bool bOldSRGB = bSRGB;
		const std::string OldDerivedDataKey = DerivedDataKey;
		const FTextureDerivedDataDiagnostic OldDiagnostic = DerivedDataDiagnostic;
		auto OldSourceData = std::move(SourceData);
		auto OldPlatformData = std::move(PlatformData);
		SourceImportData = {};
		SourceImportData.SourceLayout = ETextureCubeSourceLayout::SixFaces;
		SourceImportData.DecoderId = std::string(TextureDecoderId);
		SourceImportData.DecoderVersion = TextureDecoderVersion;
		SourceImportData.ProjectionVersion = TextureCubeProjectionVersion;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			SourceImportData.GetMutableFace(static_cast<ETextureCubeFace>(FaceIndex)) =
				MakeSourceFile(StoredPaths[FaceIndex], Hashes[FaceIndex]);
		bSRGB = Settings.bSRGB;
		SourceData = std::move(NewSourceData);
		PlatformData = std::move(NewPlatformData);
		DerivedDataKey = NewDerivedKey;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Reimported six-face TextureCube and populated the DDC.",
			.bSourceDecoderInvoked = true};
		const Asset::FAssetResult SaveResult = Asset::SavePackage(GetPackage());
		if (!SaveResult)
		{
			SourceImportData = OldSourceImportData;
			bSRGB = bOldSRGB;
			SourceData = std::move(OldSourceData);
			PlatformData = std::move(OldPlatformData);
			DerivedDataKey = OldDerivedDataKey;
			DerivedDataDiagnostic = OldDiagnostic;
			RollbackFiles();
			OutError = SaveResult.Message;
			return false;
		}
		for (const std::filesystem::path& Backup : Backups)
			if (!Backup.empty()) std::filesystem::remove(Backup, ErrorCode);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		return true;
	}

	auto DTextureCube::BuildPanoramaFromEncodedBytes(
		std::span<const uint8> EncodedBytes,
		std::string_view ExtensionHint,
		const FSourcePath& InSourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
#if DURIN_WITH_EDITOR
		if (EncodedBytes.empty() || InSourcePath.IsEmpty())
		{
			OutError = "Panorama candidate requires captured bytes and mounted provenance.";
			return false;
		}
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		const TextureBuild::FEquirectangularTextureCubeProjectionSettings ProjectionSettings{
			.FaceDimension = Settings.FaceDimension,
			.ExposureEV = Settings.ExposureEV};
		std::string Extension(ExtensionHint);
		std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		if (Asset::IsRadianceHDRExtension(Extension))
		{
			Asset::FDecodedFloatImage Panorama;
			Asset::FRadianceHDRDecodeLimits Limits;
			Limits.MaximumDecodedPixels = TextureBuild::MaximumPanoramaPixels;
			if (!Asset::DecodeRadianceHDRFromMemory(EncodedBytes, Panorama, OutError, Limits)
				|| !TextureBuild::ProjectEquirectangularTextureCube(
					Panorama, ProjectionSettings, *NewSourceData, OutError)) return false;
			SourceWidth = Panorama.Width;
			SourceHeight = Panorama.Height;
		}
		else
		{
			Asset::FDecodedImage Panorama;
			Asset::FImageDecodeLimits Limits;
			Limits.MaximumDecodedPixels = TextureBuild::MaximumPanoramaPixels;
			if (!Asset::DecodeImageFromMemory(EncodedBytes, Panorama, OutError, Limits)
				|| !TextureBuild::ProjectEquirectangularTextureCube(
					Panorama, ProjectionSettings, *NewSourceData, OutError)) return false;
			SourceWidth = Panorama.Width;
			SourceHeight = Panorama.Height;
		}
		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		if (!BuildCubePlatformData(*NewSourceData, true, *NewPlatformData, OutError)) return false;
		const FXxHash128 Hash = FXxHash128::HashBuffer(EncodedBytes);
		std::string NewDerivedKey;
		if (!BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeDerivedDataSourceLayout::EquirectangularPanorama,
			.PanoramaContentHash = Hash,
			.FaceDimension = Settings.FaceDimension,
			.ExposureEV = Settings.ExposureEV,
			.bSRGB = true,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, NewDerivedKey, OutError)
			|| !StoreTextureCubeDerivedData(NewDerivedKey, *NewPlatformData, OutError)) return false;
		SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		SourceImportData = {
			.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
			.Panorama = MakeSourceFile(InSourcePath.Path, Hash),
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion,
			.ProjectionVersion = TextureCubeProjectionVersion};
		PanoramaFaceDimension = Settings.FaceDimension;
		PanoramaExposureEV = Settings.ExposureEV;
		OriginalSourceWidth = SourceWidth;
		OriginalSourceHeight = SourceHeight;
		bSRGB = true;
		SourceData = std::move(NewSourceData);
		PlatformData = std::move(NewPlatformData);
		DerivedDataKey = std::move(NewDerivedKey);
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Built TextureCube panorama candidate from captured bytes.",
			.bSourceDecoderInvoked = true};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache = false;
		QueueRenderResourceBuild();
		MarkPackageDirty();
		OutError.clear();
		return true;
#else
		(void)EncodedBytes; (void)ExtensionHint; (void)InSourcePath; (void)Settings;
		OutError = "TextureCube source building is unavailable in runtime-only targets.";
		return false;
#endif
	}

	auto DTextureCube::BuildFacesFromEncodedBytes(
		const std::array<std::span<const uint8>, TextureCubeFaceCount>& EncodedFaces,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
#if DURIN_WITH_EDITOR
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		std::array<FXxHash128, TextureCubeFaceCount> Hashes;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (EncodedFaces[Index].empty() || SourcePaths[Index].IsEmpty()
				|| !TextureBuild::DecodeRGBA8(
					EncodedFaces[Index], NewSourceData->Faces[Index], OutError))
			{
				if (OutError.empty()) OutError = std::format("{} face source is invalid.", FaceNames[Index]);
				return false;
			}
			Hashes[Index] = FXxHash128::HashBuffer(EncodedFaces[Index]);
		}
		if (!ValidateCubeSourceData(*NewSourceData, OutError)) return false;
		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		if (!BuildCubePlatformData(*NewSourceData, Settings.bSRGB, *NewPlatformData, OutError))
			return false;
		std::string NewDerivedKey;
		if (!BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeDerivedDataSourceLayout::SixFaces,
			.FaceContentHashes = Hashes,
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, NewDerivedKey, OutError)
			|| !StoreTextureCubeDerivedData(NewDerivedKey, *NewPlatformData, OutError)) return false;
		SourceLayout = ETextureCubeSourceLayout::SixFaces;
		SourceImportData = {};
		SourceImportData.SourceLayout = ETextureCubeSourceLayout::SixFaces;
		SourceImportData.DecoderId = std::string(TextureDecoderId);
		SourceImportData.DecoderVersion = TextureDecoderVersion;
		SourceImportData.ProjectionVersion = TextureCubeProjectionVersion;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			SourceImportData.GetMutableFace(static_cast<ETextureCubeFace>(Index)) =
				MakeSourceFile(SourcePaths[Index].Path, Hashes[Index]);
		PanoramaFaceDimension = 0;
		PanoramaExposureEV = 0.0f;
		OriginalSourceWidth = NewSourceData->Faces[0].Width;
		OriginalSourceHeight = NewSourceData->Faces[0].Height;
		bSRGB = Settings.bSRGB;
		SourceData = std::move(NewSourceData);
		PlatformData = std::move(NewPlatformData);
		DerivedDataKey = std::move(NewDerivedKey);
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Built six-face TextureCube candidate from captured bytes.",
			.bSourceDecoderInvoked = true};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache = false;
		QueueRenderResourceBuild();
		MarkPackageDirty();
		OutError.clear();
		return true;
#else
		(void)EncodedFaces; (void)SourcePaths; (void)Settings;
		OutError = "TextureCube source building is unavailable in runtime-only targets.";
		return false;
#endif
	}

	auto DTextureCube::ExchangeImportedState(DTextureCube& Other) noexcept -> void
	{
		if (&Other == this) return;
		std::swap(SourceLayout, Other.SourceLayout);
		std::swap(SourceImportData, Other.SourceImportData);
		std::swap(PanoramaFaceDimension, Other.PanoramaFaceDimension);
		std::swap(PanoramaExposureEV, Other.PanoramaExposureEV);
		std::swap(OriginalSourceWidth, Other.OriginalSourceWidth);
		std::swap(OriginalSourceHeight, Other.OriginalSourceHeight);
		std::swap(bSRGB, Other.bSRGB);
		std::swap(CookedPayload, Other.CookedPayload);
		std::swap(SourceData, Other.SourceData);
		std::swap(PlatformData, Other.PlatformData);
		std::swap(DerivedDataKey, Other.DerivedDataKey);
		std::swap(DerivedDataDiagnostic, Other.DerivedDataDiagnostic);
		std::swap(bLoadedFromDerivedDataCache, Other.bLoadedFromDerivedDataCache);
		std::swap(BuildStatus, Other.BuildStatus);
		std::swap(LastBuildError, Other.LastBuildError);
		QueueRenderResourceBuild();
		Other.QueueRenderResourceBuild();
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

	auto DTextureCube::ChangePanoramaSourceReference(
		std::string_view SourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		FMountedSourceFile Source;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError))
			return false;
		const ETextureCubeSourceLayout PreviousLayout = SourceLayout;
		const FTextureCubeSourceImportData Previous = SourceImportData;
		SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		SourceImportData = {};
		SourceImportData.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		SourceImportData.Panorama.SourcePath = Source.SourcePath;
		if (!ReimportPanorama(Source.PhysicalPath.generic_string(), Settings, OutError))
		{
			SourceLayout = PreviousLayout;
			SourceImportData = Previous;
			return false;
		}
		return true;
	}

	auto DTextureCube::ChangeSourceReferences(
		const std::array<std::string, TextureCubeFaceCount>& SourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<FMountedSourceFile, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> PhysicalFiles;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!ResolveMountedSourceReference(
				GetPackage()->GetPackagePath(), SourceVirtualPaths[FaceIndex],
				Sources[FaceIndex], OutError)) return false;
			PhysicalFiles[FaceIndex] = Sources[FaceIndex].PhysicalPath.generic_string();
		}
		const ETextureCubeSourceLayout PreviousLayout = SourceLayout;
		const FTextureCubeSourceImportData Previous = SourceImportData;
		SourceLayout = ETextureCubeSourceLayout::SixFaces;
		SourceImportData = {};
		SourceImportData.SourceLayout = ETextureCubeSourceLayout::SixFaces;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			SourceImportData.GetMutableFace(static_cast<ETextureCubeFace>(FaceIndex)).SourcePath =
				Sources[FaceIndex].SourcePath;
		if (!ReimportSources(PhysicalFiles, Settings, OutError))
		{
			SourceLayout = PreviousLayout;
			SourceImportData = Previous;
			return false;
		}
		return true;
	}

	auto DTextureCube::IngestAndChangePanoramaSource(
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		FMountedSourceFile Source;
		if (!PrepareMountedSourceFile(
			FilePath, GetPackage()->GetPackagePath(),
			TargetSourceVirtualPath, Source, OutError)) return false;
		const bool bChanged =
			ChangePanoramaSourceReference(Source.SourcePath.Path, Settings, OutError);
		if (bChanged)
			CommitMountedSourceFile(Source);
		else
			RollbackMountedSourceFile(Source);
		return bChanged;
	}

	auto DTextureCube::IngestAndChangeSources(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const std::array<std::string, TextureCubeFaceCount>& TargetSourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<FMountedSourceFile, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> VirtualPaths;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!PrepareMountedSourceFile(
				FaceFiles[FaceIndex], GetPackage()->GetPackagePath(),
				TargetSourceVirtualPaths[FaceIndex], Sources[FaceIndex], OutError))
			{
				for (FMountedSourceFile& Source : Sources)
					RollbackMountedSourceFile(Source);
				return false;
			}
			VirtualPaths[FaceIndex] = Sources[FaceIndex].SourcePath.Path;
		}
		const bool bChanged = ChangeSourceReferences(VirtualPaths, Settings, OutError);
		for (FMountedSourceFile& Source : Sources)
		{
			if (bChanged)
				CommitMountedSourceFile(Source);
			else
				RollbackMountedSourceFile(Source);
		}
		return bChanged;
	}

	auto DTextureCube::ImportAsset(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath, const FTextureCubeImportSettings& Settings,
		const std::array<std::string, TextureCubeFaceCount>& SourceDestinations,
		bool bEngineAuthoringContext)
		-> FTextureCubeImportResult
	{
		std::array<std::filesystem::path, TextureCubeFaceCount> Inputs;
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		std::string ValidationError;
		if (!DecodeCubeInputs(FaceFiles, Inputs, *NewSourceData, ValidationError))
			return {false, std::move(ValidationError), nullptr};

		auto NewPlatformData = std::make_unique<FTextureCubePlatformData>();
		std::string BuildError;
		if (!BuildCubePlatformData(*NewSourceData, Settings.bSRGB, *NewPlatformData, BuildError))
			return {false, std::move(BuildError), nullptr};

		FAssetPath ParsedAssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &PathError)) return {false, std::move(PathError), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		std::array<FXxHash128, TextureCubeFaceCount> SourceHashes;
		std::array<std::string, TextureCubeFaceCount> StoredSourcePaths;
		std::array<std::filesystem::path, TextureCubeFaceCount> Destinations;
		std::array<FMountedSourceFile, TextureCubeFaceCount> MountedSources;
		auto RollbackMountedSources = [&] {
			for (FMountedSourceFile& Source : MountedSources)
				RollbackMountedSourceFile(Source);
		};
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (!MakeCanonicalSourceLocation(
					ParsedAssetPath, std::format("_{}", FaceSuffixes[FaceIndex]),
					Inputs[FaceIndex].extension().generic_string(),
					SourceDestinations[FaceIndex],
					Destinations[FaceIndex], StoredSourcePaths[FaceIndex], PathError)
				|| !PrepareMountedSourceFile(
					Inputs[FaceIndex], ParsedAssetPath.ToString(),
					StoredSourcePaths[FaceIndex], MountedSources[FaceIndex], PathError,
					bEngineAuthoringContext))
			{
				RollbackMountedSources();
				return {false, std::move(PathError), nullptr};
			}
			Destinations[FaceIndex] = MountedSources[FaceIndex].PhysicalPath;
			StoredSourcePaths[FaceIndex] = MountedSources[FaceIndex].SourcePath.Path;
			if (!HashTextureSource(
				Destinations[FaceIndex], SourceHashes[FaceIndex], PathError))
			{
				RollbackMountedSources();
				return {false, std::move(PathError), nullptr};
			}
		}

		std::string DerivedKey;
		if (!BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeDerivedDataSourceLayout::SixFaces,
			.FaceContentHashes = SourceHashes,
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, DerivedKey, PathError)
			|| !StoreTextureCubeDerivedData(DerivedKey, *NewPlatformData, PathError))
		{
			RollbackMountedSources();
			return {false, std::move(PathError), nullptr};
		}

		DTextureCube* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			RollbackMountedSources();
			return {false, CreateResult.Message, nullptr};
		}

		Texture->SourceLayout = ETextureCubeSourceLayout::SixFaces;
		Texture->SourceImportData.SourceLayout = ETextureCubeSourceLayout::SixFaces;
		Texture->SourceImportData.DecoderId = std::string(TextureDecoderId);
		Texture->SourceImportData.DecoderVersion = TextureDecoderVersion;
		Texture->SourceImportData.ProjectionVersion = TextureCubeProjectionVersion;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			Texture->SourceImportData.GetMutableFace(static_cast<ETextureCubeFace>(FaceIndex)) =
				MakeSourceFile(StoredSourcePaths[FaceIndex], SourceHashes[FaceIndex]);
		Texture->bSRGB = Settings.bSRGB;
		Texture->SourceData = std::move(NewSourceData);
		Texture->PlatformData = std::move(NewPlatformData);
		Texture->DerivedDataKey = std::move(DerivedKey);
		Texture->DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = Texture->DerivedDataKey,
			.Message = "Imported six-face TextureCube and populated the DDC.",
			.bSourceDecoderInvoked = true};
		Texture->BuildStatus = ETextureBuildStatus::Ready;
		Texture->QueueRenderResourceBuild();

		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			RollbackMountedSources();
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		for (FMountedSourceFile& Source : MountedSources)
			CommitMountedSourceFile(Source);
		return {true, {}, Texture};
	}
}

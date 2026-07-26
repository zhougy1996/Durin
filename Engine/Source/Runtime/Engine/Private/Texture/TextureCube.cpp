#include "Texture/TextureCube.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "DynamicRHI.h"
#include "Misc/Paths.h"
#include "Texture/EquirectangularTextureCube.h"
#include "Texture/TextureBuild.h"
#include "Texture/TextureCubeRenderResource.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceSuffixes = {
			"px", "nx", "py", "ny", "pz", "nz"};

		auto FaceToIndex(ETextureCubeFace Face) -> size_t
		{
			const size_t Index = static_cast<size_t>(Face);
			check(Index < TextureCubeFaceCount);
			return Index;
		}

		auto ResolveMountedFile(std::string_view VirtualPath) -> std::filesystem::path
		{
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				if (VirtualPath.starts_with(Mount.VirtualRoot))
				{
					return (std::filesystem::path(Mount.PhysicalPath) /
						std::string(VirtualPath.substr(Mount.VirtualRoot.size()))).lexically_normal();
				}
			}
			return std::filesystem::path(VirtualPath).lexically_normal();
		}

		auto ResolveCubeSource(const DTextureCube& Texture, ETextureCubeFace Face) -> std::filesystem::path
		{
			const std::filesystem::path StoredPath(Texture.GetSourceFile(Face));
			const std::filesystem::path PackageFile = ResolveMountedFile(Texture.GetPackage()->GetPackagePath());
			if (!StoredPath.is_absolute() && !Texture.GetSourceFile(Face).starts_with('/'))
			{
				return (PackageFile.parent_path() / StoredPath).lexically_normal();
			}
			const std::filesystem::path LegacyPath = ResolveMountedFile(Texture.GetSourceFile(Face));
			if (std::filesystem::is_regular_file(LegacyPath)) return LegacyPath;
			return (PackageFile.parent_path() / StoredPath.filename()).lexically_normal();
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
		}

		auto DecodePanoramaInput(const std::filesystem::path& Input,
			const FTextureCubePanoramaImportSettings& Settings, FTextureCubeSourceData& OutSourceData,
			uint32& OutSourceWidth, uint32& OutSourceHeight, bool& bOutHDR, std::string& OutError) -> bool
		{
			OutSourceData = {};
			OutSourceWidth = 0;
			OutSourceHeight = 0;
			bOutHDR = false;
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
			return true;
		}

		auto ValidatePanorama(const std::filesystem::path& Input,
			const FTextureCubePanoramaImportSettings& Settings, FTextureCubeSourceData& OutSourceData,
			FTextureCubePlatformData& OutPlatformData, uint32& OutSourceWidth, uint32& OutSourceHeight,
			bool& bOutHDR, std::string& OutError) -> bool
		{
			if (!DecodePanoramaInput(Input, Settings, OutSourceData, OutSourceWidth, OutSourceHeight, bOutHDR, OutError))
				return false;
			return BuildCubePlatformData(OutSourceData, true, OutPlatformData, OutError);
		}
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
		, RenderResource(std::make_shared<FTextureCubeRenderResource>())
	{
		static const bool RegisteredAssetContributors = [] {
			Asset::RegisterAssetMoveContributor(DTextureCube::StaticClass(), [](DObject* Object, const FAssetPath& OldPath,
				const FAssetPath& NewPath, Asset::FAssetMoveContribution& Out) -> Asset::FAssetResult {
				auto* Texture = Cast<DTextureCube>(Object);
				if (!Texture) return {};
				const std::filesystem::path OldPackage = ResolveMountedFile(OldPath.ToString());
				const std::filesystem::path NewPackage = ResolveMountedFile(NewPath.ToString());
				if (Texture->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					const std::string Original = Texture->PanoramaSourceFile;
					if (Original.empty()) return {};
					const std::filesystem::path SourceName(Original);
					const std::filesystem::path OldSource = SourceName.is_absolute()
						? SourceName : OldPackage.parent_path() / SourceName;
					const std::string Replacement = OldPath.GetAssetName() == NewPath.GetAssetName()
						? SourceName.filename().generic_string()
						: MakePanoramaSourceFileName(NewPath.GetAssetName(), SourceName.extension().generic_string());
					const std::filesystem::path NewSource = NewPackage.parent_path() / Replacement;
					if (OldSource.lexically_normal() != NewSource.lexically_normal())
						Out.Files.emplace_back(OldSource.lexically_normal(), NewSource.lexically_normal());
					if (Replacement != Original)
					{
						Out.Apply = [Texture, Replacement] { Texture->PanoramaSourceFile = Replacement; };
						Out.Rollback = [Texture, Original] { Texture->PanoramaSourceFile = Original; };
					}
					return {};
				}
				if (Texture->GetSourceLayout() != ETextureCubeSourceLayout::SixFaces)
					return {Asset::EAssetError::UnsupportedProperty, "Texture cube source layout is invalid."};
				std::array<std::string, TextureCubeFaceCount> Originals;
				std::array<std::string, TextureCubeFaceCount> Replacements;
				for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
				{
					const ETextureCubeFace Face = static_cast<ETextureCubeFace>(FaceIndex);
					Originals[FaceIndex] = Texture->GetSourceFile(Face);
					if (Originals[FaceIndex].empty()) continue;
					const std::filesystem::path SourceName(Originals[FaceIndex]);
					const std::filesystem::path OldSource = SourceName.is_absolute() ? SourceName : OldPackage.parent_path() / SourceName;
					Replacements[FaceIndex] = OldPath.GetAssetName() == NewPath.GetAssetName()
						? SourceName.filename().generic_string()
						: MakeSourceFileName(NewPath.GetAssetName(), FaceIndex, SourceName.extension().generic_string());
					const std::filesystem::path NewSource = NewPackage.parent_path() / Replacements[FaceIndex];
					if (OldSource.lexically_normal() != NewSource.lexically_normal())
						Out.Files.emplace_back(OldSource.lexically_normal(), NewSource.lexically_normal());
				}
				Out.Apply = [Texture, Replacements] {
					for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
						Texture->GetMutableSourceFile(static_cast<ETextureCubeFace>(FaceIndex)) = Replacements[FaceIndex];
				};
				Out.Rollback = [Texture, Originals] {
					for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
						Texture->GetMutableSourceFile(static_cast<ETextureCubeFace>(FaceIndex)) = Originals[FaceIndex];
				};
				return {};
			});
			Asset::RegisterAssetDeleteContributor(DTextureCube::StaticClass(), [](DObject* Object,
				Asset::FAssetDeleteContribution& Out) -> Asset::FAssetResult {
				auto* Texture = Cast<DTextureCube>(Object);
				if (!Texture) return {};
				if (Texture->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					if (!Texture->PanoramaSourceFile.empty()) Out.Files.push_back(Texture->ResolvePanoramaSource());
					return {};
				}
				if (Texture->GetSourceLayout() != ETextureCubeSourceLayout::SixFaces)
					return {Asset::EAssetError::UnsupportedProperty, "Texture cube source layout is invalid."};
				for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
				{
					const ETextureCubeFace Face = static_cast<ETextureCubeFace>(FaceIndex);
					if (!Texture->GetSourceFile(Face).empty()) Out.Files.push_back(ResolveCubeSource(*Texture, Face));
				}
				return {};
			});
			return true;
		}();
		(void)RegisteredAssetContributors;
	}

	DTextureCube::~DTextureCube()
	{
		const uint64 ReleaseRevision = ++BuildRevision;
		if (GDynamicRHI != nullptr) RenderResource->QueueRelease(ReleaseRevision);
	}

	auto DTextureCube::GetSourceFile(ETextureCubeFace Face) const -> const std::string&
	{
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: return PositiveXSourceFile;
		case ETextureCubeFace::NegativeX: return NegativeXSourceFile;
		case ETextureCubeFace::PositiveY: return PositiveYSourceFile;
		case ETextureCubeFace::NegativeY: return NegativeYSourceFile;
		case ETextureCubeFace::PositiveZ: return PositiveZSourceFile;
		case ETextureCubeFace::NegativeZ: return NegativeZSourceFile;
		}
		checkf(false, "Invalid cube face");
		return PositiveXSourceFile;
	}

	auto DTextureCube::GetMutableSourceFile(ETextureCubeFace Face) -> std::string&
	{
		return const_cast<std::string&>(std::as_const(*this).GetSourceFile(Face));
	}

	auto DTextureCube::ResolvePanoramaSource() const -> std::filesystem::path
	{
		const std::filesystem::path StoredPath(PanoramaSourceFile);
		const std::filesystem::path PackageFile = ResolveMountedFile(GetPackage()->GetPackagePath());
		if (!StoredPath.is_absolute() && !PanoramaSourceFile.starts_with('/'))
			return (PackageFile.parent_path() / StoredPath).lexically_normal();
		const std::filesystem::path LegacyPath = ResolveMountedFile(PanoramaSourceFile);
		if (std::filesystem::is_regular_file(LegacyPath)) return LegacyPath;
		return (PackageFile.parent_path() / StoredPath.filename()).lexically_normal();
	}

	auto DTextureCube::GetBuiltFaceDimension() const -> uint32
	{
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
		const uint64 Revision = ++BuildRevision;
		if (GDynamicRHI != nullptr) RenderResource->QueueRelease(Revision);
	}

	auto DTextureCube::QueueRenderResourceBuild() -> void
	{
		check(PlatformData && PlatformData->IsValid());
		const uint64 Revision = ++BuildRevision;
		if (GDynamicRHI != nullptr)
			RenderResource->QueueBuild(std::make_shared<const FTextureCubePlatformData>(*PlatformData), Revision);
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
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		if (SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
		{
			if (PanoramaSourceFile.empty())
			{
				OutError = "Panorama layout has no source file.";
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				SourceData.reset();
				InvalidatePlatformData();
				return false;
			}
			const std::filesystem::path SourcePath = ResolvePanoramaSource();
			if (!std::filesystem::is_regular_file(SourcePath))
			{
				OutError = std::format("Panorama source file does not exist: {}", PanoramaSourceFile);
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				SourceData.reset();
				InvalidatePlatformData();
				return false;
			}
			bool bHDR = false;
			if (!DecodePanoramaInput(SourcePath,
				{.FaceDimension = PanoramaFaceDimension, .ExposureEV = PanoramaExposureEV},
				*NewSourceData, OriginalSourceWidth, OriginalSourceHeight, bHDR, OutError))
			{
				BuildStatus = OutError.find("decode failed") != std::string::npos
					? ETextureBuildStatus::DecodeFailure : ETextureBuildStatus::BuildFailure;
				LastBuildError = OutError;
				SourceData.reset();
				InvalidatePlatformData();
				return false;
			}
			SourceData = std::move(NewSourceData);
			return RebuildPlatformData(OutError);
		}
		if (SourceLayout != ETextureCubeSourceLayout::SixFaces)
		{
			OutError = "Texture cube source layout is invalid.";
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			SourceData.reset();
			InvalidatePlatformData();
			return false;
		}
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			const ETextureCubeFace Face = static_cast<ETextureCubeFace>(FaceIndex);
			if (GetSourceFile(Face).empty())
			{
				OutError = std::format("{} face has no source file.", FaceNames[FaceIndex]);
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				SourceData.reset();
				InvalidatePlatformData();
				return false;
			}
			const std::filesystem::path SourcePath = ResolveCubeSource(*this, Face);
			if (!std::filesystem::is_regular_file(SourcePath))
			{
				OutError = std::format("{} face source file does not exist: {}", FaceNames[FaceIndex], GetSourceFile(Face));
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				SourceData.reset();
				InvalidatePlatformData();
				return false;
			}
			if (!TextureBuild::DecodeRGBA8(SourcePath.generic_string(), NewSourceData->Faces[FaceIndex], OutError))
			{
				OutError = std::format("{} face decode failed: {}", FaceNames[FaceIndex], OutError);
				BuildStatus = ETextureBuildStatus::DecodeFailure;
				LastBuildError = OutError;
				SourceData.reset();
				InvalidatePlatformData();
				return false;
			}
		}
		if (!ValidateCubeSourceData(*NewSourceData, OutError))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			SourceData.reset();
			InvalidatePlatformData();
			return false;
		}
		SourceData = std::move(NewSourceData);
		return RebuildPlatformData(OutError);
	}

	auto DTextureCube::RefreshBuildStatus() -> void
	{
		if (RenderResource && RenderResource->GetFailedRevision() == BuildRevision)
		{
			if (RenderResource->GetFailureReason() == ETextureRenderFailure::UnsupportedFormat)
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
		const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportResult
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
		if (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		const std::filesystem::path DestinationDirectory =
			ResolveMountedFile(ParsedAssetPath.ToString()).parent_path();
		const std::string SourceFileName = MakePanoramaSourceFileName(
			ParsedAssetPath.GetAssetName(), Input.extension().generic_string());
		const std::filesystem::path Destination = DestinationDirectory / SourceFileName;
		if (std::filesystem::exists(Destination))
			return {false, std::format("Imported panorama source already exists: {}",
				Destination.generic_string()), nullptr};

		DTextureCube* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};

		std::error_code ErrorCode;
		std::filesystem::create_directories(DestinationDirectory, ErrorCode);
		ErrorCode.clear();
		if (!std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::none, ErrorCode))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy panorama source: {}", ErrorCode.message()), nullptr};
		}

		Texture->SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		Texture->PanoramaSourceFile = SourceFileName;
		Texture->PanoramaFaceDimension = Settings.FaceDimension;
		Texture->PanoramaExposureEV = Settings.ExposureEV;
		Texture->OriginalSourceWidth = SourceWidth;
		Texture->OriginalSourceHeight = SourceHeight;
		Texture->bSRGB = true;
		Texture->SourceData = std::move(NewSourceData);
		Texture->PlatformData = std::move(NewPlatformData);
		Texture->BuildStatus = ETextureBuildStatus::Ready;

		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			std::filesystem::remove(Destination, ErrorCode);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
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

		const std::filesystem::path Input = std::filesystem::absolute(PanoramaFile).lexically_normal();
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

		const std::filesystem::path OldSource = ResolvePanoramaSource();
		const std::filesystem::path DestinationDirectory =
			ResolveMountedFile(GetPackage()->GetPackagePath()).parent_path();
		const std::string NewSourceFileName = MakePanoramaSourceFileName(
			GetName(), Input.extension().generic_string());
		const std::filesystem::path NewSource = DestinationDirectory / NewSourceFileName;
		std::error_code ErrorCode;
		const bool bInputIsDestination = Input.lexically_normal() == NewSource.lexically_normal();
		const std::filesystem::path TemporarySource =
			NewSource.generic_string() + ".reimport.tmp";
		const std::filesystem::path BackupSource =
			NewSource.generic_string() + ".reimport.backup";
		if (!bInputIsDestination)
		{
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

		const std::string OldSourceFileName = PanoramaSourceFile;
		const uint32 OldFaceDimension = PanoramaFaceDimension;
		const float OldExposureEV = PanoramaExposureEV;
		const uint32 OldSourceWidth = OriginalSourceWidth;
		const uint32 OldSourceHeight = OriginalSourceHeight;
		auto OldSourceData = std::move(SourceData);
		auto OldPlatformData = std::move(PlatformData);
		PanoramaSourceFile = NewSourceFileName;
		PanoramaFaceDimension = Settings.FaceDimension;
		PanoramaExposureEV = Settings.ExposureEV;
		OriginalSourceWidth = NewSourceWidth;
		OriginalSourceHeight = NewSourceHeight;
		SourceData = std::move(NewSourceData);
		PlatformData = std::move(NewPlatformData);

		const Asset::FAssetResult SaveResult = Asset::SavePackage(GetPackage());
		if (!SaveResult)
		{
			PanoramaSourceFile = OldSourceFileName;
			PanoramaFaceDimension = OldFaceDimension;
			PanoramaExposureEV = OldExposureEV;
			OriginalSourceWidth = OldSourceWidth;
			OriginalSourceHeight = OldSourceHeight;
			SourceData = std::move(OldSourceData);
			PlatformData = std::move(OldPlatformData);
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
			if (OldSource != NewSource) std::filesystem::remove(OldSource, ErrorCode);
		}
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		return true;
	}

	auto DTextureCube::ImportAsset(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath, const FTextureCubeImportSettings& Settings) -> FTextureCubeImportResult
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
		if (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		const std::filesystem::path DestinationDirectory = ResolveMountedFile(ParsedAssetPath.ToString()).parent_path();
		std::array<std::string, TextureCubeFaceCount> SourceFileNames;
		std::array<std::filesystem::path, TextureCubeFaceCount> Destinations;
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			SourceFileNames[FaceIndex] = MakeSourceFileName(ParsedAssetPath.GetAssetName(), FaceIndex,
				Inputs[FaceIndex].extension().generic_string());
			Destinations[FaceIndex] = DestinationDirectory / SourceFileNames[FaceIndex];
			if (std::filesystem::exists(Destinations[FaceIndex]))
				return {false, std::format("Imported {} face source already exists: {}",
					FaceNames[FaceIndex], Destinations[FaceIndex].generic_string()), nullptr};
		}

		DTextureCube* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};

		std::error_code ErrorCode;
		std::filesystem::create_directories(DestinationDirectory, ErrorCode);
		size_t CopiedCount = 0;
		for (; CopiedCount < TextureCubeFaceCount; ++CopiedCount)
		{
			ErrorCode.clear();
			if (!std::filesystem::copy_file(Inputs[CopiedCount], Destinations[CopiedCount],
				std::filesystem::copy_options::none, ErrorCode)) break;
		}
		if (CopiedCount != TextureCubeFaceCount)
		{
			const std::string CopyError = ErrorCode.message();
			for (size_t Index = 0; Index < CopiedCount; ++Index) std::filesystem::remove(Destinations[Index], ErrorCode);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy {} face source: {}", FaceNames[CopiedCount], CopyError), nullptr};
		}

		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			Texture->GetMutableSourceFile(static_cast<ETextureCubeFace>(FaceIndex)) = SourceFileNames[FaceIndex];
		Texture->SourceLayout = ETextureCubeSourceLayout::SixFaces;
		Texture->bSRGB = Settings.bSRGB;
		Texture->SourceData = std::move(NewSourceData);
		Texture->PlatformData = std::move(NewPlatformData);
		Texture->BuildStatus = ETextureBuildStatus::Ready;
		Texture->QueueRenderResourceBuild();

		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			for (const std::filesystem::path& Destination : Destinations) std::filesystem::remove(Destination, ErrorCode);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		return {true, {}, Texture};
	}
}

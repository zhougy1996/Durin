#include "Texture/TextureCube.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "DynamicRHI.h"
#include "Misc/Paths.h"
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
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				if (!TextureBuild::BuildMipChain(SourceData.Faces[FaceIndex], ETextureUsage::Color, bSRGB,
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
			BuildStatus = ETextureBuildStatus::UploadFailure;
			LastBuildError = "GPU cube texture creation or upload failed.";
		}
	}

	auto DTextureCube::ImportAsset(const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath, const FTextureCubeImportSettings& Settings) -> FTextureCubeImportResult
	{
		std::array<std::filesystem::path, TextureCubeFaceCount> Inputs;
		auto NewSourceData = std::make_unique<FTextureCubeSourceData>();
		for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			if (FaceFiles[FaceIndex].empty()) return {false, std::format("{} face source is missing.", FaceNames[FaceIndex]), nullptr};
			Inputs[FaceIndex] = std::filesystem::absolute(FaceFiles[FaceIndex]).lexically_normal();
			if (!std::filesystem::is_regular_file(Inputs[FaceIndex]))
				return {false, std::format("{} face source file does not exist.", FaceNames[FaceIndex]), nullptr};
			if (!Asset::IsSupportedImageExtension(Inputs[FaceIndex].extension().generic_string()))
				return {false, std::format("{} face uses an unsupported texture source format.", FaceNames[FaceIndex]), nullptr};
			std::string DecodeError;
			if (!TextureBuild::DecodeRGBA8(Inputs[FaceIndex].generic_string(), NewSourceData->Faces[FaceIndex], DecodeError))
				return {false, std::format("{} face decode failed: {}", FaceNames[FaceIndex], DecodeError), nullptr};
		}
		std::string ValidationError;
		if (!ValidateCubeSourceData(*NewSourceData, ValidationError))
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

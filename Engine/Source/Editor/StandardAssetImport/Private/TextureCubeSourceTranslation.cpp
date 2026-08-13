#include "TextureCubeSourceTranslation.h"

#include "Asset/MountedSource.h"
#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Texture/TextureCubeBuilder.h"
#include "TextureCubeBuildAdapter.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::Asset::Import
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceSuffixes = {
			"px", "nx", "py", "ny", "pz", "nz"};

		auto MutationContext(bool bEngineAuthoringContext)
			-> Asset::EMountedSourceMutationContext
		{
			return bEngineAuthoringContext
				? Asset::EMountedSourceMutationContext::EngineAuthoring
				: Asset::EMountedSourceMutationContext::DependencySafe;
		}

		auto NormalizePanorama(Asset::FDecodedImage&& Image)
			-> Asset::Build::TextureCubeBuilder::FTexturePanoramaImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height, .SourceChannelCount = Image.SourceChannelCount,
				.bHasTransparency = Image.bHasTransparency};
		}

		auto NormalizePanorama(Asset::FDecodedFloatImage&& Image)
			-> Asset::Build::TextureCubeBuilder::FTexturePanoramaFloatImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height};
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Suffix,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPath.ToString());
			if (!Lookup)
			{
				OutError = std::format(
					"TextureCube asset {} is not beneath a registered package mount.",
					AssetPath.ToString());
				return false;
			}
			if (RequestedSourcePath.empty())
			{
				std::filesystem::path RelativeAssetPath(
					std::string(AssetPath.ToString().substr(Lookup.Mount->VirtualRoot.size())));
				RelativeAssetPath.replace_filename(std::format("{}{}{}",
					RelativeAssetPath.stem().generic_string(), Suffix, Extension));
				OutStoredPath = Lookup.Mount->VirtualRoot
					+ (std::filesystem::path("Textures") / RelativeAssetPath)
						.lexically_normal().generic_string();
			}
			else
			{
				OutStoredPath = RequestedSourcePath;
			}
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					OutStoredPath, PathUtilities::EPathExistence::AllowMissing);
			if (Resolved) return true;
			OutError = Resolved.Message;
			return false;
		}

		auto LoadMountedSource(
			const FSourcePath& SourcePath,
			Asset::FMountedSourceFile& OutSource,
			std::string_view PackagePath,
			std::string& OutError) -> bool
		{
			return Asset::ResolveMountedSourceReference(
				PackagePath, SourcePath.Path, OutSource, OutError);
		}

		auto BuildPanorama(
			DTextureCube& Texture,
			const Asset::FMountedSourceFile& Source,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Source.PhysicalPath.generic_string()))
			{
				OutError = std::format(
					"Failed to read mounted TextureCube panorama '{}'.",
					Source.SourcePath.Path);
				return false;
			}
			const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
			if (Asset::IsRadianceHDRExtension(
				Source.PhysicalPath.extension().generic_string()))
			{
				Asset::FDecodedFloatImage Panorama;
				if (!Asset::DecodeRadianceHDRFromMemory(Bytes, Panorama, OutError,
					{.MaximumDecodedPixels =
						Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels}))
				{
					OutError = std::format(
						"TextureCube panorama decode failed: {}", OutError);
					return false;
				}
				return BuildAndPublishTextureCubePanorama(Texture,
						NormalizePanorama(std::move(Panorama)), Hash,
						Source.SourcePath, Settings, OutError);
			}
			Asset::FDecodedImage Panorama;
			if (!Asset::DecodeImageFromMemory(Bytes, Panorama, OutError,
				{.MaximumDecodedPixels =
					Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels}))
			{
				OutError = std::format(
					"TextureCube panorama decode failed: {}", OutError);
				return false;
			}
			return BuildAndPublishTextureCubePanorama(Texture,
					NormalizePanorama(std::move(Panorama)), Hash,
					Source.SourcePath, Settings, OutError);
		}

		auto BuildFaces(
			DTextureCube& Texture,
			const std::array<Asset::FMountedSourceFile, TextureCubeFaceCount>& Sources,
			const FTextureCubeImportSettings& Settings,
			std::string& OutError) -> bool
		{
			FTextureCubeSourceData SourceData;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			std::array<FSourcePath, TextureCubeFaceCount> Paths;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				std::vector<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(
						Bytes, Sources[Index].PhysicalPath.generic_string())
					|| !TranslateTexture2DSource(Bytes, SourceData.Faces[Index], OutError))
				{
					if (OutError.empty())
						OutError = std::format("Failed to read {} TextureCube face.", FaceNames[Index]);
					return false;
				}
				Hashes[Index] = FXxHash128::HashBuffer(Bytes);
				Paths[Index] = Sources[Index].SourcePath;
			}
			return BuildAndPublishTextureCubeFaces(
				Texture, std::move(SourceData), Hashes, Paths, Settings, OutError);
		}

		template <typename Builder>
		auto BuildAndSaveCandidate(
			DTextureCube& Texture,
			Builder&& Build,
			std::string& OutError) -> bool
		{
			auto* Candidate = NewObject<DTextureCube>(nullptr, "TextureCubeAuthoringCandidate");
			if (!Build(*Candidate))
			{
				MarkAsGarbage(Candidate);
				return false;
			}
			Texture.ExchangeImportedState(*Candidate);
			const Asset::FAssetResult Saved = Asset::SavePackage(Texture.GetPackage());
			if (!Saved)
			{
				Texture.ExchangeImportedState(*Candidate);
				OutError = Saved.Message;
				MarkAsGarbage(Candidate);
				return false;
			}
			MarkAsGarbage(Candidate);
			return true;
		}

		auto MakeValidation(const Asset::Build::FTextureCubeBuildProduct& Product, bool bHDR)
			-> FTextureCubeImportValidation
		{
			return {.bValid = true, .SourceLayout = Product.SourceLayout,
				.SourceWidth = Product.SourceWidth, .SourceHeight = Product.SourceHeight,
				.Dimension = Product.PlatformData->Faces[0].Mips[0].Width,
				.MipCount = static_cast<uint32>(
					Product.PlatformData->Faces[0].Mips.size()),
				.PixelFormat = Product.PlatformData->PixelFormat, .bHDR = bHDR};
		}
	}

	auto ValidateTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings) -> FTextureCubeImportValidation
	{
		FTextureCubeSourceData SourceData;
		std::array<FXxHash128, TextureCubeFaceCount> Hashes;
		std::string Error;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, FaceFiles[Index])
				|| !TranslateTexture2DSource(Bytes, SourceData.Faces[Index], Error))
				return {false, std::format("{} face decode failed: {}", FaceNames[Index],
					Error.empty() ? "source is unavailable" : Error)};
			Hashes[Index] = FXxHash128::HashBuffer(Bytes);
		}
		Asset::Build::FTextureCubeBuildProduct Product;
		if (!Asset::Build::BuildTextureCubeFaces(
			std::move(SourceData), Hashes, Settings, Product, Error))
			return {false, std::move(Error)};
		return MakeValidation(Product, false);
	}

	auto ValidateTextureCubePanorama(
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportValidation
	{
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PanoramaFile))
			return {false, "Panorama source is unavailable."};
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		std::string Error;
		Asset::Build::FTextureCubeBuildProduct Product;
		const bool bHDR = Asset::IsRadianceHDRExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string());
		if (bHDR)
		{
			Asset::FDecodedFloatImage Panorama;
			if (!Asset::DecodeRadianceHDRFromMemory(Bytes, Panorama, Error,
				{.MaximumDecodedPixels = Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels})
				|| !Asset::Build::BuildTextureCubePanorama(
					NormalizePanorama(std::move(Panorama)), Hash, Settings, Product, Error))
				return {false, std::move(Error)};
		}
		else
		{
			Asset::FDecodedImage Panorama;
			if (!Asset::DecodeImageFromMemory(Bytes, Panorama, Error,
				{.MaximumDecodedPixels = Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels})
				|| !Asset::Build::BuildTextureCubePanorama(
					NormalizePanorama(std::move(Panorama)), Hash, Settings, Product, Error))
				return {false, std::move(Error)};
		}
		return MakeValidation(Product, bHDR);
	}

	auto ImportTextureCubePanorama(
		std::string_view PanoramaFile,
		std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string_view SourceDestination,
		bool bEngineAuthoringContext) -> FTextureCubeImportResult
	{
		const std::filesystem::path Input =
			std::filesystem::absolute(PanoramaFile).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return {false, "Panorama source is unavailable.", nullptr};
		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath)
			|| Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(ParsedAssetPath, "_panorama",
			Input.extension().generic_string(), SourceDestination,
			StoredSourcePath, Error)) return {false, std::move(Error), nullptr};
		Asset::FMountedSourceFile Source;
		if (!Asset::PrepareMountedSourceFile(Input, ParsedAssetPath.ToString(),
			StoredSourcePath, Source, Error, MutationContext(bEngineAuthoringContext)))
			return {false, std::move(Error), nullptr};
		DTextureCube* Texture = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!Created || !BuildPanorama(*Texture, Source, Settings, Error))
		{
			Asset::RollbackMountedSourceFile(Source);
			if (Created) Asset::UnloadPackage(ParsedAssetPath);
			return {false, Created ? std::move(Error) : Created.Message, nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Texture->GetPackage());
		if (!Saved)
		{
			Asset::RollbackMountedSourceFile(Source);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, Saved.Message, nullptr};
		}
		Asset::CommitMountedSourceFile(Source);
		return {true, {}, Texture};
	}

	auto ImportTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath,
		const FTextureCubeImportSettings& Settings,
		const std::array<std::string, TextureCubeFaceCount>& SourceDestinations,
		bool bEngineAuthoringContext) -> FTextureCubeImportResult
	{
		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath)
			|| Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		std::array<Asset::FMountedSourceFile, TextureCubeFaceCount> Sources;
		auto Rollback = [&] {
			for (auto& Source : Sources) Asset::RollbackMountedSourceFile(Source);
		};
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const std::filesystem::path Input =
				std::filesystem::absolute(FaceFiles[Index]).lexically_normal();
			if (!std::filesystem::is_regular_file(Input))
			{
				Rollback();
				return {false, std::format("{} face source is unavailable.", FaceNames[Index]), nullptr};
			}
			std::string StoredSourcePath;
			if (!MakeCanonicalSourceLocation(ParsedAssetPath,
				std::format("_{}", FaceSuffixes[Index]), Input.extension().generic_string(),
				SourceDestinations[Index], StoredSourcePath, Error)
				|| !Asset::PrepareMountedSourceFile(Input, ParsedAssetPath.ToString(),
					StoredSourcePath, Sources[Index], Error,
					MutationContext(bEngineAuthoringContext)))
			{
				Rollback();
				return {false, std::move(Error), nullptr};
			}
		}
		DTextureCube* Texture = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!Created || !BuildFaces(*Texture, Sources, Settings, Error))
		{
			Rollback();
			if (Created) Asset::UnloadPackage(ParsedAssetPath);
			return {false, Created ? std::move(Error) : Created.Message, nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Texture->GetPackage());
		if (!Saved)
		{
			Rollback();
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, Saved.Message, nullptr};
		}
		for (auto& Source : Sources) Asset::CommitMountedSourceFile(Source);
		return {true, {}, Texture};
	}

	auto ReimportTextureCubePanorama(
		DTextureCube& Texture,
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (Texture.GetSourceLayout() != ETextureCubeSourceLayout::EquirectangularPanorama
			|| !Texture.GetPackage())
		{
			OutError = "Only packaged panorama-backed texture cubes can be reimported.";
			return false;
		}
		Asset::FMountedSourceFile Source;
		if (!LoadMountedSource(Texture.GetSourceImportData().Panorama.SourcePath,
			Source, Texture.GetPackage()->GetPackagePath(), OutError)) return false;
		std::error_code Error;
		if (!std::filesystem::equivalent(Source.PhysicalPath,
			std::filesystem::absolute(PanoramaFile).lexically_normal(), Error) || Error)
		{
			OutError =
				"Reimport is read-only and must use the persisted mounted panorama source.";
			return false;
		}
		return BuildAndSaveCandidate(Texture,
			[&](DTextureCube& Candidate) {
				return BuildPanorama(Candidate, Source, Settings, OutError);
			}, OutError);
	}

	auto ReimportTextureCubeFaces(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (Texture.GetSourceLayout() != ETextureCubeSourceLayout::SixFaces
			|| !Texture.GetPackage())
		{
			OutError = "Only packaged six-face texture cubes can be reimported.";
			return false;
		}
		std::array<Asset::FMountedSourceFile, TextureCubeFaceCount> Sources;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!LoadMountedSource(Texture.GetSourceImportData().GetFace(
					static_cast<ETextureCubeFace>(Index)).SourcePath,
				Sources[Index], Texture.GetPackage()->GetPackagePath(), OutError)) return false;
			std::error_code Error;
			if (!std::filesystem::equivalent(Sources[Index].PhysicalPath,
				std::filesystem::absolute(FaceFiles[Index]).lexically_normal(), Error) || Error)
			{
				OutError =
					"Reimport is read-only and must use every persisted mounted face source.";
				return false;
			}
		}
		return BuildAndSaveCandidate(Texture,
			[&](DTextureCube& Candidate) {
				return BuildFaces(Candidate, Sources, Settings, OutError);
			}, OutError);
	}

	auto ChangeTextureCubePanoramaSourceReference(
		DTextureCube& Texture,
		std::string_view SourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		Asset::FMountedSourceFile Source;
		if (!Asset::ResolveMountedSourceReference(Texture.GetPackage()->GetPackagePath(),
			SourceVirtualPath, Source, OutError)) return false;
		return BuildAndSaveCandidate(Texture,
			[&](DTextureCube& Candidate) {
				return BuildPanorama(Candidate, Source, Settings, OutError);
			}, OutError);
	}

	auto ChangeTextureCubeFaceSourceReferences(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& SourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<Asset::FMountedSourceFile, TextureCubeFaceCount> Sources;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			if (!Asset::ResolveMountedSourceReference(
				Texture.GetPackage()->GetPackagePath(), SourceVirtualPaths[Index],
				Sources[Index], OutError)) return false;
		return BuildAndSaveCandidate(Texture,
			[&](DTextureCube& Candidate) {
				return BuildFaces(Candidate, Sources, Settings, OutError);
			}, OutError);
	}

	auto IngestAndChangeTextureCubePanoramaSource(
		DTextureCube& Texture,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		Asset::FMountedSourceFile Source;
		if (!Asset::PrepareMountedSourceFile(FilePath,
			Texture.GetPackage()->GetPackagePath(), TargetSourceVirtualPath,
			Source, OutError)) return false;
		const bool bChanged = ChangeTextureCubePanoramaSourceReference(
			Texture, Source.SourcePath.Path, Settings, OutError);
		if (bChanged) Asset::CommitMountedSourceFile(Source);
		else Asset::RollbackMountedSourceFile(Source);
		return bChanged;
	}

	auto IngestAndChangeTextureCubeFaceSources(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const std::array<std::string, TextureCubeFaceCount>& TargetSourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged texture cubes can retain source provenance.";
			return false;
		}
		std::array<Asset::FMountedSourceFile, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> VirtualPaths;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!Asset::PrepareMountedSourceFile(FaceFiles[Index],
				Texture.GetPackage()->GetPackagePath(), TargetSourceVirtualPaths[Index],
				Sources[Index], OutError))
			{
				for (auto& Source : Sources) Asset::RollbackMountedSourceFile(Source);
				return false;
			}
			VirtualPaths[Index] = Sources[Index].SourcePath.Path;
		}
		const bool bChanged = ChangeTextureCubeFaceSourceReferences(
			Texture, VirtualPaths, Settings, OutError);
		for (auto& Source : Sources)
		{
			if (bChanged) Asset::CommitMountedSourceFile(Source);
			else Asset::RollbackMountedSourceFile(Source);
		}
		return bChanged;
	}
}

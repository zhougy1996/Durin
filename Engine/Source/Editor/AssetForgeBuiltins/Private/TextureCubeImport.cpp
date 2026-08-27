#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/TextureCubeImportData.h"

#include "Asset/AssetOperations.h"
#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureDerivedData.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceRoles = {
			"positive-x", "negative-x", "positive-y", "negative-y", "positive-z", "negative-z"};
		constexpr uint64 MaximumTextureCubeEncodedBytes = 256ull * 1024ull * 1024ull;
		constexpr std::string_view TextureCubeDecoderId = "DurinImage";
		constexpr uint32 TextureCubeDecoderVersion = 1;

		auto ResolveOwningPackagePhysicalPath(const DTextureCube& Texture,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			if (!Texture.GetPackage())
			{
				OutError = "TextureCube source capture requires an owning package.";
				return false;
			}
			const PathUtilities::FAssetPathResult Resolved =
				PathUtilities::ResolveAssetPath(Texture.GetPackage()->GetPackagePath(),
					PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved) { OutError = Resolved.Message; return false; }
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return true;
		}

		auto NormalizePanorama(Image::FDecodedImage&& Image)
			-> Asset::TextureCubeBuilder::FTexturePanoramaImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height, .SourceChannelCount = Image.SourceChannelCount,
				.bHasTransparency = Image.bHasTransparency};
		}

		auto NormalizePanorama(Image::FDecodedFloatImage&& Image)
			-> Asset::TextureCubeBuilder::FTexturePanoramaFloatImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height};
		}

		struct FCapturedCubeSource
		{
			std::string Filename;
			ESourceHintBase HintBase =
				ESourceHintBase::AssetRelative;
			std::filesystem::path PhysicalPath;
			FEncodedSourceSnapshot Snapshot;
		};

		auto CaptureCubeSource(const DTextureCube& Texture, std::string_view FilePath,
			FCapturedCubeSource& OutSource, std::string& OutError) -> bool
		{
			std::filesystem::path OwningPackagePath;
			if (!ResolveOwningPackagePhysicalPath(Texture, OwningPackagePath, OutError))
				return false;
			OutSource.PhysicalPath = std::filesystem::absolute(FilePath).lexically_normal();
			if (!std::filesystem::is_regular_file(OutSource.PhysicalPath)
				|| !MakeSourceHint(
					OutSource.PhysicalPath.generic_string(), OwningPackagePath.generic_string(),
					OutSource.HintBase, OutSource.Filename, OutError)) return false;
			return CaptureEncodedSource(OutSource.Filename, OutSource.PhysicalPath,
				OutSource.Snapshot, OutError, MaximumTextureCubeEncodedBytes);
		}

		auto PublishCubeImportData(DTextureCube& Texture,
			const std::span<const FCapturedCubeSource> Sources,
			ETextureCubeSourceLayout Layout, std::string& OutError) -> bool
		{
			FTextureCubeImportDataState State;
			State.SourceLayout = Layout;
			State.DecoderId = std::string(TextureCubeDecoderId);
			State.DecoderVersion = TextureCubeDecoderVersion;
			State.ProjectionVersion = TextureCubeProjectionVersion;
			for (size_t Index = 0; Index < Sources.size(); ++Index)
			{
				const auto& Source = Sources[Index];
				State.SourceData.Sources.push_back({
					.Role = Layout == ETextureCubeSourceLayout::EquirectangularPanorama
						? "panorama" : std::string(FaceRoles[Index]),
					.DisplayLabel = Source.PhysicalPath.filename().generic_string(),
					.Hint = Source.Filename,
					.HintBase = Source.HintBase,
					.ContentHashLow = Source.Snapshot.ContentHash.HashLow,
					.ContentHashHigh = Source.Snapshot.ContentHash.HashHigh,
					.ByteCount = Source.Snapshot.FileSize});
			}
			auto* Data = dynamic_cast<DTextureCubeImportData*>(Texture.GetAssetImportData());
			if (!Data) Data = NewObject<DTextureCubeImportData>(&Texture, "AssetImportData");
			return Data && Data->SetState(std::move(State), OutError)
				&& Texture.PublishAssetImportData(*Data, OutError);
		}

		auto SaveImportedCube(DTextureCube& Texture, std::string& OutError,
			const Asset::FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			if (!SaveOptions) return true;
			DPackage* Package = Texture.GetPackage();
			const Asset::FAssetResult Saved = Asset::SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}

		auto RebuildPanorama(DTextureCube& Texture, std::string_view FilePath,
			const FTextureCubePanoramaImportSettings& Settings, std::string& OutError,
			const Asset::FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			FCapturedCubeSource Source;
			if (!CaptureCubeSource(Texture, FilePath, Source, OutError)
				|| !IsTextureCubePanoramaSourceExtension(
					Source.PhysicalPath.extension().generic_string())) return false;
			FTextureCubePanoramaSourceData Panorama;
			if (!TranslateTextureCubePanoramaSource(Source.Snapshot.GetBytes(),
				Source.PhysicalPath.extension().generic_string(), Panorama, OutError))
			{
				OutError = std::format("TextureCube panorama decode failed: {}", OutError);
				return false;
			}
			Asset::FTextureCubeBuildProduct Product;
			if (!std::visit([&](auto&& Decoded) {
					return Asset::BuildTextureCubePanorama(std::move(Decoded),
						Source.Snapshot.ContentHash, Settings, Product, OutError);
				}, std::move(Panorama))
				|| !Asset::PublishTextureCubeProduct(Texture, std::move(Product), {
					.PanoramaHash = Source.Snapshot.ContentHash}, OutError)
				|| !PublishCubeImportData(Texture, std::span(&Source, 1),
					ETextureCubeSourceLayout::EquirectangularPanorama, OutError)) return false;
			return SaveImportedCube(Texture, OutError, SaveOptions);
		}

		auto RebuildFaces(DTextureCube& Texture,
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const FTextureCubeImportSettings& Settings, std::string& OutError,
			const Asset::FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			std::array<FCapturedCubeSource, TextureCubeFaceCount> Sources;
			std::array<std::span<const std::byte>, TextureCubeFaceCount> Encoded;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				if (!CaptureCubeSource(Texture, FaceFiles[Index], Sources[Index], OutError)
					|| !IsTextureCubeFaceSourceExtension(
						Sources[Index].PhysicalPath.extension().generic_string()))
				{
					if (OutError.empty()) OutError = std::format(
						"{} TextureCube face source is unsupported.", FaceNames[Index]);
					return false;
				}
				Encoded[Index] = Sources[Index].Snapshot.GetBytes();
				Hashes[Index] = Sources[Index].Snapshot.ContentHash;
			}
			FTextureCubeSourceData SourceData;
			Asset::FTextureCubeBuildProduct Product;
			if (!TranslateTextureCubeFaceSources(Encoded, SourceData, OutError)
				|| !Asset::BuildTextureCubeFaces(
					std::move(SourceData), Hashes, Settings, Product, OutError)
				|| !Asset::PublishTextureCubeProduct(Texture, std::move(Product), {
					.FaceHashes = Hashes}, OutError)
				|| !PublishCubeImportData(Texture, Sources,
					ETextureCubeSourceLayout::SixFaces, OutError)) return false;
			return SaveImportedCube(Texture, OutError, SaveOptions);
		}

		auto MakeValidation(const Asset::FTextureCubeBuildProduct& Product, bool bHDR)
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

	auto IsTextureCubeFaceSourceExtension(std::string_view Extension) -> bool
	{
		return IsTexture2DSourceExtension(Extension);
	}

	auto IsTextureCubePanoramaSourceExtension(std::string_view Extension) -> bool
	{
		return IsTextureCubeFaceSourceExtension(Extension)
			|| Image::IsRadianceHDRExtension(Extension);
	}

	auto TranslateTextureCubePanoramaSource(
		std::span<const std::byte> EncodedBytes,
		std::string_view ExtensionHint,
		FTextureCubePanoramaSourceData& OutSource,
		std::string& OutError) -> bool
	{
		if (!IsTextureCubePanoramaSourceExtension(ExtensionHint))
		{
			OutError = "Unsupported TextureCube panorama source format.";
			return false;
		}
		if (Image::IsRadianceHDRExtension(ExtensionHint))
		{
			Image::FDecodedFloatImage Panorama;
			if (!Image::DecodeRadianceHDRFromMemory(EncodedBytes, Panorama, OutError,
				{.MaximumDecodedPixels = Asset::TextureCubeBuilder::MaximumPanoramaPixels}))
				return false;
			OutSource = NormalizePanorama(std::move(Panorama));
			return true;
		}
		Image::FDecodedImage Panorama;
		if (!Image::DecodeImageFromMemory(EncodedBytes, Panorama, OutError,
			{.MaximumDecodedPixels = Asset::TextureCubeBuilder::MaximumPanoramaPixels}))
			return false;
		OutSource = NormalizePanorama(std::move(Panorama));
		return true;
	}

	auto TranslateTextureCubeFaceSources(
		const std::array<std::span<const std::byte>, TextureCubeFaceCount>& EncodedFaces,
		FTextureCubeSourceData& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!TranslateTexture2DSource(EncodedFaces[Index], OutSource.Faces[Index], OutError))
			{
				OutError = std::format("{} TextureCube face decode failed: {}",
					FaceNames[Index], OutError);
				OutSource = {};
				return false;
			}
		}
		return true;
	}

	auto ValidateTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings) -> FTextureCubeImportValidation
	{
		FTextureCubeSourceData SourceData;
		std::array<FXxHash128, TextureCubeFaceCount> Hashes;
		std::array<std::vector<std::byte>, TextureCubeFaceCount> Bytes;
		std::array<std::span<const std::byte>, TextureCubeFaceCount> EncodedFaces;
		std::string Error;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!IsTextureCubeFaceSourceExtension(
				std::filesystem::path(FaceFiles[Index]).extension().generic_string()))
				return {false, std::format("{} face source format is unsupported.", FaceNames[Index])};
			if (!FFileHelper::LoadFileToArray(Bytes[Index], FaceFiles[Index]))
				return {false, std::format("{} face decode failed: {}", FaceNames[Index],
					Error.empty() ? "source is unavailable" : Error)};
			EncodedFaces[Index] = Bytes[Index];
			Hashes[Index] = FXxHash128::HashBuffer(Bytes[Index]);
		}
		if (!TranslateTextureCubeFaceSources(EncodedFaces, SourceData, Error))
			return {false, std::move(Error)};
		Asset::FTextureCubeBuildProduct Product;
		if (!Asset::BuildTextureCubeFaces(
			std::move(SourceData), Hashes, Settings, Product, Error))
			return {false, std::move(Error)};
		return MakeValidation(Product, false);
	}

	auto ValidateTextureCubePanorama(
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportValidation
	{
		if (!IsTextureCubePanoramaSourceExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string()))
			return {false, "Panorama source format is unsupported."};
		std::vector<std::byte> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PanoramaFile))
			return {false, "Panorama source is unavailable."};
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		std::string Error;
		Asset::FTextureCubeBuildProduct Product;
		const bool bHDR = Image::IsRadianceHDRExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string());
		FTextureCubePanoramaSourceData Panorama;
		if (!TranslateTextureCubePanoramaSource(Bytes,
			std::filesystem::path(PanoramaFile).extension().generic_string(), Panorama, Error)
			|| !std::visit([&](auto&& Source) {
				return Asset::BuildTextureCubePanorama(
					std::move(Source), Hash, Settings, Product, Error);
			}, std::move(Panorama))) return {false, std::move(Error)};
		return MakeValidation(Product, bHDR);
	}

	auto ImportTextureCubePanorama(
		std::string_view PanoramaFile,
		std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string_view,
		bool bAllowEngineContentWrite) -> FTextureCubeImportResult
	{
		(void)bAllowEngineContentWrite;
		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		DTextureCube* Texture = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!Created || !Texture) return {false, Created.Message, nullptr};
		if (!RebuildPanorama(*Texture, PanoramaFile, Settings, Error, nullptr))
		{
			(void)Asset::UnloadPackage(
				ParsedAssetPath, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return {false, std::move(Error), nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Texture->GetPackage());
		if (!Saved) return {false, Saved.Message, Texture};
		return {true, {}, Texture};
	}

	auto ImportTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath,
		const FTextureCubeImportSettings& Settings,
		const std::array<std::string, TextureCubeFaceCount>&,
		bool bAllowEngineContentWrite) -> FTextureCubeImportResult
	{
		(void)bAllowEngineContentWrite;
		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		DTextureCube* Texture = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!Created || !Texture) return {false, Created.Message, nullptr};
		if (!RebuildFaces(*Texture, FaceFiles, Settings, Error, nullptr))
		{
			(void)Asset::UnloadPackage(
				ParsedAssetPath, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return {false, std::move(Error), nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Texture->GetPackage());
		if (!Saved) return {false, Saved.Message, Texture};
		return {true, {}, Texture};
	}

	auto ReimportTextureCubePanorama(
		DTextureCube& Texture,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		std::filesystem::path OwningPackagePath;
		if (!ResolveOwningPackagePhysicalPath(Texture, OwningPackagePath, OutError))
			return false;
		const auto* Data = dynamic_cast<const DTextureCubeImportData*>(
			Texture.GetAssetImportData());
		const FSourceFile* Source = Data
			? Data->GetSourceData().FindByRole("panorama") : nullptr;
		std::string SourcePath;
		if (!Source || !ResolveSourceHint(Source->HintBase, Source->Hint,
			OwningPackagePath.generic_string(), SourcePath, OutError)) return false;
		const Asset::FAssetBundleSaveOptions SaveOptions;
		return RebuildPanorama(Texture, SourcePath, Settings, OutError, &SaveOptions);
	}

	auto ReimportTextureCubePanoramaFromFile(
		DTextureCube& Texture,
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged texture cubes can be reimported.";
			return false;
		}
		const std::filesystem::path SourcePath =
			std::filesystem::absolute(PanoramaFile).lexically_normal();
		const Asset::FAssetBundleSaveOptions SaveOptions;
		return RebuildPanorama(Texture, SourcePath.generic_string(),
			Settings, OutError, &SaveOptions);
	}

	auto ReimportTextureCubeFaces(
		DTextureCube& Texture,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		std::filesystem::path OwningPackagePath;
		if (!ResolveOwningPackagePhysicalPath(Texture, OwningPackagePath, OutError))
			return false;
		std::array<std::string, TextureCubeFaceCount> Sources;
		const auto* Data = dynamic_cast<const DTextureCubeImportData*>(
			Texture.GetAssetImportData());
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const FSourceFile* Source = Data
				? Data->GetSourceData().FindByRole(FaceRoles[Index]) : nullptr;
			if (!Source || !ResolveSourceHint(Source->HintBase, Source->Hint,
				OwningPackagePath.generic_string(), Sources[Index], OutError))
			{
				if (OutError.empty()) OutError = "TextureCube face import data is incomplete.";
				return false;
			}
		}
		const Asset::FAssetBundleSaveOptions SaveOptions;
		return RebuildFaces(Texture, Sources, Settings, OutError, &SaveOptions);
	}

	auto ReimportTextureCubeFacesFromFile(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged texture cubes can be reimported.";
			return false;
		}
		std::array<std::string, TextureCubeFaceCount> Sources;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			Sources[Index] = std::filesystem::absolute(FaceFiles[Index])
				.lexically_normal().generic_string();
		const Asset::FAssetBundleSaveOptions SaveOptions;
		return RebuildFaces(Texture, Sources, Settings, OutError, &SaveOptions);
	}
}

#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/TextureCubeFactory.h"
#include "Asset/AssetImportData.h"

#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Texture/TextureDerivedData.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceRoles = {
			"positive-x", "negative-x", "positive-y", "negative-y", "positive-z", "negative-z"};
		constexpr uint64 MaximumTextureCubeEncodedBytes = 256ull * 1024ull * 1024ull;
		auto ResolveOwningPackagePhysicalPath(const DTextureCube& Texture,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			if (!Texture.GetPackage())
			{
				OutError = "TextureCube source capture requires an owning package.";
				return false;
			}
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(Texture.GetPackage()->GetPackagePath(),
					EMountPathExistence::AllowMissing);
			if (!Resolved) { OutError = Resolved.Message; return false; }
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return true;
		}

		auto NormalizePanorama(Image::FDecodedImage&& Image)
			-> FTextureCubePanoramaImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height, .SourceChannelCount = Image.SourceChannelCount,
				.bHasTransparency = Image.bHasTransparency};
		}

		auto NormalizePanorama(Image::FDecodedFloatImage&& Image)
			-> FTextureCubePanoramaFloatImage
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
			FAssetImportDataState State;
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
			auto* Data = Texture.GetAssetImportData();
			if (!Data) Data = NewObject<DAssetImportData>(&Texture, "AssetImportData");
			if (!Data || !Data->SetState(std::move(State), OutError)
				|| !Texture.SetAssetImportData(*Data, OutError)) return false;
			Texture.MarkPackageDirty();
			return true;
		}

		auto SaveImportedCube(DTextureCube& Texture, std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			if (!SaveOptions) return true;
			DPackage* Package = Texture.GetPackage();
			const FAssetResult Saved = SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}

		auto RebuildPanorama(DTextureCube& Texture, std::string_view FilePath,
			const FTextureCubePanoramaImportSettings& Settings, std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions) -> bool
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
			if (!std::visit([&](auto&& Decoded) {
					return BuildTextureCubeSynchronously(Texture, {
						.Input = FTextureCubePanoramaBuildInput{
							.Image = std::move(Decoded), .Settings = Settings}}, {}, OutError);
				}, std::move(Panorama))
				|| !PublishCubeImportData(Texture, std::span(&Source, 1),
					ETextureCubeSourceLayout::EquirectangularPanorama, OutError)) return false;
			return SaveImportedCube(Texture, OutError, SaveOptions);
		}

		auto RebuildFaces(DTextureCube& Texture,
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const FTextureCubeImportSettings& Settings, std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			std::array<FCapturedCubeSource, TextureCubeFaceCount> Sources;
			std::array<std::span<const std::byte>, TextureCubeFaceCount> Encoded;
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
			}
			FTextureCubeSourceData SourceData;
			FTextureCubeImportedData ImportedData;
			if (!TranslateTextureCubeFaceSources(Encoded, SourceData, OutError))
				return false;
			if (!ImportedData.SetSourceData(SourceData))
			{
				OutError = "TextureCube canonical imported faces are invalid.";
				return false;
			}
			if (!BuildTextureCubeSynchronously(Texture, {
					.Input = FTextureCubeFacesBuildInput{
						.ImportedData = std::move(ImportedData),
						.OriginalSourceWidth = SourceData.Faces[0].Width,
						.OriginalSourceHeight = SourceData.Faces[0].Height,
						.Settings = Settings}}, {}, OutError)
				|| !PublishCubeImportData(Texture, Sources,
					ETextureCubeSourceLayout::SixFaces, OutError)) return false;
			return SaveImportedCube(Texture, OutError, SaveOptions);
		}

		auto MakeValidation(const FTextureCubeCanonicalBuildInput& CanonicalInput,
			const FTextureCubeBuildProduct& Product, bool bHDR)
			-> FTextureCubeImportValidation
		{
			return {.bValid = true, .SourceLayout = CanonicalInput.SourceLayout,
				.SourceWidth = CanonicalInput.OriginalSourceWidth,
				.SourceHeight = CanonicalInput.OriginalSourceHeight,
				.Dimension = Product.PlatformData->Faces[0].Mips[0].Width,
				.MipCount = static_cast<uint32>(
					Product.PlatformData->Faces[0].Mips.size()),
				.PixelFormat = Product.PlatformData->PixelFormat, .bHDR = bHDR};
		}
	}

	DTextureCubeFactory::DTextureCubeFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DTextureCube::StaticClass();
		Formats = {"png", "jpg", "jpeg", "bmp", "tga", "hdr"};
	}

	auto DTextureCubeFactory::FactoryCreateFromFile(
		DClass* InClass,
		DObject* InParent,
		FName InName,
		EObjectFlags Flags,
		std::string_view Filename,
		DObject*,
		FFactoryDiagnostics* Diagnostics) const -> DObject*
	{
		auto Failed = [&](std::string Message) -> DObject* {
			if (Diagnostics) Diagnostics->Report(Message);
			return nullptr;
		};
		if (InClass != DTextureCube::StaticClass())
			return Failed("TextureCube factory requires the exact TextureCube class.");
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Failed("TextureCube factory requires an asset package parent.");
		auto* Texture = NewObject<DTextureCube>(InClass, Package, InName, Flags);
		if (!Texture) return Failed("TextureCube object could not be created.");

		std::string Error;
		if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < FaceFiles.size(); ++Index)
				if (FaceFiles[Index].empty())
					return Failed(std::format(
						"{} TextureCube face source is required.", FaceNames[Index]));
			if (!RebuildFaces(*Texture, FaceFiles, FaceSettings, Error, nullptr))
				return Failed(std::move(Error));
			return Texture;
		}
		if (SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
			return Failed("TextureCube factory source layout is unsupported.");
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)
			|| !IsTextureCubePanoramaSourceExtension(
				Input.extension().generic_string()))
			return Failed("TextureCube panorama source is missing or unsupported.");
		if (!RebuildPanorama(
			*Texture, Input.generic_string(), PanoramaSettings, Error, nullptr))
			return Failed(std::move(Error));
		return Texture;
	}

	auto DTextureCubeFactory::GetReimportCapabilities(
		const DObject& Object) const -> FReimportCapabilities
	{
		const auto* Texture = Cast<DTextureCube>(&Object);
		if (!Texture || !Texture->GetPackage())
			return {.Diagnostic = "Only packaged TextureCube assets can be reimported."};
		const DAssetImportData* Data = Texture->GetAssetImportData();
		bool bHasSource = false;
		if (Data && Texture->GetSourceLayout()
			== ETextureCubeSourceLayout::EquirectangularPanorama)
		{
			const FSourceFile* Source = Data->GetSourceData().FindByRole("panorama");
			bHasSource = Source && !Source->Hint.empty();
		}
		else if (Data && Texture->GetSourceLayout() == ETextureCubeSourceLayout::SixFaces)
		{
			bHasSource = std::ranges::all_of(FaceRoles, [&](std::string_view Role) {
				const FSourceFile* Source = Data->GetSourceData().FindByRole(Role);
				return Source && !Source->Hint.empty();
			});
		}
		return {.bCanReimport = bHasSource, .bCanReimportFromFile = true,
			.Diagnostic = bHasSource ? std::string{}
				: "TextureCube source import data is incomplete."};
	}

	auto DTextureCubeFactory::Reimport(
		DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Texture = Cast<DTextureCube>(&Object);
		std::string Error;
		bool bSucceeded = false;
		if (Texture && Texture->GetSourceLayout()
			== ETextureCubeSourceLayout::EquirectangularPanorama)
		{
			std::filesystem::path OwningPackagePath;
			const DAssetImportData* Data = Texture->GetAssetImportData();
			const FSourceFile* Source = Data
				? Data->GetSourceData().FindByRole("panorama") : nullptr;
			std::string SourcePath;
			if (Source && ResolveOwningPackagePhysicalPath(
				*Texture, OwningPackagePath, Error)
				&& ResolveSourceHint(Source->HintBase, Source->Hint,
					OwningPackagePath.generic_string(), SourcePath, Error))
				bSucceeded = RebuildPanorama(*Texture, SourcePath,
					{.FaceDimension = Texture->GetPanoramaFaceDimension(),
						.ExposureEV = Texture->GetPanoramaExposureEV()}, Error, nullptr);
		}
		else if (Texture && Texture->GetSourceLayout() == ETextureCubeSourceLayout::SixFaces)
		{
			std::filesystem::path OwningPackagePath;
			std::array<std::string, TextureCubeFaceCount> Sources;
			const DAssetImportData* Data = Texture->GetAssetImportData();
			bSucceeded = ResolveOwningPackagePhysicalPath(*Texture, OwningPackagePath, Error);
			for (size_t Index = 0; bSucceeded && Index < Sources.size(); ++Index)
			{
				const FSourceFile* Source = Data
					? Data->GetSourceData().FindByRole(FaceRoles[Index]) : nullptr;
				bSucceeded = Source && ResolveSourceHint(Source->HintBase, Source->Hint,
					OwningPackagePath.generic_string(), Sources[Index], Error);
			}
			if (bSucceeded) bSucceeded = RebuildFaces(
				*Texture, Sources, {.bSRGB = Texture->IsSRGB()}, Error, nullptr);
		}
		if (!bSucceeded && Error.empty()) Error = "TextureCube source import data is incomplete.";
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto DTextureCubeFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const
		-> void
	{
		auto* Texture = Cast<DTextureCube>(&Object);
		std::string Error;
		bool bSucceeded = false;
		if (Texture && Texture->GetSourceLayout()
			== ETextureCubeSourceLayout::EquirectangularPanorama
			&& Filenames.size() == 1 && !Filenames.front().empty())
		{
			const std::filesystem::path Requested =
				std::filesystem::absolute(Filenames.front()).lexically_normal();
			bSucceeded = RebuildPanorama(*Texture, Requested.generic_string(),
				{.FaceDimension = Texture->GetPanoramaFaceDimension(),
					.ExposureEV = Texture->GetPanoramaExposureEV()}, Error, nullptr);
		}
		else if (Texture && Texture->GetSourceLayout() == ETextureCubeSourceLayout::SixFaces
			&& Filenames.size() == TextureCubeFaceCount)
		{
			std::array<std::string, TextureCubeFaceCount> Sources;
			for (size_t Index = 0; Index < Sources.size(); ++Index)
				Sources[Index] = std::filesystem::absolute(Filenames[Index])
					.lexically_normal().generic_string();
			bSucceeded = RebuildFaces(
				*Texture, Sources, {.bSRGB = Texture->IsSRGB()}, Error, nullptr);
		}
		else Error = "TextureCube reimport sources do not match its source layout.";
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
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
				{.MaximumDecodedPixels = MaximumTextureCubePanoramaPixels}))
				return false;
			OutSource = NormalizePanorama(std::move(Panorama));
			return true;
		}
		Image::FDecodedImage Panorama;
		if (!Image::DecodeImageFromMemory(EncodedBytes, Panorama, OutError,
			{.MaximumDecodedPixels = MaximumTextureCubePanoramaPixels}))
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
		std::array<FByteArray, TextureCubeFaceCount> Bytes;
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
		}
		if (!TranslateTextureCubeFaceSources(EncodedFaces, SourceData, Error))
			return {false, std::move(Error)};
		FTextureCubeImportedData ImportedData;
		if (!ImportedData.SetSourceData(SourceData))
			return {false, "TextureCube canonical imported faces are invalid."};
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
		if (!InvokeTextureCubeBuildProvider({.Input = FTextureCubeFacesBuildInput{
			.ImportedData = std::move(ImportedData),
			.OriginalSourceWidth = SourceData.Faces[0].Width,
			.OriginalSourceHeight = SourceData.Faces[0].Height,
			.Settings = Settings}}, CanonicalInput, Product, Error))
			return {false, std::move(Error)};
		return MakeValidation(CanonicalInput, Product, false);
	}

	auto ValidateTextureCubePanorama(
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportValidation
	{
		if (!IsTextureCubePanoramaSourceExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string()))
			return {false, "Panorama source format is unsupported."};
		FByteArray Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PanoramaFile))
			return {false, "Panorama source is unavailable."};
		std::string Error;
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
		const bool bHDR = Image::IsRadianceHDRExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string());
		FTextureCubePanoramaSourceData Panorama;
		if (!TranslateTextureCubePanoramaSource(Bytes,
			std::filesystem::path(PanoramaFile).extension().generic_string(), Panorama, Error)
			|| !std::visit([&](auto&& Source) {
				return InvokeTextureCubeBuildProvider({
					.Input = FTextureCubePanoramaBuildInput{
						.Image = std::move(Source), .Settings = Settings}},
					CanonicalInput, Product, Error);
			}, std::move(Panorama))) return {false, std::move(Error)};
		return MakeValidation(CanonicalInput, Product, bHDR);
	}

	auto ReimportTextureCubePanorama(
		DTextureCube& Texture,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		std::filesystem::path OwningPackagePath;
		if (!ResolveOwningPackagePhysicalPath(Texture, OwningPackagePath, OutError))
			return false;
		const auto* Data = Texture.GetAssetImportData();
		const FSourceFile* Source = Data
			? Data->GetSourceData().FindByRole("panorama") : nullptr;
		std::string SourcePath;
		if (!Source || !ResolveSourceHint(Source->HintBase, Source->Hint,
			OwningPackagePath.generic_string(), SourcePath, OutError)) return false;
		const FAssetBundleSaveOptions SaveOptions;
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
		const FAssetBundleSaveOptions SaveOptions;
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
		const auto* Data = Texture.GetAssetImportData();
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
		const FAssetBundleSaveOptions SaveOptions;
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
		const FAssetBundleSaveOptions SaveOptions;
		return RebuildFaces(Texture, Sources, Settings, OutError, &SaveOptions);
	}
}

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
			const auto Resolved =
				FMountPaths::ResolveAssetPath(Texture.GetPackage()->GetPackagePath(),
					EMountPathExistence::AllowMissing);
			if (!Resolved) { OutError = (Resolved ? std::string{} : Durin::ToString(Resolved.error())); return false; }
			OutPath = Resolved->PhysicalPath;
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
			if (!std::filesystem::is_regular_file(OutSource.PhysicalPath)) return false;
			if (auto Hint = MakeSourceHint(OutSource.PhysicalPath.generic_string(), OwningPackagePath.generic_string()); !Hint)
			{ OutError = FormatSourceHintError(Hint.error()); return false; }
			else { OutSource.HintBase = Hint->Base; OutSource.Filename = std::move(Hint->Hint); }
			auto Captured = CaptureEncodedSource(OutSource.Filename, OutSource.PhysicalPath,
				MaximumTextureCubeEncodedBytes);
			if (!Captured) { OutError = FormatEncodedSourceError(Captured.error()); return false; }
			OutSource.Snapshot = std::move(*Captured);
			OutError.clear();
			return true;
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
			State.SourceData.Normalize();
			if (const auto Validation = State.Validate(); !Validation)
			{ OutError = FormatAssetImportDataError(Validation.error()); return false; }
			if (!Data) { OutError = "Could not allocate asset import data."; return false; }
			Data->SetState(std::move(State));
			Texture.SetAssetImportData(*Data);
			Texture.MarkPackageDirty();
			return true;
		}

		auto SaveImportedCube(DTextureCube& Texture, std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			if (!SaveOptions) return true;
			DPackage* Package = Texture.GetPackage();
			const FAssetWriteResult Saved = SavePackages(
				std::span<DPackage* const>(&Package, 1), *SaveOptions).Result;
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
					auto BuildResult = BuildTextureCubeSynchronously(Texture, {.Input = FTextureCubePanoramaBuildInput{.Image = std::move(Decoded), .Settings = Settings}}, {});
					OutError = BuildResult.Diagnostic;
					return static_cast<bool>(BuildResult);
				},
							std::move(Panorama))
				|| !PublishCubeImportData(Texture, std::span(&Source, 1), ETextureCubeSourceLayout::EquirectangularPanorama, OutError)) return false;
			return SaveImportedCube(Texture, OutError, SaveOptions);
		}

		auto RebuildFaces(DTextureCube& Texture,
			const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
			const FTextureCubeImportSettings& Settings, std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions) -> bool
		{
			std::array<FCapturedCubeSource, TextureCubeFaceCount> Sources;
			std::array<FByteView, TextureCubeFaceCount> Encoded;
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
			FTextureCubeDecodedFaces SourceData;
			if (!TranslateTextureCubeFaceSources(Encoded, SourceData, OutError))
				return false;
			auto BuildResult = BuildTextureCubeSynchronously(Texture, {.Input = FTextureCubeFacesBuildInput{.DecodedFaces = SourceData, .OriginalSourceWidth = SourceData.Faces[0].GetInfo().Width, .OriginalSourceHeight = SourceData.Faces[0].GetInfo().Height, .Settings = Settings}}, {});
			OutError = BuildResult.Diagnostic;
			if (!BuildResult
				|| !PublishCubeImportData(Texture, Sources, ETextureCubeSourceLayout::SixFaces, OutError)) return false;
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
		auto Reject = [&](EFactoryError Code, std::string_view SourceRole = {}) -> DObject* {
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = Code,
				.ExpectedClass = DTextureCube::StaticClass()->GetName(),
				.RequestedClass = InClass ? InClass->GetName() : std::string{},
				.Filename = std::string(Filename), .SourceRole = std::string(SourceRole),
				.SourceLayout = static_cast<uint32>(SourceLayout)});
			return nullptr;
		};
		if (InClass != DTextureCube::StaticClass())
			return Reject(EFactoryError::ExactClass);
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Reject(EFactoryError::AssetPackageParent);
		auto* Texture = NewObject<DTextureCube>(InClass, Package, InName, Flags);
		if (!Texture) return Reject(EFactoryError::ObjectCreation);

		std::string Error;
		if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < FaceFiles.size(); ++Index)
				if (FaceFiles[Index].empty())
					return Reject(EFactoryError::SourceRoleMissing, FaceNames[Index]);
			if (!RebuildFaces(*Texture, FaceFiles, FaceSettings, Error, nullptr))
				return Failed(std::move(Error));
			return Texture;
		}
		if (SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
			return Reject(EFactoryError::SourceLayout);
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Reject(EFactoryError::SourceMissing);
		if (!IsTextureCubePanoramaSourceExtension(
				Input.extension().generic_string()))
			return Reject(EFactoryError::SourceFormat);
		if (!RebuildPanorama(
			*Texture, Input.generic_string(), PanoramaSettings, Error, nullptr))
			return Failed(std::move(Error));
		return Texture;
	}

	auto DTextureCubeFactory::QueryReimportActions(std::string_view AssetClassName) const
		-> FReimportActions
	{
		if (AssetClassName != DTextureCube::StaticClass()->GetQualifiedName().ToString())
			return {};
		return {.bSupportsReimport = true, .bSupportsReimportFromFile = true};
	}

	auto DTextureCubeFactory::GetSourceFileDialogs(const DObject& Object) const
		-> std::vector<FReimportSourceFileDialog>
	{
		const auto* Cube = Cast<DTextureCube>(&Object);
		if (!Cube) return {};
		const std::string Pattern = "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr";
		if (Cube->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama)
			return {{"Reimport TextureCube Panorama From File", "Supported Images", Pattern}};
		std::vector<FReimportSourceFileDialog> Dialogs;
		for (const auto Face : FaceNames)
			Dialogs.push_back({std::format("Reimport TextureCube {} Face From File", Face),
				"Supported Images", Pattern});
		return Dialogs;
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
			if (Source && ResolveOwningPackagePhysicalPath(*Texture, OwningPackagePath, Error))
			{
				auto Resolved = ResolveSourceHint(Source->HintBase, Source->Hint, OwningPackagePath.generic_string());
				if (Resolved) { SourcePath = Resolved->generic_string(); }
				if (!Resolved) Error = FormatSourceHintError(Resolved.error());
				else bSucceeded = RebuildPanorama(*Texture, SourcePath,
					{.FaceDimension = Texture->GetPanoramaFaceDimension(),
						.ExposureEV = Texture->GetPanoramaExposureEV(), .Output = Texture->GetOutput()}, Error, nullptr);
			}
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
				bSucceeded = Source != nullptr;
				if (Source)
				{
					auto Resolved = ResolveSourceHint(Source->HintBase, Source->Hint, OwningPackagePath.generic_string());
					if (Resolved) { Sources[Index] = Resolved->generic_string(); }
					bSucceeded = Resolved.has_value();
					Error = FormatSourceHintError(Resolved.error());
				}
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
					.ExposureEV = Texture->GetPanoramaExposureEV(), .Output = Texture->GetOutput()}, Error, nullptr);
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
		FByteView EncodedBytes,
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
			auto Panorama = Image::DecodeRadianceHDRFromMemory(EncodedBytes,
				{.MaximumDecodedPixels = MaximumTextureCubePanoramaPixels});
			if (!Panorama)
			{
				OutError = Image::ToString(Panorama.error());
				return false;
			}
			OutError.clear();
			OutSource = NormalizePanorama(std::move(*Panorama));
			return true;
		}
		auto DecodeResult = Image::DecodeImageFromMemory(EncodedBytes, {.MaximumDecodedPixels = MaximumTextureCubePanoramaPixels});
		OutError = DecodeResult ? std::string{} : Image::ToString(DecodeResult.error());
		if (!DecodeResult)
			return false;
		auto Panorama = std::move(*DecodeResult);
		OutSource = NormalizePanorama(std::move(Panorama));
		return true;
	}

	auto TranslateTextureCubeFaceSources(
		const std::array<FByteView, TextureCubeFaceCount>& EncodedFaces,
		FTextureCubeDecodedFaces& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			auto DecodeResult = Image::DecodeImageFromMemory(EncodedFaces[Index], {.MaximumDecodedPixels = 16384ull * 16384ull});
			OutError = DecodeResult ? std::string{} : Image::ToString(DecodeResult.error());
			if (!DecodeResult)
			{
				OutError = std::format("{} TextureCube face decode failed: {}", FaceNames[Index], OutError);
				OutSource = {};
				return false;
			}
			auto ImageResult1 = Image::FImage::TryCreate({.Width = DecodeResult->Width, .Height = DecodeResult->Height,
					.Format = Image::ERawImageFormat::RGBA8}, std::move(DecodeResult->Pixels));
			OutError = ImageResult1 ? std::string{} : ImageResult1.error().ToString();
			if (!ImageResult1)
			{
				OutError = std::format("{} TextureCube face decode failed: {}",
					FaceNames[Index], OutError);
				OutSource = {};
				return false;
			}
			OutSource.Faces[Index] = std::move(*ImageResult1);
			if (DecodeResult->Width > 16384 || DecodeResult->Height > 16384)
			{
				OutError = std::format("{} TextureCube face dimensions {}x{} exceed the 16384 pixel limit.",
					FaceNames[Index], DecodeResult->Width, DecodeResult->Height);
				OutSource = {};
				return false;
			}
			OutSource.SourceChannelCounts[Index] = DecodeResult->SourceChannelCount;
			if (DecodeResult->bHasTransparency)
				OutSource.TransparencyMask |= static_cast<uint8>(1u << Index);
		}
		return true;
	}

	auto ValidateTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings) -> FTextureCubeImportValidation
	{
		FTextureCubeDecodedFaces SourceData;
		std::array<FByteBuffer, TextureCubeFaceCount> Bytes;
		std::array<FByteView, TextureCubeFaceCount> EncodedFaces;
		std::string Error;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!IsTextureCubeFaceSourceExtension(
				std::filesystem::path(FaceFiles[Index]).extension().generic_string()))
				return {false, std::format("{} face source format is unsupported.", FaceNames[Index])};
			auto Loaded = FFileHelper::LoadFileToArray(FaceFiles[Index]);
			if (!Loaded)
				return {false, std::format("{} face read failed: {}", FaceNames[Index], Loaded.error().ToString())};
			Bytes[Index] = std::move(*Loaded);
			EncodedFaces[Index] = Bytes[Index];
		}
		if (!TranslateTextureCubeFaceSources(EncodedFaces, SourceData, Error))
			return {false, std::move(Error)};
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
		auto BuildResult = InvokeTextureCubeBuildProvider({.Input = FTextureCubeFacesBuildInput{.DecodedFaces = SourceData, .OriginalSourceWidth = SourceData.Faces[0].GetInfo().Width, .OriginalSourceHeight = SourceData.Faces[0].GetInfo().Height, .Settings = Settings}});
		Error = BuildResult.Outcome.Diagnostic;
		CanonicalInput = BuildResult ? std::move(BuildResult.Value->CanonicalInput) : Durin::FTextureCubeCanonicalBuildInput{};
		Product = BuildResult ? std::move(BuildResult.Value->Product) : Durin::FTextureCubeBuildProduct{};
		if (!BuildResult)
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
		auto Loaded = FFileHelper::LoadFileToArray(PanoramaFile);
		if (!Loaded) return {false, std::format("Panorama source read failed: {}", Loaded.error().ToString())};
		FByteBuffer Bytes = std::move(*Loaded);
		std::string Error;
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubeBuildProduct Product;
		const bool bHDR = Image::IsRadianceHDRExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string());
		FTextureCubePanoramaSourceData Panorama;
		if (!TranslateTextureCubePanoramaSource(Bytes, std::filesystem::path(PanoramaFile).extension().generic_string(), Panorama, Error)
			|| !std::visit([&](auto&& Source) {
				   auto BuildResult = InvokeTextureCubeBuildProvider({.Input = FTextureCubePanoramaBuildInput{.Image = std::move(Source), .Settings = Settings}});
				   Error = BuildResult.Outcome.Diagnostic;
				   CanonicalInput = BuildResult ? std::move(BuildResult.Value->CanonicalInput) : Durin::FTextureCubeCanonicalBuildInput{};
				   Product = BuildResult ? std::move(BuildResult.Value->Product) : Durin::FTextureCubeBuildProduct{};
				   return static_cast<bool>(BuildResult);
			   },
						   std::move(Panorama))) return {false, std::move(Error)};
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
		if (!Source) return false;
		if (auto Resolved = ResolveSourceHint(Source->HintBase, Source->Hint, OwningPackagePath.generic_string()); !Resolved)
		{ OutError = FormatSourceHintError(Resolved.error()); return false; }
		else { SourcePath = Resolved->generic_string(); }
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
			if (!Source)
			{
				if (OutError.empty()) OutError = "TextureCube face import data is incomplete.";
				return false;
			}
			if (auto Resolved = ResolveSourceHint(Source->HintBase, Source->Hint, OwningPackagePath.generic_string()); !Resolved)
			{ OutError = FormatSourceHintError(Resolved.error()); return false; }
			else { Sources[Index] = Resolved->generic_string(); }
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

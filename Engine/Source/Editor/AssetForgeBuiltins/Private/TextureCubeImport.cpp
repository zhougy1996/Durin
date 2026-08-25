#include "AssetForge/Builtins/TextureCubeImport.h"

#include "AssetAuthoring.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Texture/TextureCubeBuilder.h"
#include "TextureCubeBuildAdapter.h"
#include "AssetForge/ImportService.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
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

		auto NormalizePanorama(Image::FDecodedImage&& Image)
			-> Asset::Build::TextureCubeBuilder::FTexturePanoramaImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height, .SourceChannelCount = Image.SourceChannelCount,
				.bHasTransparency = Image.bHasTransparency};
		}

		auto NormalizePanorama(Image::FDecodedFloatImage&& Image)
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
			Asset::FMountedSourceResolution& OutSource,
			std::string_view PackagePath,
			std::string& OutError) -> bool
		{
			return Asset::ResolveMountedSourceReference(
				PackagePath, SourcePath.Path,
				Asset::EMountedSourceExistencePolicy::RequireFile, OutSource, OutError);
		}

		template<typename TMountedSource>
		auto BuildPanorama(
			DTextureCube& Texture,
			const TMountedSource& Source,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool
		{
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(
				Source.SourcePath, Source.PhysicalPath, Snapshot, OutError)) return false;
			FTextureCubePanoramaSourceData Panorama;
			if (!TranslateTextureCubePanoramaSource(
				Snapshot.GetBytes(), Snapshot.PhysicalPath.extension().generic_string(),
				Panorama, OutError))
			{
				OutError = std::format("TextureCube panorama decode failed: {}", OutError);
				return false;
			}
			return BuildAndPublishTextureCubePanorama(Texture, std::move(Panorama),
				Snapshot.ContentHash, Snapshot.SourcePath, Settings, OutError);
		}

		template <typename TSource>
		auto BuildFaces(
			DTextureCube& Texture,
			const std::array<TSource, TextureCubeFaceCount>& Sources,
			const FTextureCubeImportSettings& Settings,
			std::string& OutError) -> bool
		{
			FTextureCubeSourceData SourceData;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			std::array<FSourcePath, TextureCubeFaceCount> Paths;
			std::array<FEncodedSourceSnapshot, TextureCubeFaceCount> Snapshots;
			std::array<std::span<const std::byte>, TextureCubeFaceCount> EncodedFaces;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				if (!CaptureEncodedSource(
					Sources[Index].SourcePath, Sources[Index].PhysicalPath,
					Snapshots[Index], OutError))
				{
					OutError = std::format("Failed to capture {} TextureCube face: {}",
						FaceNames[Index], OutError);
					return false;
				}
				EncodedFaces[Index] = Snapshots[Index].GetBytes();
				Hashes[Index] = Snapshots[Index].ContentHash;
				Paths[Index] = Snapshots[Index].SourcePath;
			}
			if (!TranslateTextureCubeFaceSources(EncodedFaces, SourceData, OutError)) return false;
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
				{.MaximumDecodedPixels = Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels}))
				return false;
			OutSource = NormalizePanorama(std::move(Panorama));
			return true;
		}
		Image::FDecodedImage Panorama;
		if (!Image::DecodeImageFromMemory(EncodedBytes, Panorama, OutError,
			{.MaximumDecodedPixels = Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels}))
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
		if (!IsTextureCubePanoramaSourceExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string()))
			return {false, "Panorama source format is unsupported."};
		std::vector<std::byte> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PanoramaFile))
			return {false, "Panorama source is unavailable."};
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		std::string Error;
		Asset::Build::FTextureCubeBuildProduct Product;
		const bool bHDR = Image::IsRadianceHDRExtension(
			std::filesystem::path(PanoramaFile).extension().generic_string());
		FTextureCubePanoramaSourceData Panorama;
		if (!TranslateTextureCubePanoramaSource(Bytes,
			std::filesystem::path(PanoramaFile).extension().generic_string(), Panorama, Error)
			|| !std::visit([&](auto&& Source) {
				return Asset::Build::BuildTextureCubePanorama(
					std::move(Source), Hash, Settings, Product, Error);
			}, std::move(Panorama))) return {false, std::move(Error)};
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
		if (!std::filesystem::is_regular_file(Input)
			|| !IsTextureCubePanoramaSourceExtension(Input.extension().generic_string()))
			return {false, "Panorama source is unavailable.", nullptr};
		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(ParsedAssetPath, "_panorama",
			Input.extension().generic_string(), SourceDestination,
			StoredSourcePath, Error)) return {false, std::move(Error), nullptr};
		Asset::FScopedMountedSourceFile Source;
		if (!Asset::PrepareMountedSourceFile(Input, ParsedAssetPath.ToString(),
			StoredSourcePath, Source, Error, MutationContext(bEngineAuthoringContext)))
			return {false, std::move(Error), nullptr};
		FImportRequest Request;
		const std::array Mounted{Source.SourcePath};
		if (!MakeTextureCubeImportRequest(Mounted,
			ETextureCubeSourceLayout::EquirectangularPanorama, ParsedAssetPath, {}, Settings,
			EImportMode::Import,
			{.OwnerId = std::format("TextureCube.Import:{}", ParsedAssetPath.ToString())},
			{}, Request, Error)) return {false, std::move(Error), nullptr};
		const FImportResult Imported = GetImportService().RunImportInline(
			std::move(Request), std::format("Import TextureCube {}", ParsedAssetPath.GetAssetName()));
		if (Imported.Outcome.State != EImportOperationState::Succeeded)
			return {false, Imported.Outcome.Diagnostic, nullptr};
		DObject* Object = nullptr;
		(void)Asset::LoadAsset(ParsedAssetPath, Object);
		auto* Texture = Cast<DTextureCube>(Object);
		if (!Texture) return {false, "TextureCube AssetForge published no asset.", nullptr};
		Source.Commit();
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
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		std::array<Asset::FScopedMountedSourceFile, TextureCubeFaceCount> Sources;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const std::filesystem::path Input =
				std::filesystem::absolute(FaceFiles[Index]).lexically_normal();
			if (!std::filesystem::is_regular_file(Input)
				|| !IsTextureCubeFaceSourceExtension(Input.extension().generic_string()))
			{
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
				return {false, std::move(Error), nullptr};
			}
		}
		std::array<FSourcePath, TextureCubeFaceCount> Mounted;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			Mounted[Index] = Sources[Index].SourcePath;
		FImportRequest Request;
		if (!MakeTextureCubeImportRequest(Mounted, ETextureCubeSourceLayout::SixFaces,
			ParsedAssetPath, Settings, {}, EImportMode::Import,
			{.OwnerId = std::format("TextureCube.Import:{}", ParsedAssetPath.ToString())},
			{}, Request, Error)) return {false, std::move(Error), nullptr};
		const FImportResult Imported = GetImportService().RunImportInline(
			std::move(Request), std::format("Import TextureCube {}", ParsedAssetPath.GetAssetName()));
		if (Imported.Outcome.State != EImportOperationState::Succeeded)
			return {false, Imported.Outcome.Diagnostic, nullptr};
		DObject* Object = nullptr;
		(void)Asset::LoadAsset(ParsedAssetPath, Object);
		auto* Texture = Cast<DTextureCube>(Object);
		if (!Texture) return {false, "TextureCube AssetForge published no asset.", nullptr};
		for (auto& Source : Sources) Source.Commit();
		return {true, {}, Texture};
	}

	auto SubmitTextureCubeImport(std::span<const std::string> SourceFiles,
		std::span<const std::string> SourceDestinations, ETextureCubeSourceLayout Layout,
		const FAssetPath& Destination, const FTextureCubeImportSettings& FaceSettings,
		const FTextureCubePanoramaImportSettings& PanoramaSettings,
		bool bEngineAuthoringContext, FImportCompletion Completion,
		std::string& OutError) -> FImportHandle
	{
		const size_t Required = Layout == ETextureCubeSourceLayout::SixFaces
			? TextureCubeFaceCount : 1;
		if (SourceFiles.size() != Required
			|| (!SourceDestinations.empty() && SourceDestinations.size() != Required))
		{
			OutError = "TextureCube source set is incomplete.";
			return {};
		}
		auto Mounted = std::make_shared<std::vector<FScopedMountedSourceFile>>(Required);
		std::vector<FSourcePath> MountedPaths(Required);
		for (size_t Index = 0; Index < Required; ++Index)
		{
			const std::filesystem::path Input =
				std::filesystem::absolute(SourceFiles[Index]).lexically_normal();
			if (!std::filesystem::is_regular_file(Input)
				|| (Layout == ETextureCubeSourceLayout::SixFaces
					? !IsTextureCubeFaceSourceExtension(Input.extension().generic_string())
					: !IsTextureCubePanoramaSourceExtension(Input.extension().generic_string())))
			{
				OutError = "TextureCube source is unavailable or unsupported.";
				return {};
			}
			const std::string_view RequestedDestination = SourceDestinations.empty()
				? std::string_view{} : std::string_view(SourceDestinations[Index]);
			const std::string Suffix = Layout == ETextureCubeSourceLayout::SixFaces
				? std::format("_{}", FaceSuffixes[Index]) : "_panorama";
			std::string StoredSourcePath;
			if (!MakeCanonicalSourceLocation(Destination, Suffix,
				Input.extension().generic_string(), RequestedDestination,
				StoredSourcePath, OutError)
				|| !PrepareMountedSourceFile(Input, Destination.ToString(), StoredSourcePath,
					(*Mounted)[Index], OutError, MutationContext(bEngineAuthoringContext))) return {};
			MountedPaths[Index] = (*Mounted)[Index].SourcePath;
		}
		FImportRequest Request;
		if (!MakeTextureCubeImportRequest(MountedPaths, Layout, Destination,
			FaceSettings, PanoramaSettings, EImportMode::Import,
			{.OwnerId = std::format("TextureCube.Import:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}}, {}, Request, OutError)) return {};
		OutError.clear();
		return GetImportService().SubmitImport(std::move(Request),
			std::format("Import TextureCube {}", Destination.GetAssetName()),
			[Mounted, Completion = std::move(Completion)](const FImportResult& Result) {
				if (Result.Outcome.State == EImportOperationState::Succeeded)
					for (FScopedMountedSourceFile& Source : *Mounted) Source.Commit();
				if (Completion) Completion(Result);
			});
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
		Asset::FMountedSourceResolution Source;
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
		FAssetPath Destination;
		FImportProvenance Existing;
		if (!FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError)
			|| !InspectTextureCubeImportProvenance(Texture, Existing, OutError)) return false;
		const std::array Mounted{Source.SourcePath};
		FImportRequest Request;
		if (!MakeTextureCubeImportRequest(Mounted,
			ETextureCubeSourceLayout::EquirectangularPanorama, Destination, {}, Settings,
			EImportMode::Reimport,
			{.OwnerId = std::format("TextureCube.Reimport:{}", Destination.ToString())},
			Existing, Request, OutError)) return false;
		const FImportResult Result = GetImportService().RunImportInline(
			std::move(Request), std::format("Reimport TextureCube {}", Destination.GetAssetName()));
		if (Result.Outcome.State == EImportOperationState::Succeeded) return true;
		OutError = Result.Outcome.Diagnostic;
		return false;
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
		std::array<Asset::FMountedSourceResolution, TextureCubeFaceCount> Sources;
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
		FAssetPath Destination;
		FImportProvenance Existing;
		if (!FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError)
			|| !InspectTextureCubeImportProvenance(Texture, Existing, OutError)) return false;
		std::array<FSourcePath, TextureCubeFaceCount> Mounted;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			Mounted[Index] = Sources[Index].SourcePath;
		FImportRequest Request;
		if (!MakeTextureCubeImportRequest(Mounted, ETextureCubeSourceLayout::SixFaces,
			Destination, Settings, {}, EImportMode::Reimport,
			{.OwnerId = std::format("TextureCube.Reimport:{}", Destination.ToString())},
			Existing, Request, OutError)) return false;
		const FImportResult Result = GetImportService().RunImportInline(
			std::move(Request), std::format("Reimport TextureCube {}", Destination.GetAssetName()));
		if (Result.Outcome.State == EImportOperationState::Succeeded) return true;
		OutError = Result.Outcome.Diagnostic;
		return false;
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
		Asset::FMountedSourceResolution Source;
		if (!Asset::ResolveMountedSourceReference(Texture.GetPackage()->GetPackagePath(),
			SourceVirtualPath, Asset::EMountedSourceExistencePolicy::RequireFile,
			Source, OutError)) return false;
		FAssetPath Destination;
		if (!FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
			return false;
		std::optional<FImportProvenance> Existing;
		FImportProvenance Persisted;
		std::string ProvenanceError;
		if (InspectTextureCubeImportProvenance(Texture, Persisted, ProvenanceError))
			Existing = std::move(Persisted);
		const std::array Mounted{Source.SourcePath};
		FImportRequest Request;
		if (!MakeTextureCubeImportRequest(Mounted,
			ETextureCubeSourceLayout::EquirectangularPanorama, Destination, {}, Settings,
			EImportMode::ReplaceSource,
			{.OwnerId = std::format("TextureCube.ReplaceSource:{}", Destination.ToString())},
			std::move(Existing), Request, OutError)) return false;
		const FImportResult Result = GetImportService().RunImportInline(
			std::move(Request), std::format("Replace TextureCube source {}", Destination.GetAssetName()));
		if (Result.Outcome.State == EImportOperationState::Succeeded) return true;
		OutError = Result.Outcome.Diagnostic;
		return false;
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
		std::array<Asset::FMountedSourceResolution, TextureCubeFaceCount> Sources;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			if (!Asset::ResolveMountedSourceReference(
				Texture.GetPackage()->GetPackagePath(), SourceVirtualPaths[Index],
				Asset::EMountedSourceExistencePolicy::RequireFile,
				Sources[Index], OutError)) return false;
		FAssetPath Destination;
		if (!FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
			return false;
		std::optional<FImportProvenance> Existing;
		FImportProvenance Persisted;
		std::string ProvenanceError;
		if (InspectTextureCubeImportProvenance(Texture, Persisted, ProvenanceError))
			Existing = std::move(Persisted);
		std::array<FSourcePath, TextureCubeFaceCount> Mounted;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			Mounted[Index] = Sources[Index].SourcePath;
		FImportRequest Request;
		if (!MakeTextureCubeImportRequest(Mounted, ETextureCubeSourceLayout::SixFaces,
			Destination, Settings, {}, EImportMode::ReplaceSource,
			{.OwnerId = std::format("TextureCube.ReplaceSource:{}", Destination.ToString())},
			std::move(Existing), Request, OutError)) return false;
		const FImportResult Result = GetImportService().RunImportInline(
			std::move(Request), std::format("Replace TextureCube sources {}", Destination.GetAssetName()));
		if (Result.Outcome.State == EImportOperationState::Succeeded) return true;
		OutError = Result.Outcome.Diagnostic;
		return false;
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
		Asset::FScopedMountedSourceFile Source;
		if (!Asset::PrepareMountedSourceFile(FilePath,
			Texture.GetPackage()->GetPackagePath(), TargetSourceVirtualPath,
			Source, OutError)) return false;
		const bool bChanged = ChangeTextureCubePanoramaSourceReference(
			Texture, Source.SourcePath.Path, Settings, OutError);
		if (bChanged) Source.Commit();
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
		std::array<Asset::FScopedMountedSourceFile, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> VirtualPaths;
		for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!Asset::PrepareMountedSourceFile(FaceFiles[Index],
				Texture.GetPackage()->GetPackagePath(), TargetSourceVirtualPaths[Index],
				Sources[Index], OutError))
			{
				return false;
			}
			VirtualPaths[Index] = Sources[Index].SourcePath.Path;
		}
		const bool bChanged = ChangeTextureCubeFaceSourceReferences(
			Texture, VirtualPaths, Settings, OutError);
		for (auto& Source : Sources)
		{
			if (bChanged) Source.Commit();
		}
		return bChanged;
	}
}

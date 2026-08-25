#include "AssetForge/Builtins/TerrainHeightmapImport.h"

#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "AssetAuthoring.h"
#include "Misc/Paths.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "TerrainHeightmapBuildAdapter.h"
#include "AssetForge/ImportService.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		constexpr std::string_view DefaultHeightmapSourceRoot = "TerrainHeightmaps";
		constexpr std::string_view Png16DecoderId = "DurinImage.Png16";
		constexpr std::string_view Raw16DecoderId = "DurinTerrainRaw16";
		constexpr uint32 TerrainSourceProfileVersion = 1;

		auto NormalizeExtension(std::string_view Extension) -> std::string
		{
			std::string Result(Extension);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Result;
		}

		auto IsSupportedHeightmapExtension(std::string_view Extension) -> bool
		{
			return Extension == ".png" || Extension == ".raw";
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view RequestedSourcePath,
			std::string_view SourceExtension,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPath.ToString());
			if (!Lookup)
			{
				OutError = std::format("Terrain heightmap asset {} is outside a registered mount.",
					AssetPath.ToString());
				return false;
			}
			std::filesystem::path Relative = RequestedSourcePath.empty()
				? std::filesystem::path(DefaultHeightmapSourceRoot)
					/ (std::string(AssetPath.GetAssetName()) + std::string(SourceExtension))
				: std::filesystem::path(RequestedSourcePath);
			if (RequestedSourcePath.starts_with('/'))
			{
				const PathUtilities::FSourcePathResult Requested = PathUtilities::ResolveSourcePath(
					RequestedSourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Requested || Requested.Mount != Lookup.Mount)
				{
					OutError = Requested ? "Heightmap source must remain in the asset mount."
						: Requested.Message;
					return false;
				}
				Relative = Requested.RelativePath;
			}
			Relative = Relative.lexically_normal();
			std::string Extension = Relative.extension().generic_string();
			Extension = NormalizeExtension(Extension);
			const std::string RelativeText = Relative.generic_string();
			if (Relative.empty() || Relative.is_absolute() || RelativeText == ".."
				|| RelativeText.starts_with("../") || Extension != SourceExtension)
			{
				OutError = std::format(
					"Heightmap source destination must be a normalized mount-relative {} path matching the source format.",
					SourceExtension);
				return false;
			}
			OutStoredPath = Lookup.Mount->VirtualRoot + RelativeText;
			return true;
		}

		template<typename TMountedSource>
		auto BuildFromMountedSource(
			DTerrainHeightmap& Heightmap,
			const TMountedSource& Source,
			std::string& OutError) -> bool
		{
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(
				Source.SourcePath, Source.PhysicalPath, Snapshot, OutError,
				MaximumTerrainHeightmapEncodedBytes)) return false;
			FTerrainHeightmapSourceData SourceData;
			if (!TranslateTerrainHeightmapSource(
				Source.PhysicalPath.extension().generic_string(), Snapshot.GetBytes(),
				SourceData, OutError)) return false;
			const std::shared_ptr<const FTerrainHeightmapPayload> ExistingPayload = Heightmap.GetPayload();
			const bool bSamplesChanged = !ExistingPayload
				|| ExistingPayload->Samples != SourceData.Samples;
			return BuildTerrainHeightmapFromSource(
				Heightmap, std::move(SourceData), Snapshot, OutError, bSamplesChanged);
		}
	}

	auto IsTerrainHeightmapSourceExtension(std::string_view Extension) -> bool
	{
		return IsSupportedHeightmapExtension(NormalizeExtension(Extension));
	}

	auto TranslateTerrainHeightmapSource(
		std::string_view Extension,
		std::span<const std::byte> EncodedBytes,
		FTerrainHeightmapSourceData& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		const std::string NormalizedExtension = NormalizeExtension(Extension);
		if (NormalizedExtension == ".png")
		{
			Image::FDecodedGrayscale16Image Decoded;
			if (!Image::DecodeGrayscale16PngFromMemory(EncodedBytes, Decoded, OutError, {
				.MaximumEncodedBytes = MaximumTerrainHeightmapEncodedBytes,
				.MaximumDecodedPixels = MaximumTerrainHeightmapSamples})) return false;
			OutSource = {
				.Samples = std::move(Decoded.Samples),
				.Width = Decoded.Width,
				.Height = Decoded.Height,
				.DecoderId = std::string(Png16DecoderId),
				.DecoderVersion = 1,
				.SourceFormat = ETerrainHeightmapSourceFormat::Png16,
				.SourceProfileVersion = TerrainSourceProfileVersion};
			if (OutSource.IsValid())
			{
				OutError.clear();
				return true;
			}
			OutSource = {};
			OutError = "Decoded terrain heightmap source is invalid.";
			return false;
		}
		if (NormalizedExtension != ".raw")
		{
			OutError = "Terrain heightmap source extension must be .png or .raw.";
			return false;
		}
		if (EncodedBytes.size() > MaximumTerrainHeightmapEncodedBytes)
		{
			OutError = "RAW16 terrain heightmap exceeds the 512 MiB encoded-source limit.";
			return false;
		}
		if (EncodedBytes.size() < 8)
		{
			OutError = "RAW16 terrain heightmap must contain at least four samples (8 bytes).";
			return false;
		}
		if ((EncodedBytes.size() & 1u) != 0)
		{
			OutError = "RAW16 terrain heightmap byte count must be even.";
			return false;
		}
		const uint64 SampleCount = EncodedBytes.size() / sizeof(uint16);
		uint64 Low = 2;
		uint64 High = MaximumTerrainHeightmapDimension;
		uint64 Dimension = 0;
		while (Low <= High)
		{
			const uint64 Middle = Low + (High - Low) / 2;
			const uint64 Square = Middle * Middle;
			if (Square == SampleCount) { Dimension = Middle; break; }
			if (Square < SampleCount) Low = Middle + 1;
			else High = Middle - 1;
		}
		if (Dimension == 0)
		{
			OutError = "RAW16 terrain heightmap sample count must be an exact square within dimensions 2..16384.";
			return false;
		}
		if (Dimension > std::numeric_limits<size_t>::max() / Dimension
			|| Dimension * Dimension != SampleCount)
		{
			OutError = "RAW16 terrain heightmap dimensions overflow checked sample arithmetic.";
			return false;
		}
		OutSource.Samples.resize(static_cast<size_t>(SampleCount));
		for (size_t Index = 0; Index < OutSource.Samples.size(); ++Index)
		{
			const size_t ByteOffset = Index * 2;
			OutSource.Samples[Index] = static_cast<uint16>(
				static_cast<uint16>(EncodedBytes[ByteOffset])
					| (static_cast<uint16>(EncodedBytes[ByteOffset + 1]) << 8));
		}
		OutSource.Width = static_cast<uint32>(Dimension);
		OutSource.Height = static_cast<uint32>(Dimension);
		OutSource.DecoderId = Raw16DecoderId;
		OutSource.DecoderVersion = 1;
		OutSource.SourceFormat = ETerrainHeightmapSourceFormat::Raw16;
		OutSource.SourceProfileVersion = TerrainSourceProfileVersion;
		OutError.clear();
		return true;
	}

	auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings,
		bool bEngineAuthoringContext) -> FTerrainHeightmapImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		std::string Extension = Input.extension().generic_string();
		Extension = NormalizeExtension(Extension);
		if (!std::filesystem::is_regular_file(Input) || !IsTerrainHeightmapSourceExtension(Extension))
			return {false, "Terrain heightmap import requires an existing .png or .raw source.", nullptr};
		FAssetPath ParsedPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedPath)
			|| Asset::FindResidentPackage(ParsedPath))
			return {false, std::format("Asset {} already exists.", ParsedPath.ToString()), nullptr};
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedPath, Settings.SourceDestination, Extension, StoredSourcePath, Error))
			return {false, std::move(Error), nullptr};
		FScopedMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(Input, ParsedPath.ToString(), StoredSourcePath,
			MountedSource, Error,
			bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe))
			return {false, std::move(Error), nullptr};
		FImportRequest Request;
		if (!MakeTerrainHeightmapImportRequest(MountedSource.SourcePath, ParsedPath,
			EImportMode::Import,
			{.OwnerId = std::format("TerrainHeightmap.Import:{}", ParsedPath.ToString())},
			{}, Request, Error)) return {false, std::move(Error), nullptr};
		const FImportResult Imported = GetImportService().RunImportInline(
			std::move(Request), std::format("Import Terrain Heightmap {}", ParsedPath.GetAssetName()));
		if (Imported.Outcome.State != EImportOperationState::Succeeded)
			return {false, Imported.Outcome.Diagnostic, nullptr};
		DObject* Object = nullptr;
		(void)Asset::LoadAsset(ParsedPath, Object);
		auto* Heightmap = Cast<DTerrainHeightmap>(Object);
		if (!Heightmap) return {false, "Terrain AssetForge published no asset.", nullptr};
		MountedSource.Commit();
		return {true, {}, Heightmap};
	}

	auto SubmitTerrainHeightmapImport(std::string_view FilePath,
		const FAssetPath& Destination, std::string_view SourceDestination,
		bool bEngineAuthoringContext, FImportCompletion Completion,
		std::string& OutError) -> FImportHandle
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		const std::string Extension = NormalizeExtension(Input.extension().generic_string());
		if (!std::filesystem::is_regular_file(Input) || !IsTerrainHeightmapSourceExtension(Extension))
		{
			OutError = "Terrain heightmap source is unavailable or unsupported.";
			return {};
		}
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(Destination, SourceDestination, Extension,
			StoredSourcePath, OutError)) return {};
		auto Mounted = std::make_shared<FScopedMountedSourceFile>();
		if (!PrepareMountedSourceFile(Input, Destination.ToString(), StoredSourcePath,
			*Mounted, OutError, bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe)) return {};
		FImportRequest Request;
		if (!MakeTerrainHeightmapImportRequest(Mounted->SourcePath, Destination,
			EImportMode::Import,
			{.OwnerId = std::format("TerrainHeightmap.Import:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}}, {}, Request, OutError)) return {};
		OutError.clear();
		return GetImportService().SubmitImport(std::move(Request),
			std::format("Import Terrain Heightmap {}", Destination.GetAssetName()),
			[Mounted, Completion = std::move(Completion)](const FImportResult& Result) {
				if (Result.Outcome.State == EImportOperationState::Succeeded) Mounted->Commit();
				if (Completion) Completion(Result);
			});
	}

	auto ChangeTerrainHeightmapSourceReference(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!Heightmap.GetPackage())
		{
			OutError = "Terrain heightmap source changes require an owning package.";
			return false;
		}
		FMountedSourceResolution Source;
		if (!ResolveMountedSourceReference(
			Heightmap.GetPackage()->GetPackagePath(), SourceVirtualPath,
			EMountedSourceExistencePolicy::RequireFile, Source, OutError)) return false;
		FAssetPath Destination;
		if (!FAssetPath::TryCreate(Heightmap.GetPackage()->GetPackagePath(), Destination, &OutError))
			return false;
		std::optional<FImportProvenance> Existing;
		FImportProvenance Persisted;
		std::string ProvenanceError;
		if (InspectTerrainHeightmapImportProvenance(Heightmap, Persisted, ProvenanceError))
			Existing = std::move(Persisted);
		FImportRequest Request;
		if (!MakeTerrainHeightmapImportRequest(Source.SourcePath, Destination,
			EImportMode::Repair,
			{.OwnerId = std::format("TerrainHeightmap.Repair:{}", Destination.ToString())},
			std::move(Existing), Request, OutError)) return false;
		const FImportResult Result = GetImportService().RunImportInline(
			std::move(Request), std::format("Repair Terrain Heightmap {}", Destination.GetAssetName()));
		if (Result.Outcome.State == EImportOperationState::Succeeded) return true;
		OutError = Result.Outcome.Diagnostic;
		return false;
	}

	auto ReimportTerrainHeightmapSource(
		DTerrainHeightmap& Heightmap,
		std::string& OutError) -> bool
	{
		return ChangeTerrainHeightmapSourceReference(
			Heightmap, Heightmap.GetSourceImportData().SourcePath.Path, OutError);
	}
}

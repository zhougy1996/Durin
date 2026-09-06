#include "ContentBrowser/TextureCubeDetails.h"

#include "Asset/PackageInspection.h"
#include "Misc/StringHelper.h"
#include "Texture/TextureCube.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		template<typename T>
		auto ReadScalar(
			const FAssetPackageInspection& Inspection,
			std::string_view Name,
			T& OutValue) -> bool
		{
			const FAssetPackageField* Field = Inspection.FindField(Name);
			return Field && Field->TryReadScalar(OutValue);
		}

		auto QuickFileFingerprint(
			std::string_view PhysicalPath,
			uintmax_t& OutSize,
			int64& OutLastWriteTimeTicks) -> bool
		{
			std::error_code Error;
			OutSize = std::filesystem::file_size(PhysicalPath, Error);
			if (Error) return false;
			const std::filesystem::file_time_type LastWrite =
				std::filesystem::last_write_time(PhysicalPath, Error);
			if (Error) return false;
			OutLastWriteTimeTicks = static_cast<int64>(
				LastWrite.time_since_epoch().count());
			return true;
		}
	}

	FTextureCubeDetailsCache::FTextureCubeDetailsCache(FBuilder InBuilder)
		: Builder(InBuilder
			? std::move(InBuilder)
			: FBuilder(BuildTextureCubeDetailsSnapshot))
	{
	}

	auto FTextureCubeDetailsCache::Get(
		std::string_view PhysicalPath,
		uint64 RegistryRevision) -> const FTextureCubeDetailsSnapshot&
	{
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		const bool bFileStatValid = QuickFileFingerprint(
			PhysicalPath, FileSize, LastWriteTimeTicks);
		if (!CachedSnapshot
			|| CachedPhysicalPath != PhysicalPath
			|| CachedRegistryRevision != RegistryRevision
			|| bCachedFileStatValid != bFileStatValid
			|| (bFileStatValid
				&& (CachedFileSize != FileSize
					|| CachedLastWriteTimeTicks != LastWriteTimeTicks)))
		{
			CachedSnapshot = Builder(PhysicalPath);
			CachedPhysicalPath = PhysicalPath;
			CachedRegistryRevision = RegistryRevision;
			CachedFileSize = FileSize;
			CachedLastWriteTimeTicks = LastWriteTimeTicks;
			bCachedFileStatValid = bFileStatValid;
		}
		return *CachedSnapshot;
	}

	auto FTextureCubeDetailsCache::Invalidate() -> void
	{
		CachedSnapshot.reset();
		CachedPhysicalPath.clear();
		CachedRegistryRevision = std::numeric_limits<uint64>::max();
		CachedFileSize = 0;
		CachedLastWriteTimeTicks = 0;
		bCachedFileStatValid = false;
	}

	auto BuildTextureCubeDetailsSnapshot(std::string_view PhysicalPath)
		-> FTextureCubeDetailsSnapshot
	{
		FTextureCubeDetailsSnapshot Snapshot;
		FAssetPackageInspection Inspection;
		const FAssetResult Result =
			InspectAssetPackage(PhysicalPath, Inspection);
		if (!Result)
		{
			Snapshot.BuildDiagnostic = Result.Message.empty()
				? "Package metadata could not be inspected."
				: Result.Message;
			return Snapshot;
		}

		Snapshot.bAvailable = true;
		Snapshot.PackageHashLow = Inspection.Fingerprint.ContentHash.HashLow;
		Snapshot.PackageHashHigh = Inspection.Fingerprint.ContentHash.HashHigh;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		const bool bHasLayout = ReadScalar(Inspection, "SourceLayout", SourceLayout);
		Snapshot.bPanorama = bHasLayout
			&& SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama;
		Snapshot.SourceLayout = !bHasLayout
			? "-"
			: Snapshot.bPanorama
			? "Equirectangular Panorama"
			: "Six Faces";

		FAssetImportInfo ImportInfo;
		std::string ImportError;
		if (InspectAssetImportInfo(
			Inspection, ImportInfo, ImportError))
		{
			if (Snapshot.bPanorama)
			{
				const auto It = std::ranges::find(
					ImportInfo.Sources, "panorama", &FSourceFile::Role);
				Snapshot.Source = It == ImportInfo.Sources.end()
					? "-"
					: It->Hint;
			}
			else
			{
				const size_t SourceCount = ImportInfo.Sources.size();
				Snapshot.Source = SourceCount == 0
					? "-"
					: std::format("{} of {} face sources", SourceCount, TextureCubeFaceCount);
			}
		}

		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		const bool bHasSourceWidth =
			ReadScalar(Inspection, "OriginalSourceWidth", SourceWidth);
		const bool bHasSourceHeight =
			ReadScalar(Inspection, "OriginalSourceHeight", SourceHeight);
		if (bHasSourceWidth && bHasSourceHeight
			&& SourceWidth != 0 && SourceHeight != 0)
			Snapshot.SourceSize = std::format("{}x{}", SourceWidth, SourceHeight);

		uint32 FaceDimension = 0;
		if (ReadScalar(Inspection, "PanoramaFaceDimension", FaceDimension))
			Snapshot.FaceOverride = FaceDimension == 0
				? "Auto"
				: std::format("{} px", FaceDimension);
		float Exposure = 0.0f;
		if (ReadScalar(Inspection, "PanoramaExposureEV", Exposure))
			Snapshot.Exposure = std::format("{:+.1f} EV", Exposure);
		if (Snapshot.bPanorama && Snapshot.Source != "-")
		{
			const std::string Extension = StringUtils::FoldAscii(
				std::filesystem::path(Snapshot.Source).extension().generic_string());
			Snapshot.InputRange = Extension == ".hdr" ? "Radiance HDR" : "LDR";
		}

		FEditorBulkDataStorageDescriptor CookedPayload;
		const FAssetPackageField* CookedField =
			Inspection.FindField("PlatformData");
		if (CookedField
			&& CookedField->TryReadBulkDataStorageDescriptor(CookedPayload)
			&& CookedPayload.StoredByteCount != 0)
			Snapshot.Output = std::format(
				"Cooked payload ({} bytes)", CookedPayload.StoredByteCount);
		Snapshot.BuildDiagnostic =
			"Runtime platform data and build diagnostics are not serialized in this package.";
		return Snapshot;
	}

	auto MakeTextureCubeDetailsProvider() -> ContentBrowser::FDetailsProvider
	{
		return [Cache = std::make_shared<FTextureCubeDetailsCache>()](const ContentBrowser::FExtensionContext& Context) {
			std::vector<ContentBrowser::FDetailRow> Rows;
			if (Context.Selection.size() != 1) return Rows;
			const auto& Details = Cache->Get(Context.Selection.front().PhysicalPath, Context.CatalogRevision);
			if (Details.bAvailable)
			{
				Rows = {{"Source Layout", Details.SourceLayout}, {"Source", Details.Source},
					{"Source Size", Details.SourceSize}};
				if (Details.bPanorama)
				{
					Rows.push_back({"Face Override", Details.FaceOverride});
					Rows.push_back({"Input Range", Details.InputRange});
					Rows.push_back({"Exposure", Details.Exposure});
				}
				Rows.push_back({"Dimensions", Details.Dimensions});
				Rows.push_back({"Faces", Details.Faces});
				Rows.push_back({"Mips", Details.Mips});
				Rows.push_back({"Output", Details.Output});
			}
			Rows.push_back({"Build", Details.BuildDiagnostic});
			return Rows;
		};
	}
}

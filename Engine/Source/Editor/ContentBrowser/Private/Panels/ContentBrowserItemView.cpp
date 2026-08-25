#include "Panels/ContentBrowserItemView.h"

#include "Asset/PackageInspection.h"
#include "Icons/FontAwesomeIcons.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Math/Vector.h"
#include "Texture/TextureCube.h"

namespace Durin::Editor::ContentBrowser::Private::ContentBrowserItemView
{
	auto ClassLeaf(std::string_view QualifiedName) -> std::string
	{
		const size_t Separator = QualifiedName.rfind("::");
		std::string Name = Separator == std::string_view::npos
			? std::string(QualifiedName)
			: std::string(QualifiedName.substr(Separator + 2));
		if (Name.starts_with('D') && Name.size() > 1) Name.erase(Name.begin());
		return Name;
	}

	namespace
	{
		template<typename T>
		auto ReadScalar(
			const Asset::FAssetPackageInspection& Inspection,
			std::string_view Name,
			T& OutValue) -> bool
		{
			const Asset::FAssetPackageField* Field = Inspection.FindField(Name);
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
		Asset::FAssetPackageInspection Inspection;
		const Asset::FAssetResult Result =
			Asset::InspectAssetPackage(PhysicalPath, Inspection);
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

		FTextureCubeSourceImportData ImportData;
		const Asset::FAssetPackageField* ImportField =
			Inspection.FindField("SourceImportData");
		if (ImportField
			&& ImportField->TryReadStruct(
				FTextureCubeSourceImportData::StaticStruct(), &ImportData))
		{
			if (Snapshot.bPanorama)
				Snapshot.Source = ImportData.Panorama.SourcePath.Path.empty()
					? "-"
					: ImportData.Panorama.SourcePath.Path;
			else
			{
				size_t SourceCount = 0;
				for (uint8 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
					if (ImportData.GetFace(
						static_cast<ETextureCubeFace>(FaceIndex)).HasSource())
						++SourceCount;
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
			std::string Extension =
				std::filesystem::path(Snapshot.Source).extension().generic_string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			Snapshot.InputRange = Extension == ".hdr" ? "Radiance HDR" : "LDR";
		}

		Asset::FCookedPayloadDescriptor CookedPayload;
		const Asset::FAssetPackageField* CookedField =
			Inspection.FindField("CookedPayload");
		if (CookedField
			&& CookedField->TryReadStruct(
				Asset::FCookedPayloadDescriptor::StaticStruct(), &CookedPayload)
			&& CookedPayload.StoredSize != 0)
			Snapshot.Output = std::format(
				"Cooked payload ({} bytes)", CookedPayload.StoredSize);
		Snapshot.BuildDiagnostic =
			"Runtime platform data and build diagnostics are not serialized in this package.";
		return Snapshot;
	}

	auto FGridMetrics::FromPreviewExtent(float InPreviewExtent) -> FGridMetrics
	{
		FGridMetrics Metrics;
		Metrics.PreviewExtent = InPreviewExtent;
		Metrics.IconFontSize =
			MonaImGui::QuantizeDynamicFontSize(InPreviewExtent * 0.68f);
		Metrics.NameFontSize = MonaImGui::QuantizeDynamicFontSize(std::clamp(
			InPreviewExtent * 0.18f,
			ImGui::GetFontSize() * 0.78f,
			ImGui::GetFontSize() * 1.20f));
		Metrics.CardInset = MonaImGui::ScaleUI(2.0f);
		Metrics.CardRounding = MonaImGui::ScaleUI(7.0f);
		Metrics.ContentInset = MonaImGui::ScaleUI(7.0f);
		Metrics.NameGap = MonaImGui::ScaleUI(5.0f);
		Metrics.NameAreaHeight =
			Metrics.NameFontSize * 2.0f + MonaImGui::ScaleUI(4.0f);
		Metrics.CellWidth =
			InPreviewExtent + (Metrics.ContentInset + Metrics.CardInset) * 2.0f;
		Metrics.TileHeight = Metrics.CardInset + Metrics.ContentInset
			+ InPreviewExtent + Metrics.NameGap + Metrics.NameAreaHeight
			+ Metrics.ContentInset + Metrics.CardInset;
		Metrics.RowHeight =
			Metrics.TileHeight + ImGui::GetStyle().ItemSpacing.y + 1.0f;
		Metrics.BadgeSize = std::clamp(
			InPreviewExtent * 0.22f,
			MonaImGui::ScaleUI(18.0f),
			MonaImGui::ScaleUI(28.0f));
		Metrics.BadgeInset = MonaImGui::ScaleUI(4.0f);
		return Metrics;
	}

	auto FGridMetrics::CardMin(const ImVec2& TileStart) const -> ImVec2
	{
		return ImVec2(TileStart.x + CardInset, TileStart.y + CardInset);
	}

	auto FGridMetrics::CardMax(const ImVec2& TileStart, const ImVec2& TileSize) const
		-> ImVec2
	{
		return ImVec2(
			TileStart.x + TileSize.x - CardInset,
			TileStart.y + TileSize.y - CardInset);
	}

	auto FGridMetrics::PreviewMin(
		const ImVec2& TileStart,
		const ImVec2& TileSize) const -> ImVec2
	{
		return ImVec2(
			TileStart.x + (TileSize.x - PreviewExtent) * 0.5f,
			TileStart.y + CardInset + ContentInset);
	}

	auto FGridMetrics::PreviewMax(
		const ImVec2& TileStart,
		const ImVec2& TileSize) const -> ImVec2
	{
		const ImVec2 Min = PreviewMin(TileStart, TileSize);
		return ImVec2(Min.x + PreviewExtent, Min.y + PreviewExtent);
	}

	auto FGridMetrics::NameMin(const ImVec2& TileStart, const ImVec2& TileSize) const
		-> ImVec2
	{
		const ImVec2 Preview = PreviewMin(TileStart, TileSize);
		return ImVec2(
			TileStart.x + CardInset + ContentInset,
			Preview.y + PreviewExtent + NameGap);
	}

	auto FGridMetrics::NameMax(const ImVec2& TileStart, const ImVec2& TileSize) const
		-> ImVec2
	{
		const ImVec2 Min = NameMin(TileStart, TileSize);
		return ImVec2(
			TileStart.x + TileSize.x - CardInset - ContentInset,
			Min.y + NameAreaHeight);
	}

	auto ResolveThumbnailPresentation(const ::Durin::Editor::FAssetThumbnailView& Thumbnail)
		-> EThumbnailPresentation
	{
		if (Thumbnail.State == ::Durin::Editor::EAssetThumbnailState::Ready && Thumbnail.Texture)
			return EThumbnailPresentation::Ready;
		if (Thumbnail.State == ::Durin::Editor::EAssetThumbnailState::Queued
			|| Thumbnail.State == ::Durin::Editor::EAssetThumbnailState::Loading
			|| Thumbnail.State == ::Durin::Editor::EAssetThumbnailState::Uploading)
			return EThumbnailPresentation::Loading;
		if (Thumbnail.State == ::Durin::Editor::EAssetThumbnailState::Failed)
			return EThumbnailPresentation::Failed;
		return EThumbnailPresentation::Icon;
	}

	auto TypeLabel(const FContentBrowserItem& Item) -> std::string
	{
		return ContentBrowserModel::TypeLabel(Item);
	}

	auto Icon(const FContentBrowserItem& Item) -> const char*
	{
		if (Item.Kind == EContentBrowserItemKind::Folder) return Icons::Folder;
		if (Item.Kind == EContentBrowserItemKind::File) return Icons::File;
		if (Item.Kind == EContentBrowserItemKind::Redirector)
			return Icons::ArrowRight;
		const std::string Type = TypeLabel(Item);
		if (Type == "StaticMesh" || Type == "Skeletal Mesh" || Type == "Texture Cube") return Icons::Cube;
		if (Type == "Skeleton") return Icons::List;
		if (Type == "Animation Clip") return Icons::Play;
		if (Type == "Level") return Icons::Home;
		return Icons::FileLines;
	}

	auto FormatFileSize(uintmax_t Bytes) -> std::string
	{
		if (Bytes < 1024) return std::format("{} B", Bytes);
		if (Bytes < 1024 * 1024) return std::format("{:.1f} KB", Bytes / 1024.0);
		if (Bytes < 1024ull * 1024ull * 1024ull)
			return std::format("{:.1f} MB", Bytes / (1024.0 * 1024.0));
		return std::format("{:.2f} GB", Bytes / (1024.0 * 1024.0 * 1024.0));
	}

	auto FormatFileTime(const std::filesystem::file_time_type& Time) -> std::string
	{
		if (Time == std::filesystem::file_time_type{}) return "-";
		const auto SysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
			Time - std::filesystem::file_time_type::clock::now()
			+ std::chrono::system_clock::now());
		const std::time_t TimeT = std::chrono::system_clock::to_time_t(SysTime);
		std::tm LocalTime{};
#ifdef _WIN32
		localtime_s(&LocalTime, &TimeT);
#else
		localtime_r(&TimeT, &LocalTime);
#endif
		std::array<char, 32> Buffer{};
		std::strftime(Buffer.data(), Buffer.size(), "%Y-%m-%d %H:%M", &LocalTime);
		return Buffer.data();
	}

	auto DrawTransparencyGrid(
		ImDrawList& DrawList,
		const ImVec2& Min,
		const ImVec2& Size) -> void
	{
		const float CheckerSize = MonaImGui::ScaleUI(7.0f);
		const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);
		DrawList.PushClipRect(Min, Max, true);
		for (float Y = 0.0f; Y < Size.y; Y += CheckerSize)
			for (float X = 0.0f; X < Size.x; X += CheckerSize)
			{
				const bool bLight =
					(static_cast<int32>(X / CheckerSize)
						+ static_cast<int32>(Y / CheckerSize))
					% 2
					== 0;
				const ImU32 Color = ImGui::GetColorU32(
					bLight
						? ImVec4(0.62f, 0.62f, 0.62f, 1.0f)
						: ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
				DrawList.AddRectFilled(
					ImVec2(Min.x + X, Min.y + Y),
					ImVec2(
						Min.x + std::min(X + CheckerSize, Size.x),
						Min.y + std::min(Y + CheckerSize, Size.y)),
					Color);
			}
		DrawList.PopClipRect();
	}

	auto DrawTextureCubeBadge(
		ImDrawList& DrawList,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMax) -> void
	{
		const ImVec2 BadgeMax(
			PreviewMax.x - Metrics.BadgeInset,
			PreviewMax.y - Metrics.BadgeInset);
		const ImVec2 BadgeMin(
			BadgeMax.x - Metrics.BadgeSize,
			BadgeMax.y - Metrics.BadgeSize);
		DrawList.AddRectFilled(
			BadgeMin,
			BadgeMax,
			ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.07f, 0.82f)),
			Metrics.BadgeSize * 0.24f);
		const float IconSize =
			MonaImGui::QuantizeDynamicFontSize(Metrics.BadgeSize * 0.58f);
		const ImVec2 IconExtent =
			ImGui::GetFont()->CalcTextSizeA(IconSize, FLT_MAX, 0.0f, Icons::Cube);
		const ImVec2 IconPosition(
			BadgeMin.x + (Metrics.BadgeSize - IconExtent.x) * 0.5f,
			BadgeMin.y + (Metrics.BadgeSize - IconExtent.y) * 0.5f);
		DrawList.AddText(
			ImGui::GetFont(),
			IconSize,
			IconPosition,
			ImGui::GetColorU32(ImVec4(0.90f, 0.94f, 1.0f, 1.0f)),
			Icons::Cube);
	}

	auto DrawThumbnail(
		const FContentBrowserItem& Item,
		const ::Durin::Editor::FAssetThumbnailView& Thumbnail,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMin,
		const ImVec2& PreviewMax,
		const ImVec2& CursorAfterTile) -> bool
	{
		if (ResolveThumbnailPresentation(Thumbnail) != EThumbnailPresentation::Ready
			|| !Mona::GetActiveUIBackend())
			return false;

		const float Scale = std::min(
			Metrics.PreviewExtent / static_cast<float>(Thumbnail.Width),
			Metrics.PreviewExtent / static_cast<float>(Thumbnail.Height));
		const ImVec2 ImageSize(
			std::max(1.0f, Thumbnail.Width * Scale),
			std::max(1.0f, Thumbnail.Height * Scale));
		const ImVec2 ImagePosition(
			PreviewMin.x + (Metrics.PreviewExtent - ImageSize.x) * 0.5f,
			PreviewMin.y + (Metrics.PreviewExtent - ImageSize.y) * 0.5f);
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(PreviewMin, PreviewMax, true);
		if (Thumbnail.bHasTransparency && Thumbnail.bShowTransparencyGrid)
			DrawTransparencyGrid(*DrawList, ImagePosition, ImageSize);
		ImGui::SetCursorScreenPos(ImagePosition);
		const bool bDrewThumbnail = Mona::GetActiveUIBackend()->DrawImage(
			Thumbnail.Texture, FVector2f(ImageSize.x, ImageSize.y));
		ImGui::SetCursorScreenPos(CursorAfterTile);
		DrawList->PopClipRect();
		if (bDrewThumbnail && Item.Kind == EContentBrowserItemKind::Asset
			&& ContentBrowserModel::TypeLabel(Item) == "Texture Cube")
			DrawTextureCubeBadge(*DrawList, Metrics, PreviewMax);
		return bDrewThumbnail;
	}

	auto DrawFallbackIcon(
		const FContentBrowserItem& Item,
		EThumbnailPresentation ThumbnailPresentation,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMin,
		const ImVec2& PreviewMax) -> void
	{
		const MonaImGui::EUIThemeColor IconColorRole =
			Item.Kind == EContentBrowserItemKind::Folder
			? MonaImGui::EUIThemeColor::Folder
			: Item.Kind == EContentBrowserItemKind::Asset
			? MonaImGui::EUIThemeColor::Asset
			: Item.Kind == EContentBrowserItemKind::Redirector
			? MonaImGui::EUIThemeColor::Warning
			: MonaImGui::EUIThemeColor::SourceFile;
		const ImVec2 IconExtent = ImGui::GetFont()->CalcTextSizeA(
			Metrics.IconFontSize, FLT_MAX, 0.0f, Icon(Item));
		const ImVec2 IconPosition(
			PreviewMin.x + (Metrics.PreviewExtent - IconExtent.x) * 0.5f,
			PreviewMin.y + (Metrics.PreviewExtent - IconExtent.y) * 0.5f);
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->AddText(
			ImGui::GetFont(),
			Metrics.IconFontSize,
			IconPosition,
			MonaImGui::GetThemeColorU32(IconColorRole),
			Icon(Item));
		if (ThumbnailPresentation == EThumbnailPresentation::Loading)
			DrawList->AddText(
				ImVec2(
					PreviewMax.x - MonaImGui::ScaleUI(14.0f),
					PreviewMin.y),
				ImGui::GetColorU32(ImGuiCol_TextDisabled),
				"...");
		else if (ThumbnailPresentation == EThumbnailPresentation::Failed)
			DrawList->AddText(
				ImVec2(
					PreviewMax.x - MonaImGui::ScaleUI(16.0f),
					PreviewMin.y),
				MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Warning),
				Icons::Warning);
	}

	auto DrawLabel(
		const FContentBrowserItem& Item,
		const FGridMetrics& Metrics,
		const ImVec2& NameMin,
		const ImVec2& NameMax) -> void
	{
		const float TextWidth =
			std::max(MonaImGui::ScaleUI(40.0f), NameMax.x - NameMin.x);
		const ImVec2 TextExtent = ImGui::GetFont()->CalcTextSizeA(
			Metrics.NameFontSize,
			FLT_MAX,
			TextWidth,
			Item.Name.c_str());
		const ImVec2 TextPosition(
			NameMin.x + std::max(0.0f, (TextWidth - TextExtent.x) * 0.5f),
			NameMin.y);
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(NameMin, NameMax, true);
		DrawList->AddText(
			ImGui::GetFont(),
			Metrics.NameFontSize,
			TextPosition,
			ImGui::GetColorU32(ImGuiCol_Text),
			Item.Name.c_str(),
			nullptr,
			TextWidth);
		DrawList->PopClipRect();
	}
} // namespace Durin::Editor::ContentBrowser::Private::ContentBrowserItemView

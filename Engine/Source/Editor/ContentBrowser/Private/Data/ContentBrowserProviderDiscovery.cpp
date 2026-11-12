#include "ContentBrowserNativeDataSource.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserFileDataDiscovery.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/RegistryOperations.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Panels/ContentBrowserChanges.h"
#include "Operations/ContentBrowserPaths.h"
namespace Durin::ContentBrowser::Private
{
	struct FContentBrowserAssetSnapshot
	{
		uint64 Revision = 0, MountRevision = 0;
		std::vector<FContentBrowserNativeItem> Items;
	};
	using ContentBrowserFilesystem::NormalizePath;
	auto CaptureContentBrowserAssets() -> std::shared_ptr<const FContentBrowserAssetSnapshot>
	{
		const auto Query = CaptureAssetQuerySnapshot();
		auto Result = std::make_shared<FContentBrowserAssetSnapshot>();
		Result->Revision = Query.Revision;
		Result->MountRevision = FMountPaths::GetMountRegistryRevision();
		std::unordered_map<FPackagePath, size_t> AssetCounts;
		for (const auto& [Path, Asset] : Query.Assets)
			++AssetCounts[Path.GetPackagePath()];
		for (const auto& [Path, Asset] : Query.Assets)
		{
			const auto PackagePath = Path.GetPackagePath();
			const auto* Disk = Query.FindPackageExact(PackagePath);
			const auto Resolved = FMountPaths::ResolveAssetPath(PackagePath.GetView());
			if (!Disk && !Resolved)
				continue;
			FContentBrowserNativeItem Item{
				.Kind = Asset.IsRedirector() ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset,
				.Name = AssetCounts[PackagePath] == 1 ? std::string(PackagePath.GetPackageName()) : std::string(Path.GetAssetName()),
				.VirtualPath = Path.ToString(),
				.PackagePath = PackagePath,
				.PhysicalPath = NormalizePath(Disk ? Disk->PhysicalPath : Resolved->PhysicalPath.generic_string() + ".dasset"),
				.AssetClassName = Asset.AssetClassName,
				.Extension = ".dasset",
				.RedirectDestination = Asset.RedirectDestination};
			if (Disk)
			{
				Item.PackageFileSize = Disk->FileSize;
				Item.BulkFileSize = Disk->BulkSegmentExtent;
				Item.FileSize = Item.PackageFileSize + Item.BulkFileSize;
				Item.LastWriteTime = Disk->LastWriteTime;
			}
			Result->Items.push_back(std::move(Item));
		}
		return Result;
	}
	auto CaptureRoots(std::string_view Path) -> std::vector<FContentBrowserNativeItem>
	{
		if (Path != "/")
			return {};
		std::vector<FContentBrowserNativeItem> Roots;
		for (const auto& Mount : FContentBrowserPaths::CaptureMounts())
		{
			auto Virtual = Mount.VirtualRoot;
			while (Virtual.size() > 1 && Virtual.back() == '/')
				Virtual.pop_back();
			Roots.push_back({.Kind = EContentBrowserItemKind::Folder,
							 .Name = std::filesystem::path(Virtual).filename().generic_string(),
							 .VirtualPath = std::move(Virtual),
							 .PhysicalPath = Mount.PhysicalRoot});
		}
		return Roots;
	}
	class FAssetCompiledFilter final : public IContentBrowserDataCompiledFilter
	{
	  public:
		std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog;
		std::string Directory, VirtualDirectory;
		std::vector<FContentBrowserNativeItem> Roots;
		bool Recursive = false;
		auto Capture(const Tasks::FCancellationToken& Cancellation, const FEntryStatusQuery&) const -> FContentBrowserDataCapture override
		{
			FContentBrowserDataCapture Result;
			if (!Roots.empty())
			{
				for (const auto& Root : Roots) Result.Items.push_back(MakeNativeItem(Root, true));
				if (Recursive)
					for (const auto& Root : Roots)
					{
						FAssetCompiledFilter Child;
						Child.Catalog = Catalog;
						Child.Directory = Root.PhysicalPath;
						Child.VirtualDirectory = Root.VirtualPath;
						Child.Recursive = true;
						auto Data = Child.Capture(Cancellation, {});
						Result.Items.insert(Result.Items.end(), std::make_move_iterator(Data.Items.begin()),
											std::make_move_iterator(Data.Items.end()));
					}
				return Result;
			}
			if (Directory.empty())
				return Result;
			std::unordered_set<std::string> Folders;
			for (const auto& Item : Catalog->Items)
			{
				if (Cancellation.IsCanceled())
					break;
				const auto Parent = std::filesystem::path(Item.PhysicalPath).parent_path().generic_string();
				if (Parent != Directory && !FPaths::IsLexicalDescendantPath(Parent, Directory, true))
					continue;
				if (Recursive || Parent == Directory)
					Result.Items.push_back(MakeNativeItem(Item, true));
				auto Folder = std::filesystem::path(Parent);
				while (Folder.generic_string() != Directory && FPaths::IsLexicalDescendantPath(Folder.generic_string(), Directory, true))
				{
					const auto Path = Folder.generic_string();
					if ((Recursive || Folder.parent_path().generic_string() == Directory) && Folders.insert(Path).second)
						Result.Items.push_back(MakeNativeItem(
							{.Kind = EContentBrowserItemKind::Folder, .Name = Folder.filename().generic_string(), .VirtualPath = (std::filesystem::path(VirtualDirectory) / Folder.lexically_relative(Directory)).lexically_normal().generic_string(), .PhysicalPath = Path}, true));
					Folder = Folder.parent_path();
				}
			}
			return Result;
		}
	};
	class FFileCompiledFilter final : public IContentBrowserDataCompiledFilter
	{
	  public:
		std::string Directory, VirtualDirectory;
		std::vector<FContentBrowserNativeItem> Roots;
		bool Recursive = false;
		auto Capture(const Tasks::FCancellationToken& Cancellation, const FEntryStatusQuery& Query) const
			-> FContentBrowserDataCapture override
		{
			FContentBrowserFileDataDiscovery Discovery;
			Discovery.SetEntryStatusQueryForTesting(Query);
			if (Roots.empty())
				return Discovery.CaptureItems(Directory, VirtualDirectory, Recursive, Cancellation);
			FContentBrowserDataCapture Result;
			for (const auto& Root : Roots) Result.Items.push_back(MakeNativeItem(Root, false));
			if (Recursive)
				for (const auto& Root : Roots)
				{
					if (Cancellation.IsCanceled())
						break;
					auto Data = Discovery.CaptureItems(Root.PhysicalPath, Root.VirtualPath, true, Cancellation);
					Result.Items.insert(Result.Items.end(), std::make_move_iterator(Data.Items.begin()),
										std::make_move_iterator(Data.Items.end()));
					for (auto& Diagnostic : Data.Diagnostics)
						if (Result.Diagnostics.size() < 8)
							Result.Diagnostics.push_back(std::move(Diagnostic));
						else
							++Result.SuppressedDiagnosticCount;
					Result.SuppressedDiagnosticCount += Data.SuppressedDiagnosticCount;
				}
			return Result;
		}
	};
} // namespace Durin::ContentBrowser::Private
namespace Durin
{
	using namespace ContentBrowser;
	using namespace ContentBrowser::Private;
	auto DContentBrowserAssetDataSource::GetDiscoveryRevision() const -> uint64
	{
		return GetAssetQueryRevision();
	}
	auto DContentBrowserAssetDataSource::IsDiscoveryPathAffected(uint64 Since, std::string_view Path, bool Recursive) const -> bool
	{
		const auto Changes = CaptureAssetQueryChanges(Since);
		if (Changes.bFullRefresh)
			return true;
		const auto Physical = FContentBrowserPaths::VirtualToPhysical(Path);
		return std::ranges::any_of(Changes.Changes,
								   [&](const auto& Change)
								   {
									   return ContentBrowserChanges::Affects(Change, Physical, Recursive) ||
											  !ContentBrowserChanges::IntroducedChild(Change, Physical).empty();
								   });
	}
	auto DContentBrowserAssetDataSource::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter,
													   FContentBrowserDataCompiledFilter& Out) const -> void
	{
		Super::CompileFilter(Path, Filter, Out);
		if (!IsActive())
			return;
		const auto Revision = GetAssetQueryRevision(), Mount = FMountPaths::GetMountRegistryRevision();
		if (!DiscoveryCatalog || DiscoveryCatalog->Revision != Revision || DiscoveryCatalog->MountRevision != Mount)
		{
			auto Catalog = CaptureContentBrowserAssets();
			// CaptureContentBrowserAssets returns a freshly owned immutable catalog.
			DiscoveryCatalog = std::move(Catalog);
		}
		auto Data = std::make_shared<FAssetCompiledFilter>();
		Data->Catalog = DiscoveryCatalog;
		Data->Directory = FContentBrowserPaths::VirtualToPhysical(Path);
		Data->VirtualDirectory = Path;
		Data->Roots = CaptureRoots(Path);
		Data->Recursive = Filter.bRecursive;
		Out.CompiledFilters[this].Data = std::move(Data);
	}
	auto DContentBrowserFileDataSource::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter,
													  FContentBrowserDataCompiledFilter& Out) const -> void
	{
		Super::CompileFilter(Path, Filter, Out);
		if (!IsActive())
			return;
		auto Data = std::make_shared<FFileCompiledFilter>();
		Data->Directory = FContentBrowserPaths::VirtualToPhysical(Path);
		Data->VirtualDirectory = Path;
		Data->Roots = CaptureRoots(Path);
		Data->Recursive = Filter.bRecursive;
		Out.CompiledFilters[this].Data = std::move(Data);
	}
} // namespace Durin

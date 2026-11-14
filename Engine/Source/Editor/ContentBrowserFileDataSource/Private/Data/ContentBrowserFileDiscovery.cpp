#include "ContentBrowserFileDataSource.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserFileDataDiscovery.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/RegistryOperations.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "Misc/ContentFilesystem.h"
#include "AssetRegistry/ContentChangeScopes.h"
#include "Misc/ContentMountPaths.h"
namespace Durin::ContentBrowser::FileDataSource::Private
{
	auto CaptureRoots(std::string_view Path) -> std::vector<FContentBrowserNativeItem>
	{
		if (Path != "/")
			return {};
		std::vector<FContentBrowserNativeItem> Roots;
		for (const auto& Mount : ContentStorage::FContentMountPaths::CaptureMounts())
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
			for (const auto& Root : Roots) Result.Items.push_back(MakeNativeItemDescription(Root));
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
}
namespace Durin
{
	using namespace ContentBrowser;
	using namespace ContentBrowser::FileDataSource::Private;



	auto DContentBrowserFileDataSource::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter,
													  FContentBrowserDataCompiledFilter& Out) const -> void
	{
		Super::CompileFilter(Path, Filter, Out);
		if (!IsActive())
			return;
		auto Data = std::make_shared<FFileCompiledFilter>();
		Data->Directory = ContentStorage::FContentMountPaths::VirtualToPhysical(Path);
		Data->VirtualDirectory = Path;
		Data->Roots = CaptureRoots(Path);
		Data->Recursive = Filter.bRecursive;
		Out.CompiledFilters[this] = std::move(Data);
	}
} // namespace Durin

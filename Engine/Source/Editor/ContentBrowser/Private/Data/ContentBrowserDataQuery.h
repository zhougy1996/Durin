#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSubsystem.h"
#include "Panels/ContentBrowserItemDiscovery.h"

namespace Durin::ContentBrowser::Private
{
	// Native capture optimization carried through the ordinary provider query contract.
	struct FNativeContentBrowserFilter final : IContentBrowserDataFilterExtra
	{
		std::shared_ptr<const FContentBrowserItemsSnapshot> Snapshot;
	};
	// Per-view capture/cancellation state. Provider enumeration and payload binding run on the owner thread.
	class FContentBrowserDataQuery
	{
	public:
		FContentBrowserDataQuery() = default; // Pure discovery fixtures and native source capture preparation.
		explicit FContentBrowserDataQuery(const FContentBrowserHostContext& InHost)
			: Host(InHost)
			, bServiceBound(true)
		{
		}
		auto IsAcceptingRequests() const -> bool;
		auto StopRequestAdmission() -> void { bAccepting = false; }
		auto GetRevision() const -> uint64;
		auto IsAffectedByUpdates(uint64 Since, std::string_view VirtualDirectory, bool bRecursive) const -> bool;
		auto IsProviderDirectory(std::string_view Physical) const -> bool;
		auto Tick(float DeltaSeconds) -> void;
		auto ProjectItems(FContentBrowserItemsSnapshot Snapshot, std::string_view VirtualDirectory, bool bRecursive) const -> FContentBrowserItemsSnapshot;
		auto ProjectDirectory(FContentBrowserDirectorySnapshot Snapshot, std::string_view PhysicalDirectory) const -> FContentBrowserDirectorySnapshot;
		auto PrepareDiscoveryCatalog(std::shared_ptr<const FContentBrowserAssetSnapshot> Previous = {}) const -> std::shared_ptr<const FContentBrowserAssetSnapshot>;
		using FItemsCapture = TMoveOnlyFunction<std::unique_ptr<FContentBrowserItemsSnapshot>()>;
		using FDirectoryCapture = TMoveOnlyFunction<std::unique_ptr<FContentBrowserDirectorySnapshot>()>;
		auto PrepareItemsCapture(std::string Directory, bool bRecursive, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog, FEntryStatusQuery Query, std::shared_ptr<Tasks::FCancellationToken> Cancellation = {}) const -> FItemsCapture;
		auto PrepareDirectoryCapture(std::string Directory, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog, FEntryStatusQuery Query, std::shared_ptr<Tasks::FCancellationToken> Cancellation = {}) const -> FDirectoryCapture;

	private:
		FContentBrowserHostContext Host;
		bool bServiceBound = false;
		bool bAccepting = true;
		std::shared_ptr<FContentBrowserItemDiscovery> ItemDiscovery = std::make_shared<FContentBrowserItemDiscovery>();
	};
} // namespace Durin::ContentBrowser::Private

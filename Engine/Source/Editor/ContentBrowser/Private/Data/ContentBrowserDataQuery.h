#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSubsystem.h"
#include "Panels/ContentBrowserDataTypes.h"
namespace Durin::ContentBrowser::Private
{
	class FContentBrowserDataQuery
	{
	  public:
		explicit FContentBrowserDataQuery(const FContentBrowserHostContext& InHost) : Host(InHost), bServiceBound(true) {}
		auto IsAcceptingRequests() const -> bool;
		auto StopRequestAdmission() -> void { bAccepting = false; }
		auto GetRevision() const -> uint64;
		auto IsAffectedByUpdates(uint64, std::string_view, bool) const -> bool;
		auto IsProviderDirectory(std::string_view Virtual) const -> bool;
		auto Tick(float DeltaSeconds) -> void;
		auto ProjectItems(FContentBrowserItemsSnapshot, std::string_view, bool) const -> FContentBrowserItemsSnapshot;
		auto ProjectDirectory(FContentBrowserDirectorySnapshot, std::string_view) const -> FContentBrowserDirectorySnapshot;
		using FItemsCapture = TMoveOnlyFunction<std::unique_ptr<FContentBrowserItemsSnapshot>()>;
		using FDirectoryCapture = TMoveOnlyFunction<std::unique_ptr<FContentBrowserDirectorySnapshot>()>;
		auto PrepareItemsCapture(std::string Virtual, bool Recursive, FEntryStatusQuery Query = {},
								 std::shared_ptr<Tasks::FCancellationToken> Cancellation = {}) const -> FItemsCapture;
		auto PrepareDirectoryCapture(std::string Virtual, FEntryStatusQuery Query = {},
									 std::shared_ptr<Tasks::FCancellationToken> Cancellation = {}) const -> FDirectoryCapture;

	  protected:
		FContentBrowserDataQuery() = default;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> StandaloneSources;

	  private:
		auto GetSources() const -> std::vector<TStrongObjectPtr<DContentBrowserDataSource>>;
		auto OwnsSource(const DContentBrowserDataSource*) const -> bool;
		FContentBrowserHostContext Host;
		bool bServiceBound = false, bAccepting = true;
		struct FSourceRevision
		{
			TWeakObjectPtr<DContentBrowserDataSource> Source;
			uint64 Generation = 0, Revision = 0;
		};
		struct FRevision
		{
			uint64 Service = 0;
			std::vector<FSourceRevision> Sources;
		};
		mutable uint64 Revision = 0;
		mutable std::map<uint64, FRevision> Revisions;
	};
} // namespace Durin::ContentBrowser::Private

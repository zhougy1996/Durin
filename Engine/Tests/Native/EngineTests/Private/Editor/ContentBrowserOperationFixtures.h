#pragma once
#include "CoreMinimal.h"
#include "Editor/ContentBrowserQueryTestSupport.h"
#include "ContentBrowserItemAttributeHelpers.h"
#include "FileHelpers.h"
#include "AssetTools/ContentDeletionRequest.h"
#include "AssetRegistry/Catalog.h"
namespace Durin::ContentBrowser::Private
{
	using namespace ContentStorage;
	class FOperationFixture
	{
	public:
		explicit FOperationFixture(FContentBrowserOperationContext Policy = {}) : Host(std::move(Policy)) {}
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Host.ReportDiagnostic = std::move(Sink); }
		auto CanModify(std::string* Error = nullptr) const -> bool { if (Error) Error->clear(); const bool Allowed = Accepting && (!Host.CanMutateContent || Host.CanMutateContent()) && Accepting; if (!Allowed && Error) *Error="Content mutation is currently disabled."; return Allowed; }
		auto StopRequestAdmission() -> void { Accepting = false; }
	protected:
		auto Policy() const -> FContentBrowserOperationContext { auto Result=Host; Result.CanMutateContent=[this] { return CanModify(); }; return Result; }
		auto Report(std::string_view Error) -> void { if (Host.ReportDiagnostic && !Error.empty()) Host.ReportDiagnostic(ELogLevel::Error, Error); }
		auto Bind(FStorageItemFixture Item) -> FContentBrowserItem
		{
			for (const auto& [Path, Package] : CaptureAssetCatalogSnapshot().Assets)
				if ((!Item.PhysicalPath.empty() && std::filesystem::path(Item.PhysicalPath).lexically_normal() == std::filesystem::path(Package.PhysicalPath).lexically_normal()) || (Item.Kind == EContentBrowserItemKind::Asset && Item.PackagePath == Path))
				{ Item.Kind=EContentBrowserItemKind::Asset; Item.PackagePath=Path; Item.PhysicalPath=Package.PhysicalPath; if (Item.VirtualPath.empty() && !Package.TopLevelAssets.empty()) Item.VirtualPath=Package.TopLevelAssets.front().AssetPath.ToString(); break; }
			return Query.BindContributionsForTesting(Item, Policy());
		}
		static auto Describe(const FContentBrowserItem& Item, FStorageItemFixture& Out) -> void
		{
			Out={}; if (Item.GetInternalItems().empty()) return;
			Out.Kind=Item.IsFolder() ? EContentBrowserItemKind::Folder : Item.IsRedirector() ? EContentBrowserItemKind::Redirector : Item.IsAsset() ? EContentBrowserItemKind::Asset : EContentBrowserItemKind::File;
			Out.Name=Item.GetItemName(); Out.VirtualPath=Item.GetVirtualPath(); Item.GetItemPhysicalPath(Out.PhysicalPath); Out.PackagePath=ItemAttributes::PackagePath(Item); Out.AssetClassName=ItemAttributes::String(Item,ContentBrowserItemAttributes::Class); Out.Extension=ItemAttributes::String(Item,ContentBrowserItemAttributes::Extension);
		}
		FProviderQueryFixture Query;
		FContentBrowserOperationContext Host;
		bool Accepting=true;
	};
	class FProviderFolderFixture : public FOperationFixture
	{
	public:
		using FOperationFixture::FOperationFixture;
		auto CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* Error=nullptr) -> bool
		{
			if (!CanModify(Error)) return false;
			const auto Virtual=FContentMountPaths::PhysicalToVirtualDirectory(Directory); if (Virtual.empty()) { if (Error) *Error="Folders require an automatically scanned content mount."; return false; }
			return Query.FindSourceForTesting(FName("Asset"))->CanCreateFolder((std::filesystem::path(Virtual)/Name).generic_string(),Error);
		}
		auto CreateFolder(std::string_view Directory, std::string_view Name, FStorageItemFixture& Out) -> bool
		{
			Out={}; std::string Error; if (!CanCreateFolder(Directory,Name,&Error)) { Report(Error); return false; }
			FContentBrowserItemDataTemporaryContext Pending;
			const auto Virtual=FContentMountPaths::PhysicalToVirtualDirectory(Directory);
			auto* Source=Query.FindSourceForTesting(FName("Asset"));
			if (!Source->CreateFolder((std::filesystem::path(Virtual)/Name).generic_string(),Pending,Policy())) return false;
			FContentBrowserItemData Data; const bool Done=Pending.FinalizeItem(Name,Data); if (Data.GetOwnerDataSource()) Describe(FContentBrowserItem{.InternalItems={std::move(Data)}},Out); return Done;
		}
		auto PrepareFolder(std::string_view Directory, FStorageItemFixture& Out) -> bool
		{
			Out={}; for (int I=0; I<1000; ++I) { const auto Name=I ? std::format("New Folder ({})",I+1) : std::string("New Folder"); if (CanCreateFolder(Directory,Name)) { Out.Kind=EContentBrowserItemKind::Folder; Out.Name=Name; Out.PhysicalPath=(std::filesystem::path(Directory)/Name).generic_string(); return true; } } return false;
		}
	};
	class FProviderAssetActionFixture : public FOperationFixture
	{
	public:
		using FOperationFixture::FOperationFixture;
		auto CanDuplicateItem(const FStorageItemFixture& Item) -> bool { return Bind(Item).CanDuplicate(); }
		auto DuplicateItem(const FStorageItemFixture& Item, FStorageItemFixture& Out) -> bool { auto Bound=Bind(Item); FContentBrowserItem New; const bool Done=Bound.Duplicate(New); Describe(New,Out); return Done; }
		auto CopyToFolder(const FStorageItemFixture& Item, std::string_view Directory, FStorageItemFixture& Out) -> bool
		{
			auto Bound=Bind(Item); Out={};
			if (Bound.GetInternalItems().empty()) { Report("No provider contribution to copy."); return false; }
			const auto Virtual=FContentMountPaths::PhysicalToVirtualDirectory(Directory); const auto Result=Bound.Copy(Virtual);
			if (Result)
			{
				const auto Target = (std::filesystem::path(Virtual) / std::filesystem::path(Item.VirtualPath).filename()).generic_string();
				Query.FindSourceForTesting(FName("Asset"))->EnumerateItemsAtPath(Target, [&](FContentBrowserItemData&& New) { Describe(FContentBrowserItem{.InternalItems = {std::move(New)}}, Out); return false; });
			}
			return Result;
		}
		auto FixUpRedirectorsInFolder(std::string_view Path) -> bool { return static_cast<DContentBrowserAssetDataSource*>(Query.FindSourceForTesting(FName("Asset")))->FixUpRedirectorsInFolder(Path,Policy()); }
	};
	class FProviderSaveFixture : public FOperationFixture
	{
	public:
		using FOperationFixture::FOperationFixture;
		auto CanSaveItem(const FStorageItemFixture& Item, bool Mode=false, std::string* Error=nullptr) -> bool
		{
			auto Bound=Bind(Item); const auto* Data=Bound.GetPrimaryInternalItem(); return Data && Data->GetOwnerDataSource() && Data->GetOwnerDataSource()->CanSaveItem(*Data,Mode==true ? EContentBrowserItemSaveFlags::None : EContentBrowserItemSaveFlags::SaveOnlyIfDirty,Error);
		}
		auto BulkSaveItems(std::span<const FStorageItemFixture> Items, bool Mode=false) -> bool
		{
			FContentBrowserItem Batch; for (const auto& Item : Items) { auto Bound=Bind(Item); Batch.InternalItems.insert(Batch.InternalItems.end(),Bound.InternalItems.begin(),Bound.InternalItems.end()); }
			return Batch.Save(Mode==true ? EContentBrowserItemSaveFlags::None : EContentBrowserItemSaveFlags::SaveOnlyIfDirty);
		}
	};
	class FDeletionRequestFixture
	{
	public:
		explicit FDeletionRequestFixture(FContentBrowserOperationContext Host={}, bool Assets=true, bool Files=true) : Operation({.CanMutateContent=Host.CanMutateContent,.ReportDiagnostic=Host.ReportDiagnostic,.NotifyContentMutation=Host.NotifyContentMutation},Assets,Files) {}
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Operation.SetDiagnosticSink(std::move(Sink)); }
		template<typename T> auto PrepareDeleteItems(const T& Items) -> FContentDeletionPlanPtr { std::vector<FContentDeletionInput> Inputs; for (const auto& Item : Items) Inputs.push_back({Item.Name,Item.PhysicalPath}); return Operation.PrepareDeleteItems(Inputs); }
		auto DismissDeletion(FContentDeletionPlanPtr Plan) -> void { Operation.DismissDeletion(std::move(Plan)); }
		auto DeleteItems(FContentDeletionPlanPtr& Plan, FContentDeletionHooks Hooks={}) -> FContentDeletionResult
		{ return Operation.DeleteItems(Plan, std::move(Hooks)); }
	private:
		FContentDeletionRequest Operation;
	};
}

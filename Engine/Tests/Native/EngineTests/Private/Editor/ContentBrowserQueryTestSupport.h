#pragma once
#include "CoreMinimal.h"
#include "AssetRegistry/Catalog.h"
#include "Misc/Paths.h"
#include "NativeDObjectTestSupport.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "Threading/RunnableThread.h"
#include "ContentBrowserAssetDataSource.h"
#include "ContentBrowserFileDataSource.h"
#include "ContentBrowserDataQuery.h"
#include "Editor/ContentBrowserStorageFixture.h"
#include "ContentBrowserItemAttributeHelpers.h"
#include "Editor/EditorEngine.h"
namespace Durin::ContentBrowser::Private
{
	// Use the normal subsystem collection without activating another global editor.
	class FProviderServiceFixture
	{
	protected:
		FProviderServiceFixture()
		{
			Testing::InitializeDObjectSystemForTests();
			Editor = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
			Collection = std::make_unique<FSubsystemCollection>(*Editor, DEditorSubsystem::StaticClass());
			Collection->Initialize({{.Type = DContentBrowserDataSubsystem::StaticClass()}});
			Data = TStrongObjectPtr<DContentBrowserDataSubsystem>(static_cast<DContentBrowserDataSubsystem*>(Collection->Find(DContentBrowserDataSubsystem::StaticClass())));
		}
		~FProviderServiceFixture() { Collection->Deinitialize(); }
		TStrongObjectPtr<DEditorEngine> Editor;
		std::unique_ptr<FSubsystemCollection> Collection;
		TStrongObjectPtr<DContentBrowserDataSubsystem> Data;
	};
	class FProviderQueryFixture final : private FProviderServiceFixture, public FContentBrowserDataQuery
	{
	public:
		FProviderQueryFixture() : FContentBrowserDataQuery(Data.Get())
		{
			AddSource(*NewObject<DContentBrowserAssetDataSource>(Data.Get(), {}));
			AddSource(*NewObject<DContentBrowserFileDataSource>(Data.Get(), {}));
		}
		auto GetServiceForTesting() const -> DContentBrowserDataSubsystem* { return Data.Get(); }

		auto FindSourceForTesting(FName Name) -> DContentBrowserDataSource* { for (const auto& Source : Sources) if (Source->GetDataSourceName()==Name) return Source.Get(); return nullptr; }
		auto BindItemForTesting(FContentBrowserItem Item) -> FContentBrowserItem
		{
			for (const auto& Source : Sources)
			{
				auto Data=Source->BindItemData(*Item.GetPrimaryInternalItem());
				if (!Data.GetOwnerDataSource()) continue;
				Item.InternalItems = {std::move(Data)}; break;
			}
			return Item;
		}
		auto AddSource(DContentBrowserDataSource& Source) -> void
		{
			if (std::ranges::none_of(Sources, [&](const auto& Entry) { return Entry.Get() == &Source; }))
				Sources.emplace_back(&Source);
			Source.Initialize();
			require(Data->ActivateDataSource(Source.GetDataSourceName()));
		}
		auto BindContributionsForTesting(const FStorageItemFixture& Description, const FContentBrowserOperationContext& Host) -> FContentBrowserItem
		{
			FContentBrowserItem Result;
			for (const auto& Source : Sources)
			{
				if (Cast<DContentBrowserAssetDataSource>(Source.Get()) && Description.Kind == EContentBrowserItemKind::Folder && !Description.PhysicalPath.empty())
				{
					const auto Catalog = CaptureAssetCatalogSnapshot();
					if (std::ranges::none_of(Catalog.Assets, [&](const auto& Entry) { return FPaths::IsLexicalDescendantPath(Entry.second.PhysicalPath, Description.PhysicalPath, true); })) continue;
				}
				auto Data = Source->BindItemData(*(MakeFixtureItem(Description, Cast<DContentBrowserAssetDataSource>(Source.Get()) != nullptr)).GetPrimaryInternalItem(), Host);
				if (Data.GetOwnerDataSource()) Result.InternalItems.push_back(std::move(Data));
			}
			return Result;
		}
		~FProviderQueryFixture()
		{
			for (const auto& Source : Sources)
				Source->Shutdown();
		}

	  private:
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Sources;
	};
} // namespace Durin::ContentBrowser::Private

namespace Durin
{
	class FVirtualBrowserQuerySource : public DContentBrowserDataSource
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FVirtualBrowserQuerySource, DContentBrowserDataSource, FVirtualBrowserQuerySource::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FVirtualBrowserQuerySource)
	  public:
		explicit FVirtualBrowserQuerySource(const FObjectInitializer& I) : Super(I) {}
		std::vector<ContentBrowser::FContentBrowserItem> Rows;
		FName SourceName = FName("Memory");
		int Copies = 0;
		bool UseCompiledCapture = false;
		std::shared_ptr<std::atomic<int>> CaptureCalls = std::make_shared<std::atomic<int>>(0);
		std::function<void()> BeforeBind;
		mutable std::function<void()> BeforeCompile;
		std::function<void()> BeforeCapture;
		std::function<void(const ContentBrowser::FContentBrowserOperationContext&)> InspectBindingPolicy;
		mutable int DiskSizeReads = 0;
		mutable std::function<void()> BeforePhysicalPath;
		class FValueCapture final : public ContentBrowser::IContentBrowserDataCompiledFilter
		{
		  public:
			std::vector<ContentBrowser::FContentBrowserItemData> Items;
			std::shared_ptr<std::atomic<int>> Calls;
			std::function<void()> BeforeCapture;
			auto Capture(const Tasks::FCancellationToken&, const ContentBrowser::FEntryStatusQuery&) const
				-> ContentBrowser::FContentBrowserDataCapture override
			{
				++*Calls;
				if (BeforeCapture) BeforeCapture();
				return {.Items = Items};
			}
		};
		auto CompileFilter(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter& Filter,
						   ContentBrowser::FContentBrowserDataCompiledFilter& Out) const -> void override
		{
			if (auto Callback = std::exchange(BeforeCompile, {})) Callback();
			Super::CompileFilter(Path, Filter, Out);
			if (!UseCompiledCapture)
				return;
			auto Data = std::make_shared<FValueCapture>();
			Data->Calls = CaptureCalls;
			Data->BeforeCapture = BeforeCapture;
			for (const auto& Row : Rows) if (const auto* Description = Row.GetPrimaryInternalItem()) Data->Items.push_back(*Description);
			Out.CompiledFilters[this] = std::move(Data);
		}
		std::string PreviewPath;
		auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData& Data, bool IncludeMetaData, std::string_view Key, ContentBrowser::FContentBrowserItemDataAttributeValue& Out) const -> bool override
		{
			using namespace ContentBrowser::ContentBrowserItemAttributes;
			Out={};
			if (Data.GetOwnerDataSource()!=this) return false;
			if (Key==ItemDiskSize) ++DiskSizeReads;
			if (Key==ItemTypeDisplayName) Out=ContentBrowser::FContentBrowserItemDataAttributeValue("Memory object");
			else if (!PreviewPath.empty() && Key==ItemDiskSize) Out=ContentBrowser::FContentBrowserItemDataAttributeValue(int64(128));
			else if (!PreviewPath.empty() && Key==Extension) Out=ContentBrowser::FContentBrowserItemDataAttributeValue(".png");
			else if (!PreviewPath.empty() && Key==LastWriteTime) Out=ContentBrowser::FContentBrowserItemDataAttributeValue(int64(0));
			else return false;
			if (IncludeMetaData) Out.SetMetaData({.DisplayName="Type"});
			return true;
		}
		auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData& Data, std::string& Out) const -> bool override
		{
			if (auto Callback = std::exchange(BeforePhysicalPath, {})) Callback();
			Out.clear();
			if (Data.GetOwnerDataSource()!=this || PreviewPath.empty()) return false;
			Out=PreviewPath; return true;
		}
		auto GetDataSourceName() const -> FName override { return SourceName; }
		auto BindItemData(const ContentBrowser::FContentBrowserItemData& Item, const ContentBrowser::FContentBrowserOperationContext& Host = {})
			-> ContentBrowser::FContentBrowserItemData override
		{
			if (auto Callback = std::exchange(BeforeBind, {}))
				Callback();
			if (auto Inspect = std::exchange(InspectBindingPolicy, {})) Inspect(Host);
			return {*this, Item.GetItemFlags(), Item.GetVirtualPath(), Item.GetItemName(), Item.GetDisplayName(), {}};
		}
		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter& Filter,
										  const std::function<bool(ContentBrowser::FContentBrowserItemData&&)>& Callback) -> void override
		{
			CheckGameThread();
			auto Scope = std::filesystem::path(Filter.Path).lexically_normal().generic_string();
			while (Scope.size() > 1 && Scope.back() == '/')
				Scope.pop_back();
			for (auto Item : Rows)
			{
				const auto Relative = std::filesystem::path(Item.GetVirtualPath()).lexically_relative(Scope);
				if (Relative.empty() || Relative == "." || *Relative.begin() == "..")
					continue;
				if (!Filter.Filter.bRecursive && std::filesystem::path(Item.GetVirtualPath()).parent_path().generic_string() != Scope)
					continue;
				if (Filter.Filter.Kind && *Filter.Filter.Kind != (Item.IsFolder() ? ContentBrowser::EContentBrowserItemKind::Folder : ContentBrowser::EContentBrowserItemKind::File))
					continue;
				if (!Callback(BindItemData(*Item.GetPrimaryInternalItem())))
					return;
			}
		}
		auto CanCopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view, std::string*) const -> bool override
		{
			return Item.GetOwnerDataSource() == this;
		}
		auto CopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view Destination) -> bool override
		{ return BulkCopyItems(std::span{&Item, 1}, Destination); }
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view) -> bool override
		{
			++Copies;
			return !Items.empty();
		}


	};
	inline auto FVirtualBrowserQuerySource::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FVirtualBrowserQuerySource", Class, nullptr, sizeof(FVirtualBrowserQuerySource),
									  alignof(FVirtualBrowserQuerySource), EClassFlags::None,
									  InternalConstructor<FVirtualBrowserQuerySource>, &DContentBrowserDataSource::StaticClass);
		return Class;
	}
} // namespace Durin

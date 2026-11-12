#pragma once
#include "CoreMinimal.h"
#include "NativeDObjectTestSupport.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "Threading/RunnableThread.h"
#include "ContentBrowserNativeDataSource.h"
#include "Data/ContentBrowserDataQuery.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserItemAttributeHelpers.h"
namespace Durin::ContentBrowser::Private
{
	class FNativeContentBrowserTestQuery final : public FContentBrowserDataQuery, private IContentBrowserItemDataSink
	{
	  public:
		FNativeContentBrowserTestQuery()
		{
			Testing::InitializeDObjectSystemForTests();
			StandaloneSources.emplace_back(NewObject<DContentBrowserAssetDataSource>(nullptr, {}));
			StandaloneSources.emplace_back(NewObject<DContentBrowserFileDataSource>(nullptr, {}));
			for (const auto& Source : StandaloneSources)
			{
				Source->Initialize();
				Source->SetDataSink(this);
			}
		}
		auto BindItemForTesting(FContentBrowserItem Item) -> FContentBrowserItem
		{
			for (const auto& Source : StandaloneSources)
			{
				auto Data=Source->CreateItemData(Item);
				if (!Data.GetOwnerDataSource()) continue;
				Item.InternalItems = {std::move(Data)}; break;
			}
			return Item;
		}
		auto AddSource(DContentBrowserDataSource& Source) -> void
		{
			if (std::ranges::none_of(StandaloneSources, [&](const auto& Entry) { return Entry.Get() == &Source; }))
				StandaloneSources.emplace_back(&Source);
			Source.Initialize();
			Source.SetDataSink(this);
		}
		auto BindContributionsForTesting(const FContentBrowserNativeItem& Description, const FContentBrowserOperationContext& Host) -> FContentBrowserItem
		{
			FContentBrowserItem Result;
			for (const auto& Source : StandaloneSources)
			{
				auto Data = Source->CreateItemData(MakeNativeItem(Description, Cast<DContentBrowserAssetDataSource>(Source.Get()) != nullptr), Host);
				if (Data.GetOwnerDataSource()) Result.InternalItems.push_back(std::move(Data));
			}
			return Result;
		}
		~FNativeContentBrowserTestQuery()
		{
			for (const auto& Source : StandaloneSources)
				Source->Shutdown();
		}

	  private:
		auto QueueItemDataUpdate(FContentBrowserItemDataUpdate&&) -> void override {}
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
		uint64 Revision = 1;
		int Copies = 0;
		bool UseCompiledCapture = false;
		std::function<void()> BeforeBind;
		class FValueCapture final : public ContentBrowser::IContentBrowserDataCompiledFilter
		{
		  public:
			std::vector<ContentBrowser::FContentBrowserItem> Items;
			auto Capture(const Tasks::FCancellationToken&, const ContentBrowser::FEntryStatusQuery&) const
				-> ContentBrowser::FContentBrowserDataCapture override
			{
				return {.Items = Items};
			}
		};
		auto CompileFilter(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter& Filter,
						   ContentBrowser::FContentBrowserDataCompiledFilter& Out) const -> void override
		{
			Super::CompileFilter(Path, Filter, Out);
			if (!UseCompiledCapture)
				return;
			auto Data = std::make_shared<FValueCapture>();
			Data->Items = Rows;
			Out.CompiledFilters[this].Data = std::move(Data);
		}
		std::string PreviewPath;
		auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData& Data, bool IncludeMetaData, std::string_view Key, ContentBrowser::FContentBrowserItemDataAttributeValue& Out) const -> bool override
		{
			using namespace ContentBrowser::ContentBrowserItemAttributes;
			Out={};
			if (Data.GetOwnerDataSource()!=this) return false;
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
			Out.clear();
			if (Data.GetOwnerDataSource()!=this || PreviewPath.empty()) return false;
			Out=PreviewPath; return true;
		}
		auto GetDataSourceName() const -> FName override { return FName("Memory"); }
		auto GetDiscoveryRevision() const -> uint64 override { return Revision; }
		auto CreateItemData(const ContentBrowser::FContentBrowserItem& Item, const ContentBrowser::FContentBrowserOperationContext& = {})
			-> ContentBrowser::FContentBrowserItemData override
		{
			if (auto Callback = std::exchange(BeforeBind, {}))
				Callback();
			return {*this, Item.GetItemFlags(), Item.GetVirtualPath(), Item.GetItemName(), Item.GetDisplayName(), {}};
		}
		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter& Filter,
										  const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback,
										  const ContentBrowser::FContentBrowserOperationContext&) -> void override
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
				Item.InternalItems = {CreateItemData(Item)};
				if (!Callback(std::move(Item)))
					return;
			}
		}
		auto CanCopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view, std::string*) const -> bool override
		{
			return Item.GetOwnerDataSource() == this;
		}
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view) -> ContentBrowser::FContentBrowserOperationResult override
		{
			++Copies;
			ContentBrowser::FContentBrowserOperationResult Result;
			for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), ContentBrowser::EContentBrowserEffectStatus::Committed);
			return Result;
		}
		auto PrepareCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view Destination) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override
		{
			std::vector<ContentBrowser::FContentBrowserItemData> Captured(Items.begin(), Items.end());
			std::vector<ContentBrowser::FContentBrowserItemContributionKey> Keys;
			for (const auto& Item : Captured) Keys.push_back(Item.GetContributionKey());
			return std::make_unique<ContentBrowser::FContentBrowserItemDataTransferContext>(std::move(Keys), std::vector<ContentBrowser::FContentBrowserTransferDestination>{},
				[this, Captured, Destination = std::string(Destination)](std::string*) { return std::ranges::all_of(Captured, [&](const auto& Item) { return Item.GetOwnerDataSource() == this && CanCopyItem(Item, Destination, nullptr) && Item.GetOwnerDataSource() == this; }); },
				[this, Captured, Destination = std::string(Destination)](const auto&) { return BulkCopyItems(Captured, Destination); });
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

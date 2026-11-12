#pragma once
#include "CoreMinimal.h"
#include "NativeDObjectTestSupport.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "Threading/RunnableThread.h"
#include "ContentBrowserNativeDataSource.h"
#include "Data/ContentBrowserDataQuery.h"
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
		auto AddSource(DContentBrowserDataSource& Source) -> void
		{
			if (std::ranges::none_of(StandaloneSources, [&](const auto& Entry) { return Entry.Get() == &Source; }))
				StandaloneSources.emplace_back(&Source);
			Source.Initialize();
			Source.SetDataSink(this);
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
			auto Capture(const Tasks::FCancellationToken&, const ContentBrowser::FEntryStatusQuery&) const -> ContentBrowser::FContentBrowserDataCapture override { return {.Items=Items}; }
		};
		auto CompileFilter(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter& Filter, ContentBrowser::FContentBrowserDataCompiledFilter& Out) const -> void override
		{
			Super::CompileFilter(Path, Filter, Out);
			if (!UseCompiledCapture) return;
			auto Data=std::make_shared<FValueCapture>(); Data->Items=Rows;
			Out.CompiledFilters[this].Data=std::move(Data);
		}
		auto GetDataSourceName() const -> FName override { return FName("Memory"); }
		auto GetDiscoveryRevision() const -> uint64 override { return Revision; }
		auto CreateItemData(const ContentBrowser::FContentBrowserItem& Item, const ContentBrowser::FContentBrowserHostContext& = {})
			-> ContentBrowser::FContentBrowserItemData override
		{
			if (auto Callback=std::exchange(BeforeBind, {})) Callback();
			return {*this, Item.Kind, Item.VirtualPath, {}};
		}
		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter& Filter,
										  const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback,
										  const ContentBrowser::FContentBrowserHostContext&) -> void override
		{
			CheckGameThread();
			auto Scope = std::filesystem::path(Filter.Path).lexically_normal().generic_string();
			while (Scope.size() > 1 && Scope.back() == '/')
				Scope.pop_back();
			for (auto Item : Rows)
			{
				const auto Relative = std::filesystem::path(Item.VirtualPath).lexically_relative(Scope);
				if (Relative.empty() || Relative == "." || *Relative.begin() == "..")
					continue;
				if (!Filter.Filter.bRecursive && std::filesystem::path(Item.VirtualPath).parent_path().generic_string() != Scope)
					continue;
				if (Filter.Filter.Kind && *Filter.Filter.Kind != Item.Kind)
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
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override
		{
			++Copies;
			return true;
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

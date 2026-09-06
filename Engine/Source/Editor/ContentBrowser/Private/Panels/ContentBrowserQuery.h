#pragma once

#include "Panels/ContentBrowserDataTypes.h"
#include <iterator>
#include <unordered_set>

namespace Durin::Editor::ContentBrowser::Private
{
	// Query settings carry no filesystem or registry ownership.
	struct FContentBrowserQuerySettings
	{
		std::string Search;
		EContentBrowserTypeFilter TypeFilter = EContentBrowserTypeFilter::All;
		EContentBrowserSortColumn SortColumn = EContentBrowserSortColumn::Name;
		bool bSortAscending = true;
		bool bShowHiddenFiles = false;
		bool bShowRedirectors = false;
	};

	// Cheap copies retain both the immutable source and its ordered indices. Item
	// references and iterators survive model refresh while a range copy is retained.
	class FContentBrowserItemRange
	{
		struct FStorage
		{
			std::shared_ptr<const FContentBrowserItemsSnapshot> Snapshot;
			std::vector<size_t> Indices;
		};
	public:
		class FIterator
		{
		public:
			using iterator_category = std::random_access_iterator_tag;
			using iterator_concept = std::random_access_iterator_tag;
			using value_type = FContentBrowserItem;
			using difference_type = std::ptrdiff_t;
			using reference = const FContentBrowserItem&;
			using pointer = const FContentBrowserItem*;
			FIterator() = default;
			FIterator(const FStorage* InStorage, difference_type InPosition)
				: Storage(InStorage), Position(InPosition) {}
			auto operator*() const -> reference { return Storage->Snapshot->Items[Storage->Indices[Position]]; }
			auto operator->() const -> pointer { return &**this; }
			auto operator[](difference_type Offset) const -> reference { return *(*this + Offset); }
			auto operator++() -> FIterator& { ++Position; return *this; }
			auto operator++(int) -> FIterator { auto Previous = *this; ++*this; return Previous; }
			auto operator--() -> FIterator& { --Position; return *this; }
			auto operator--(int) -> FIterator { auto Previous = *this; --*this; return Previous; }
			auto operator+=(difference_type Offset) -> FIterator& { Position += Offset; return *this; }
			auto operator-=(difference_type Offset) -> FIterator& { Position -= Offset; return *this; }
			friend auto operator+(FIterator It, difference_type Offset) -> FIterator { return It += Offset; }
			friend auto operator+(difference_type Offset, FIterator It) -> FIterator { return It += Offset; }
			friend auto operator-(FIterator It, difference_type Offset) -> FIterator { return It -= Offset; }
			friend auto operator-(FIterator A, FIterator B) -> difference_type { return A.Position - B.Position; }
			auto operator==(const FIterator&) const -> bool = default;
			auto operator<=>(const FIterator& Other) const { return Position <=> Other.Position; }
		private:
			const FStorage* Storage = nullptr;
			difference_type Position = 0;
		};

		FContentBrowserItemRange() = default;
		// Indices must address Snapshot and are ordered exactly as readers should see them.
		FContentBrowserItemRange(std::shared_ptr<const FContentBrowserItemsSnapshot> Snapshot,
			std::vector<size_t> Indices)
			: Storage(std::make_shared<const FStorage>(FStorage{std::move(Snapshot), std::move(Indices)})) {}
		auto begin() const -> FIterator { return {Storage.get(), 0}; }
		auto end() const -> FIterator { return {Storage.get(), static_cast<std::ptrdiff_t>(size())}; }
		auto size() const -> size_t { return Storage ? Storage->Indices.size() : 0; }
		auto empty() const -> bool { return size() == 0; }
		auto operator[](size_t Index) const -> const FContentBrowserItem& { return *(begin() + Index); }
		auto front() const -> const FContentBrowserItem& { return (*this)[0]; }
		auto GetIndices() const -> std::span<const size_t>
		{
			return Storage ? std::span<const size_t>(Storage->Indices) : std::span<const size_t>{};
		}
	private:
		std::shared_ptr<const FStorage> Storage;
	};

	namespace ContentBrowserQuery
	{
		// Owner-thread projection: type presentations come from the editor extension registry.
		// No filesystem enumeration, live asset-registry access, or item copies occur here.
		auto Project(std::shared_ptr<const FContentBrowserItemsSnapshot> Snapshot,
			std::string_view PhysicalDirectory, const FContentBrowserQuerySettings& Settings)
			-> FContentBrowserItemRange;
		auto TypeLabel(const FContentBrowserItem& Item) -> std::string;
		// Materialize only selected records for command APIs that require owned values.
		auto CopySelection(const FContentBrowserItemRange& Items,
			const std::unordered_set<std::string>& Selection) -> std::vector<FContentBrowserItem>;
	}
}

#pragma once

#include "CoreDObjectAPI.h"
#include "Misc/EnumClassFlags.h"

#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Durin
{
	inline constexpr uint32 ContainerOpsVersion = 1;

	enum class EContainerOpResult : uint8
	{
		Success,
		Unsupported,
		InvalidInput,
		OutOfRange,
		NotFound,
		DuplicateKey,
		EquivalentKey,
		AllocationFailure,
		ConstructionFailure,
		BackendRejected
	};

	enum class EArrayOpsFlags : uint16
	{
		None = 0,
		Count = 1 << 0,
		ConstTraversal = 1 << 1,
		MutableTraversal = 1 << 2,
		RandomAccess = 1 << 3,
		Clear = 1 << 4,
		Reserve = 1 << 5,
		DefaultGrow = 1 << 6,
		Shrink = 1 << 7,
		DetachedStorage = 1 << 8,
		TransactionalCommit = 1 << 9
	};
	ENUM_CLASS_FLAGS(EArrayOpsFlags)

	enum class EMapOpsFlags : uint16
	{
		None = 0,
		Count = 1 << 0,
		ConstTraversal = 1 << 1,
		MutableMappedTraversal = 1 << 2,
		Clear = 1 << 3,
		Reserve = 1 << 4,
		Lookup = 1 << 5,
		MutableLookup = 1 << 6,
		Insert = 1 << 7,
		Remove = 1 << 8,
		RenameKey = 1 << 9,
		DetachedStorage = 1 << 10,
		TransactionalCommit = 1 << 11
	};
	ENUM_CLASS_FLAGS(EMapOpsFlags)

	using FArrayConstVisitor = bool (*)(void* Context, uint64 Index, const void* Element);
	using FArrayMutableVisitor = bool (*)(void* Context, uint64 Index, void* Element);
	using FMapConstVisitor = bool (*)(void* Context, const void* Key, const void* Value);
	using FMapMutableVisitor = bool (*)(void* Context, const void* Key, void* Value);

	struct FArrayOps
	{
		uint32 Version = ContainerOpsVersion;
		EArrayOpsFlags Flags = EArrayOpsFlags::None;
		uint32 ContainerSize = 0;
		uint32 ContainerAlignment = 0;
		void (*Initialize)(void* Container) = nullptr;
		void (*Destroy)(void* Container) = nullptr;
		uint64 (*Num)(const void* Container) = nullptr;
		EContainerOpResult (*VisitConst)(const void* Container, FArrayConstVisitor Visitor, void* Context) = nullptr;
		EContainerOpResult (*VisitMutable)(void* Container, FArrayMutableVisitor Visitor, void* Context) = nullptr;
		EContainerOpResult (*GetConstAt)(const void* Container, uint64 Index, const void** OutElement) = nullptr;
		EContainerOpResult (*GetMutableAt)(void* Container, uint64 Index, void** OutElement) = nullptr;
		EContainerOpResult (*Clear)(void* Container) = nullptr;
		EContainerOpResult (*Reserve)(void* Container, uint64 Count) = nullptr;
		EContainerOpResult (*Resize)(void* Container, uint64 Count) = nullptr;
		EContainerOpResult (*CreateDetached)(void** OutContainer) = nullptr;
		void (*DestroyDetached)(void* Container) = nullptr;
		EContainerOpResult (*Commit)(void* Destination, void* Detached) = nullptr;
	};

	struct FMapOps
	{
		uint32 Version = ContainerOpsVersion;
		EMapOpsFlags Flags = EMapOpsFlags::None;
		uint32 ContainerSize = 0;
		uint32 ContainerAlignment = 0;
		void (*Initialize)(void* Container) = nullptr;
		void (*Destroy)(void* Container) = nullptr;
		uint64 (*Num)(const void* Container) = nullptr;
		EContainerOpResult (*VisitConst)(const void* Container, FMapConstVisitor Visitor, void* Context) = nullptr;
		EContainerOpResult (*VisitMutable)(void* Container, FMapMutableVisitor Visitor, void* Context) = nullptr;
		EContainerOpResult (*Lookup)(const void* Container, const void* Key, const void** OutValue) = nullptr;
		EContainerOpResult (*LookupMutable)(void* Container, const void* Key, void** OutValue) = nullptr;
		EContainerOpResult (*Clear)(void* Container) = nullptr;
		EContainerOpResult (*Reserve)(void* Container, uint64 Count) = nullptr;
		EContainerOpResult (*InsertCopy)(void* Container, const void* Key, const void* Value) = nullptr;
		EContainerOpResult (*Remove)(void* Container, const void* Key) = nullptr;
		EContainerOpResult (*RenameKey)(void* Container, const void* OldKey, const void* NewKey) = nullptr;
		EContainerOpResult (*CreateDetached)(void** OutContainer) = nullptr;
		void (*DestroyDetached)(void* Container) = nullptr;
		EContainerOpResult (*Commit)(void* Destination, void* Detached) = nullptr;
	};

	inline auto IsValidArrayOps(const FArrayOps* Ops) -> bool
	{
		if (!Ops || Ops->Version != ContainerOpsVersion || Ops->ContainerSize == 0
			|| Ops->ContainerAlignment == 0 || (Ops->ContainerAlignment & (Ops->ContainerAlignment - 1)) != 0
			|| !Ops->Initialize || !Ops->Destroy)
		{
			return false;
		}
		auto Matches = [Ops](EArrayOpsFlags Flag, auto Callback)
		{
			return EnumHasAnyFlags(Ops->Flags, Flag) == (Callback != nullptr);
		};
		return Matches(EArrayOpsFlags::Count, Ops->Num)
			&& Matches(EArrayOpsFlags::ConstTraversal, Ops->VisitConst)
			&& Matches(EArrayOpsFlags::MutableTraversal, Ops->VisitMutable)
			&& (EnumHasAnyFlags(Ops->Flags, EArrayOpsFlags::RandomAccess) == (Ops->GetConstAt && Ops->GetMutableAt))
			&& Matches(EArrayOpsFlags::Clear, Ops->Clear)
			&& Matches(EArrayOpsFlags::Reserve, Ops->Reserve)
			&& (EnumHasAnyFlags(Ops->Flags, EArrayOpsFlags::DefaultGrow | EArrayOpsFlags::Shrink) == (Ops->Resize != nullptr))
			&& (EnumHasAnyFlags(Ops->Flags, EArrayOpsFlags::DetachedStorage) == (Ops->CreateDetached && Ops->DestroyDetached))
			&& Matches(EArrayOpsFlags::TransactionalCommit, Ops->Commit);
	}

	inline auto IsValidMapOps(const FMapOps* Ops) -> bool
	{
		if (!Ops || Ops->Version != ContainerOpsVersion || Ops->ContainerSize == 0
			|| Ops->ContainerAlignment == 0 || (Ops->ContainerAlignment & (Ops->ContainerAlignment - 1)) != 0
			|| !Ops->Initialize || !Ops->Destroy)
		{
			return false;
		}
		auto Matches = [Ops](EMapOpsFlags Flag, auto Callback)
		{
			return EnumHasAnyFlags(Ops->Flags, Flag) == (Callback != nullptr);
		};
		return Matches(EMapOpsFlags::Count, Ops->Num)
			&& Matches(EMapOpsFlags::ConstTraversal, Ops->VisitConst)
			&& Matches(EMapOpsFlags::MutableMappedTraversal, Ops->VisitMutable)
			&& Matches(EMapOpsFlags::Lookup, Ops->Lookup)
			&& Matches(EMapOpsFlags::MutableLookup, Ops->LookupMutable)
			&& Matches(EMapOpsFlags::Clear, Ops->Clear)
			&& Matches(EMapOpsFlags::Reserve, Ops->Reserve)
			&& Matches(EMapOpsFlags::Insert, Ops->InsertCopy)
			&& Matches(EMapOpsFlags::Remove, Ops->Remove)
			&& Matches(EMapOpsFlags::RenameKey, Ops->RenameKey)
			&& (EnumHasAnyFlags(Ops->Flags, EMapOpsFlags::DetachedStorage) == (Ops->CreateDetached && Ops->DestroyDetached))
			&& Matches(EMapOpsFlags::TransactionalCommit, Ops->Commit);
	}

	class FDetachedContainerStorage
	{
	public:
		FDetachedContainerStorage() = default;
		FDetachedContainerStorage(const FDetachedContainerStorage&) = delete;
		auto operator=(const FDetachedContainerStorage&) -> FDetachedContainerStorage& = delete;
		FDetachedContainerStorage(FDetachedContainerStorage&& Other) noexcept
			: Container(std::exchange(Other.Container, nullptr)), Destroy(std::exchange(Other.Destroy, nullptr)) {}
		auto operator=(FDetachedContainerStorage&& Other) noexcept -> FDetachedContainerStorage&
		{
			if (this != &Other)
			{
				Reset();
				Container = std::exchange(Other.Container, nullptr);
				Destroy = std::exchange(Other.Destroy, nullptr);
			}
			return *this;
		}
		~FDetachedContainerStorage() { Reset(); }

		auto Create(const FArrayOps& Ops) -> EContainerOpResult { return Create(Ops.CreateDetached, Ops.DestroyDetached); }
		auto Create(const FMapOps& Ops) -> EContainerOpResult { return Create(Ops.CreateDetached, Ops.DestroyDetached); }
		auto Get() const -> void* { return Container; }
		auto Reset() -> void
		{
			if (Container && Destroy) Destroy(Container);
			Container = nullptr;
			Destroy = nullptr;
		}

	private:
		auto Create(EContainerOpResult (*CreateFn)(void**), void (*DestroyFn)(void*)) -> EContainerOpResult
		{
			Reset();
			if (!CreateFn || !DestroyFn) return EContainerOpResult::Unsupported;
			const EContainerOpResult Result = CreateFn(&Container);
			if (Result == EContainerOpResult::Success) Destroy = DestroyFn;
			else Container = nullptr;
			return Result;
		}

		void* Container = nullptr;
		void (*Destroy)(void*) = nullptr;
	};

	namespace Private
	{
		template<typename Container>
		auto InitializeContainer(void* Value) -> void { std::construct_at(static_cast<Container*>(Value)); }
		template<typename Container>
		auto DestroyContainer(void* Value) -> void { std::destroy_at(static_cast<Container*>(Value)); }
		template<typename Container>
		auto CreateDetachedContainer(void** OutContainer) -> EContainerOpResult
		{
			if (!OutContainer) return EContainerOpResult::InvalidInput;
			*OutContainer = nullptr;
			try { *OutContainer = new (std::nothrow) Container(); }
			catch (const std::bad_alloc&) { return EContainerOpResult::AllocationFailure; }
			catch (...) { return EContainerOpResult::ConstructionFailure; }
			return *OutContainer ? EContainerOpResult::Success : EContainerOpResult::AllocationFailure;
		}
		template<typename Container>
		auto DestroyDetachedContainer(void* ContainerValue) -> void { delete static_cast<Container*>(ContainerValue); }

		template<typename Container>
		auto ContainerNum(const void* Value) -> uint64 { return static_cast<uint64>(static_cast<const Container*>(Value)->size()); }

		template<typename Container>
		auto ReserveContainer(void* Value, uint64 Count) -> EContainerOpResult
		{
			if (!Value || Count > static_cast<uint64>(std::numeric_limits<typename Container::size_type>::max()))
				return EContainerOpResult::InvalidInput;
			try { static_cast<Container*>(Value)->reserve(static_cast<typename Container::size_type>(Count)); }
			catch (const std::bad_alloc&) { return EContainerOpResult::AllocationFailure; }
			catch (...) { return EContainerOpResult::BackendRejected; }
			return EContainerOpResult::Success;
		}

		template<typename Container>
		auto ClearContainer(void* Value) -> EContainerOpResult
		{
			if (!Value) return EContainerOpResult::InvalidInput;
			static_cast<Container*>(Value)->clear();
			return EContainerOpResult::Success;
		}

		template<typename T, typename Allocator>
		struct TVectorOps
		{
			using Container = std::vector<T, Allocator>;

			static auto VisitConst(const void* Value, FArrayConstVisitor Visitor, void* Context) -> EContainerOpResult
			{
				if (!Value || !Visitor) return EContainerOpResult::InvalidInput;
				uint64 Index = 0;
				for (const T& Element : *static_cast<const Container*>(Value))
					if (!Visitor(Context, Index++, std::addressof(Element))) break;
				return EContainerOpResult::Success;
			}
			static auto VisitMutable(void* Value, FArrayMutableVisitor Visitor, void* Context) -> EContainerOpResult
			{
				if (!Value || !Visitor) return EContainerOpResult::InvalidInput;
				uint64 Index = 0;
				for (T& Element : *static_cast<Container*>(Value))
					if (!Visitor(Context, Index++, std::addressof(Element))) break;
				return EContainerOpResult::Success;
			}
			static auto GetConstAt(const void* Value, uint64 Index, const void** OutElement) -> EContainerOpResult
			{
				if (!Value || !OutElement) return EContainerOpResult::InvalidInput;
				const Container& Vector = *static_cast<const Container*>(Value);
				if (Index >= Vector.size()) return EContainerOpResult::OutOfRange;
				*OutElement = std::addressof(Vector[static_cast<typename Container::size_type>(Index)]);
				return EContainerOpResult::Success;
			}
			static auto GetMutableAt(void* Value, uint64 Index, void** OutElement) -> EContainerOpResult
			{
				if (!Value || !OutElement) return EContainerOpResult::InvalidInput;
				Container& Vector = *static_cast<Container*>(Value);
				if (Index >= Vector.size()) return EContainerOpResult::OutOfRange;
				*OutElement = std::addressof(Vector[static_cast<typename Container::size_type>(Index)]);
				return EContainerOpResult::Success;
			}
			static auto Resize(void* Value, uint64 Count) -> EContainerOpResult
			{
				if (!Value || Count > static_cast<uint64>(std::numeric_limits<typename Container::size_type>::max()))
					return EContainerOpResult::InvalidInput;
				Container& Vector = *static_cast<Container*>(Value);
				const auto Target = static_cast<typename Container::size_type>(Count);
				while (Vector.size() > Target) Vector.pop_back();
				if constexpr (std::is_default_constructible_v<T>
					&& (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>))
				{
					const auto Original = Vector.size();
					try { while (Vector.size() < Target) Vector.emplace_back(); }
					catch (const std::bad_alloc&) { while (Vector.size() > Original) Vector.pop_back(); return EContainerOpResult::AllocationFailure; }
					catch (...) { while (Vector.size() > Original) Vector.pop_back(); return EContainerOpResult::ConstructionFailure; }
					return EContainerOpResult::Success;
				}
				return Vector.size() == Target ? EContainerOpResult::Success : EContainerOpResult::Unsupported;
			}
			static auto Commit(void* Destination, void* Detached) -> EContainerOpResult
			{
				if (!Destination || !Detached) return EContainerOpResult::InvalidInput;
				static_cast<Container*>(Destination)->swap(*static_cast<Container*>(Detached));
				return EContainerOpResult::Success;
			}
		};

		template<typename K, typename V, typename Hash, typename Equal, typename Allocator>
		struct TUnorderedMapOps
		{
			using Container = std::unordered_map<K, V, Hash, Equal, Allocator>;
			static auto VisitConst(const void* Value, FMapConstVisitor Visitor, void* Context) -> EContainerOpResult
			{
				if (!Value || !Visitor) return EContainerOpResult::InvalidInput;
				for (const auto& [Key, Mapped] : *static_cast<const Container*>(Value))
					if (!Visitor(Context, std::addressof(Key), std::addressof(Mapped))) break;
				return EContainerOpResult::Success;
			}
			static auto VisitMutable(void* Value, FMapMutableVisitor Visitor, void* Context) -> EContainerOpResult
			{
				if (!Value || !Visitor) return EContainerOpResult::InvalidInput;
				for (auto& [Key, Mapped] : *static_cast<Container*>(Value))
					if (!Visitor(Context, std::addressof(Key), std::addressof(Mapped))) break;
				return EContainerOpResult::Success;
			}
			static auto Lookup(const void* Value, const void* Key, const void** OutValue) -> EContainerOpResult
			{
				if (!Value || !Key || !OutValue) return EContainerOpResult::InvalidInput;
				const Container& Map = *static_cast<const Container*>(Value);
				const auto It = Map.find(*static_cast<const K*>(Key));
				if (It == Map.end()) return EContainerOpResult::NotFound;
				*OutValue = std::addressof(It->second);
				return EContainerOpResult::Success;
			}
			static auto LookupMutable(void* Value, const void* Key, void** OutValue) -> EContainerOpResult
			{
				if (!Value || !Key || !OutValue) return EContainerOpResult::InvalidInput;
				Container& Map = *static_cast<Container*>(Value);
				const auto It = Map.find(*static_cast<const K*>(Key));
				if (It == Map.end()) return EContainerOpResult::NotFound;
				*OutValue = std::addressof(It->second);
				return EContainerOpResult::Success;
			}
			static auto InsertCopy(void* Value, const void* Key, const void* Mapped) -> EContainerOpResult
				requires std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>
			{
				if (!Value || !Key || !Mapped) return EContainerOpResult::InvalidInput;
				try
				{
					const auto [It, bInserted] = static_cast<Container*>(Value)->emplace(*static_cast<const K*>(Key), *static_cast<const V*>(Mapped));
					(void)It;
					return bInserted ? EContainerOpResult::Success : EContainerOpResult::DuplicateKey;
				}
				catch (const std::bad_alloc&) { return EContainerOpResult::AllocationFailure; }
				catch (...) { return EContainerOpResult::ConstructionFailure; }
			}
			static auto Remove(void* Value, const void* Key) -> EContainerOpResult
			{
				if (!Value || !Key) return EContainerOpResult::InvalidInput;
				return static_cast<Container*>(Value)->erase(*static_cast<const K*>(Key)) == 1
					? EContainerOpResult::Success : EContainerOpResult::NotFound;
			}
			static auto RenameKey(void* Value, const void* OldKey, const void* NewKey) -> EContainerOpResult
				requires std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>
			{
				if (!Value || !OldKey || !NewKey) return EContainerOpResult::InvalidInput;
				Container& Map = *static_cast<Container*>(Value);
				const K& Old = *static_cast<const K*>(OldKey);
				const K& New = *static_cast<const K*>(NewKey);
				const auto OldIt = Map.find(Old);
				if (OldIt == Map.end()) return EContainerOpResult::NotFound;
				if (Map.key_eq()(Old, New)) return EContainerOpResult::EquivalentKey;
				if (Map.find(New) != Map.end()) return EContainerOpResult::DuplicateKey;
				try
				{
					const auto [NewIt, bInserted] = Map.emplace(New, OldIt->second);
					if (!bInserted) return EContainerOpResult::DuplicateKey;
					(void)NewIt;
					Map.erase(OldIt);
					return EContainerOpResult::Success;
				}
				catch (const std::bad_alloc&) { return EContainerOpResult::AllocationFailure; }
				catch (...) { return EContainerOpResult::ConstructionFailure; }
			}
			static auto Commit(void* Destination, void* Detached) -> EContainerOpResult
			{
				if (!Destination || !Detached) return EContainerOpResult::InvalidInput;
				static_cast<Container*>(Destination)->swap(*static_cast<Container*>(Detached));
				return EContainerOpResult::Success;
			}
		};
	}

	template<typename Container>
	struct TArrayOpsResolver;

	template<typename T>
	struct TArrayOpsResolver<std::vector<T>>
	{
		static auto Get() -> const FArrayOps*
		{
			using Container = std::vector<T>;
			using Adapter = Private::TVectorOps<T, std::allocator<T>>;
			static const FArrayOps Ops = []
			{
				FArrayOps Result;
				Result.Flags = EArrayOpsFlags::Count | EArrayOpsFlags::ConstTraversal | EArrayOpsFlags::MutableTraversal
					| EArrayOpsFlags::RandomAccess | EArrayOpsFlags::Clear | EArrayOpsFlags::Shrink
					| EArrayOpsFlags::DetachedStorage;
				if constexpr (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>) Result.Flags |= EArrayOpsFlags::Reserve;
				if constexpr (std::is_default_constructible_v<T>
					&& (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>)) Result.Flags |= EArrayOpsFlags::DefaultGrow;
				if constexpr (noexcept(std::declval<Container&>().swap(std::declval<Container&>())))
					Result.Flags |= EArrayOpsFlags::TransactionalCommit;
				Result.ContainerSize = sizeof(Container);
				Result.ContainerAlignment = alignof(Container);
				Result.Initialize = &Private::InitializeContainer<Container>;
				Result.Destroy = &Private::DestroyContainer<Container>;
				Result.Num = &Private::ContainerNum<Container>;
				Result.VisitConst = &Adapter::VisitConst;
				Result.VisitMutable = &Adapter::VisitMutable;
				Result.GetConstAt = &Adapter::GetConstAt;
				Result.GetMutableAt = &Adapter::GetMutableAt;
				Result.Clear = &Private::ClearContainer<Container>;
				if constexpr (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>) Result.Reserve = &Private::ReserveContainer<Container>;
				Result.Resize = &Adapter::Resize;
				Result.CreateDetached = &Private::CreateDetachedContainer<Container>;
				Result.DestroyDetached = &Private::DestroyDetachedContainer<Container>;
				if constexpr (noexcept(std::declval<Container&>().swap(std::declval<Container&>()))) Result.Commit = &Adapter::Commit;
				return Result;
			}();
			return &Ops;
		}
	};

	template<typename Container>
	auto ResolveArrayOps() -> const FArrayOps* { return TArrayOpsResolver<Container>::Get(); }

	template<typename Container>
	struct TMapOpsResolver;

	template<typename K, typename V>
	struct TMapOpsResolver<std::unordered_map<K, V>>
	{
		static auto Get() -> const FMapOps*
		{
			using Container = std::unordered_map<K, V>;
			using Adapter = Private::TUnorderedMapOps<K, V, std::hash<K>, std::equal_to<K>, std::allocator<std::pair<const K, V>>>;
			static const FMapOps Ops = []
			{
				FMapOps Result;
				Result.Flags = EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal | EMapOpsFlags::MutableMappedTraversal
					| EMapOpsFlags::Clear | EMapOpsFlags::Reserve | EMapOpsFlags::Lookup | EMapOpsFlags::MutableLookup
					| EMapOpsFlags::Remove | EMapOpsFlags::DetachedStorage;
				if constexpr (std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>)
					Result.Flags |= EMapOpsFlags::Insert | EMapOpsFlags::RenameKey;
				if constexpr (noexcept(std::declval<Container&>().swap(std::declval<Container&>())))
					Result.Flags |= EMapOpsFlags::TransactionalCommit;
				Result.ContainerSize = sizeof(Container);
				Result.ContainerAlignment = alignof(Container);
				Result.Initialize = &Private::InitializeContainer<Container>;
				Result.Destroy = &Private::DestroyContainer<Container>;
				Result.Num = &Private::ContainerNum<Container>;
				Result.VisitConst = &Adapter::VisitConst;
				Result.VisitMutable = &Adapter::VisitMutable;
				Result.Lookup = &Adapter::Lookup;
				Result.LookupMutable = &Adapter::LookupMutable;
				Result.Clear = &Private::ClearContainer<Container>;
				Result.Reserve = &Private::ReserveContainer<Container>;
				if constexpr (std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>)
				{
					Result.InsertCopy = &Adapter::InsertCopy;
					Result.RenameKey = &Adapter::RenameKey;
				}
				Result.Remove = &Adapter::Remove;
				Result.CreateDetached = &Private::CreateDetachedContainer<Container>;
				Result.DestroyDetached = &Private::DestroyDetachedContainer<Container>;
				if constexpr (noexcept(std::declval<Container&>().swap(std::declval<Container&>()))) Result.Commit = &Adapter::Commit;
				return Result;
			}();
			return &Ops;
		}
	};

	template<typename Container>
	auto ResolveMapOps() -> const FMapOps* { return TMapOpsResolver<Container>::Get(); }
}

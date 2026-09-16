#pragma once

#include "DObject/Object.h"
#include <compare>

namespace Durin
{
	template<typename T, typename = void>
	struct TIsCompleteType : std::false_type {};
	template<typename T>
	struct TIsCompleteType<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};
	class FArchive;
	// Process-local identity, independent of pointer representation and liveness.
	// Capture/resolve on the game thread; independent copies may be compared on workers.
	// Never serialize this identity into an asset or package.
	class FObjectKey
	{
	public:
		FObjectKey() = default;
		FObjectKey(std::nullptr_t) {}
		COREDOBJECT_API explicit FObjectKey(const DObject* Object);
		auto IsNull() const -> bool { return Index == std::numeric_limits<uint32>::max(); }
		COREDOBJECT_API auto ResolveObjectPtr() const -> DObject*;
		// In-memory transaction snapshots only; never a persistent identity format.
		COREDOBJECT_API auto SerializeForSnapshot(FArchive& Archive) -> void;
		auto GetHash() const noexcept -> size_t
		{
			return std::hash<uint64>{}((static_cast<uint64>(Index) << 32) | Generation);
		}
		friend auto operator<=>(const FObjectKey&, const FObjectKey&) = default;

	private:
		uint32 Index = std::numeric_limits<uint32>::max();
		uint32 Generation = 0;
		friend class FDObjectArray;
		friend class FWeakObjectPtr;
		friend class FObjectPtr;
	};

	static_assert(sizeof(FObjectKey) == sizeof(uint64));
	static_assert(std::is_trivially_copyable_v<FObjectKey>);

	struct FObjectKeyHash
	{
		auto operator()(const FObjectKey& Key) const noexcept -> size_t { return Key.GetHash(); }
	};
	inline auto ResolveObjectKey(FObjectKey Key) -> DObject* { return Key.ResolveObjectPtr(); }
	inline auto IsObjectKeyNull(FObjectKey Key) -> bool { return Key.IsNull(); }

	template<typename T>
	class TObjectKey
	{
	public:
		TObjectKey() = default;
		TObjectKey(std::nullptr_t) {}
		explicit TObjectKey(const T* Object) : Key(ToDObject(Object)) {}
		explicit TObjectKey(FObjectKey InKey) : Key(InKey) {}
		auto GetKey() const -> FObjectKey { return Key; }
		auto IsNull() const -> bool { return Key.IsNull(); }
		auto GetHash() const noexcept -> size_t { return Key.GetHash(); }
		auto ResolveObjectPtr() const -> T*
		{
			if constexpr (TIsCompleteType<T>::value)
			{
				static_assert(std::is_base_of_v<DObject, T>);
				return static_cast<T*>(Key.ResolveObjectPtr());
			}
			else return reinterpret_cast<T*>(Key.ResolveObjectPtr());
		}
		friend auto operator<=>(const TObjectKey&, const TObjectKey&) = default;
	private:
		static auto ToDObject(const T* Object) -> const DObject*
		{
			if constexpr (TIsCompleteType<T>::value)
			{
				static_assert(std::is_base_of_v<DObject, T>);
				return static_cast<const DObject*>(Object);
			}
			else return reinterpret_cast<const DObject*>(Object);
		}
		FObjectKey Key;
	};
}

template<> struct std::hash<Durin::FObjectKey> : Durin::FObjectKeyHash {};
template<typename T> struct std::hash<Durin::TObjectKey<T>>
{
	auto operator()(const Durin::TObjectKey<T>& Key) const noexcept -> size_t { return Key.GetHash(); }
};

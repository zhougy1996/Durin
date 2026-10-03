#pragma once

#include "Misc/CoreTypes.h"
#include "Containers/ContainersFwd.h"

#include "Misc/CoreStd.h"

#include "CoreAPI.h"
#include <typeinfo>

namespace Durin
{
	// Shares an immutable contiguous byte allocation between payload owners.
	class FSharedByteBuffer
	{
	public:
		FSharedByteBuffer() = default;

		CORE_API static auto Copy(FByteView Bytes) -> FSharedByteBuffer;
		CORE_API static auto Take(FByteBuffer Bytes) -> FSharedByteBuffer;
		static auto Share(std::shared_ptr<const FByteBuffer> Bytes)
			-> FSharedByteBuffer
		{
			return FSharedByteBuffer(std::move(Bytes));
		}

		auto GetBytes() const -> FByteView
		{
			return Storage && Size ? FByteView(Storage.get() + Offset, Size) : FByteView();
		}
		auto GetSize() const -> uint64 { return static_cast<uint64>(GetBytes().size()); }
		// Full backing allocation capacity, including bytes outside a retained subview.
		// Excludes owner/control-block overhead; logical wire budgets continue to use GetSize.
		auto GetRetainedCapacityBytes() const -> uint64 { return Storage.use_count() ? CapacityBytes : 0; }
		auto size() const -> size_t { return GetBytes().size(); }
		auto data() const -> const std::byte* { return GetBytes().data(); }
		auto begin() const -> const std::byte* { return GetBytes().data(); }
		auto end() const -> const std::byte* { return GetBytes().data() + GetBytes().size(); }
		auto operator[](size_t Index) const -> const std::byte& { return GetBytes()[Index]; }
		operator FByteView() const { return GetBytes(); }
		auto IsEmpty() const -> bool { return GetBytes().empty(); }
		auto SharesStorageWith(const FSharedByteBuffer& Other) const -> bool
		{
			return !Storage.owner_before(Other.Storage) && !Other.Storage.owner_before(Storage);
		}
		auto MakeView(uint64 InOffset, uint64 InSize) const -> FSharedByteBuffer
		{
			if (Storage.use_count() == 0 || InOffset > Size || InSize > Size - InOffset) return {};
			auto Result = *this;
			Result.Offset += static_cast<size_t>(InOffset);
			Result.Size = static_cast<size_t>(InSize);
			return Result;
		}

		// Native provenance is local allocation information, never a serialized type tag.
		// Byte buffers loaded from archives cannot acquire it by requesting a typed view.
		template<typename T> requires (std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
		static auto TakeNative(std::vector<T> Values) -> FSharedByteBuffer
		{
			auto Owner = std::make_shared<const std::vector<T>>(std::move(Values));
			FSharedByteBuffer Result;
			Result.Size = Owner->size() * sizeof(T);
			Result.CapacityBytes = Owner->capacity() * sizeof(T);
			Result.NativeType = &typeid(T);
			Result.Storage = std::shared_ptr<const std::byte>(Owner,
				reinterpret_cast<const std::byte*>(Owner->data()));
			return Result;
		}
		template<typename T> requires (std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
		auto GetNativeView() const -> std::optional<std::span<const T>>
		{
			if (!NativeType || Storage.use_count() == 0 || *NativeType != typeid(T) || Offset % sizeof(T) || Size % sizeof(T))
				return std::nullopt;
			if (!Size) return std::span<const T>{};
			const auto* Bytes = Storage.get() + Offset;
			if (reinterpret_cast<uintptr_t>(Bytes) % alignof(T)) return std::nullopt;
			return std::span<const T>(reinterpret_cast<const T*>(Bytes), Size / sizeof(T));
		}

	private:
		explicit FSharedByteBuffer(std::shared_ptr<const FByteBuffer> InStorage)
			: Size(InStorage ? InStorage->size() : 0), CapacityBytes(InStorage ? InStorage->capacity() : 0)
		{
			if (InStorage) Storage = std::shared_ptr<const std::byte>(InStorage, InStorage->data());
		}

		size_t Offset = 0;
		size_t Size = 0;
		size_t CapacityBytes = 0;
		std::shared_ptr<const std::byte> Storage;
		const std::type_info* NativeType = nullptr;
	};
}

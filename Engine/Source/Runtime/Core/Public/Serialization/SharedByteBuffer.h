#pragma once

#include "CoreAPI.h"

#include <memory>
#include <span>
#include <vector>

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
			return Storage ? FByteView(*Storage).subspan(Offset, Size)
				: FByteView();
		}
		auto GetSize() const -> uint64 { return static_cast<uint64>(GetBytes().size()); }
		auto size() const -> size_t { return GetBytes().size(); }
		auto data() const -> const std::byte* { return GetBytes().data(); }
		auto begin() const -> const std::byte* { return GetBytes().data(); }
		auto end() const -> const std::byte* { return GetBytes().data() + GetBytes().size(); }
		auto operator[](size_t Index) const -> const std::byte& { return GetBytes()[Index]; }
		operator FByteView() const { return GetBytes(); }
		auto IsEmpty() const -> bool { return GetBytes().empty(); }
		auto SharesStorageWith(const FSharedByteBuffer& Other) const -> bool
		{
			return Storage == Other.Storage;
		}
		auto MakeView(uint64 InOffset, uint64 InSize) const -> FSharedByteBuffer
		{
			if (!Storage || InOffset > Size || InSize > Size - InOffset) return {};
			return FSharedByteBuffer(Storage, Offset + static_cast<size_t>(InOffset),
				static_cast<size_t>(InSize));
		}

	private:
		explicit FSharedByteBuffer(std::shared_ptr<const FByteBuffer> InStorage)
			: Size(InStorage ? InStorage->size() : 0), Storage(std::move(InStorage))
		{
		}
		FSharedByteBuffer(std::shared_ptr<const FByteBuffer> InStorage,
			size_t InOffset, size_t InSize)
			: Offset(InOffset), Size(InSize), Storage(std::move(InStorage))
		{
		}

		size_t Offset = 0;
		size_t Size = 0;
		std::shared_ptr<const FByteBuffer> Storage;
	};
}

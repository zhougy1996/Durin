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

		CORE_API static auto Copy(std::span<const std::byte> Bytes) -> FSharedByteBuffer;
		CORE_API static auto Take(FByteArray Bytes) -> FSharedByteBuffer;
		static auto Share(std::shared_ptr<const FByteArray> Bytes)
			-> FSharedByteBuffer
		{
			return FSharedByteBuffer(std::move(Bytes));
		}

		auto GetBytes() const -> std::span<const std::byte>
		{
			return Storage ? std::span<const std::byte>(*Storage) : std::span<const std::byte>();
		}
		auto GetSize() const -> uint64 { return static_cast<uint64>(GetBytes().size()); }
		auto size() const -> size_t { return GetBytes().size(); }
		auto data() const -> const std::byte* { return GetBytes().data(); }
		auto begin() const -> const std::byte* { return GetBytes().data(); }
		auto end() const -> const std::byte* { return GetBytes().data() + GetBytes().size(); }
		auto operator[](size_t Index) const -> const std::byte& { return GetBytes()[Index]; }
		operator std::span<const std::byte>() const { return GetBytes(); }
		auto IsEmpty() const -> bool { return GetBytes().empty(); }
		auto SharesStorageWith(const FSharedByteBuffer& Other) const -> bool
		{
			return Storage == Other.Storage;
		}

	private:
		explicit FSharedByteBuffer(std::shared_ptr<const FByteArray> InStorage)
			: Storage(std::move(InStorage))
		{
		}

		std::shared_ptr<const FByteArray> Storage;
	};
}

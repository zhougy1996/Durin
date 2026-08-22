#pragma once

#include "Misc/Guid.h"
#include "Templates/CheckedArithmetic.h"

namespace Durin::Asset::BulkContainer
{
	using Durin::IsPowerOfTwo;
	using Durin::TryAdd;
	using Durin::TryAlignUp;
	using Durin::TryMultiply;
	using Durin::TryNarrowSize;

	// Categorizes private physical-container failures without owning format diagnostics.
	enum class EFailure
	{
		None,
		InvalidArgument,
		LimitExceeded,
		ArithmeticOverflow,
		Truncated,
		NonzeroPadding,
		TrailingNonzeroPadding,
		DuplicateOrUnsortedKey,
		InvalidLayout,
		TrailingBytes
	};

	struct FFailure
	{
		EFailure Category = EFailure::None;
		uint64 Offset = 0;
	};

	// Reads exact little-endian values from a non-owning bounded span with latched failure.
	class FBoundedReader
	{
	public:
		explicit FBoundedReader(std::span<const std::byte> InBytes, uint64 MaximumBytes)
			: Bytes(InBytes)
		{
			if (InBytes.size() > MaximumBytes)
				Latch(EFailure::LimitExceeded, 0);
		}

		template<typename T>
		auto Read(T& OutValue) -> bool
		{
			static_assert(std::is_unsigned_v<T>);
			std::span<const std::byte> Raw;
			if (!ReadBytes(sizeof(T), Raw)) return false;
			T Candidate = 0;
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Candidate |= static_cast<T>(std::to_integer<uint8>(Raw[Index])) << (Index * 8);
			OutValue = Candidate;
			return true;
		}

		auto ReadGuid(FGuid& OutValue) -> bool
		{
			std::span<const std::byte> Raw;
			if (!ReadBytes(16, Raw)) return false;
			auto ReadWord = [&](size_t Begin) {
				uint32 Value = 0;
				for (size_t Index = 0; Index < sizeof(uint32); ++Index)
					Value |= static_cast<uint32>(std::to_integer<uint8>(Raw[Begin + Index])) << (Index * 8);
				return Value;
			};
			const FGuid Candidate{
				ReadWord(0), ReadWord(4), ReadWord(8), ReadWord(12)};
			OutValue = Candidate;
			return true;
		}

		auto ReadBytes(uint64 Size, std::span<const std::byte>& OutValue) -> bool
		{
			if (!IsValid()) return false;
			uint64 End = 0;
			if (!TryAdd(Cursor, Size, Bytes.size(), End))
			{
				Latch(EFailure::Truncated, Cursor);
				return false;
			}
			size_t Offset = 0, NarrowSize = 0;
			if (!TryNarrowSize(Cursor, Offset) || !TryNarrowSize(Size, NarrowSize))
			{
				Latch(EFailure::LimitExceeded, Cursor);
				return false;
			}
			const std::span<const std::byte> Candidate = Bytes.subspan(Offset, NarrowSize);
			Cursor = End;
			OutValue = Candidate;
			return true;
		}

		auto Tell() const -> uint64 { return Cursor; }
		auto Remaining() const -> uint64 { return Cursor <= Bytes.size() ? Bytes.size() - Cursor : 0; }
		auto IsValid() const -> bool { return Failure.Category == EFailure::None; }
		auto GetFailure() const -> FFailure { return Failure; }

	private:
		auto Latch(EFailure Category, uint64 Offset) -> void
		{
			if (Failure.Category == EFailure::None) Failure = {Category, Offset};
		}

		std::span<const std::byte> Bytes;
		uint64 Cursor = 0;
		FFailure Failure;
	};

	// Owns an unpublished bounded byte candidate until successful detached transfer.
	class FBoundedWriter
	{
	public:
		explicit FBoundedWriter(uint64 InMaximumBytes)
			: MaximumBytes(InMaximumBytes)
		{
		}

		template<typename T>
		requires std::is_unsigned_v<T>
		auto Write(T Value) -> bool
		{
			std::array<std::byte, sizeof(T)> Raw{};
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Raw[Index] = static_cast<std::byte>(Value >> (Index * 8));
			return Write(Raw);
		}

		auto WriteGuid(const FGuid& Value) -> bool
		{
			std::array<std::byte, 16> Raw{};
			const std::array Words{Value.A, Value.B, Value.C, Value.D};
			for (size_t Word = 0; Word < Words.size(); ++Word)
				for (size_t Byte = 0; Byte < sizeof(uint32); ++Byte)
					Raw[Word * sizeof(uint32) + Byte] = static_cast<std::byte>(
						Words[Word] >> (Byte * 8));
			return Write(Raw);
		}

		auto Write(std::span<const std::byte> Value) -> bool
		{
			if (!IsValid()) return false;
			uint64 End = 0;
			if (!TryAdd(Bytes.size(), Value.size(), MaximumBytes, End)
				|| End > std::numeric_limits<size_t>::max())
			{
				Latch(EFailure::LimitExceeded, Bytes.size());
				return false;
			}
			const size_t Begin = Bytes.size();
			Bytes.resize(static_cast<size_t>(End));
			if (!Value.empty())
				std::memcpy(Bytes.data() + Begin, Value.data(), Value.size());
			return true;
		}

		auto PadTo(uint64 Offset) -> bool
		{
			if (!IsValid()) return false;
			if (Offset < Bytes.size())
			{
				Latch(EFailure::InvalidArgument, Bytes.size());
				return false;
			}
			size_t NarrowOffset = 0;
			if (Offset > MaximumBytes || !TryNarrowSize(Offset, NarrowOffset))
			{
				Latch(EFailure::LimitExceeded, Bytes.size());
				return false;
			}
			Bytes.resize(NarrowOffset, std::byte{0});
			return true;
		}

		auto TryTake(std::vector<std::byte>& OutBytes) -> bool
		{
			if (!IsValid()) return false;
			OutBytes = std::move(Bytes);
			return true;
		}

		auto Tell() const -> uint64 { return Bytes.size(); }
		auto View() const -> std::span<const std::byte> { return Bytes; }
		auto IsValid() const -> bool { return Failure.Category == EFailure::None; }
		auto GetFailure() const -> FFailure { return Failure; }

	private:
		auto Latch(EFailure Category, uint64 Offset) -> void
		{
			if (Failure.Category == EFailure::None) Failure = {Category, Offset};
		}

		uint64 MaximumBytes = 0;
		std::vector<std::byte> Bytes;
		FFailure Failure;
	};

	template<typename T, typename Projection, typename Less = std::less<>>
	auto TryMakeSortedProjection(
		std::span<const T> Values,
		Projection Project,
		std::vector<const T*>& OutValues,
		Less Compare = {}) -> bool
	{
		std::vector<const T*> Candidate;
		Candidate.reserve(Values.size());
		for (const T& Value : Values) Candidate.push_back(&Value);
		std::ranges::stable_sort(Candidate, [&](const T* Left, const T* Right) {
			return Compare(std::invoke(Project, *Left), std::invoke(Project, *Right));
		});
		for (size_t Index = 1; Index < Candidate.size(); ++Index)
		{
			const auto& Left = std::invoke(Project, *Candidate[Index - 1]);
			const auto& Right = std::invoke(Project, *Candidate[Index]);
			if (!Compare(Left, Right)) return false;
		}
		OutValues = std::move(Candidate);
		return true;
	}

	struct FLayoutItem
	{
		uint64 Size = 0;
		uint64 Alignment = 1;
	};

	struct FPayloadRange
	{
		uint64 Offset = 0;
		uint64 Size = 0;
		uint64 Alignment = 1;
	};

	// Supplies format-owned bounds and compatibility choices to the neutral layout engine.
	struct FLayoutPolicy
	{
		uint64 MaximumCount = 0;
		uint64 MaximumPayloadBytes = 0;
		uint64 MaximumContainerBytes = 0;
		bool RequireCanonicalOffsets = true;
		bool AllowTrailingZeroPadding = false;
	};

	inline auto TryBuildLayout(
		uint64 DataOffset,
		std::span<const FLayoutItem> Items,
		const FLayoutPolicy& Policy,
		std::vector<FPayloadRange>& OutRanges,
		uint64& OutFileSize,
		FFailure* OutFailure = nullptr) -> bool
	{
		auto Fail = [&](EFailure Category, uint64 Offset) {
			if (OutFailure) *OutFailure = {Category, Offset};
			return false;
		};
		std::vector<FPayloadRange> Candidate;
		uint64 Cursor = DataOffset;
		if (Items.size() > Policy.MaximumCount || DataOffset > Policy.MaximumContainerBytes)
			return Fail(EFailure::LimitExceeded, DataOffset);
		Candidate.reserve(Items.size());
		for (const FLayoutItem& Item : Items)
		{
			uint64 Offset = 0, End = 0;
			if (!IsPowerOfTwo(Item.Alignment))
				return Fail(EFailure::InvalidArgument, Cursor);
			if (Item.Size > Policy.MaximumPayloadBytes)
				return Fail(EFailure::LimitExceeded, Cursor);
			if (Cursor > std::numeric_limits<uint64>::max() - (Item.Alignment - 1))
				return Fail(EFailure::ArithmeticOverflow, Cursor);
			if (!TryAlignUp(Cursor, Item.Alignment, Policy.MaximumContainerBytes, Offset))
				return Fail(EFailure::LimitExceeded, Cursor);
			if (Item.Size > std::numeric_limits<uint64>::max() - Offset)
				return Fail(EFailure::ArithmeticOverflow, Offset);
			if (!TryAdd(Offset, Item.Size, Policy.MaximumContainerBytes, End))
				return Fail(EFailure::LimitExceeded, Offset);
			Candidate.push_back({Offset, Item.Size, Item.Alignment});
			Cursor = End;
		}
		OutRanges = std::move(Candidate);
		OutFileSize = Cursor;
		if (OutFailure) *OutFailure = {};
		return true;
	}

	inline auto IsZeroRange(std::span<const std::byte> Bytes, uint64 Begin, uint64 End) -> bool
	{
		if (Begin > End || End > Bytes.size()) return false;
		for (uint64 Offset = Begin; Offset < End; ++Offset)
			if (Bytes[static_cast<size_t>(Offset)] != std::byte{0}) return false;
		return true;
	}

	inline auto TryProjectRange(
		std::span<const std::byte> Bytes,
		uint64 Offset,
		uint64 Size,
		std::span<const std::byte>& OutRange) -> bool
	{
		uint64 End = 0;
		if (!TryAdd(Offset, Size, Bytes.size(), End)) return false;
		size_t NarrowOffset = 0, NarrowSize = 0;
		if (!TryNarrowSize(Offset, NarrowOffset) || !TryNarrowSize(Size, NarrowSize)) return false;
		OutRange = Bytes.subspan(NarrowOffset, NarrowSize);
		return true;
	}

	inline auto ValidateLayout(
		std::span<const std::byte> Bytes,
		uint64 DirectoryEnd,
		uint64 DataOffset,
		std::span<const FPayloadRange> Ranges,
		const FLayoutPolicy& Policy,
		FFailure* OutFailure = nullptr) -> bool
	{
		auto Fail = [&](EFailure Category, uint64 Offset) {
			if (OutFailure) *OutFailure = {Category, Offset};
			return false;
		};
		if (Bytes.size() > Policy.MaximumContainerBytes || Ranges.size() > Policy.MaximumCount)
			return Fail(EFailure::LimitExceeded, 0);
		if (DirectoryEnd > DataOffset || DataOffset > Bytes.size())
			return Fail(EFailure::InvalidLayout, DirectoryEnd);
		if (!IsZeroRange(Bytes, DirectoryEnd, DataOffset))
			return Fail(EFailure::NonzeroPadding, DirectoryEnd);
		uint64 PreviousEnd = DataOffset;
		for (const FPayloadRange& Range : Ranges)
		{
			uint64 CanonicalOffset = 0, End = 0;
			if (Range.Size > Policy.MaximumPayloadBytes)
				return Fail(EFailure::LimitExceeded, Range.Offset);
			if (!IsPowerOfTwo(Range.Alignment))
				return Fail(EFailure::InvalidArgument, Range.Offset);
			if (!TryAlignUp(PreviousEnd, Range.Alignment, Bytes.size(), CanonicalOffset))
				return Fail(EFailure::InvalidLayout, Range.Offset);
			if (Range.Offset < CanonicalOffset)
				return Fail(EFailure::InvalidLayout, Range.Offset);
			if (Policy.RequireCanonicalOffsets && Range.Offset != CanonicalOffset)
				return Fail(EFailure::InvalidLayout, Range.Offset);
			if (!TryAdd(Range.Offset, Range.Size, Bytes.size(), End))
				return Fail(EFailure::InvalidLayout, Range.Offset);
			if (!IsZeroRange(Bytes, PreviousEnd, Range.Offset))
				return Fail(EFailure::NonzeroPadding, PreviousEnd);
			PreviousEnd = End;
		}
		if (PreviousEnd != Bytes.size())
		{
			if (!Policy.AllowTrailingZeroPadding)
				return Fail(EFailure::TrailingBytes, PreviousEnd);
			if (!IsZeroRange(Bytes, PreviousEnd, Bytes.size()))
				return Fail(EFailure::TrailingNonzeroPadding, PreviousEnd);
		}
		if (OutFailure) *OutFailure = {};
		return true;
	}
}

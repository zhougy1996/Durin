#pragma once
#include <cstring>

struct FClangKeepDebugInfo
{
};

inline constexpr uint32_t FNameMaxSize = 1024;

namespace Doge
{

	enum class ENameCase : uint8
	{
		CaseSensitive,
		IgnoreCase,
	};

	enum class EName : uint32
	{
		None = 0
	};

	struct FNameEntryId
	{
	public:
		FNameEntryId()
			: Value(0)
		{
		}
		explicit FNameEntryId(uint64 InValue)
			: Value(InValue)
		{
		}

		auto IsNone() const -> bool { return Value == 0; }

		auto GetValue() const -> uint64 { return Value; }

		auto ToInt() const -> uint32 { return Value; }

		auto operator==(const FNameEntryId& Other) const -> bool { return Value == Other.Value; }
		auto operator!=(const FNameEntryId& Other) const -> bool { return Value != Other.Value; }
		auto operator<(const FNameEntryId& Other) const -> bool { return Value < Other.Value; }
		auto operator>(const FNameEntryId& Other) const -> bool { return Value > Other.Value; }

		explicit operator bool() const { return Value != 0; }


	private:
		uint32 Value;
	};

	CORE_API uint64 GetTypeHash(FNameEntryId Id);

	struct FNameBuffer;

	struct FNameEntryHeader
	{
		uint16 Len = 0;
	};

	struct FNameEntry
	{
	private:
		FNameEntryId ComparisonId;

		FNameEntryHeader Header;

		union
		{
			U8Char AnsiName[FNameMaxSize];
			uint8 NameData[0];
		};

	public:
		FNameEntry() = default;

		[[nodiscard]] FORCEINLINE auto GetPlainNameString() const -> FString
		{
			return FString{&AnsiName[0], Header.Len};
		}

		static constexpr auto GetDataOffset() -> int32 { return offsetof(FNameEntry, NameData); }

		FORCEINLINE auto MakeView() const -> FU8StringView;

		[[nodiscard]] FORCEINLINE auto GetComparisonId() const -> FNameEntryId { return ComparisonId; }

		[[nodiscard]] FORCEINLINE auto GetHeader() const -> FNameEntryHeader { return Header; }

		[[nodiscard]] FORCEINLINE auto GetLength() const -> uint16 { return Header.Len; }

		[[nodiscard]] FORCEINLINE auto IsValid() const -> bool { return Header.Len > 0; }

	private:
		const U8Char* GetUnterminatedName() const;

		FNameEntry(FClangKeepDebugInfo);
		FNameEntry(const FNameEntry&) = delete;
		FNameEntry(FNameEntry&&) = delete;
		FNameEntry& operator=(const FNameEntry&) = delete;
		FNameEntry& operator=(FNameEntry&&) = delete;

		friend struct FNameHelper;
		friend struct FNamePool;
		friend class FNameEntryAllocator;
		friend class FNamePoolShardBase;
	};

	class FName
	{
	public:
		static constexpr uint32 MaxSize = FNameMaxSize;

		CORE_API FName();

		CORE_API FName(const U8Char* Name);

		CORE_API FName(const U8Char* Name, int32 InNumber);

		CORE_API FName(FU8StringView View, int32 InNumber);

		CORE_API FName(const FName& Other);

		[[nodiscard]] FORCEINLINE auto GetNumber() const -> uint32 { return Number; }

		[[nodiscard]] FORCEINLINE CORE_API auto Equals(const FName& Other, ENameCase CompareMethod = ENameCase::IgnoreCase, const bool bCompareNumber = true) const -> bool;

		[[nodiscard]] CORE_API auto ToString() const -> FString;

		[[nodiscard]] CORE_API auto GetComparisonNameEntry() const -> const FNameEntry*;

		[[nodiscard]] CORE_API auto GetDisplayNameEntry() const -> const FNameEntry*;

		[[nodiscard]] CORE_API static auto ResolveEntry(FNameEntryId LookupId) -> const FNameEntry*;

		[[nodiscard]] FORCEINLINE auto IsNone() const -> bool { return ComparisonIndex.IsNone() && Number == 0; }

		[[nodiscard]] friend FORCEINLINE auto GetTypeHash(FName Name) -> uint64
		{
			return GetTypeHash(Name.GetComparisonIndex()) + Name.GetNumber();
		}

		[[nodiscard]] FORCEINLINE auto operator==(const FName& Other) const -> bool
		{
			return ToUnstableInt() == Other.ToUnstableInt();
		}

		[[nodiscard]] FORCEINLINE bool operator!=(FName Other) const
		{
			return !(*this == Other);
		}

	private:
		static constexpr auto InValidNameCharacters = STR("\"' ,\n\r\t");

		static constexpr uint32 NoNumberInternal = 0;

		static inline constexpr auto NumberInternalToExternal(uint32 InternalNumber) -> uint32
		{
			return InternalNumber - 1;
		};

		static inline constexpr auto NumberExternalToInternal(int32 ExternalNumber) -> uint32
		{
			return ExternalNumber + 1;
		}

		[[nodiscard]] FORCEINLINE auto GetDisplayIndex() const -> FNameEntryId { return DisplayIndex; }

		[[nodiscard]] FORCEINLINE auto GetComparisonIndex() const -> FNameEntryId { return ComparisonIndex; }

		[[nodiscard]] FORCEINLINE auto ToUnstableInt() const -> uint64
		{
			static_assert(offsetof(FName, ComparisonIndex) == 0);
			static_assert(offsetof(FName, Number) == 4);
			static_assert((offsetof(FName, Number) + sizeof(Number)) == sizeof(uint64));

			uint64 Result;
			std::memcpy(&Result, this, sizeof(Result));
			return Result;
		}

	private:


		FNameEntryId ComparisonIndex;

		uint32 Number = 0;

		FNameEntryId DisplayIndex;

		friend struct FNameHash;
		friend struct FNameHelper;
	};

	CORE_API auto FNameInit() -> void;


}

template<>
struct std::hash<Doge::FName>
{
	size_t operator()(const Doge::FName& Name) const noexcept
	{
		return GetTypeHash(Name);
	}
};

struct FNameDebugVisualizer
{
	CORE_API explicit FNameDebugVisualizer(FClangKeepDebugInfo);
	CORE_API uint8_t** GetBlocks();

private:
	static constexpr uint32_t EntryStride = alignof(Doge::FNameEntry);
	static constexpr uint32_t OffsetBits = 16;
	static constexpr uint32_t BlockBits = 13;
	static constexpr uint32_t OffsetMask = (1 << OffsetBits) - 1;
	static constexpr uint32_t UnusedMask = UINT32_MAX << BlockBits << OffsetBits;
	static constexpr uint32_t MaxLength = FNameMaxSize;
};

extern uint8_t** GNameBlocksDebug;

// It is used in the natvis file for debugging purposes, each .exe or.dll owns this variable, so it is not shared between modules.
// It will be initialized only once, and it will not be changed during the program execution.
// So, it is safe to use it in the natvis file for debugging purposes.
#ifdef DOGE_VISUALIZERS_HELPERS
inline uint8_t** GNameBlocksDebug = FNameDebugVisualizer(FClangKeepDebugInfo{}).GetBlocks();
#endif // DOGE_VISUALIZERS_HELPERS

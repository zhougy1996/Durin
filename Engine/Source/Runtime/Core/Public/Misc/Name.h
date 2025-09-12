#pragma once

inline constexpr uint32 FNameMaxSize = 1024;

struct FClangKeepDebugInfo
{
};

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
		: Value_(0)
	{
	}
	explicit FNameEntryId(uint64 Value)
		: Value_(Value)
	{
	}

	auto IsNone() const -> bool { return Value_ == 0; }

	auto GetValue() const -> uint64 { return Value_; }

	auto ToInt() const -> uint32 { return Value_; }

	auto operator==(const FNameEntryId& Other) const -> bool { return Value_ == Other.Value_; }
	auto operator!=(const FNameEntryId& Other) const -> bool { return Value_ != Other.Value_; }
	auto operator<(const FNameEntryId& Other) const -> bool { return Value_ < Other.Value_; }
	auto operator>(const FNameEntryId& Other) const -> bool { return Value_ > Other.Value_; }

	explicit operator bool() const { return Value_ != 0; }


private:
	uint32 Value_; // TODO: 32 bits
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
		UTF8Char AnsiName[FNameMaxSize];
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
	const UTF8Char* GetUnterminatedName() const;

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

	CORE_API FName() = default;

	CORE_API FName(const UTF8Char* Name);

	CORE_API FName(const UTF8Char* Name, int32 Number);

	CORE_API FName(FU8StringView View, int32 Number);

	CORE_API FName(const FName& Other);

	[[nodiscard]] FORCEINLINE auto GetNumber() const -> uint32 { return Number_; }

	[[nodiscard]] FORCEINLINE CORE_API auto Equals(const FName& Other, ENameCase CompareMethod = ENameCase::IgnoreCase, const bool bCompareNumber = true) const -> bool;

	[[nodiscard]] CORE_API auto ToString() const -> FString;

	[[nodiscard]] CORE_API auto GetComparisonNameEntry() const -> const FNameEntry*;

	[[nodiscard]] CORE_API auto GetDisplayNameEntry() const -> const FNameEntry*;

	[[nodiscard]] CORE_API static auto ResolveEntry(FNameEntryId LookupId) -> const FNameEntry*;

	[[nodiscard]] friend FORCEINLINE auto GetTypeHash(FName Name) -> uint64
	{
		return GetTypeHash(Name.GetComparisonIndex()) + Name.GetNumber();
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

	[[nodiscard]] FORCEINLINE auto GetDisplayIndex() const -> FNameEntryId { return DisplayIndex_; }

	[[nodiscard]] FORCEINLINE auto GetComparisonIndex() const -> FNameEntryId { return ComparisonIndex_; }


private:
	FNameEntryId DisplayIndex_;

	FNameEntryId ComparisonIndex_;

	uint32 Number_ = 0;

	friend struct FNameHash;
	friend struct FNameHelper;
};


template<>
struct std::hash<FName>
{
	size_t operator()(FName Name) const noexcept
	{
		return GetTypeHash(Name);
	}
};

CORE_API auto RegisterDogeNames() -> void;

struct FNameDebugVisualizer
{
	CORE_API FNameDebugVisualizer(FClangKeepDebugInfo);
	CORE_API uint8** GetBlocks();

private:
	static constexpr uint32 EntryStride = alignof(FNameEntry);
	static constexpr uint32 OffsetBits = 16;
	static constexpr uint32 BlockBits = 13;
	static constexpr uint32 OffsetMask = (1 << OffsetBits) - 1;
	static constexpr uint32 UnusedMask = UINT32_MAX << BlockBits << OffsetBits;
	static constexpr uint32 MaxLength = FNameMaxSize;
};

extern uint8** GNameBlocksDebug;

// It is used in the natvis file for debugging purposes, each .exe or.dll owns this variable, so it is not shared between modules.
// It will be initialized only once, and it will not be changed during the program execution.
// So, it is safe to use it in the natvis file for debugging purposes.
#ifdef DOGE_VISUALIZERS_HELPERS
inline uint8** GNameBlocksDebug = FNameDebugVisualizer(FClangKeepDebugInfo{}).GetBlocks();
#endif // DOGE_VISUALIZERS_HELPERS


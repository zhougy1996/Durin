#pragma once

inline constexpr uint32 MaxNameSize = 1024;

struct FClangKeepDebugInfo
{
};

enum class ENameCase : uint8
{
	CaseSensitive,
	IgnoreCase,
};

struct FNameEntryId
{
public:
	FNameEntryId()
		: Value(0)
	{
	}
	explicit FNameEntryId(uint64 Value)
		: Value(Value)
	{
	}

	auto IsNone() const -> bool { return Value == 0; }

	auto GetValue() const -> uint64 { return Value; }

	auto ToInt() const -> uint32 { return static_cast<uint32>(Value); }

	auto operator==(const FNameEntryId& Other) const -> bool { return Value == Other.Value; }
	auto operator!=(const FNameEntryId& Other) const -> bool { return Value != Other.Value; }
	auto operator<(const FNameEntryId& Other) const -> bool { return Value < Other.Value; }
	auto operator>(const FNameEntryId& Other) const -> bool { return Value > Other.Value; }

	struct FHash
	{
		size_t operator()(const FNameEntryId& Id) const noexcept;
	};

private:
	uint64 Value; // TODO: 32 bits
};

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
		UTF8Char AnsiName[MaxNameSize];
		uint8 NameData[0];
	};

public:
	FNameEntry() = default;

	// TODO: Remove this constructor
	FNameEntry(FU8StringView Name)
	{
	}

	[[nodiscard]] FORCEINLINE auto GetPlainNameString() const -> FString
	{
		return FString{&AnsiName[0]};
	}

	static constexpr auto GetDataOffset() -> int32 { return offsetof(FNameEntry, NameData); }

	FORCEINLINE auto MakeView(FNameBuffer& Buffer) const -> FU8StringView;

	[[nodiscard]] FORCEINLINE auto GetComparisonId() const -> FNameEntryId { return ComparisonId; }

	[[nodiscard]] FORCEINLINE auto GetHeader() const -> FNameEntryHeader { return Header; }

	[[nodiscard]] FORCEINLINE auto GetLength() const -> uint16 { return Header.Len; }

	[[nodiscard]] FORCEINLINE auto IsValid() const -> bool { return Header.Len > 0; }

private:
	const UTF8Char* GetUnterminatedName(UTF8Char(&OptionalDecodeBuffer)[MaxNameSize]) const;

	FNameEntry(FClangKeepDebugInfo);
	FNameEntry(const FNameEntry&) = delete;
	FNameEntry(FNameEntry&&) = delete;
	FNameEntry& operator=(const FNameEntry&) = delete;
	FNameEntry& operator=(FNameEntry&&) = delete;

	friend struct FNameHelper;
	friend struct FNamePool;
	friend class FNameEntryAllocator;
};

class FName
{
public:
	static constexpr uint32 MaxSize = MaxNameSize;

	CORE_API FName() = default;

	CORE_API FName(const UTF8Char* Name);

	CORE_API FName(const UTF8Char* Name, int32 Number);

	CORE_API FName(FU8StringView View, int32 Number);

	CORE_API FName(const FName& Other);

	[[nodiscard]] FORCEINLINE auto GetNumber() const -> uint32 { return Number_; }

	[[nodiscard]] FORCEINLINE CORE_API auto Equals(const FName& Other, ENameCase CompareMethod = ENameCase::IgnoreCase, const bool bCompareNumber = false) const -> bool;

	[[nodiscard]] CORE_API auto ToString() const -> FString;

	[[nodiscard]] CORE_API auto GetComparisonNameEntry() const -> const FNameEntry*;

	[[nodiscard]] CORE_API auto GetDisplayNameEntry() const -> const FNameEntry*;

	[[nodiscard]] CORE_API static auto ResolveEntry(FNameEntryId LookupId) -> const FNameEntry*;

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

class FNamePool
{
public:
	FNamePool() = default;
	~FNamePool() = default;

	auto Store(FU8StringView View) -> FNameEntryId;

	auto Find(FU8StringView View) -> FNameEntryId;

	// Make sure the ID is valid before calling this
	[[nodiscard]] auto Resolve(FNameEntryId Id) -> FNameEntry&;

	static auto Get() -> FNamePool&;

private:
	std::unordered_map<FNameEntryId, FNameEntry, FNameEntryId::FHash> NameEntries_;
};

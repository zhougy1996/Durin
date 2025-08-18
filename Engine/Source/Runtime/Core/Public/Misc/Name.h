#pragma once
enum class ENameCase : uint8
{
	CaseSensitive,
	IgnoreCase,
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

	auto operator==(const FNameEntryId& Other) const -> bool { return Value_ == Other.Value_; }
	auto operator!=(const FNameEntryId& Other) const -> bool { return Value_ != Other.Value_; }
	auto operator<(const FNameEntryId& Other) const -> bool { return Value_ < Other.Value_; }
	auto operator>(const FNameEntryId& Other) const -> bool { return Value_ > Other.Value_; }

	struct Hash
	{
		std::size_t operator()(const FNameEntryId& Id) const noexcept
		{
			return std::hash<uint64>()(Id.Value_);
		}
	};

private:
	uint64 Value_;
};

struct FNameEntry
{
public:
	FNameEntry() = default;

	FNameEntry(FU8StringView Name)
		: Name_(Name)
	{
	}

	[[nodiscard]] FORCEINLINE auto GetPlainNameString() const -> FString { return Name_; }

private:
	FNameEntryId ComparisonId_;

	FU8String Name_;

	friend struct FNameHelper;
	friend struct FNamePool;
};

class FName
{
public:
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
	static constexpr uint32 MaxSize = 1024;

	static constexpr uint32 NoNumberInternal = 0;

	static inline constexpr auto NumberInternalToExternal(uint32 InternalNumber) -> uint32
	{
		return InternalNumber - 1;
	};

	static inline constexpr auto NumberExternalToInternal(int32 ExternalNumber) -> uint32
	{
		return ExternalNumber + 1;
	}

	[[nodiscard]] FORCEINLINE auto GetDisplayIndex() const -> FNameEntryId { return DisplayEntryId_; }

	[[nodiscard]] FORCEINLINE auto GetComparisonIndex() const -> FNameEntryId { return ComparisonEntryId_; }


private:
	FNameEntryId DisplayEntryId_;

	FNameEntryId ComparisonEntryId_;

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
	std::unordered_map<FNameEntryId, FNameEntry, FNameEntryId::Hash> NameEntries_;
};
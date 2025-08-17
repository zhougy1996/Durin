#pragma once
enum class ENameCase : uint8
{
	CaseSensitive,
	IgnoreCase,
};

struct FNameEntryId
{
public:
	FNameEntryId() : Value_(0) {}
	explicit FNameEntryId(uint32 Value) : Value_(Value) {}

	auto IsNone() const -> bool { return Value_ == 0; }
	auto GetValue() const -> uint32 { return Value_; }

	auto operator==(const FNameEntryId& Other) const -> bool { return Value_ == Other.Value_; }
	auto operator!=(const FNameEntryId& Other) const -> bool { return Value_ != Other.Value_; }
	auto operator<(const FNameEntryId& Other) const -> bool { return Value_ < Other.Value_; }
	auto operator>(const FNameEntryId& Other) const -> bool { return Value_ > Other.Value_; }

	struct Hash
	{
		std::size_t operator()(const FNameEntryId& Id) const noexcept
		{
			return std::hash<uint32>()(Id.Value_);
		}
	};

private:
	uint32 Value_;
};

struct FNameEntry
{
public:
	FNameEntry() = default;
	FNameEntry(FU8StringView Name) : Name_(Name) {}
	auto GetName() const -> FU8StringView { return Name_; }

private:
	FU8String Name_;
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

	[[nodiscard]] FORCEINLINE CORE_API auto Equals(const FName& Other, ENameCase CompareMethod = ENameCase::CaseSensitive, const bool bCompareNumber = false) const -> bool;

private:
	FNameEntryId DisplayEntryId_;
	// FNameEntryId ComparisonEntryId_;
	uint32 Number_ = 0;

friend struct FNameHelper;
};

class FNamePool
{
public:
	FNamePool() = default;
	~FNamePool() = default;

	auto Store(FU8StringView View) -> FNameEntryId;

	auto Find(FU8StringView View) -> FNameEntryId;

	static auto Get() -> FNamePool&;

private:

	std::unordered_map<FNameEntryId, FNameEntry, FNameEntryId::Hash> NameEntries_;
};
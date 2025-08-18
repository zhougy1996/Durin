#include "Misc/Name.h"

#define NAME_NO_NUMBER_INTERNAL 0

struct FNameHelper
{
	static uint32 ParseNumber(const char* Name, int32& InOutLen)
	{
		const int32 Len = InOutLen;
		int32 Digits = 0;
		for (const char* It = Name + Len - 1; It >= Name && *It >= '0' && *It <= '9'; --It)
		{
			++Digits;
		}

		const char* FirstDigit = Name + Len - Digits;
		static constexpr int32 MaxDigitsInt32 = 10;
		if (Digits && Digits < Len && *(FirstDigit - 1) == '_' && Digits <= MaxDigitsInt32)
		{
			// check for the case where there are multiple digits after the _ and the first one
			// is a 0 ("Rocket_04"). Can't split this case. (So, we check if the first char
			// is not 0 or the length of the number is 1 (since ROcket_0 is valid)
			if (Digits == 1 || *FirstDigit != '0')
			{
				int Number = std::stoi(std::string(Name + Len - Digits, static_cast<size_t>(Digits)));

				if (Number < INT_MAX)
				{
					InOutLen -= 1 + Digits;
					return static_cast<uint32>(Number + 1);
					// return static_cast<uint32>(NAME_EXTERNAL_TO_INTERNAL(Number));
				}
			}
		}

		return NAME_NO_NUMBER_INTERNAL;
	}

	static FName MakeWithNumber(const FU8StringView View, uint32 InternalNumber)
	{
		FName Name;

		Name.DisplayIndex_ = FNamePool::Get().Store(View);
		Name.ComparisonIndex_ = ResolveComparisonId(Name.DisplayIndex_);
		Name.Number_ = InternalNumber;

		return Name;
	}

	static FName MakeDetectNumber(FU8StringView View)
	{
		if (View.length() == 0)
		{
			return FName();
		}

		int Len = View.length();
		uint32 InternalNumber = ParseNumber(View.data(), /* may be shortened */ Len);
		return MakeWithNumber(FU8StringView{View.data(), static_cast<size_t>(Len)}, InternalNumber);
	}

	static auto ResolveComparisonId(FNameEntryId DisplayId) -> FNameEntryId
	{
		if (DisplayId.IsNone()) { return FNameEntryId(); }

		FNamePool& Pool = FNamePool::Get();

		return FNamePool::Get().Resolve(DisplayId).ComparisonId_;
	}


};

struct FNameHash
{
	// Support for both UTF-8 and UTF-16 strings
	template<typename CharType>
	static auto GenerateHash(const CharType* Str, size_t Len) -> uint64
	{
		auto HashFunctor = std::hash<FANSIStringView>{};
		return static_cast<uint64>(HashFunctor(FANSIStringView(reinterpret_cast<const char*>(Str), Len * sizeof(CharType))));
	}

	template<typename CharType>
	static auto GenerateLowerCaseHash(const CharType* Str, size_t Len) -> uint64
	{
		CharType LowerName[FName::MaxSize];

		for (size_t i = 0; i < Len && i < FName::MaxSize - 1; ++i)
		{
			LowerName[i] = std::tolower(Str[i]);
		}

		return FNameHash::GenerateHash(LowerName, Len);
	}

	static uint64 GenerateLowerCaseHash(FU8StringView Name)
	{
		return GenerateLowerCaseHash(Name.data(), Name.length());
	}

	static uint64 GenerateHash(FU8StringView Name)
	{
		return GenerateHash(Name.data(), Name.length());
	}
};

FName::FName(const UTF8Char* Name)
	: FName(FNameHelper::MakeDetectNumber(FU8StringView(Name)))
{
}

FName::FName(const UTF8Char* Name, int32 Number)
{
}

FName::FName(FU8StringView View, int32 Number)
{
	FNameEntryId EntryId = FNamePool::Get().Store(View);
	DisplayIndex_ = EntryId;
	// ComparisonIndex_ = EntryId; // Assuming we want to use the same entry for comparison
	Number_ = Number;
}

FName::FName(const FName& Other)
	: ComparisonIndex_(Other.ComparisonIndex_)
	, DisplayIndex_(Other.DisplayIndex_)
	, Number_(Other.Number_)
{
}

auto FName::Equals(const FName& Other, ENameCase CompareMethod /*= ENameCase::IgnoreCase*/, const bool bCompareNumber /*= true*/) const -> bool
{
	return ((CompareMethod == ENameCase::IgnoreCase) ? (GetComparisonIndex() == Other.GetComparisonIndex()) : (GetDisplayIndex() == Other.GetDisplayIndex()))
		   && (!bCompareNumber || Number_ == Other.Number_);
}

auto FName::ToString() const -> FString
{
	FString PlainNameString = GetDisplayNameEntry()->GetPlainNameString();
	if (Number_ == NoNumberInternal)
	{
		return PlainNameString;
	}
	else
	{
		return PlainNameString + "_" + std::to_string(NumberInternalToExternal(Number_));
	}
}

auto FName::GetComparisonNameEntry() const -> const FNameEntry*
{
	return ResolveEntry(ComparisonIndex_);
}

auto FName::GetDisplayNameEntry() const -> const FNameEntry*
{
	return ResolveEntry(DisplayIndex_);
}

auto FName::ResolveEntry(FNameEntryId LookupId) -> const FNameEntry*
{
	FNamePool& Pool = FNamePool::Get();
	return &(Pool.Resolve(LookupId));
}

auto FNamePool::Store(FU8StringView View) -> FNameEntryId
{
	FNameEntryId DisplayId(FNameHash::GenerateHash(View));
	auto It = NameEntries_.find(DisplayId);
	if (It != NameEntries_.end())
	{
		// Display ID already exists, return it
		// In this case, the comparison ID is also already set in the table
		return It->first;
	}

	FNameEntryId ComparisonId(FNameHash::GenerateLowerCaseHash(View));

	FNameEntry NewEntry(View);
	NewEntry.ComparisonId_ = ComparisonId;

	// Check if the comparison ID already exists
	if (!NameEntries_.count(ComparisonId))
	{
		NameEntries_.emplace(ComparisonId, NewEntry);
	}

	NameEntries_.emplace(DisplayId, NewEntry);
	
	return DisplayId;
}

auto FNamePool::Find(FU8StringView View) -> FNameEntryId
{
	// First, try to find the display ID
	FNameEntryId DisplayId(FNameHash::GenerateHash(View));
	auto DisplayIter = NameEntries_.find(DisplayId);

	if (DisplayIter != NameEntries_.end())
	{
		// If the display ID exists, return it
		return DisplayIter->first;
	}

	// If not found, try to find the comparison ID
	FNameEntryId ComparisonId(FNameHash::GenerateLowerCaseHash(View));
	auto ComparisonIter = NameEntries_.find(ComparisonId);
	if (ComparisonIter != NameEntries_.end())
	{
		// If the comparison ID exists, return it
		return ComparisonIter->first;
	}

	return FNameEntryId(); // Return an invalid ID if not found
}

auto FNamePool::Resolve(FNameEntryId Id) -> FNameEntry&
{
	auto It = NameEntries_.find(Id);
	check(It != NameEntries_.end() && "FNamePool::Resolve: Invalid ID");
	return It->second;
}

auto FNamePool::Get() -> FNamePool&
{
	static FNamePool Instance;
	return Instance;
}

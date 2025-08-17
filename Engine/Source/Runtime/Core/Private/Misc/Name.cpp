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
		FNameEntryId EntryId = FNamePool::Get().Store(View);
		FName Name;

		Name.DisplayEntryId_ = EntryId;
		// Name.ComparisonEntryId_ = EntryId; // Assuming we want to use the same entry for comparison
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
	DisplayEntryId_ = EntryId;
	// ComparisonEntryId_ = EntryId; // Assuming we want to use the same entry for comparison
	Number_ = Number;
}

FName::FName(const FName& Other)
	: DisplayEntryId_(Other.DisplayEntryId_)
	, Number_(Other.Number_)
{
}

CORE_API auto FName::Equals(const FName& Other, ENameCase CompareMethod /*= ENameCase::CaseSensitive*/, const bool bCompareNumber /*= true*/) const -> bool
{
	// TODO: // Implement the actual comparison logic based on the CompareMethod
	return (DisplayEntryId_ == Other.DisplayEntryId_) &&
		   (!bCompareNumber || Number_ == Other.Number_);
}

auto FNamePool::Store(FU8StringView View) -> FNameEntryId
{
	size_t Hash = std::hash<FU8StringView>{}(View);
	auto It = NameEntries_.find(FNameEntryId(static_cast<uint32>(Hash)));
	if (It != NameEntries_.end())
	{
		return It->first; // Return existing entry ID
	}
	FNameEntry NewEntry(View);
	NameEntries_.emplace(FNameEntryId(static_cast<uint32>(Hash)), NewEntry);
	return FNameEntryId(static_cast<uint32>(Hash)); // Return new entry ID
}

auto FNamePool::Find(FU8StringView View) -> FNameEntryId
{
	size_t Hash = std::hash<FU8StringView>{}(View);
	auto It = NameEntries_.find(FNameEntryId(static_cast<uint32>(Hash)));
	if (It != NameEntries_.end())
	{
		return It->first; // Return existing entry ID
	}

	return FNameEntryId(); // Return an invalid ID if not found
}

auto FNamePool::Get() -> FNamePool&
{
	static FNamePool Instance;
	return Instance;
}

#include "Misc/Name.h"
#include <cstdlib>

#define NAME_NO_NUMBER_INTERNAL 0

struct FNameBuffer
{
	UTF8Char Name[MaxNameSize];
};

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

		return FNamePool::Get().Resolve(DisplayId).ComparisonId;
	}
};

static constexpr uint32 FNamePoolShardBits = 10;
static constexpr uint32 FNamePoolShardCount = 1 << FNamePoolShardBits;
static constexpr uint32 FNamePoolInitialSlotBits = 8;
static constexpr uint32 FNamePoolInitialSlotCountPerShard = 1 << FNamePoolInitialSlotBits;

static constexpr uint32 FNameMaxBlockBits = 13;
static constexpr uint32 FNameMaxBlockCount = 1 << FNameMaxBlockBits;

static constexpr uint32 FNameBlockOffsetBits = 16;
static constexpr uint32 FNameBlockOffsetCapacity = 1 << FNameBlockOffsetBits;
static constexpr uint32 FNameBlockOffsetMask = FNameBlockOffsetCapacity - 1;

static constexpr uint32 FNameEntryIdBits = FNameMaxBlockBits + FNameBlockOffsetBits;
static constexpr uint32 FNameEntryIdMask = (1 << FNameEntryIdBits) - 1;

// Hash bits are used to determine the shard and slot index in the pool.
// Hi: | Probe hash |            |    Shard index bits    |
// Lo: | Unmasked slot index bits                         |
//     | Probe hash |  Block bits  |  Block offset bits   |

struct FNameSlot
{
	uint32 IdAndHash = 0;

	static constexpr uint32 ProbeHashShift = FNameEntryIdBits;
	static constexpr uint32 ProbeHashMask = ~FNameEntryIdMask;

	FNameSlot() {}

	FNameSlot(FNameEntryId Value, uint32 ProbeHash)
		: IdAndHash((Value.ToInt() & FNameEntryIdMask) | (ProbeHash << ProbeHashShift))
	{
	}


	FNameEntryId GetId() const
	{
		return FNameEntryId(IdAndHash & FNameEntryIdMask);
	}

	uint32 GetProbeHash() const
	{
		return IdAndHash & ProbeHashMask;
	}

	bool operator==(const FNameSlot& Other) const
	{
		return IdAndHash == Other.IdAndHash;
	}

	bool Used() const { return !!IdAndHash; }
};


struct FNameHash
{
	uint32 ShardIndex;
	uint32 UnmaskedSlotIndex;
	uint32 SlotProbeHash;
	FNameEntryHeader EntryProbeHeader;

	static constexpr uint32 ShardMask = FNamePoolShardCount - 1;

	FNameHash(const UTF8Char* Str, int32 Len)
		: FNameHash(FNameHash::GenerateLowerCaseHash(Str, Len), Len, IsAnsiNone(Str, Len))
	{

	}

	FNameHash(const UTF8Char* Str, int32 Len, uint64 Hash)
		: FNameHash(Hash, Len, IsAnsiNone(Str, Len))
	{
	}

	FNameHash(uint64 Hash, int32 Len, bool bIsNone)
	{
		uint32 Hi = static_cast<uint32>(Hash >> 32);
		uint32 Lo = static_cast<uint32>(Hash);

		// "None" has FNameEntryId with a value of zero
		// Always set a bit in SlotProbeHash for "None" to distinguish unused slot values from None
		// @see FNameSlot::Used()
		uint32 IsNoneBit = bIsNone << FNameSlot::ProbeHashShift;

		static_assert((ShardMask & FNameSlot::ProbeHashMask) == 0, "Masks overlap");

		ShardIndex = Hi & ShardMask;
		UnmaskedSlotIndex = Lo;
		SlotProbeHash = (Hi & FNameSlot::ProbeHashMask) | IsNoneBit;
		EntryProbeHeader.Len = Len;
	}

	auto operator==(const FNameHash& Other) const -> bool
	{
		return ShardIndex == Other.ShardIndex
			   && UnmaskedSlotIndex == Other.UnmaskedSlotIndex
			   && SlotProbeHash == Other.SlotProbeHash;
	}

	auto GetProbeStart(uint32 SlotMask) const -> uint32
	{
		return (UnmaskedSlotIndex & SlotMask);
	}

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

	// Check if the name is "none"
	static bool IsAnsiNone(const UTF8Char* Str, int32 Len)
	{
		if (Len != 4)
		{
			return false;
		}

#if PLATFORM_LITTLE_ENDIAN
		static constexpr uint32 NoneAsInt = 0X454E4F4E;
#else
		static constexpr uint32 NoneAsInt = 0X4E4F4E45;
#endif
		static constexpr uint32 ToUpperMask = 0XDFDFDFDF;

		uint32 FourChars;
		memcpy((void*)&FourChars, Str, 4);

		return (FourChars & ToUpperMask) == NoneAsInt;
	}
};

template<typename CharType>
FORCENOINLINE static auto HashLowerCase(const CharType* Str, size_t Len) -> FNameHash
{
	CharType LowerName[FName::MaxSize];

	for (size_t i = 0; i < Len && i < FName::MaxSize - 1; ++i)
	{
		LowerName[i] = std::tolower(Str[i]);
	}

	return FNameHash(LowerName, Len);
}

template<ENameCase Sensitivity>
FNameHash HashName(FU8StringView Name);

template<>
FNameHash HashName<ENameCase::IgnoreCase>(FU8StringView Name)
{
	return HashLowerCase(Name.data(), Name.length());
}
template<>
FNameHash HashName<ENameCase::CaseSensitive>(FU8StringView Name)
{
	return FNameHash(Name.data(), Name.length());
}

template<ENameCase Sensitivity>
struct FNameValue
{
	FU8StringView Name;

	FNameHash Hash;

	FNameEntryId ComparisonId;

	explicit FNameValue(FU8StringView InName)
		: Name(InName)
		, Hash(HashName<Sensitivity>(InName))
	{
	}

	FNameValue(FU8StringView InName, FNameHash InHash)
		: Name(InName)
		, Hash(InHash)
	{
	}

	FNameValue(FU8StringView InName, uint64 InHash)
		: Name(InName)
		, Hash(InName.data(), InName.length(), InHash)
	{
	}
};

/** An unpacked FNameEntryId */
struct FNameEntryHandle
{
	uint32 Block = 0;
	uint32 Offset = 0;

	FNameEntryHandle(uint32 InBlock, uint32 InOffset)
		: Block(InBlock)
		, Offset(InOffset)
	{
		check(Block < FNameMaxBlockCount);
		check(Offset < FNameBlockOffsetCapacity);
	}

	FNameEntryHandle(FNameEntryId Id)
		: Block(Id.ToInt() >> FNameBlockOffsetBits)
		, Offset(Id.ToInt() & FNameBlockOffsetMask)
	{
	}

	static uint32 GetTypeHash(FNameEntryHandle Handle)
	{
		uint32 HashValue = (Handle.Block << (32 - FNameMaxBlockBits)) + Handle.Block // Let block index impact most hash bits
						   + (Handle.Offset << FNameBlockOffsetBits) + Handle.Offset // Let offset impact most hash bits
						   + (Handle.Offset >> 4);									 // Reduce impact of non-uniformly distributed entry name lengths
		return HashValue;
	}

	// Implicit conversion to FNameEntryId
	operator FNameEntryId() const
	{
		return FNameEntryId(Block << FNameBlockOffsetBits | Offset);
	}

	struct FHash
	{
		size_t operator()(const FNameEntryHandle& Handle) const noexcept
		{
			return std::hash<uint32>()(GetTypeHash(Handle));
		}
	};

	explicit operator bool() const { return Block | Offset; }
};

auto FNameEntry::MakeView(FNameBuffer& Buffer) const -> FU8StringView
{
	return FU8StringView();
}

FNameEntry::FNameEntry(FClangKeepDebugInfo)
{
}

size_t FNameEntryId::FHash::operator()(const FNameEntryId& Id) const noexcept
{
	return std::hash<uint32>()(FNameEntryHandle::GetTypeHash(FNameEntryHandle(Id)));
}

class FNameEntryAllocator
{
public:
	static constexpr uint8 Stride = alignof(FNameEntry);
	static constexpr uint32 BlockSizeBytes = Stride * FNameBlockOffsetCapacity;

	FNameEntryAllocator()
	{
		Blocks[0] = AllocBlock();
	}

	auto ReserveBlocks(uint32 Num) -> void
	{
		std::lock_guard<std::mutex> _(Lock);

		for (uint32 Idx = Num - 1; Idx > CurrentBlock && Blocks[Idx] == nullptr; --Idx)
		{
			Blocks[Idx] = AllocBlock();
		}
	}

	static auto TryPlace(uint32 AvailableBytes, FU8StringView Name) -> uint32
	{
		uint32 NameBytes = Name.length();					// Length of unterminated name
		return NameBytes <= AvailableBytes ? NameBytes : 0; // +1 for null terminator
	}

	// Allocate and store Name.
	auto AllocateRegular(FU8StringView Name) -> FNameEntryHandle
	{
		std::lock_guard<std::mutex> _(Lock);

		uint32 Bytes = TryPlace(BlockSizeBytes - CurrentByteCursor, Name);

		if (Bytes == 0) // Not enough space in the current block
		{
			AllocateNewBlock();
			Bytes = TryPlace(BlockSizeBytes - CurrentByteCursor, Name);
			check(Bytes > 0);
		}

		return Allocate(Bytes);
	}

	auto Create(FU8StringView Name, std::optional<FNameEntryId> ComparisonId, FNameEntryHeader Header) -> FNameEntryHandle
	{
		FNameEntryHandle Handle = AllocateRegular(Name);
		FNameEntry& Entry = Resolve(Handle);

		Entry.ComparisonId = ComparisonId.value_or(FNameEntryId());
		Entry.Header = Header;

		memcpy(Entry.NameData, Name.data(), Name.length());
		return Handle;
	}

	auto Resolve(FNameEntryHandle Handle) const -> FNameEntry&
	{
		// Lock not needed
		return *reinterpret_cast<FNameEntry*>(Blocks[Handle.Block] + Stride * Handle.Offset);
	}

	inline FNameEntryHandle Allocate(uint32 Bytes)
	{
		check(Bytes % Stride == 0);
		check(CurrentByteCursor % Stride == 0);
		check(CurrentByteCursor + Bytes <= BlockSizeBytes);

		uint32 ByteOffset = CurrentByteCursor;
		CurrentByteCursor += Bytes;
		return FNameEntryHandle(CurrentBlock, ByteOffset / Stride);
	}

	uint8* AllocBlock()
	{
		return (uint8*)_aligned_malloc(BlockSizeBytes, Stride);
	}

	void AllocateNewBlock()
	{
		++CurrentBlock;
		CurrentByteCursor = 0;

		if (Blocks[CurrentBlock] == nullptr)
		{
			Blocks[CurrentBlock] = AllocBlock();
		}
	}

	std::mutex Lock;

	std::atomic<uint32> CurrentBlock = 0;
	uint32 CurrentByteCursor = 0;
	std::atomic<uint8*> Blocks[FNameMaxBlockCount] = {};
};

template<ENameCase Sensitivity>
static FORCEINLINE bool EqualsSameDimensions(FU8StringView A, FU8StringView B)
{
	check(A.length() == B.length());

	uint32 Len = A.length();

	if (Sensitivity == ENameCase::CaseSensitive)
	{
		strncmp(A.data(), B.data(), Len);
	}
	else
	{
		strnicmp(A.data(), B.data(), Len);
	}
}

class alignas(64) FNamePoolShardBase
{
public:
	void Initialize(FNameEntryAllocator& InEntries)
	{
		Entries_ = &InEntries;

		Slots_ = (FNameSlot*)_aligned_malloc(FNamePoolInitialSlotCountPerShard * sizeof(FNameSlot), alignof(FNameSlot));
		memset(Slots_, 0, FNamePoolInitialSlotCountPerShard * sizeof(FNameSlot));

		CapacityMask_ = FNamePoolInitialSlotCountPerShard - 1;
	}

	auto Capacity() const -> uint32
	{
		return CapacityMask_ + 1;
	}

	auto NumCreatedEntries() const -> uint32
	{
		return NumCreatedEntries_.load(std::memory_order_relaxed);
	}

	template<ENameCase Sensitivity>
	FORCEINLINE static bool EntryEqualsValue(const FNameEntry& Entry, const FNameValue<Sensitivity>& Value)
	{
		// Header checked first to make sure the 
		return Entry.Header == Value.Hash.EntryProbeHeader && EqualsSameDimensions<Sensitivity>(Entry, Value.Name);
	}

protected:

	uint32 UsedSlots_ = 0;

	uint32 CapacityMask_ = 0;

	FNameSlot* Slots_ = nullptr;

	FNameEntryAllocator* Entries_ = nullptr;

	std::atomic<uint32> NumCreatedEntries_{0};
};

template<ENameCase Sensitivity>
class FNamePoolShard : public FNamePoolShardBase
{
public:
	FNameEntryId Find(const FNameValue<Sensitivity>& Value) const
	{
		FNameEntryId Result;
		{
			// TODO: lock here

			FNameSlot& Slot = Probe(Value);
			Result = Slot.GetId();
		};
		return Result;
	}

	FORCEINLINE auto Probe(const FNameValue<Sensitivity>& Value) const -> FNameSlot&
	{
		auto Predicate = [&Value](const FNameSlot& Slot) -> bool
		{
			return Slot.GetProbeHash() == Value.Hash.SlotProbeHash && EntryEqualsValue<Sensitivity>(Entries->Resolve(Slot.GetId()), Value);
		};
	}

	template<typename PredicateFn>
	FORCEINLINE auto Probe(const FNameValue<Sensitivity>& Value, PredicateFn Predicate) const -> FNameSlot&
	{
		const uint32 Mask = CapacityMask;

		// Linear probing
		for (uint32 Index = FNameHash::GetProbeStart(UnmaskedSlotIndex, Mask); true; Index = (Index + 1) & Mask)
		{
			FNameSlot& Slot = Slots[Index];
			if (!Slot.Used() || Predicate(Slot))
			{
				return Slot;
			}
		}
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
	NewEntry.ComparisonId = ComparisonId;

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

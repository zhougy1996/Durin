#include "Misc/Name.h"
#include <cstdlib>

static constexpr uint32 FNameNoNumberInternal = 0;

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

CORE_API auto RegisterDogeNames() -> void
{
	FName(STR("None"));
}

struct FNameBuffer
{
	UTF8Char Name[FNameMaxSize];
};

static bool operator==(FNameEntryHeader A, FNameEntryHeader B)
{
	static_assert(sizeof(FNameEntryHeader) == 2, "");
	return (uint16&)A == (uint16&)B;
}

struct FNameSlot
{
	uint32 IdAndHash = 0;

	static constexpr uint32 ProbeHashShift = FNameEntryIdBits;
	static constexpr uint32 ProbeHashMask = ~FNameEntryIdMask;

	FNameSlot() {}

	FNameSlot(FNameEntryId ValueWithNoProbeHash, uint32 ProbeHash)
		: IdAndHash(ValueWithNoProbeHash.ToInt() | ProbeHash)
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
	uint32 ShardIndex;		  // Low 10 bits of Hi, probably
	uint32 UnmaskedSlotIndex; // Lo, actually
	uint32 SlotProbeHash;	  // High 3 bits of Hi, with a bit set for "None"
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

	static auto GetProbeStart(uint32 UnmaskedSlotIndex, uint32 SlotMask) -> uint32
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

using FNameComparisonValue = FNameValue<ENameCase::IgnoreCase>;
using FNameDisplayValue = FNameValue<ENameCase::CaseSensitive>;

FORCEINLINE std::optional<FNameEntryId> GetExistingComparisonId(const FNameComparisonValue& Value) { return std::optional<FNameEntryId>(); }
FORCEINLINE std::optional<FNameEntryId> GetExistingComparisonId(const FNameDisplayValue& Value) { return Value.ComparisonId; }

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

auto FNameEntry::MakeView() const -> FU8StringView
{
	const UTF8Char* Data = GetUnterminatedName();
	return FU8StringView{Data, Header.Len};
}

const UTF8Char* FNameEntry::GetUnterminatedName() const
{
	return static_cast<const UTF8Char*>(&AnsiName[0]);
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
		Blocks_[0] = AllocBlock();
	}

	auto ReserveBlocks(uint32 Num) -> void
	{
		std::lock_guard<std::mutex> _(Lock_);

		for (uint32 Idx = Num - 1; Idx > CurrentBlock_ && Blocks_[Idx] == nullptr; --Idx)
		{
			Blocks_[Idx] = AllocBlock();
		}
	}

	static auto GetDefaultNameSize(FU8StringView Name) -> uint32
	{
		constexpr uint32 HeaderSize = FNameEntry::GetDataOffset();
		uint32 Bytes = HeaderSize + Name.length() * sizeof(UTF8Char);
		return (Bytes + (Stride - 1)) & ~(Stride - 1);
	}

	static auto TryPlace(uint32 AvailableBytes, FU8StringView Name) -> uint32
	{
		constexpr uint32 HeaderSize = FNameEntry::GetDataOffset();
		uint32 NameBytes = GetDefaultNameSize(Name);
		return NameBytes <= AvailableBytes ? NameBytes : 0;
	}

	// Allocate and store Name.
	auto AllocateRegular(FU8StringView Name) -> FNameEntryHandle
	{
		std::lock_guard<std::mutex> _(Lock_);

		uint32 Bytes = TryPlace(BlockSizeBytes - CurrentByteCursor_, Name);

		if (Bytes == 0) // Not enough space in the current block
		{
			AllocateNewBlock();
			Bytes = TryPlace(BlockSizeBytes - CurrentByteCursor_, Name);
			check(Bytes > 0);
		}

		return Allocate(Bytes);
	}

	auto Create(FU8StringView Name, std::optional<FNameEntryId> ComparisonId, FNameEntryHeader Header) -> FNameEntryHandle
	{
		FNameEntryHandle Handle = AllocateRegular(Name);
		FNameEntry& Entry = Resolve(Handle);

		Entry.ComparisonId = ComparisonId.value_or(FNameEntryId(Handle));
		Entry.Header = Header;

		memcpy(Entry.NameData, Name.data(), Name.length());
		return Handle;
	}

	auto Resolve(FNameEntryHandle Handle) const -> FNameEntry&
	{
		// Lock not needed
		return *reinterpret_cast<FNameEntry*>(Blocks_[Handle.Block] + Stride * Handle.Offset);
	}

	inline FNameEntryHandle Allocate(uint32 Bytes)
	{
		check(Bytes % Stride == 0);
		check(CurrentByteCursor_ % Stride == 0);
		check(CurrentByteCursor_ + Bytes <= BlockSizeBytes);

		uint32 ByteOffset = CurrentByteCursor_;
		CurrentByteCursor_ += Bytes;
		return FNameEntryHandle(CurrentBlock_, ByteOffset / Stride);
	}

	uint8* AllocBlock()
	{
		return (uint8*)_aligned_malloc(BlockSizeBytes, Stride);
	}

	void AllocateNewBlock()
	{
		++CurrentBlock_;
		CurrentByteCursor_ = 0;

		if (Blocks_[CurrentBlock_] == nullptr)
		{
			Blocks_[CurrentBlock_] = AllocBlock();
		}
	}

	auto GetBlocksForDebugVisualizer() -> uint8** { return (uint8**)Blocks_; }

private:
	std::mutex Lock_;

	std::atomic<uint32> CurrentBlock_ = 0;
	uint32 CurrentByteCursor_ = 0;
	std::atomic<uint8*> Blocks_[FNameMaxBlockCount] = {};
};

template<ENameCase Sensitivity>
static FORCEINLINE auto EqualsSameDimensions(FU8StringView A, FU8StringView B) -> bool
{
	check(A.length() == B.length());

	uint32 Len = A.length();

	if (Sensitivity == ENameCase::CaseSensitive)
	{
		return !strncmp(A.data(), B.data(), Len);
	}
	else
	{
		return !strnicmp(A.data(), B.data(), Len);
	}
}

template<ENameCase Sensitivity>
bool EqualsSameDimensions(const FNameEntry& Entry, FU8StringView Name)
{
	return EqualsSameDimensions<Sensitivity>(Entry.MakeView(), Name);
}

class alignas(64) FNamePoolShardBase
{
public:
	void Initialize(FNameEntryAllocator& InEntries)
	{
		Entries_ = &InEntries;

		Slots_ = (FNameSlot*)_aligned_malloc(FNamePoolInitialSlotCountPerShard * sizeof(FNameSlot), alignof(FNameSlot));
		check(Slots_ != nullptr);
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
		return Entry.Header == Value.Hash.EntryProbeHeader && EqualsSameDimensions<Sensitivity>(Entry.MakeView(), Value.Name);
	}

protected:
	// Realloc slots when 90% full
	static constexpr uint32 LoadFactorQuotient = 9;
	static constexpr uint32 LoadFactorDivisor = 10;

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

	FORCEINLINE auto Insert(const FNameValue<Sensitivity>& Value, bool& bCreatedNewEntry) -> FNameEntryId
	{
		// TODO: lock here
		FNameSlot& Slot = Probe(Value);

		if (Slot.Used())
		{
			return Slot.GetId();
		}
		bCreatedNewEntry = true;

		return CreateAndInsertEntry(Slot, Value);
	}

	
	auto InsertExistingEntry(FNameHash Hash, FNameEntryId ExistingId) -> void
	{
		FNameSlot NewLookup(ExistingId, Hash.SlotProbeHash);

		// lock here

		FNameSlot& Slot = Probe(Hash.UnmaskedSlotIndex, [=](FNameSlot Old) { return Old == NewLookup; });
		if (!Slot.Used())
		{
			ClaimSlot(Slot, NewLookup);
		}
	}

private:
	void ClaimSlot(FNameSlot& UnusedSlot, FNameSlot NewValue)
	{
		check(!UnusedSlot.Used());

		UnusedSlot = NewValue;

		++UsedSlots_;
		if (UsedSlots_ * LoadFactorDivisor > LoadFactorQuotient * Capacity())
		{
			Grow();
		}
	}

	FNameEntryId CreateAndInsertEntry(FNameSlot& Slot, const FNameValue<Sensitivity>& Value)
	{
		FNameEntryId NewEntryId = Entries_->Create(Value.Name, GetExistingComparisonId(Value), Value.Hash.EntryProbeHeader);

		ClaimSlot(Slot, FNameSlot(NewEntryId, Value.Hash.SlotProbeHash));

		NumCreatedEntries_.fetch_add(1, std::memory_order_relaxed);

		return NewEntryId;
	}

	auto Grow() -> void
	{
		Grow(Capacity() * 2);
	}

	auto Grow(const uint32 NewCapacity) -> void
	{
		check(NewCapacity > Capacity());

		std::span<FNameSlot> OldSlots(Slots_, Capacity());
		const uint32 OldUsedSlots = UsedSlots_;

		Slots_ = (FNameSlot*)_aligned_realloc(Slots_, NewCapacity * sizeof(FNameSlot), alignof(FNameSlot));
		check(Slots_ != nullptr);
		memset(Slots_ + Capacity(), 0, (NewCapacity - Capacity()) * sizeof(FNameSlot));

		CapacityMask_ = NewCapacity - 1;
		UsedSlots_ = 0;

		constexpr uint32 PrefetchDepth = 8;
		FNameSlot PrefetchedSlots[PrefetchDepth];
		uint32 NumPrefetched = 0;

		for (FNameSlot OldSlot : OldSlots)
		{
			if (OldSlot.Used())
			{
				FPlatformMisc::Prefetch(&Entries_->Resolve(OldSlot.GetId()));
				PrefetchedSlots[NumPrefetched] = OldSlot;

				if (++NumPrefetched == PrefetchDepth)
				{
					for (FNameSlot PrefetchedSlot : PrefetchedSlots)
					{
						RehashAndInsert(PrefetchedSlot);
					}

					NumPrefetched = 0;
				}
			}
		}

		// Rehash remaining prefetched slots
		std::span<FNameSlot> RemainingPrefetchedSlots(PrefetchedSlots, NumPrefetched);
		for (FNameSlot PrefetchedSlot : RemainingPrefetchedSlots)
		{
			RehashAndInsert(PrefetchedSlot);
		}

		check(OldUsedSlots == UsedSlots_);

		_aligned_free(OldSlots.data());
	}

	FORCEINLINE auto Probe(const FNameValue<Sensitivity>& Value) const -> FNameSlot&
	{
		auto Predicate = [&Value, this](const FNameSlot& Slot) -> bool {
			return Slot.GetProbeHash() == Value.Hash.SlotProbeHash && EntryEqualsValue<Sensitivity>(Entries_->Resolve(Slot.GetId()), Value);
		};

		return Probe(Value.Hash.UnmaskedSlotIndex, Predicate);
	}

	template<typename PredicateFn>
	FORCEINLINE auto Probe(uint32 UnmaskedSlotIndex, PredicateFn Predicate) const -> FNameSlot&
	{
		const uint32 Mask = CapacityMask_;

		// Linear probing
		for (uint32 Index = FNameHash::GetProbeStart(UnmaskedSlotIndex, Mask); true; Index = (Index + 1) & Mask)
		{
			FNameSlot& Slot = Slots_[Index];
			if (!Slot.Used() || Predicate(Slot))
			{
				return Slot;
			}
		}
	}

	void RehashAndInsert(FNameSlot OldSlot)
	{
		check(OldSlot.Used());

		const FNameEntry& Entry = Entries_->Resolve(OldSlot.GetId());

		FU8StringView Name = Entry.MakeView();
		FNameHash Hash = HashName<Sensitivity>(Name);
		FNameSlot& NewSlot = Probe(Hash.UnmaskedSlotIndex, [](FNameSlot Slot) { return false; });
		NewSlot = OldSlot;
		++UsedSlots_;
	}
};

class FNamePool
{
public:
	FNamePool();
	~FNamePool() = default;

	auto Store(FU8StringView Name) -> FNameEntryId;

	auto Find(FU8StringView Name) -> FNameEntryId;

	auto Resolve(FNameEntryHandle Id) -> FNameEntry& { return Entries_.Resolve(Id); }

	static bool bInitialized;

	static auto Get() -> FNamePool&;

	auto GetBlocksForDebugVisualizer() -> uint8**;

	auto ReuseComparisonEntry(const FNameDisplayValue& Value) -> bool;

	auto StoreDisplayValue(const FNameDisplayValue& DisplayValue, bool bAddedComparisonEntry) -> FNameEntryId;

private:
	FNameEntryAllocator Entries_;

	FNamePoolShard<ENameCase::IgnoreCase> ComparisonShards_[FNamePoolShardCount];

	FNamePoolShard<ENameCase::CaseSensitive> DisplayShards_[FNamePoolShardCount];
};

bool FNamePool::bInitialized = false;
alignas(FNamePool) static uint8 NamePoolData[sizeof(FNamePool)];

FNamePool::FNamePool()
{
	for (FNamePoolShardBase& Shard : ComparisonShards_)
	{
		Shard.Initialize(Entries_);
	}

	for (FNamePoolShardBase& Shard : DisplayShards_)
	{
		Shard.Initialize(Entries_);
	}
}

auto FNamePool::Store(FU8StringView Name) -> FNameEntryId
{
	FNameDisplayValue DisplayValue(Name);
	if (FNameEntryId Existing = DisplayShards_[DisplayValue.Hash.ShardIndex].Find(DisplayValue))
	{
		return Existing;
	}

	bool bAdded = false;

	// Insert comparison name first since display value must contain comparison name
	FNameComparisonValue ComparisonValue(Name);
	DisplayValue.ComparisonId = ComparisonShards_[ComparisonValue.Hash.ShardIndex].Insert(ComparisonValue, bAdded);

	FNameEntryId DisplayId = StoreDisplayValue(DisplayValue, bAdded);

	return DisplayId;
}

auto FNamePool::Find(FU8StringView Name) -> FNameEntryId
{
	// First try to find the display name, then the comparison name
	FNameDisplayValue DisplayValue(Name);
	if (FNameEntryId Existing = DisplayShards_[DisplayValue.Hash.ShardIndex].Find(DisplayValue))
	{
		return Existing;
	}

	FNameComparisonValue ComparisonValue(Name);
	return ComparisonShards_[ComparisonValue.Hash.ShardIndex].Find(ComparisonValue);
}

auto FNamePool::Get() -> FNamePool&
{
	static FNamePool* Singleton = []() -> FNamePool* {
		check(!bInitialized);

		bInitialized = true;
		new (NamePoolData) FNamePool();

		return reinterpret_cast<FNamePool*>(NamePoolData);
	}();
	return *Singleton;
}

auto FNamePool::GetBlocksForDebugVisualizer() -> uint8**
{
	return Entries_.GetBlocksForDebugVisualizer();
}

auto FNamePool::ReuseComparisonEntry(const FNameDisplayValue& DisplayValue) -> bool
{
	return EqualsSameDimensions<ENameCase::CaseSensitive>(Resolve(DisplayValue.ComparisonId), DisplayValue.Name);
}

auto FNamePool::StoreDisplayValue(const FNameDisplayValue& DisplayValue, bool bAddedComparisonEntry) -> FNameEntryId
{
	FNameEntryId Out = DisplayValue.ComparisonId;
	auto& DisplayShard = DisplayShards_[DisplayValue.Hash.ShardIndex];

	if (bAddedComparisonEntry || ReuseComparisonEntry(DisplayValue))
	{
		DisplayShards_->InsertExistingEntry(DisplayValue.Hash, DisplayValue.ComparisonId);
	}
	else
	{
		Out = DisplayShard.Insert(DisplayValue, bAddedComparisonEntry);
	}

	return Out;
}

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

		return FNameNoNumberInternal;
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

		FNameEntry& DisplayEntry = FNamePool::Get().Resolve(DisplayId);
		return DisplayEntry.ComparisonId;
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

FNameDebugVisualizer::FNameDebugVisualizer(FClangKeepDebugInfo)
{
}

uint8** FNameDebugVisualizer::GetBlocks()
{
	static_assert(EntryStride == FNameEntryAllocator::Stride, "Natvis constants out of sync with actual constants");
	static_assert(BlockBits == FNameMaxBlockBits, "Natvis constants out of sync with actual constants");
	static_assert(OffsetBits == FNameBlockOffsetBits, "Natvis constants out of sync with actual constants");

	return ((FNamePool*)(NamePoolData))->GetBlocksForDebugVisualizer();
}

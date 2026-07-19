#include "Misc/Name.h"

#include "HAL/PlatformMisc.h"

#include <cstdlib>

namespace Durin
{
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

	CORE_API auto FNameInit() -> void
	{
		const FName NoneName(STR("None"));
		check(NoneName.IsNone()); // Make sure "None" is registered correctly at startup and have index zero
	}

	static bool operator==(FNameEntryHeader A, FNameEntryHeader B)
	{
		static_assert(sizeof(FNameEntryHeader) == 2, "");
		return reinterpret_cast<uint16&>(A) == reinterpret_cast<uint16&>(B);
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

		FNameHash(const char* Str, int32 Len)
			: FNameHash(FNameHash::GenerateLowerCaseHash(Str, Len), Len, IsAnsiNone(Str, Len))
		{
		}

		FNameHash(const char* Str, int32 Len, uint64 Hash)
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

		static auto GenerateHash(const char* Str, size_t Len) -> uint64
		{
			auto HashFunctor = std::hash<std::string_view>{};
			return HashFunctor(std::string_view(Str, Len * sizeof(char)));
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

		static uint64 GenerateLowerCaseHash(std::string_view Name)
		{
			return GenerateLowerCaseHash(Name.data(), Name.length());
		}

		static uint64 GenerateHash(std::string_view Name)
		{
			return GenerateHash(Name.data(), Name.length());
		}

		// Check if the name is "none"
		static bool IsAnsiNone(const char* Str, int32 Len)
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
	FNameHash HashName(std::string_view Name);

	template<>
	FNameHash HashName<ENameCase::IgnoreCase>(std::string_view Name)
	{
		return HashLowerCase(Name.data(), Name.length());
	}
	template<>
	FNameHash HashName<ENameCase::CaseSensitive>(std::string_view Name)
	{
		return FNameHash(Name.data(), Name.length());
	}

	template<ENameCase Sensitivity>
	struct FNameValue
	{
		std::string_view Name;

		FNameHash Hash;

		FNameEntryId ComparisonId;

		explicit FNameValue(std::string_view InName)
			: Name(InName)
			, Hash(HashName<Sensitivity>(InName))
		{
		}

		FNameValue(std::string_view InName, FNameHash InHash)
			: Name(InName)
			, Hash(InHash)
		{
		}

		FNameValue(std::string_view InName, uint64 InHash)
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

		// Implicit conversion to FNameEntryId
		operator FNameEntryId() const
		{
			return FNameEntryId(Block << FNameBlockOffsetBits | Offset);
		}

		explicit operator bool() const { return Block | Offset; }
	};

	auto FNameEntry::MakeView() const -> std::string_view
	{
		const char* Data = GetUnterminatedName();
		return std::string_view{Data, Header.Len};
	}

	auto FNameEntry::GetUnterminatedName() const -> const char*
	{
		return static_cast<const char*>(&Utf8Name[0]);
	}

	FNameEntry::FNameEntry(FClangKeepDebugInfo)
	{
	}

	static auto GetTypeHash(FNameEntryHandle Handle) -> uint64
	{
		uint64 Hash = 0;
		uint64 Block = Handle.Block;
		uint64 Offset = Handle.Offset;

		Hash = (Block << (64 - FNameMaxBlockBits)) ^ (Block << 32) ^ (Offset << (32 - FNameBlockOffsetBits)) ^ Offset;

		Hash ^= (Hash >> 33);
		Hash *= 0xff51afd7ed558ccdULL;
		Hash ^= (Hash >> 33);
		Hash *= 0xc4ceb9fe1a85ec53ULL;
		Hash ^= (Hash >> 33);

		return Hash;
	}

	auto GetTypeHash(FNameEntryId Id) -> uint64
	{
		return GetTypeHash(FNameEntryHandle(Id));
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
			std::unique_lock<std::mutex> _(Lock);

			for (uint32 Idx = Num - 1; Idx > CurrentBlock && Blocks[Idx] == nullptr; --Idx)
			{
				Blocks[Idx] = AllocBlock();
			}
		}

		static auto GetDefaultNameSize(std::string_view Name) -> uint32
		{
			constexpr uint32 HeaderSize = FNameEntry::GetDataOffset();
			uint32 Bytes = HeaderSize + Name.length() * sizeof(char);
			return (Bytes + (Stride - 1)) & ~(Stride - 1);
		}

		static auto TryPlace(uint32 AvailableBytes, std::string_view Name) -> uint32
		{
			constexpr uint32 HeaderSize = FNameEntry::GetDataOffset();
			uint32 NameBytes = GetDefaultNameSize(Name);
			return NameBytes <= AvailableBytes ? NameBytes : 0;
		}

		// Allocate and store Name.
		auto AllocateRegular(std::string_view Name) -> FNameEntryHandle
		{
			std::unique_lock<std::mutex> _(Lock);

			uint32 Bytes = TryPlace(BlockSizeBytes - CurrentByteCursor, Name);

			if (Bytes == 0) // Not enough space in the current block
			{
				AllocateNewBlock();
				Bytes = TryPlace(BlockSizeBytes - CurrentByteCursor, Name);
				check(Bytes > 0);
			}

			return Allocate(Bytes);
		}

		auto Create(std::string_view Name, std::optional<FNameEntryId> ComparisonId, FNameEntryHeader Header) -> FNameEntryHandle
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

		static uint8* AllocBlock()
		{
			return static_cast<uint8*>(FPlatformMisc::AlignedAlloc(BlockSizeBytes, Stride));
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

		auto GetBlocksForDebugVisualizer() -> uint8** { return (uint8**)Blocks; }

	private:
		std::mutex Lock;

		std::atomic<uint32> CurrentBlock = 0;
		uint32 CurrentByteCursor = 0;
		std::atomic<uint8*> Blocks[FNameMaxBlockCount] = {};
	};

	template<ENameCase Sensitivity>
	static FORCEINLINE auto EqualsSameDimensions(std::string_view A, std::string_view B) -> bool
	{
		check(A.length() == B.length());

		uint32 Len = A.length();

		if (Sensitivity == ENameCase::CaseSensitive)
		{
			return !strncmp(A.data(), B.data(), Len);
		}
		else
		{
			return !FPlatformMisc::Strnicmp(A.data(), B.data(), Len);
		}
	}

	template<ENameCase Sensitivity>
	bool EqualsSameDimensions(const FNameEntry& Entry, std::string_view Name)
	{
		return EqualsSameDimensions<Sensitivity>(Entry.MakeView(), Name);
	}

	class alignas(64) FNamePoolShardBase
	{
	public:
		void Initialize(FNameEntryAllocator& InEntries)
		{
			Entries = &InEntries;

			Slots = (FNameSlot*)FPlatformMisc::AlignedAlloc(FNamePoolInitialSlotCountPerShard * sizeof(FNameSlot), alignof(FNameSlot));
			check(Slots != nullptr);
			memset(Slots, 0, FNamePoolInitialSlotCountPerShard * sizeof(FNameSlot));

			CapacityMask = FNamePoolInitialSlotCountPerShard - 1;
		}

		auto Capacity() const -> uint32
		{
			return CapacityMask + 1;
		}

		auto NumCreated() const -> uint32
		{
			return NumCreatedEntries.load(std::memory_order_relaxed);
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

		mutable std::shared_mutex Lock;

		uint32 UsedSlots = 0;

		uint32 CapacityMask = 0;

		FNameSlot* Slots = nullptr;

		FNameEntryAllocator* Entries = nullptr;

		std::atomic<uint32> NumCreatedEntries{0};
	};

	template<ENameCase Sensitivity>
	class FNamePoolShard : public FNamePoolShardBase
	{
	public:
		FNameEntryId Find(const FNameValue<Sensitivity>& Value) const
		{
			FNameEntryId Result;
			{
				std::shared_lock<std::shared_mutex> _(Lock);

				FNameSlot& Slot = Probe(Value);
				Result = Slot.GetId();
			};
			return Result;
		}

		FORCEINLINE auto Insert(const FNameValue<Sensitivity>& Value, bool& bCreatedNewEntry) -> FNameEntryId
		{
			std::unique_lock<std::shared_mutex> _(Lock);

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

			std::unique_lock<std::shared_mutex> _(Lock);

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

			++UsedSlots;
			if (UsedSlots * LoadFactorDivisor > LoadFactorQuotient * Capacity())
			{
				Grow();
			}
		}

		FNameEntryId CreateAndInsertEntry(FNameSlot& Slot, const FNameValue<Sensitivity>& Value)
		{
			FNameEntryId NewEntryId = Entries->Create(Value.Name, GetExistingComparisonId(Value), Value.Hash.EntryProbeHeader);

			ClaimSlot(Slot, FNameSlot(NewEntryId, Value.Hash.SlotProbeHash));

			NumCreatedEntries.fetch_add(1, std::memory_order_relaxed);

			return NewEntryId;
		}

		auto Grow() -> void
		{
			Grow(Capacity() * 2);
		}

		auto Grow(const uint32 NewCapacity) -> void
		{
			check(NewCapacity > Capacity());

			const uint32 OldCapacity = Capacity();
			FNameSlot* OldSlots = Slots;
			const uint32 OldUsedSlots = UsedSlots;

			// Rehashing needs the complete old table to remain alive. Realloc may move and
			// release that table, so allocate the replacement separately and free only
			// after every occupied slot has been transferred.
			FNameSlot* NewSlots = static_cast<FNameSlot*>(FPlatformMisc::AlignedAlloc(NewCapacity * sizeof(FNameSlot), alignof(FNameSlot)));
			check(NewSlots != nullptr);
			memset(NewSlots, 0, NewCapacity * sizeof(FNameSlot));

			Slots = NewSlots;
			CapacityMask = NewCapacity - 1;
			UsedSlots = 0;

			constexpr uint32 PrefetchDepth = 8;
			FNameSlot PrefetchedSlots[PrefetchDepth];
			uint32 NumPrefetched = 0;

			for (FNameSlot OldSlot : std::span<FNameSlot>(OldSlots, OldCapacity))
			{
				if (OldSlot.Used())
				{
					FPlatformMisc::Prefetch(&Entries->Resolve(OldSlot.GetId()));
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

			check(OldUsedSlots == UsedSlots);

			FPlatformMisc::AlignedFree(OldSlots);
		}

		FORCEINLINE auto Probe(const FNameValue<Sensitivity>& Value) const -> FNameSlot&
		{
			auto Predicate = [&Value, this](const FNameSlot& Slot) -> bool {
				return Slot.GetProbeHash() == Value.Hash.SlotProbeHash && EntryEqualsValue<Sensitivity>(Entries->Resolve(Slot.GetId()), Value);
			};

			return Probe(Value.Hash.UnmaskedSlotIndex, Predicate);
		}

		template<typename PredicateFn>
		FORCEINLINE auto Probe(uint32 UnmaskedSlotIndex, PredicateFn Predicate) const -> FNameSlot&
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

		void RehashAndInsert(FNameSlot OldSlot)
		{
			check(OldSlot.Used());

			const FNameEntry& Entry = Entries->Resolve(OldSlot.GetId());

			std::string_view Name = Entry.MakeView();
			FNameHash Hash = HashName<Sensitivity>(Name);
			FNameSlot& NewSlot = Probe(Hash.UnmaskedSlotIndex, [](FNameSlot Slot) { return false; });
			NewSlot = OldSlot;
			++UsedSlots;
		}
	};

	class FNamePool
	{
	public:
		FNamePool();
		~FNamePool() = default;

		auto Store(std::string_view Name) -> FNameEntryId;

		auto Find(std::string_view Name) const -> FNameEntryId;

		auto Resolve(FNameEntryHandle Id) -> FNameEntry& { return Entries.Resolve(Id); }

		static bool bInitialized;

		static auto Get() -> FNamePool&;

		auto GetBlocksForDebugVisualizer() -> uint8**;

		auto ReuseComparisonEntry(const FNameDisplayValue& Value) -> bool;

		auto StoreDisplayValue(const FNameDisplayValue& DisplayValue, bool bAddedComparisonEntry) -> FNameEntryId;

	private:
		FNameEntryAllocator Entries;

		FNamePoolShard<ENameCase::IgnoreCase> ComparisonShards[FNamePoolShardCount];

		FNamePoolShard<ENameCase::CaseSensitive> DisplayShards[FNamePoolShardCount];
	};

	bool FNamePool::bInitialized = false;
	alignas(FNamePool) static uint8 NamePoolData[sizeof(FNamePool)];

	CORE_API auto IsFNameInitialized() -> bool
	{
		return FNamePool::bInitialized;
	}

	FNamePool::FNamePool()
	{
		for (FNamePoolShardBase& Shard : ComparisonShards)
		{
			Shard.Initialize(Entries);
		}

		for (FNamePoolShardBase& Shard : DisplayShards)
		{
			Shard.Initialize(Entries);
		}
	}

	auto FNamePool::Store(std::string_view Name) -> FNameEntryId
	{
		FNameDisplayValue DisplayValue(Name);
		if (FNameEntryId Existing = DisplayShards[DisplayValue.Hash.ShardIndex].Find(DisplayValue))
		{
			return Existing;
		}

		bool bAdded = false;

		// Insert comparison name first since display value must contain comparison name
		FNameComparisonValue ComparisonValue(Name);
		DisplayValue.ComparisonId = ComparisonShards[ComparisonValue.Hash.ShardIndex].Insert(ComparisonValue, bAdded);

		FNameEntryId DisplayId = StoreDisplayValue(DisplayValue, bAdded);

		return DisplayId;
	}

	auto FNamePool::Find(std::string_view Name) const -> FNameEntryId
	{
		// First try to find the display name, then the comparison name
		FNameDisplayValue DisplayValue(Name);
		if (FNameEntryId Existing = DisplayShards[DisplayValue.Hash.ShardIndex].Find(DisplayValue))
		{
			return Existing;
		}

		FNameComparisonValue ComparisonValue(Name);
		return ComparisonShards[ComparisonValue.Hash.ShardIndex].Find(ComparisonValue);
	}

	auto FNamePool::Get() -> FNamePool&
	{
		static FNamePool* Singleton = []() -> FNamePool* {
			check(!bInitialized);

			new (NamePoolData) FNamePool();
			bInitialized = true;

			return reinterpret_cast<FNamePool*>(NamePoolData);
		}();
		return *Singleton;
	}

	auto FNamePool::GetBlocksForDebugVisualizer() -> uint8**
	{
		return Entries.GetBlocksForDebugVisualizer();
	}

	auto FNamePool::ReuseComparisonEntry(const FNameDisplayValue& DisplayValue) -> bool
	{
		return EqualsSameDimensions<ENameCase::CaseSensitive>(Resolve(DisplayValue.ComparisonId), DisplayValue.Name);
	}

	auto FNamePool::StoreDisplayValue(const FNameDisplayValue& DisplayValue, bool bAddedComparisonEntry) -> FNameEntryId
	{
		FNameEntryId Out = DisplayValue.ComparisonId;
		auto& DisplayShard = DisplayShards[DisplayValue.Hash.ShardIndex];

		if (bAddedComparisonEntry || ReuseComparisonEntry(DisplayValue))
		{
			DisplayShard.InsertExistingEntry(DisplayValue.Hash, DisplayValue.ComparisonId);
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

		static FName MakeWithNumber(const std::string_view View, uint32 InternalNumber)
		{
			FName Name;

			Name.DisplayIndex = FNamePool::Get().Store(View);
			Name.ComparisonIndex = ResolveComparisonId(Name.DisplayIndex);
			Name.Number = InternalNumber;

			return Name;
		}

		static FName MakeDetectNumber(std::string_view View)
		{
			if (View.length() == 0)
			{
				return FName();
			}

			int Len = View.length();
			uint32 InternalNumber = ParseNumber(View.data(), /* may be shortened */ Len);
			return MakeWithNumber(std::string_view{View.data(), static_cast<size_t>(Len)}, InternalNumber);
		}

		static auto ResolveComparisonId(FNameEntryId DisplayId) -> FNameEntryId
		{
			if (DisplayId.IsNone())
			{
				return FNameEntryId();
			}

			FNamePool& Pool = FNamePool::Get();

			FNameEntry& DisplayEntry = FNamePool::Get().Resolve(DisplayId);
			return DisplayEntry.ComparisonId;
		}
	};


	FName::FName()
	{
	}


	FName::FName(std::string_view View)
		: FName(FNameHelper::MakeDetectNumber(View))
	{
	}

	FName::FName(const char* Name)
		: FName(std::string_view(Name))
	{
	}

	FName::FName(const char* Name, int32 InNumber)
		: FName(std::string_view(Name), InNumber)
	{
	}

	FName::FName(const std::string& Name)
		: FName(Name.c_str())
	{
	}

	FName::FName(std::string_view View, int32 InNumber)
		: FName(FNameHelper::MakeWithNumber(View, NumberExternalToInternal(InNumber)))
	{
	}

	FName::FName(const FName& Other)
		: ComparisonIndex(Other.ComparisonIndex)
		, DisplayIndex(Other.DisplayIndex)
		, Number(Other.Number)
	{
	}

	auto FName::Equals(const FName& Other, ENameCase CompareMethod /*= ENameCase::IgnoreCase*/, const bool bCompareNumber /*= true*/) const -> bool
	{
		return ((CompareMethod == ENameCase::IgnoreCase) ? (GetComparisonIndex() == Other.GetComparisonIndex()) : (GetDisplayIndex() == Other.GetDisplayIndex()))
			   && (!bCompareNumber || Number == Other.Number);
	}

	auto FName::ToString() const -> std::string
	{
		std::string PlainNameString = GetDisplayNameEntry()->GetPlainNameString();
		if (Number == NoNumberInternal)
		{
			return PlainNameString;
		}
		else
		{
			return PlainNameString + "_" + std::to_string(NumberInternalToExternal(Number));
		}
	}

	auto FName::GetComparisonNameEntry() const -> const FNameEntry*
	{
		return ResolveEntry(ComparisonIndex);
	}

	auto FName::GetDisplayNameEntry() const -> const FNameEntry*
	{
		return ResolveEntry(DisplayIndex);
	}

	auto FName::ResolveEntry(FNameEntryId LookupId) -> const FNameEntry*
	{
		FNamePool& Pool = FNamePool::Get();
		return &(Pool.Resolve(LookupId));
	}
} // namespace Durin

FNameDebugVisualizer::FNameDebugVisualizer(FClangKeepDebugInfo)
{
}

uint8_t** FNameDebugVisualizer::GetBlocks()
{
	static_assert(EntryStride == Durin::FNameEntryAllocator::Stride, "Natvis constants out of sync with actual constants");
	static_assert(BlockBits == Durin::FNameMaxBlockBits, "Natvis constants out of sync with actual constants");
	static_assert(OffsetBits == Durin::FNameBlockOffsetBits, "Natvis constants out of sync with actual constants");

	return reinterpret_cast<Durin::FNamePool*>(Durin::NamePoolData)->GetBlocksForDebugVisualizer();
}

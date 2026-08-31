#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DefaultObjectGraph.h"
#include "DObject/DefaultDeltaPlan.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/GarbageCollectionScheduler.h"
#include "DObject/AssetPath.h"
#include "DObject/Package.h"
#include "DObject/PropertyKindTraits.h"
#include "CoreGlobals.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Math/Color.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>
#include <cstddef>

namespace StructOpsTest
{
	inline uint32 DefaultSequence = 0;
	inline Durin::DStruct* ReentrantDefaultStruct = nullptr;
	inline Durin::DObject* DefaultReferenceTarget = nullptr;
	inline uint32 SideEffectSequence = 0;
	inline std::vector<Durin::DObject*> SideEffectObjects;

	auto ConstructStableDefault(void* Destination) -> void
	{
		std::construct_at(static_cast<int32*>(Destination), 7);
	}

	auto ConstructChangingDefault(void* Destination) -> void
	{
		std::construct_at(static_cast<int32*>(Destination), static_cast<int32>(++DefaultSequence));
	}

	auto ConstructReentrantDefault(void* Destination) -> void
	{
		(void)ReentrantDefaultStruct->GetDefaultValue();
		std::construct_at(static_cast<int32*>(Destination), 7);
	}

	struct FReferenceDefault
	{
		Durin::DObject* Value = nullptr;
	};

	struct FAuthoritativeText
	{
		std::string Value = "default";
	};

	auto ConstructAuthoritativeText(void* Destination) -> void
	{
		std::construct_at(static_cast<FAuthoritativeText*>(Destination));
	}

	auto DestroyAuthoritativeText(void* Value) -> void
	{
		std::destroy_at(static_cast<FAuthoritativeText*>(Value));
	}

	auto IdenticalAuthoritativeText(const void* Left, const void* Right) -> bool
	{
		auto Fold = [](std::string Value) {
			std::ranges::transform(Value, Value.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Value;
		};
		return Fold(static_cast<const FAuthoritativeText*>(Left)->Value)
			== Fold(static_cast<const FAuthoritativeText*>(Right)->Value);
	}

	auto ConstructReferenceDefault(void* Destination) -> void
	{
		std::construct_at(
			static_cast<FReferenceDefault*>(Destination),
			FReferenceDefault{DefaultReferenceTarget});
	}

	auto ConstructSideEffectDefault(void* Destination) -> void
	{
		SideEffectObjects.push_back(Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName(std::format("StructDefaultSideEffect{}", ++SideEffectSequence))));
		std::construct_at(static_cast<int32*>(Destination), 7);
	}

	auto DestroyReferenceDefault(void* Value) -> void
	{
		std::destroy_at(static_cast<FReferenceDefault*>(Value));
	}

	auto CollectReferenceDefault(void* Value, Durin::FReferenceCollector& Collector) -> void
	{
		Collector.AddReferencedObject(static_cast<FReferenceDefault*>(Value)->Value);
	}

	auto SerializeDefault(Durin::FArchive&, void*) -> void
	{
	}

	struct FUnsupportedArchiveLayout
	{
		int32 Value = 0;
	};
	struct FIncompleteAuthoredStruct
	{
		int32 Value = 0;
	};

	struct FOrdinary
	{
		int32 Value = 7;
	};

	struct FMoveOnly
	{
		int32 Value = 0;

		FMoveOnly() = default;
		FMoveOnly(const FMoveOnly&) = delete;
		auto operator=(const FMoveOnly&) -> FMoveOnly& = delete;
		FMoveOnly(FMoveOnly&&) = default;
		auto operator=(FMoveOnly&&) -> FMoveOnly& = default;
	};

	struct FDeletedDefault
	{
		FDeletedDefault() = delete;
		explicit FDeletedDefault(int32 InValue)
			: Value(InValue)
		{
		}
		int32 Value = 0;
	};

	struct FNonTrivial
	{
		std::string Value;
	};

	struct FCustomOps
	{
		int32 Value = 0;
	};

	struct FMalformedIdentical
	{
	};
} // namespace StructOpsTest

namespace Durin
{
	template<>
	struct TDStructOpsTraits<StructOpsTest::FCustomOps>
		: TDStructOpsTraitsBase<StructOpsTest::FCustomOps>
	{
		static constexpr bool bWithZeroConstruct = true;
		static constexpr bool bWithIdentical = true;
		static constexpr bool bWithSerializer = true;
		static constexpr bool bWithPostDeserialize = true;
		static constexpr bool bWithReferenceCollector = true;
		static constexpr bool bHasCompleteAuthoredFields = false;

		static auto ZeroConstruct(void* Destination) -> void
		{
			std::construct_at(static_cast<StructOpsTest::FCustomOps*>(Destination));
		}

		static auto Identical(
			const StructOpsTest::FCustomOps& Left,
			const StructOpsTest::FCustomOps& Right
		) -> bool
		{
			return Left.Value == Right.Value;
		}

		static auto Serialize(FArchive&, StructOpsTest::FCustomOps&) -> void {}
		static auto PostDeserialize(
			StructOpsTest::FCustomOps&,
			FDStructPostDeserializeContext&
		) -> bool { return true; }
		static auto CollectReferences(
			StructOpsTest::FCustomOps&,
			FReferenceCollector&
		) -> void {}
	};

	template<>
	struct TDStructOpsTraits<StructOpsTest::FMalformedIdentical>
		: TDStructOpsTraitsBase<StructOpsTest::FMalformedIdentical>
	{
		static constexpr bool bWithIdentical = true;
		static auto Identical(
			const StructOpsTest::FMalformedIdentical&,
			const StructOpsTest::FMalformedIdentical&
		) -> void {}
	};
} // namespace Durin

template<typename T>
concept CArchiveWritable = requires(Durin::FArchive& Archive, T& Value)
{
	Archive << Value;
};

static_assert(CArchiveWritable<int32>);
static_assert(CArchiveWritable<Durin::FGuid>);
static_assert(!CArchiveWritable<StructOpsTest::FUnsupportedArchiveLayout>);

static_assert(!Durin::Private::CValidDStructIdenticalTrait<
			  StructOpsTest::FMalformedIdentical,
			  Durin::TDStructOpsTraits<StructOpsTest::FMalformedIdentical>>);
static_assert(!std::is_aggregate_v<Durin::DurinCodeGen::FPropertyParamsBase>);
static_assert(!std::is_constructible_v<
			  Durin::DurinCodeGen::FPropertyParamsBase,
			  const char*,
			  Durin::EPropertyFlags,
			  uint16,
			  uint16,
			  Durin::DurinCodeGen::EPropertyGenFlags,
			  Durin::DurinCodeGen::EPropertyParamLayout>);
static_assert(Durin::DurinCodeGen::TIsPlainPropertyMapping<
			  int32,
			  Durin::DurinCodeGen::EPropertyGenFlags::Int32>);
static_assert(!Durin::DurinCodeGen::TIsPlainPropertyMapping<
			  int32,
			  Durin::DurinCodeGen::EPropertyGenFlags::Float>);
static_assert(!std::is_same_v<
			  Durin::DurinCodeGen::FInt32PropertyParams,
			  Durin::DurinCodeGen::FPropertyParamsBase>);
static_assert(!std::is_same_v<
			  Durin::DurinCodeGen::FInt32PropertyParams,
			  Durin::DurinCodeGen::FFloatPropertyParams>);

namespace
{
	struct FReflectedPropertyOwnerForTest
	{
		int32 Value = 0;
		Durin::DObject* ObjectValue = nullptr;
		Durin::TObjectPtr<Durin::DObject> ObjectPtrValue;
		std::string StringValue;
		Durin::FName NameValue;
		std::vector<Durin::DObject*> ObjectArray;
		std::vector<Durin::TObjectPtr<Durin::DObject>> ObjectPtrArray;
		std::unordered_map<std::string, int32> StringToInt;
		std::vector<std::vector<int32>> NestedScores;
		std::unordered_map<std::string, std::vector<Durin::DObject*>> ObjectLists;
	};

	enum class EReflectedEnumForTest : uint8
	{
		A,
		B = 4
	};

	enum class ESignedEnumValueForTest : int8
	{
		Negative = -1,
		Positive = 1
	};

	enum class EUnsignedEnumValueForTest : uint64
	{
		Low = 0,
		High = std::numeric_limits<uint64>::max()
	};

	struct FBuiltInLeafOwnerForTest
	{
		bool BoolValue = false;
		int8 Int8Value = 0;
		int16 Int16Value = 0;
		int32 Int32Value = 0;
		int64 Int64Value = 0;
		uint8 UInt8Value = 0;
		uint16 UInt16Value = 0;
		uint32 UInt32Value = 0;
		uint64 UInt64Value = 0;
		float FloatValue = 0.0f;
		double DoubleValue = 0.0;
		std::string StringValue;
		Durin::FName NameValue;
		Durin::FGuid GuidValue;
		int32 FixedValues[2] = {};
		int8 EnumInt8 = 0;
		int16 EnumInt16 = 0;
		int32 EnumInt32 = 0;
		int64 EnumInt64 = 0;
		uint8 EnumUInt8 = 0;
		uint16 EnumUInt16 = 0;
		uint32 EnumUInt32 = 0;
		uint64 EnumUInt64 = 0;
		Durin::DObject* RawObject = nullptr;
		Durin::TObjectPtr<Durin::DObject> ObjectPtr;
	};

	template<Durin::DurinCodeGen::EEnumUnderlyingType UnderlyingType, typename TValue>
	auto GetBuiltInLeafEnumForTest() -> Durin::DEnum*
	{
		static Durin::DEnum* Enum = new Durin::DEnum(
			Durin::EC_StaticConstructor,
			Durin::FName(std::format("BuiltInLeafEnum{}", static_cast<uint8>(UnderlyingType))),
			Durin::FName(std::format("BuiltInLeafEnum{}", static_cast<uint8>(UnderlyingType))),
			Durin::FName(std::format("BuiltInLeafEnum{}", static_cast<uint8>(UnderlyingType))),
			"",
			true,
			UnderlyingType,
			static_cast<uint16>(sizeof(TValue)),
			std::vector<Durin::FEnumValue>{},
			Durin::EObjectFlags::Transient
		);
		return Enum;
	}

TEST(FCoreDObjectReflectionTests, PropertyKindTraitsClassifyKinds)
{
	using enum Durin::DurinCodeGen::EPropertyGenFlags;
	using Durin::DurinCodeGen::AllPropertyKinds;
	using Durin::DurinCodeGen::IsBitwiseIdentityKind;
	using Durin::DurinCodeGen::IsFixedWidthScalarKind;
	using Durin::DurinCodeGen::IsFloatingPointKind;
	using Durin::DurinCodeGen::IsIntegralKind;
	using Durin::DurinCodeGen::IsNumericKind;
	using Durin::DurinCodeGen::IsSignedIntegralKind;
	using Durin::DurinCodeGen::IsUnsignedIntegralKind;

	constexpr std::array SignedIntegralKinds{Int8, Int16, Int32, Int64};
	constexpr std::array UnsignedIntegralKinds{UInt8, UInt16, UInt32, UInt64};
	constexpr std::array FloatingPointKinds{Float, Double};
	constexpr std::array FixedWidthScalarKinds{
		Bool,
		Int8, Int16, Int32, Int64,
		UInt8, UInt16, UInt32, UInt64,
		Float, Double,
		Enum,
		Byte,
	};
	constexpr std::array BitwiseIdentityKinds{
		Bool,
		Int8, Int16, Int32, Int64,
		UInt8, UInt16, UInt32, UInt64,
		Float, Double,
		Enum,
		Byte,
	};
	auto Contains = [](const auto& Kinds, const auto Kind) {
		return std::ranges::find(Kinds, Kind) != Kinds.end();
	};

	for (const auto Kind : AllPropertyKinds)
	{
		const bool bSignedIntegral = Contains(SignedIntegralKinds, Kind);
		const bool bUnsignedIntegral = Contains(UnsignedIntegralKinds, Kind);
		const bool bFloatingPoint = Contains(FloatingPointKinds, Kind);
		EXPECT_EQ(IsSignedIntegralKind(Kind), bSignedIntegral);
		EXPECT_EQ(IsUnsignedIntegralKind(Kind), bUnsignedIntegral);
		EXPECT_EQ(IsIntegralKind(Kind), bSignedIntegral || bUnsignedIntegral);
		EXPECT_EQ(IsFloatingPointKind(Kind), bFloatingPoint);
		EXPECT_EQ(IsNumericKind(Kind), bSignedIntegral || bUnsignedIntegral || bFloatingPoint);
		EXPECT_EQ(IsFixedWidthScalarKind(Kind), Contains(FixedWidthScalarKinds, Kind));
		EXPECT_EQ(IsBitwiseIdentityKind(Kind), Contains(BitwiseIdentityKinds, Kind));
	}
}

TEST(FCoreDObjectReflectionTests, BitwiseIdentityDistinguishesFloatingPointRepresentations)
{
	Durin::FNumericProperty Property(
		Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::None, 1, 0, sizeof(float),
		Durin::DurinCodeGen::EPropertyGenFlags::Float, nullptr
	);
	const float PositiveZero = 0.0f;
	const float NegativeZero = -0.0f;
	EXPECT_EQ(PositiveZero, NegativeZero);

	Durin::FPropertyIdentityDiagnostic Diagnostic;
	EXPECT_EQ(
		Durin::ComparePropertyValues(
			&Property, &PositiveZero, 0, &NegativeZero, 0, &Diagnostic),
		Durin::EPropertyIdentityResult::Different
	);
	EXPECT_EQ(Diagnostic.LogicalKind, Durin::DurinCodeGen::EPropertyGenFlags::Float);
	EXPECT_EQ(Diagnostic.Reason, Durin::EPropertyIdentityReason::ValueMismatch);
}

TEST(FCoreDObjectReflectionTests, ByteBlobArchiveRoundTripsAndRejectsTruncationTransactionally)
{
	std::vector<std::byte> Source{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
	std::vector<std::byte> Bytes;
	Durin::FObjectMemoryWriter Writer(Bytes, Durin::EArchivePurpose::PropertySnapshot);
	Writer.SerializeByteBlob(Source);
	ASSERT_FALSE(Writer.HasError());

	std::vector<std::byte> Loaded;
	Durin::FObjectMemoryReader Reader(Bytes, Durin::EArchivePurpose::PropertySnapshot);
	Reader.SerializeByteBlob(Loaded);
	ASSERT_FALSE(Reader.HasError());
	EXPECT_EQ(Loaded, Source);

	Bytes.pop_back();
	std::vector<std::byte> Preserved{std::byte{0x7f}};
	Durin::FObjectMemoryReader Truncated(Bytes, Durin::EArchivePurpose::PropertySnapshot);
	Truncated.SerializeByteBlob(Preserved);
	EXPECT_TRUE(Truncated.HasError());
	EXPECT_EQ(Preserved, (std::vector<std::byte>{std::byte{0x7f}}));

	std::vector<std::byte> Oversized(sizeof(uint64));
	const uint64 OversizedCount = 1024ull * 1024 * 1024 + 1;
	for (size_t Index = 0; Index < Oversized.size(); ++Index)
		Oversized[Index] = static_cast<std::byte>(OversizedCount >> (Index * 8));
	Durin::FObjectMemoryReader OversizedReader(
		Oversized, Durin::EArchivePurpose::PropertySnapshot);
	OversizedReader.SerializeByteBlob(Preserved);
	EXPECT_TRUE(OversizedReader.HasError());
	EXPECT_EQ(Preserved, (std::vector<std::byte>{std::byte{0x7f}}));
}

	auto GetInvalidBuiltInLeafEnumForTest() -> Durin::DEnum*
	{
		static Durin::DEnum* Enum = new Durin::DEnum(
			Durin::EC_StaticConstructor,
			Durin::FName("InvalidBuiltInLeafEnum"),
			Durin::FName("InvalidBuiltInLeafEnum"),
			Durin::FName("InvalidBuiltInLeafEnum"),
			"",
			true,
			Durin::DurinCodeGen::EEnumUnderlyingType::UInt64,
			static_cast<uint16>(sizeof(uint8)),
			std::vector<Durin::FEnumValue>{},
			Durin::EObjectFlags::Transient
		);
		return Enum;
	}

	template<typename T>
	auto VectorPropertyNum(const void* Container) -> uint64
	{
		const auto* Value = static_cast<const std::vector<T>*>(Container);
		return static_cast<uint64>(Value->size());
	}

	template<typename T>
	auto VectorPropertyGetElement(const void* Container, uint64 Index) -> const void*
	{
		const auto* Value = static_cast<const std::vector<T>*>(Container);
		return &(*Value)[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto VectorPropertyGetMutableElement(void* Container, uint64 Index) -> void*
	{
		auto* Value = static_cast<std::vector<T>*>(Container);
		return &(*Value)[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto VectorPropertyResize(void* Container, uint64 Num) -> bool
	{
		auto* Value = static_cast<std::vector<T>*>(Container);
		Value->resize(static_cast<size_t>(Num));
		return true;
	}

	template<typename T>
	auto GVectorPropertyHelper() -> const Durin::FArrayOps*
	{
		return Durin::ResolveArrayOps<std::vector<T>>();
	}

	template<typename K, typename V>
	using TTestMap = std::unordered_map<K, V>;

	template<typename K, typename V>
	auto MapPropertyNum(const void* Container) -> uint64
	{
		return static_cast<uint64>(static_cast<const TTestMap<K, V>*>(Container)->size());
	}

	template<typename K, typename V>
	auto MapPropertyGetKey(const void* Container, uint64 Index) -> const void*
	{
		auto It = static_cast<const TTestMap<K, V>*>(Container)->begin();
		std::advance(It, static_cast<size_t>(Index));
		return &It->first;
	}

	template<typename K, typename V>
	auto MapPropertyGetValue(const void* Container, uint64 Index) -> const void*
	{
		auto It = static_cast<const TTestMap<K, V>*>(Container)->begin();
		std::advance(It, static_cast<size_t>(Index));
		return &It->second;
	}

	template<typename K, typename V>
	auto MapPropertyGetMutableValue(void* Container, uint64 Index) -> void*
	{
		auto It = static_cast<TTestMap<K, V>*>(Container)->begin();
		std::advance(It, static_cast<size_t>(Index));
		return &It->second;
	}

	template<typename K, typename V>
	auto MapPropertyClear(void* Container) -> void { static_cast<TTestMap<K, V>*>(Container)->clear(); }

	template<typename T>
	auto MapPropertyCreateValue() -> void* { return new T(); }

	template<typename T>
	auto MapPropertyCreateValueCopy(const void* Value) -> void* { return new T(*static_cast<const T*>(Value)); }

	template<typename T>
	auto MapPropertyDestroyValue(void* Value) -> void { delete static_cast<T*>(Value); }

	template<typename K, typename V>
	auto MapPropertyInsert(void* Container, const void* Key, const void* Value) -> bool
	{
		static_cast<TTestMap<K, V>*>(Container)->insert_or_assign(*static_cast<const K*>(Key), *static_cast<const V*>(Value));
		return true;
	}

	template<typename K, typename V>
	auto MapPropertyContains(const void* Container, const void* Key) -> bool
	{
		return static_cast<const TTestMap<K, V>*>(Container)->contains(*static_cast<const K*>(Key));
	}

	template<typename K, typename V>
	auto MapPropertyRenameKey(void* Container, const void* OldKey, const void* NewKey) -> bool
	{
		auto* Map = static_cast<TTestMap<K, V>*>(Container);
		const K OldKeyCopy = *static_cast<const K*>(OldKey);
		const K NewKeyCopy = *static_cast<const K*>(NewKey);
		if (OldKeyCopy == NewKeyCopy || Map->contains(NewKeyCopy)) return false;
		auto Node = Map->extract(OldKeyCopy);
		if (Node.empty()) return false;
		Node.key() = NewKeyCopy;
		Map->insert(std::move(Node));
		return true;
	}

	template<typename K, typename V>
	auto MapPropertyRemove(void* Container, const void* Key) -> bool
	{
		return static_cast<TTestMap<K, V>*>(Container)->erase(*static_cast<const K*>(Key)) != 0;
	}

	template<typename K, typename V>
	auto GMapPropertyHelper() -> const Durin::FMapOps*
	{
		return Durin::ResolveMapOps<TTestMap<K, V>>();
	}

	Durin::DEnum* Z_Construct_DEnum_EReflectedEnumForTest_NoRegister();

	struct FReflectedEnumPropertyOwnerForTest
	{
		EReflectedEnumForTest Mode = EReflectedEnumForTest::A;
	};

	struct FWideEnumPropertyOwnerForTest
	{
		ESignedEnumValueForTest Signed = ESignedEnumValueForTest::Negative;
		EUnsignedEnumValueForTest Unsigned = EUnsignedEnumValueForTest::High;
	};

	enum class EShutdownDestroyCheckpoint
	{
		BeforeRelease,
		FencePending,
		FinishDestroy,
		Destructor
	};

	struct FShutdownDestroyScheduler
	{
		bool bFenceComplete = false;
		std::vector<EShutdownDestroyCheckpoint> Checkpoints;

		auto Record(EShutdownDestroyCheckpoint Checkpoint) -> void
		{
			if (Checkpoints.empty() || Checkpoints.back() != Checkpoint)
			{
				Checkpoints.push_back(Checkpoint);
			}
		}
	};

	class DLifecycleTestObject : public Durin::DObject
	{
	public:
		inline static uint64 BeginDestroyCount = 0;
		inline static uint64 FinishDestroyCount = 0;
		inline static uint64 DestructorCount = 0;
		bool bReadyForFinishDestroy = true;
		bool bCollectGarbageInBeginDestroy = false;
		FShutdownDestroyScheduler* ShutdownDestroyScheduler = nullptr;

		explicit DLifecycleTestObject(const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
		}

		~DLifecycleTestObject() override
		{
			++DestructorCount;
			if (ShutdownDestroyScheduler)
			{
				ShutdownDestroyScheduler->Record(EShutdownDestroyCheckpoint::Destructor);
			}
		}

		auto BeginDestroy() -> void override
		{
			++BeginDestroyCount;
			if (bCollectGarbageInBeginDestroy) Durin::CollectGarbage();
			if (ShutdownDestroyScheduler)
			{
				ShutdownDestroyScheduler->Record(EShutdownDestroyCheckpoint::BeforeRelease);
				ShutdownDestroyScheduler->Record(EShutdownDestroyCheckpoint::FencePending);
			}
		}

		auto IsReadyForFinishDestroy() -> bool override
		{
			if (!ShutdownDestroyScheduler) return bReadyForFinishDestroy;
			if (!ShutdownDestroyScheduler->bFenceComplete)
			{
				ShutdownDestroyScheduler->Record(EShutdownDestroyCheckpoint::FencePending);
				return false;
			}
			return true;
		}

		auto FinishDestroy() -> void override
		{
			++FinishDestroyCount;
			if (ShutdownDestroyScheduler)
			{
				ShutdownDestroyScheduler->Record(EShutdownDestroyCheckpoint::FinishDestroy);
			}
		}

		static auto ResetLifecycleCounts() -> void
		{
			BeginDestroyCount = 0;
			FinishDestroyCount = 0;
			DestructorCount = 0;
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DLifecycleTestObject(X);
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor,
					Durin::FName("DLifecycleTestObject"),
					sizeof(DLifecycleTestObject),
					alignof(DLifecycleTestObject),
					Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DLifecycleTestObject>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DLifecycleTestObject");
				Durin::DObjectForceRegistration(Class);
			}
			return Class;
		}
	};

	std::vector<std::string> GOrderedDefaultConstruction;

	void ConstructOrderedDefault(const Durin::FObjectInitializer& ObjectInitializer)
	{
		if (ObjectInitializer.Purpose == Durin::EObjectConstructionPurpose::ClassDefaultObject)
		{
			GOrderedDefaultConstruction.push_back(ObjectInitializer.Class->GetName());
		}
		new (ObjectInitializer.GetObj()) DLifecycleTestObject(ObjectInitializer);
	}

	class DRecursiveDefaultObjectForTest : public Durin::DObject
	{
	public:
		inline static uint64 DestructorCount = 0;

		explicit DRecursiveDefaultObjectForTest(
			const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
			if (IsClassDefaultObject())
			{
				(void)Durin::NewObject<DLifecycleTestObject>(
					this,
					Durin::FName("RecursiveDefaultInner"),
					Durin::EObjectConstructionPurpose::ClassDefaultSubobject);
				(void)StaticClass()->GetDefaultObject();
			}
		}

		~DRecursiveDefaultObjectForTest() override
		{
			++DestructorCount;
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DRecursiveDefaultObjectForTest(X);
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor,
					Durin::FName("DRecursiveDefaultObjectForTest"),
					sizeof(DRecursiveDefaultObjectForTest),
					alignof(DRecursiveDefaultObjectForTest),
					Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DRecursiveDefaultObjectForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DRecursiveDefaultObjectForTest");
				Durin::DObjectForceRegistration(Class);
			}
			return Class;
		}
	};

	class DDefaultGraphOwnerForTest : public Durin::DObject
	{
	public:
		explicit DDefaultGraphOwnerForTest(
			const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
			const Durin::EObjectConstructionPurpose ChildPurpose = IsClassDefaultObject()
				? Durin::EObjectConstructionPurpose::ClassDefaultSubobject
				: Durin::EObjectConstructionPurpose::RuntimeObject;
			Child = Durin::NewObject<DLifecycleTestObject>(this, Durin::FName("StableChild"), ChildPurpose);
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DDefaultGraphOwnerForTest(X);
		}

		auto Serialize(Durin::FArchive& Ar) -> void override
		{
			DObject::Serialize(Ar);
			const Durin::FName Owner("Tests::DDefaultGraphOwnerForTest");
			auto SerializeFirst = [&] {
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("NativeFirst"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeFirst;
			};
			auto SerializeSecond = [&] {
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("NativeSecond"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeSecond;
			};
			if (bReverseNativeOrder) { SerializeSecond(); SerializeFirst(); }
			else { SerializeFirst(); SerializeSecond(); }
			{
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("NativeOnlyStruct"),
					Durin::FArchiveLogicalTypeDescriptor::Struct(
						Durin::FName("Tests::FNativeOnlyStruct"), 1)});
				auto ValueField = Durin::EnterArchiveField(Ar, {Durin::FName("Tests::FNativeOnlyStruct"),
					Durin::FName("Value"), Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeOnlyStructValue;
			}
			{
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("NativeMap"),
					Durin::FArchiveLogicalTypeDescriptor::Map(
						Durin::FArchiveLogicalTypeDescriptor::String(),
						Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32))});
				uint64 Count = 1;
				Ar << Count;
				std::string Key = "Key";
				{
					auto KeyScope = Durin::EnterArchiveMapKey(Ar, 0);
					Ar << Key;
				}
				{
					auto ValueScope = Durin::EnterArchiveMapValue(Ar, 0);
					Ar << NativeMapValue;
				}
			}
			if (bEmitLateField && !Ar.IsDiscovering())
			{
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("LateField"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeFirst;
			}
			if (bEmitOptionalField)
			{
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("OptionalField"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeFirst;
			}
			if (bEmitDeepField)
			{
				std::vector<Durin::FArchiveFieldScope> Scopes;
				for (uint32 Depth = 0; Depth < Durin::DefaultDeltaMaxDepth + 2; ++Depth)
				{
					Scopes.push_back(Durin::EnterArchiveField(Ar, {Durin::FName("Tests::FDeepDelta"),
						Durin::FName(std::format("Depth{}", Depth)),
						Durin::FArchiveLogicalTypeDescriptor::Struct(Durin::FName("Tests::FDeepDelta"))}));
				}
				auto Leaf = Durin::EnterArchiveField(Ar, {Durin::FName("Tests::FDeepDelta"), Durin::FName("Leaf"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeFirst;
			}
			if (bEmitOversizedArray)
			{
				auto Field = Durin::EnterArchiveField(Ar, {Owner, Durin::FName("OversizedArray"),
					Durin::FArchiveLogicalTypeDescriptor::Array(
						Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32))});
				uint64 Count = Durin::DefaultDeltaMaxFields + 1;
				Ar << Count;
			}
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor,
					Durin::FName("Tests::DDefaultGraphOwnerForTest"),
					sizeof(DDefaultGraphOwnerForTest),
					alignof(DDefaultGraphOwnerForTest),
					Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DDefaultGraphOwnerForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->SetTypeNames("DDefaultGraphOwnerForTest", "", "");
				Class->Register(Durin::DClass::StaticClass, "", "DDefaultGraphOwnerForTest");
				Durin::DObjectForceRegistration(Class);
				const auto Params = Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"Child", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DDefaultGraphOwnerForTest, Child),
					&Durin::DObject::StaticClass
				);
				auto* ChildProperty = new Durin::FObjectProperty(
					Durin::FFieldVariant(Class), Durin::FName("Child"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DDefaultGraphOwnerForTest, Child),
					static_cast<uint16>(sizeof(Durin::DObject*)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(),
					true, Params.ReadObjectValue, Params.WriteObjectValue
				);
				auto* ClassSpecificProperty = new Durin::FStructProperty(
					Durin::FFieldVariant(Class), Durin::FName("ClassSpecific"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DDefaultGraphOwnerForTest, ClassSpecific),
					Durin::Z_Construct_DStruct_FVector3()
				);
				auto* FixedProperty = new Durin::FNumericProperty(
					Durin::FFieldVariant(Class), Durin::FName("Fixed"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::None, 2,
					STRUCT_OFFSET_UINT16(DDefaultGraphOwnerForTest, Fixed),
					sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
				auto* ExactFloatProperty = new Durin::FNumericProperty(
					Durin::FFieldVariant(Class), Durin::FName("ExactFloat"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DDefaultGraphOwnerForTest, ExactFloat),
					sizeof(double), Durin::DurinCodeGen::EPropertyGenFlags::Double, nullptr);
				auto* BlobProperty = new Durin::FProperty(
					Durin::FFieldVariant(Class), Durin::FName("Blob"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DDefaultGraphOwnerForTest, Blob),
					static_cast<uint16>(sizeof(std::vector<std::byte>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Blob, nullptr);
				const auto BlobOps = Durin::DurinCodeGen::MakePropertyValueOps<std::vector<std::byte>>();
				BlobProperty->SetValueLifecycle(BlobOps.ValueSize, BlobOps.ValueAlignment,
					BlobOps.InitializeValue, BlobOps.DestroyValue,
					BlobOps.CopyConstructValue, BlobOps.CopyAssignValue);
				ChildProperty->Next = ClassSpecificProperty;
				ClassSpecificProperty->Next = FixedProperty;
				FixedProperty->Next = ExactFloatProperty;
				ExactFloatProperty->Next = BlobProperty;
				Class->ChildProperties = ChildProperty;
			}
			return Class;
		}

		Durin::TObjectPtr<Durin::DObject> Child;
		Durin::FVector3 ClassSpecific{1.0, 2.0, 3.0};
		int32 Fixed[2]{4, 5};
		double ExactFloat = std::bit_cast<double>(uint64{0x7FF8000000000042ull});
		std::vector<std::byte> Blob;
		int32 NativeFirst = 7;
		int32 NativeSecond = 9;
		int32 NativeOnlyStructValue = 3;
		int32 NativeMapValue = 11;
		bool bReverseNativeOrder = false;
		bool bEmitLateField = false;
		bool bEmitOptionalField = false;
		bool bEmitDeepField = false;
		bool bEmitOversizedArray = false;
	};

	class DAuthoritativeDeltaOwnerForTest : public Durin::DObject
	{
	public:
		explicit DAuthoritativeDeltaOwnerForTest(
			const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
			Value.Value = "Default";
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DAuthoritativeDeltaOwnerForTest(X);
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DStruct* TextStruct = nullptr;
			if (!TextStruct)
			{
				static Durin::FDStructOps Ops;
				Ops.Flags = Durin::EDStructOpsFlags::DefaultConstruct
					| Durin::EDStructOpsFlags::Destroy
					| Durin::EDStructOpsFlags::Identical
					| Durin::EDStructOpsFlags::AuthoredFieldsComplete;
				Ops.DefaultConstruct = &StructOpsTest::ConstructAuthoritativeText;
				Ops.Destroy = &StructOpsTest::DestroyAuthoritativeText;
				Ops.Identical = &StructOpsTest::IdenticalAuthoritativeText;
				static const Durin::DurinCodeGen::FStringPropertyParams TextProperty = {
					"Value", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(StructOpsTest::FAuthoritativeText, Value)};
				static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
					&TextProperty};
				auto NoRegister = []() -> Durin::DStruct* {
					if (!TextStruct)
					{
						TextStruct = new Durin::DStruct(
							Durin::EC_StaticConstructor, Durin::FName("Tests::FAuthoritativeText"),
							Durin::FName("FAuthoritativeText"), sizeof(StructOpsTest::FAuthoritativeText),
							alignof(StructOpsTest::FAuthoritativeText), Durin::EObjectFlags::NoFlags);
						TextStruct->Register(
							Durin::DStruct::StaticClass, "/Cpp/CoreDObjectTests", "Tests::FAuthoritativeText");
					}
					return TextStruct;
				};
				const Durin::DurinCodeGen::FStructParams Params = {
					NoRegister, "Tests::FAuthoritativeText", "FAuthoritativeText",
					sizeof(StructOpsTest::FAuthoritativeText), alignof(StructOpsTest::FAuthoritativeText),
					Properties, std::size(Properties), &Ops};
				TextStruct = Durin::DurinCodeGen::ConstructDStruct(Params);
			}

			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, Durin::FName("Tests::DAuthoritativeDeltaOwnerForTest"),
					sizeof(DAuthoritativeDeltaOwnerForTest), alignof(DAuthoritativeDeltaOwnerForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DAuthoritativeDeltaOwnerForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DAuthoritativeDeltaOwnerForTest");
				Durin::DObjectForceRegistration(Class);
				Class->ChildProperties = new Durin::FStructProperty(
					Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DAuthoritativeDeltaOwnerForTest, Value), TextStruct);
			}
			return Class;
		}

		StructOpsTest::FAuthoritativeText Value;
	};

	class DOverrideContainerOwnerForTest : public Durin::DObject
	{
	public:
		explicit DOverrideContainerOwnerForTest(
			const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
			Values.push_back(0);
			Lookup.emplace("Stable", 0);
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DOverrideContainerOwnerForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, Durin::FName("DOverrideContainerOwnerForTest"),
					sizeof(DOverrideContainerOwnerForTest), alignof(DOverrideContainerOwnerForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DOverrideContainerOwnerForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DOverrideContainerOwnerForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = [] {
				static const Durin::DurinCodeGen::FInt32PropertyParams ValuesInner = {
					"Values_Inner", Durin::EPropertyFlags::None, 1, 0};
				static const char* const ValuesLegacyNames[] = {"LegacyValues", "OlderValues"};
				static const Durin::DurinCodeGen::FArrayPropertyParams ValuesProperty =
					Durin::DurinCodeGen::WithLegacyNames(
						Durin::DurinCodeGen::FArrayPropertyParams{
							"Values", Durin::EPropertyFlags::None, 1,
							STRUCT_OFFSET_UINT16(DOverrideContainerOwnerForTest, Values),
							&ValuesInner, &GVectorPropertyHelper<int32>},
						ValuesLegacyNames, std::size(ValuesLegacyNames));
				static const Durin::DurinCodeGen::FStringPropertyParams LookupKey = {
					"Lookup_Key", Durin::EPropertyFlags::None, 1, 0};
				static const Durin::DurinCodeGen::FInt32PropertyParams LookupValue = {
					"Lookup_Value", Durin::EPropertyFlags::None, 1, 0};
				static const Durin::DurinCodeGen::FMapPropertyParams LookupProperty = {
					"Lookup", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DOverrideContainerOwnerForTest, Lookup),
					&LookupKey, &LookupValue,
					&GMapPropertyHelper<std::string, int32>};
				static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
					&ValuesProperty, &LookupProperty};
				static const Durin::DurinCodeGen::FClassParams Params = {
					&DOverrideContainerOwnerForTest::StaticClassNoRegister,
					"DOverrideContainerOwnerForTest", "DOverrideContainerOwnerForTest",
					Properties, std::size(Properties)};
				return Durin::DurinCodeGen::ConstructDClass(Params);
			}();
			return Class;
		}

		std::vector<int32> Values;
		TTestMap<std::string, int32> Lookup;
	};

	class DLifecycleReferenceOwnerForTest : public Durin::DObject
	{
	public:
		struct FWeakNested
		{
			Durin::TWeakObjectPtr<Durin::DObject> Reference;
		};

		struct FNativeStruct
		{
			int32 Value = 0;
			Durin::FName Label;
		};

		explicit DLifecycleReferenceOwnerForTest(const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DLifecycleReferenceOwnerForTest(X);
		}

		static auto WeakNestedStructNoRegister() -> Durin::DStruct*
		{
			static Durin::DStruct* Struct = nullptr;
			if (!Struct)
			{
				Struct = new Durin::DStruct(Durin::EC_StaticConstructor,
					Durin::FName("FWeakNested"), Durin::FName("FWeakNested"),
					sizeof(FWeakNested), alignof(FWeakNested), Durin::EObjectFlags::Transient);
				Struct->Register(Durin::DStruct::StaticClass, "", "FWeakNested");
			}
			return Struct;
		}

		static auto WeakNestedStruct() -> Durin::DStruct*
		{
			static const auto Reference = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<
				Durin::TWeakObjectPtr<Durin::DObject>>(
				"Reference", Durin::EPropertyFlags::Transient, 1,
				STRUCT_OFFSET_UINT16(FWeakNested, Reference), &Durin::DObject::StaticClass);
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Reference};
			static const Durin::DurinCodeGen::FStructParams Params = {
				&WeakNestedStructNoRegister, "Tests::FWeakNested", "FWeakNested",
				sizeof(FWeakNested), alignof(FWeakNested), Properties, std::size(Properties),
				&Durin::GetDStructOps<FWeakNested>()};
			return Durin::DurinCodeGen::ConstructDStruct(Params);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor,
					Durin::FName("DLifecycleReferenceOwnerForTest"),
					sizeof(DLifecycleReferenceOwnerForTest),
					alignof(DLifecycleReferenceOwnerForTest),
					Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DLifecycleReferenceOwnerForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DLifecycleReferenceOwnerForTest");
				Durin::DObjectForceRegistration(Class);
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			Durin::DClass* Class = StaticClassNoRegister();

			static bool bPropertiesConstructed = false;
			if (!bPropertiesConstructed)
			{
				static const Durin::DurinCodeGen::FInt32PropertyParams ValuePropertyParams = {
					"Value",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, Value)
				};
				static const Durin::DurinCodeGen::FBoolPropertyParams BoolPropertyParams = {
					"bEnabled",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, bEnabled)
				};
				static const Durin::DurinCodeGen::FStringPropertyParams NamePropertyParams = {
					"Label",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, Label)
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams ReferencePropertyParams =
					Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
						"Reference",
						Durin::EPropertyFlags::None,
						1,
						STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, Reference),
						&Durin::DObject::StaticClass
					);
				static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrReferencePropertyParams =
					Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
						"ObjectPtrReference",
						Durin::EPropertyFlags::None,
						1,
						STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, ObjectPtrReference),
						&Durin::DObject::StaticClass
					);
				static const Durin::DurinCodeGen::FObjectPropertyParams RawReferencesInnerPropertyParams =
					Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
						"RawReferences_Inner",
						Durin::EPropertyFlags::None,
						1,
						0,
						&Durin::DObject::StaticClass
					);
				static const Durin::DurinCodeGen::FArrayPropertyParams RawReferencesPropertyParams = {
					"RawReferences",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, RawReferences),
					&RawReferencesInnerPropertyParams,
					&GVectorPropertyHelper<Durin::DObject*>
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrReferencesInnerPropertyParams =
					Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
						"ObjectPtrReferences_Inner",
						Durin::EPropertyFlags::None,
						1,
						0,
						&Durin::DObject::StaticClass
					);
				static const Durin::DurinCodeGen::FArrayPropertyParams ObjectPtrReferencesPropertyParams = {
					"ObjectPtrReferences",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, ObjectPtrReferences),
					&ObjectPtrReferencesInnerPropertyParams,
					&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
				};
				static const Durin::DurinCodeGen::FInt32PropertyParams ScoresInnerPropertyParams = {
					"Scores_Inner",
					Durin::EPropertyFlags::None,
					1,
					0
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoresPropertyParams = {
					"Scores",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, Scores),
					&ScoresInnerPropertyParams,
					&GVectorPropertyHelper<int32>
				};
				static const Durin::DurinCodeGen::FStringPropertyParams TagsInnerPropertyParams = {
					"Tags_Inner",
					Durin::EPropertyFlags::None,
					1,
					0
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams TagsPropertyParams = {
					"Tags",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, Tags),
					&TagsInnerPropertyParams,
					&GVectorPropertyHelper<std::string>
				};
				static const Durin::DurinCodeGen::FEnumPropertyParams ModesInnerPropertyParams = {
					"Modes_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ModesPropertyParams = {
					"Modes",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, Modes),
					&ModesInnerPropertyParams,
					&GVectorPropertyHelper<EReflectedEnumForTest>
				};
				static const Durin::DurinCodeGen::FInt32PropertyParams ScoreGroupsInnerInnerPropertyParams = {
					"ScoreGroups_Inner_Inner",
					Durin::EPropertyFlags::None,
					1,
					0
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoreGroupsInnerPropertyParams = {
					"ScoreGroups_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					&ScoreGroupsInnerInnerPropertyParams,
					&GVectorPropertyHelper<int32>
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoreGroupsPropertyParams = {
					"ScoreGroups",
					Durin::EPropertyFlags::None,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, ScoreGroups),
					&ScoreGroupsInnerPropertyParams,
					&GVectorPropertyHelper<std::vector<int32>>
				};
				static const Durin::DurinCodeGen::FInt32PropertyParams TransientPropertyParams = {
					"TransientValue",
					Durin::EPropertyFlags::Transient,
					1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, TransientValue)
				};
				static const auto WeakReferencePropertyParams =
					Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<Durin::TWeakObjectPtr<Durin::DObject>>(
						"WeakReference", Durin::EPropertyFlags::Transient, 1,
						STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, WeakReference),
						&Durin::DObject::StaticClass);
				static const auto WeakExternalPropertyParams =
					Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<Durin::TWeakObjectPtr<Durin::DObject>>(
						"WeakExternal", Durin::EPropertyFlags::Transient, 1,
						STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, WeakExternal),
						&Durin::DObject::StaticClass);
				static const auto WeakReferencesInnerPropertyParams =
					Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<Durin::TWeakObjectPtr<Durin::DObject>>(
						"WeakReferences_Inner", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass);
				static const Durin::DurinCodeGen::FArrayPropertyParams WeakReferencesPropertyParams = {
					"WeakReferences", Durin::EPropertyFlags::Transient, 1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, WeakReferences),
					&WeakReferencesInnerPropertyParams,
					&GVectorPropertyHelper<Durin::TWeakObjectPtr<Durin::DObject>>};
				static const Durin::DurinCodeGen::FStringPropertyParams WeakMapKeyPropertyParams = {
					"WeakMap_Key", Durin::EPropertyFlags::None, 1, 0};
				static const auto WeakMapValuePropertyParams =
					Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<Durin::TWeakObjectPtr<Durin::DObject>>(
						"WeakMap_Value", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass);
				static const Durin::DurinCodeGen::FMapPropertyParams WeakMapPropertyParams = {
					"WeakMap", Durin::EPropertyFlags::Transient, 1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, WeakMap),
					&WeakMapKeyPropertyParams, &WeakMapValuePropertyParams,
					&GMapPropertyHelper<std::string, Durin::TWeakObjectPtr<Durin::DObject>>};
				static const Durin::DurinCodeGen::FStructPropertyParams WeakNestedPropertyParams = {
					"WeakNested", Durin::EPropertyFlags::Transient, 1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, WeakNested),
					&WeakNestedStruct};
				static const Durin::DurinCodeGen::FStructPropertyParams WeakNestedArrayInnerPropertyParams = {
					"WeakNestedArray_Inner", Durin::EPropertyFlags::None, 1, 0, &WeakNestedStruct};
				static const Durin::DurinCodeGen::FArrayPropertyParams WeakNestedArrayPropertyParams = {
					"WeakNestedArray", Durin::EPropertyFlags::Transient, 1,
					STRUCT_OFFSET_UINT16(DLifecycleReferenceOwnerForTest, WeakNestedArray),
					&WeakNestedArrayInnerPropertyParams, &GVectorPropertyHelper<FWeakNested>};
				static const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[] = {
					&ValuePropertyParams,
					&BoolPropertyParams,
					&NamePropertyParams,
					&ReferencePropertyParams,
					&ObjectPtrReferencePropertyParams,
					&RawReferencesPropertyParams,
					&ObjectPtrReferencesPropertyParams,
					&ScoresPropertyParams,
					&TagsPropertyParams,
					&ModesPropertyParams,
					&ScoreGroupsPropertyParams,
					&TransientPropertyParams,
					&WeakReferencePropertyParams,
					&WeakExternalPropertyParams,
					&WeakReferencesPropertyParams,
					&WeakMapPropertyParams,
					&WeakNestedPropertyParams,
					&WeakNestedArrayPropertyParams
				};
				static const Durin::DurinCodeGen::FClassParams ClassParams = {
					&DLifecycleReferenceOwnerForTest::StaticClassNoRegister,
					"DLifecycleReferenceOwnerForTest",
					"DLifecycleReferenceOwnerForTest",
					PropertyParams,
					18
				};
				Durin::DurinCodeGen::ConstructDClass(ClassParams);
				bPropertiesConstructed = true;
			}
			return Class;
		}

		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) -> void override
		{
			DObject::AddReferencedObjects(Collector);
			Collector.AddReferencedObject(NativeReference);
		}

		auto Serialize(Durin::FArchive& Ar) -> void override
		{
			SerializePurposes.push_back(Ar.GetPurpose());
			if (!bSkipSuperSerialize) DObject::Serialize(Ar);
			if (Ar.HasError()) return;

			const Durin::FName DeclaringType("Tests::DLifecycleReferenceOwnerForTest");
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("NativeScalar"),
					Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
				Ar << NativeScalar;
			}
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("NativeStruct"),
					Durin::FArchiveLogicalTypeDescriptor::Struct(
						Durin::FName("Tests::DLifecycleReferenceOwnerForTest::FNativeStruct"), 1)});
				{
					auto ValueField = Durin::EnterArchiveField(Ar, {
						Durin::FName("Tests::DLifecycleReferenceOwnerForTest::FNativeStruct"),
						Durin::FName("Value"), Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
					Ar << NativeStruct.Value;
				}
				{
					auto LabelField = Durin::EnterArchiveField(Ar, {
						Durin::FName("Tests::DLifecycleReferenceOwnerForTest::FNativeStruct"),
						Durin::FName("Label"), Durin::FArchiveLogicalTypeDescriptor::Name()});
					Ar << NativeStruct.Label;
				}
			}
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("NativeValues"),
					Durin::FArchiveLogicalTypeDescriptor::Array(
						Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32))});
				uint64 Count = static_cast<uint64>(NativeValues.size());
				Ar << Count;
				if (Ar.IsLoading() && !Ar.HasError())
				{
					if (Count > 1024)
					{
						Ar.Fail(Durin::EArchiveFailureCode::InvalidData,
							"NativeValues exceeds its test serializer bound.");
					}
					else
					{
						std::vector<int32> Loaded(static_cast<size_t>(Count));
						for (int32& Value : Loaded) Ar << Value;
						if (!Ar.HasError()) NativeValues = std::move(Loaded);
					}
				}
				else
				{
					for (int32& Value : NativeValues) Ar << Value;
				}
			}
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("SerializedNativeReference"),
					Durin::FArchiveLogicalTypeDescriptor::Object(Durin::DObject::StaticClass()->GetQualifiedName())});
				Durin::DObject* ReferenceValue = Ar.IsSaving()
					? SerializedNativeReference : nullptr;
				Durin::SerializeArchiveObjectReference(Ar, ReferenceValue);
				if (Ar.IsLoading() && !Ar.HasError()) SerializedNativeReference = ReferenceValue;
			}
			if (bEmitLateReference)
			{
				auto Field = Durin::EnterArchiveField(Ar, {DeclaringType, Durin::FName("EmissionOnlyReference"),
					Durin::FArchiveLogicalTypeDescriptor::Object(Durin::DObject::StaticClass()->GetQualifiedName())});
				Durin::DObject* ReferenceValue = Ar.IsDiscovering() ? nullptr : EmissionOnlyReference;
				Durin::SerializeArchiveObjectReference(Ar, ReferenceValue);
			}
			if (bInjectSerializeFailure)
				Ar.Fail(Durin::EArchiveFailureCode::MalformedSerializer,
					"Injected test object serialization failure.");
		}

		auto PostLoad(std::string& OutError) -> bool override
		{
			++PostLoadCallCount;
			if (!bRejectPostLoad) return true;
			OutError = "Injected test object PostLoad failure.";
			return false;
		}

		int32 Value = 0;
		bool bEnabled = false;
		std::string Label;
		Durin::DObject* Reference = nullptr;
		Durin::TObjectPtr<Durin::DObject> ObjectPtrReference;
		std::vector<Durin::DObject*> RawReferences;
		std::vector<Durin::TObjectPtr<Durin::DObject>> ObjectPtrReferences;
		std::vector<int32> Scores;
		std::vector<std::string> Tags;
		std::vector<EReflectedEnumForTest> Modes;
		std::vector<std::vector<int32>> ScoreGroups;
		int32 TransientValue = 0;
		Durin::TWeakObjectPtr<Durin::DObject> WeakReference;
		Durin::TWeakObjectPtr<Durin::DObject> WeakExternal;
		std::vector<Durin::TWeakObjectPtr<Durin::DObject>> WeakReferences;
		std::unordered_map<std::string, Durin::TWeakObjectPtr<Durin::DObject>> WeakMap;
		FWeakNested WeakNested;
		std::vector<FWeakNested> WeakNestedArray;
		Durin::DObject* NativeReference = nullptr;
		int32 NativeScalar = 0;
		FNativeStruct NativeStruct;
		std::vector<int32> NativeValues;
		Durin::DObject* SerializedNativeReference = nullptr;
		Durin::DObject* EmissionOnlyReference = nullptr;
		std::vector<Durin::EArchivePurpose> SerializePurposes;
		bool bSkipSuperSerialize = false;
		bool bEmitLateReference = false;
		bool bInjectSerializeFailure = false;
		bool bRejectPostLoad = false;
		int32 PostLoadCallCount = 0;
	};

	struct FGCReferenceLeafForTest
	{
		Durin::TObjectPtr<Durin::DObject> Reference;
		Durin::TObjectPtr<Durin::DObject> StaticReferences[2];
		int32 NonReferenceValue = 0;
	};

	struct FGCReferenceNestedForTest
	{
		FGCReferenceLeafForTest Leaf;
	};

	auto GetGCReferenceLeafStructForTest() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = [] {
			static const Durin::DurinCodeGen::FObjectPropertyParams Reference =
				Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"Reference", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(FGCReferenceLeafForTest, Reference),
					&Durin::DObject::StaticClass
				);
			static const Durin::DurinCodeGen::FObjectPropertyParams StaticReferences =
				Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"StaticReferences", Durin::EPropertyFlags::None, 2,
					STRUCT_OFFSET_UINT16(FGCReferenceLeafForTest, StaticReferences),
					&Durin::DObject::StaticClass
				);
			static const Durin::DurinCodeGen::FInt32PropertyParams NonReferenceValue = {
				"NonReferenceValue", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(FGCReferenceLeafForTest, NonReferenceValue)
			};
			static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
				&Reference, &StaticReferences, &NonReferenceValue
			};
			static Durin::DStruct* RawStruct = nullptr;
			auto NoRegister = []() -> Durin::DStruct* {
				if (!RawStruct)
				{
					RawStruct = new Durin::DStruct(
						Durin::EC_StaticConstructor,
						Durin::FName("FGCReferenceLeafForTest"),
						Durin::FName("FGCReferenceLeafForTest"),
						sizeof(FGCReferenceLeafForTest), alignof(FGCReferenceLeafForTest), Durin::EObjectFlags::NoFlags
					);
					RawStruct->Register(Durin::DStruct::StaticClass, "", "FGCReferenceLeafForTest");
				}
				return RawStruct;
			};
			static const Durin::DurinCodeGen::FStructParams Params = {
				NoRegister, "FGCReferenceLeafForTest", "FGCReferenceLeafForTest",
				sizeof(FGCReferenceLeafForTest), alignof(FGCReferenceLeafForTest), Properties, std::size(Properties)
			};
			return Durin::DurinCodeGen::ConstructDStruct(Params);
		}();
		return Struct;
	}

	auto GetGCReferenceNestedStructForTest() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = [] {
			static const Durin::DurinCodeGen::FStructPropertyParams Leaf = {
				"Leaf", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(FGCReferenceNestedForTest, Leaf),
				&GetGCReferenceLeafStructForTest
			};
			static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {&Leaf};
			static Durin::DStruct* RawStruct = nullptr;
			auto NoRegister = []() -> Durin::DStruct* {
				if (!RawStruct)
				{
					RawStruct = new Durin::DStruct(
						Durin::EC_StaticConstructor,
						Durin::FName("FGCReferenceNestedForTest"),
						Durin::FName("FGCReferenceNestedForTest"),
						sizeof(FGCReferenceNestedForTest), alignof(FGCReferenceNestedForTest), Durin::EObjectFlags::NoFlags
					);
					RawStruct->Register(Durin::DStruct::StaticClass, "", "FGCReferenceNestedForTest");
				}
				return RawStruct;
			};
			static const Durin::DurinCodeGen::FStructParams Params = {
				NoRegister, "FGCReferenceNestedForTest", "FGCReferenceNestedForTest",
				sizeof(FGCReferenceNestedForTest), alignof(FGCReferenceNestedForTest), Properties, std::size(Properties)
			};
			return Durin::DurinCodeGen::ConstructDStruct(Params);
		}();
		return Struct;
	}

	struct FEditorOnlyNestedForTest
	{
		int32 RuntimeValue = 0;
		int32 EditorValue = 0;
	};

	auto GetEditorOnlyNestedStructForTest() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = [] {
			static Durin::FDStructOps Ops;
			Ops.Flags = Durin::EDStructOpsFlags::AuthoredFieldsComplete;
			static const Durin::DurinCodeGen::FInt32PropertyParams RuntimeValue = {
				"RuntimeValue", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(FEditorOnlyNestedForTest, RuntimeValue)};
			static const Durin::DurinCodeGen::FInt32PropertyParams EditorValue = {
				"EditorValue", Durin::EPropertyFlags::EditorOnly, 1,
				STRUCT_OFFSET_UINT16(FEditorOnlyNestedForTest, EditorValue)};
			static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
				&RuntimeValue, &EditorValue};
			static Durin::DStruct* RawStruct = nullptr;
			auto NoRegister = []() -> Durin::DStruct* {
				if (!RawStruct)
				{
					RawStruct = new Durin::DStruct(
						Durin::EC_StaticConstructor, "FEditorOnlyNestedForTest",
						"FEditorOnlyNestedForTest", sizeof(FEditorOnlyNestedForTest),
						alignof(FEditorOnlyNestedForTest), Durin::EObjectFlags::NoFlags);
					RawStruct->Register(
						Durin::DStruct::StaticClass, "", "FEditorOnlyNestedForTest");
				}
				return RawStruct;
			};
			static const Durin::DurinCodeGen::FStructParams Params = {
				NoRegister, "FEditorOnlyNestedForTest", "FEditorOnlyNestedForTest",
				sizeof(FEditorOnlyNestedForTest), alignof(FEditorOnlyNestedForTest),
				Properties, std::size(Properties), &Ops};
			return Durin::DurinCodeGen::ConstructDStruct(Params);
		}();
		return Struct;
	}

	class DEditorOnlyArchiveOwnerForTest : public Durin::DObject
	{
	public:
		explicit DEditorOnlyArchiveOwnerForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}
		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DEditorOnlyArchiveOwnerForTest(X);
		}
		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DEditorOnlyArchiveOwnerForTest",
					sizeof(DEditorOnlyArchiveOwnerForTest),
					alignof(DEditorOnlyArchiveOwnerForTest), Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None, Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DEditorOnlyArchiveOwnerForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(
					Durin::DClass::StaticClass, "", "DEditorOnlyArchiveOwnerForTest");
			}
			return Class;
		}
		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = [] {
				static const Durin::DurinCodeGen::FInt32PropertyParams RuntimeTop = {
					"RuntimeTop", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DEditorOnlyArchiveOwnerForTest, RuntimeTop)};
				static const Durin::DurinCodeGen::FInt32PropertyParams EditorTop = {
					"EditorTop", Durin::EPropertyFlags::EditorOnly, 1,
					STRUCT_OFFSET_UINT16(DEditorOnlyArchiveOwnerForTest, EditorTop)};
				static const Durin::DurinCodeGen::FStructPropertyParams Nested = {
					"Nested", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DEditorOnlyArchiveOwnerForTest, Nested),
					&GetEditorOnlyNestedStructForTest};
				static const Durin::DurinCodeGen::FStructPropertyParams Fixed = {
					"Fixed", Durin::EPropertyFlags::None, 2,
					STRUCT_OFFSET_UINT16(DEditorOnlyArchiveOwnerForTest, Fixed),
					&GetEditorOnlyNestedStructForTest};
				static const Durin::DurinCodeGen::FStructPropertyParams ArrayInner = {
					"Array_Inner", Durin::EPropertyFlags::None, 1, 0,
					&GetEditorOnlyNestedStructForTest};
				static const Durin::DurinCodeGen::FArrayPropertyParams Array = {
					"Array", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DEditorOnlyArchiveOwnerForTest, Array),
					&ArrayInner, &GVectorPropertyHelper<FEditorOnlyNestedForTest>};
				static const Durin::DurinCodeGen::FStringPropertyParams MapKey = {
					"Map_Key", Durin::EPropertyFlags::None, 1, 0};
				static const Durin::DurinCodeGen::FStructPropertyParams MapValue = {
					"Map_Value", Durin::EPropertyFlags::None, 1, 0,
					&GetEditorOnlyNestedStructForTest};
				static const Durin::DurinCodeGen::FMapPropertyParams Map = {
					"Map", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DEditorOnlyArchiveOwnerForTest, Map),
					&MapKey, &MapValue,
					&GMapPropertyHelper<std::string, FEditorOnlyNestedForTest>};
				static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
					&RuntimeTop, &EditorTop, &Nested, &Fixed, &Array, &Map};
				static const Durin::DurinCodeGen::FClassParams Params = {
					&StaticClassNoRegister, "DEditorOnlyArchiveOwnerForTest",
					"DEditorOnlyArchiveOwnerForTest", Properties, std::size(Properties)};
				return Durin::DurinCodeGen::ConstructDClass(Params);
			}();
			return Class;
		}

		int32 RuntimeTop = 1;
		int32 EditorTop = 2;
		FEditorOnlyNestedForTest Nested{3, 4};
		FEditorOnlyNestedForTest Fixed[2]{{5, 6}, {7, 8}};
		std::vector<FEditorOnlyNestedForTest> Array{{9, 10}};
		TTestMap<std::string, FEditorOnlyNestedForTest> Map{{"Value", {11, 12}}};
	};

	class DGCReferenceSchemaBaseForTest : public Durin::DObject
	{
	public:
		explicit DGCReferenceSchemaBaseForTest(const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X) { new (X.GetObj()) DGCReferenceSchemaBaseForTest(X); }

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, Durin::FName("DGCReferenceSchemaBaseForTest"),
					sizeof(DGCReferenceSchemaBaseForTest), alignof(DGCReferenceSchemaBaseForTest), Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None, Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DGCReferenceSchemaBaseForTest>
				);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DGCReferenceSchemaBaseForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = [] {
				static const Durin::DurinCodeGen::FObjectPropertyParams BaseReference =
					Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
						"BaseReference", Durin::EPropertyFlags::None, 1,
						STRUCT_OFFSET_UINT16(DGCReferenceSchemaBaseForTest, BaseReference),
						&Durin::DObject::StaticClass
					);
				static const Durin::DurinCodeGen::FObjectPropertyParams RawReference =
					Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
						"RawReference", Durin::EPropertyFlags::None, 1,
						STRUCT_OFFSET_UINT16(DGCReferenceSchemaBaseForTest, RawReference),
						&Durin::DObject::StaticClass
					);
				static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {&BaseReference, &RawReference};
				static const Durin::DurinCodeGen::FClassParams Params = {
					&DGCReferenceSchemaBaseForTest::StaticClassNoRegister,
					"DGCReferenceSchemaBaseForTest", "DGCReferenceSchemaBaseForTest", Properties, std::size(Properties)
				};
				return Durin::DurinCodeGen::ConstructDClass(Params);
			}();
			return Class;
		}

		Durin::TObjectPtr<Durin::DObject> BaseReference;
		Durin::DObject* RawReference = nullptr;
	};

	class DGCReferenceSchemaDerivedForTest : public DGCReferenceSchemaBaseForTest
	{
	public:
		explicit DGCReferenceSchemaDerivedForTest(const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DGCReferenceSchemaBaseForTest(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X) { new (X.GetObj()) DGCReferenceSchemaDerivedForTest(X); }

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, Durin::FName("DGCReferenceSchemaDerivedForTest"),
					sizeof(DGCReferenceSchemaDerivedForTest), alignof(DGCReferenceSchemaDerivedForTest), Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None, Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DGCReferenceSchemaDerivedForTest>
				);
				Class->SetSuperStructBase(DGCReferenceSchemaBaseForTest::StaticClassNoRegister());
				Class->Register(Durin::DClass::StaticClass, "", "DGCReferenceSchemaDerivedForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*;

		FGCReferenceNestedForTest Nested;
		std::vector<FGCReferenceLeafForTest> StructArray;
		std::vector<std::vector<Durin::TObjectPtr<Durin::DObject>>> NestedArrays;
		TTestMap<std::string, Durin::TObjectPtr<Durin::DObject>> DirectMap;
		TTestMap<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>> ArrayMap;
		Durin::TObjectPtr<Durin::DObject> DuplicateReference;
		Durin::TSoftObjectPtr<Durin::DObject> SoftReference;
	};

	auto DGCReferenceSchemaDerivedForTest::StaticClass() -> Durin::DClass*
	{
		static Durin::DClass* Class = [] {
			static const Durin::DurinCodeGen::FStructPropertyParams Nested = {
				"Nested", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, Nested),
				&GetGCReferenceNestedStructForTest
			};

			static const Durin::DurinCodeGen::FStructPropertyParams StructArrayInner = {
				"StructArray_Inner", Durin::EPropertyFlags::None, 1, 0,
				&GetGCReferenceLeafStructForTest
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams StructArray = {
				"StructArray", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, StructArray),
				&StructArrayInner, &GVectorPropertyHelper<FGCReferenceLeafForTest>
			};

			static const Durin::DurinCodeGen::FObjectPropertyParams NestedArraysInnerInner =
				Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"NestedArrays_Inner_Inner", Durin::EPropertyFlags::None, 1, 0,
					&Durin::DObject::StaticClass
				);
			static const Durin::DurinCodeGen::FArrayPropertyParams NestedArraysInner = {
				"NestedArrays_Inner", Durin::EPropertyFlags::None, 1, 0,
				&NestedArraysInnerInner,
				&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams NestedArrays = {
				"NestedArrays", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, NestedArrays),
				&NestedArraysInner,
				&GVectorPropertyHelper<std::vector<Durin::TObjectPtr<Durin::DObject>>>
			};

			static const Durin::DurinCodeGen::FStringPropertyParams DirectMapKey = {
				"DirectMap_Key", Durin::EPropertyFlags::None, 1, 0
			};
			static const Durin::DurinCodeGen::FObjectPropertyParams DirectMapValue =
				Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"DirectMap_Value", Durin::EPropertyFlags::None, 1, 0,
					&Durin::DObject::StaticClass
				);
			static const Durin::DurinCodeGen::FMapPropertyParams DirectMap = {
				"DirectMap", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, DirectMap),
				&DirectMapKey, &DirectMapValue,
				&GMapPropertyHelper<std::string, Durin::TObjectPtr<Durin::DObject>>
			};

			static const Durin::DurinCodeGen::FStringPropertyParams ArrayMapKey = {
				"ArrayMap_Key", Durin::EPropertyFlags::None, 1, 0
			};
			static const Durin::DurinCodeGen::FObjectPropertyParams ArrayMapValueInner =
				Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"ArrayMap_Value_Inner", Durin::EPropertyFlags::None, 1, 0,
					&Durin::DObject::StaticClass
				);
			static const Durin::DurinCodeGen::FArrayPropertyParams ArrayMapValue = {
				"ArrayMap_Value", Durin::EPropertyFlags::None, 1, 0,
				&ArrayMapValueInner,
				&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
			};
			static const Durin::DurinCodeGen::FMapPropertyParams ArrayMap = {
				"ArrayMap", Durin::EPropertyFlags::None, 1,
				STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, ArrayMap),
				&ArrayMapKey, &ArrayMapValue,
				&GMapPropertyHelper<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>>
			};

			static const Durin::DurinCodeGen::FObjectPropertyParams DuplicateReference =
				Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
					"DuplicateReference", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, DuplicateReference),
					&Durin::DObject::StaticClass
				);
			static const auto SoftReference =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<Durin::TSoftObjectPtr<Durin::DObject>>(
					"SoftReference", Durin::EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DGCReferenceSchemaDerivedForTest, SoftReference),
					&Durin::DObject::StaticClass
				);

			static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
				&Nested, &StructArray, &NestedArrays, &DirectMap, &ArrayMap, &DuplicateReference,
				&SoftReference
			};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&DGCReferenceSchemaDerivedForTest::StaticClassNoRegister,
				"DGCReferenceSchemaDerivedForTest", "DGCReferenceSchemaDerivedForTest",
				Properties, std::size(Properties)
			};
			return Durin::DurinCodeGen::ConstructDClass(Params);
		}();
		return Class;
	}

	Durin::DClass* Z_Construct_DClass_FReflectedPropertyOwnerForTest_NoRegister()
	{
		static Durin::DClass* Class = new Durin::DClass(
			Durin::EC_StaticConstructor,
			Durin::FName("FReflectedPropertyOwnerForTest"),
			sizeof(FReflectedPropertyOwnerForTest),
			alignof(FReflectedPropertyOwnerForTest),
			Durin::EObjectFlags::NoFlags,
			Durin::EClassFlags::None,
			Durin::EClassCastFlags::DClass,
			nullptr
		);
		return Class;
	}

	Durin::DClass* Z_Construct_DClass_FReflectedEnumPropertyOwnerForTest_NoRegister()
	{
		static Durin::DClass* Class = new Durin::DClass(
			Durin::EC_StaticConstructor,
			Durin::FName("FReflectedEnumPropertyOwnerForTest"),
			sizeof(FReflectedEnumPropertyOwnerForTest),
			alignof(FReflectedEnumPropertyOwnerForTest),
			Durin::EObjectFlags::NoFlags,
			Durin::EClassFlags::None,
			Durin::EClassCastFlags::DClass,
			nullptr
		);
		return Class;
	}

	Durin::DEnum* Z_Construct_DEnum_EReflectedEnumForTest_NoRegister()
	{
		static Durin::DEnum* Enum = nullptr;
		if (!Enum)
		{
			std::vector<Durin::FEnumValue> Values = {
				{Durin::FName("A"), 0, "Alpha"},
				{Durin::FName("B"), 4},
				{Durin::FName("AliasB"), 4, "Second B"},
			};
			Enum = new Durin::DEnum(
				Durin::EC_StaticConstructor,
				Durin::FName("EReflectedEnumForTest"),
				Durin::FName("EReflectedEnumForTest"),
				Durin::FName("EReflectedEnumForTest"),
				"Reflected Enum For Test",
				true,
				Durin::DurinCodeGen::EEnumUnderlyingType::UInt8,
				static_cast<uint16>(sizeof(EReflectedEnumForTest)),
				std::move(Values),
				Durin::EObjectFlags::NoFlags
			);
			Enum->Register(Durin::DEnum::StaticClass, "", "EReflectedEnumForTest");
		}
		return Enum;
	}

	Durin::DStruct* GBuiltInLeafOwnerStructForTest = nullptr;

	auto GetBuiltInLeafOwnerStructNoRegister() -> Durin::DStruct*
	{
		if (!GBuiltInLeafOwnerStructForTest)
		{
			GBuiltInLeafOwnerStructForTest = new Durin::DStruct(
				Durin::EC_StaticConstructor,
				Durin::FName("FBuiltInLeafOwnerForTest"),
				Durin::FName("FBuiltInLeafOwnerForTest"),
				sizeof(FBuiltInLeafOwnerForTest),
				alignof(FBuiltInLeafOwnerForTest),
				Durin::EObjectFlags::Transient
			);
			GBuiltInLeafOwnerStructForTest->Register(
				Durin::DStruct::StaticClass, "", "FBuiltInLeafOwnerForTest"
			);
		}
		return GBuiltInLeafOwnerStructForTest;
	}

	auto GetBuiltInLeafOwnerStructForTest() -> Durin::DStruct*
	{
		using namespace Durin::DurinCodeGen;
		static const FBoolPropertyParams BoolValue = {"BoolValue", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, BoolValue)};
		static const FInt8PropertyParams Int8Value = {"Int8Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, Int8Value)};
		static const FInt16PropertyParams Int16Value = {"Int16Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, Int16Value)};
		static const FInt32PropertyParams Int32Value = {"Int32Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, Int32Value)};
		static const FInt64PropertyParams Int64Value = {"Int64Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, Int64Value)};
		static const FUInt8PropertyParams UInt8Value = {"UInt8Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, UInt8Value)};
		static const FUInt16PropertyParams UInt16Value = {"UInt16Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, UInt16Value)};
		static const FUInt32PropertyParams UInt32Value = {"UInt32Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, UInt32Value)};
		static const FUInt64PropertyParams UInt64Value = {"UInt64Value", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, UInt64Value)};
		static const FFloatPropertyParams FloatValue = {"FloatValue", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, FloatValue)};
		static const FDoublePropertyParams DoubleValue = {"DoubleValue", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, DoubleValue)};
		static const FStringPropertyParams StringValue = {"StringValue", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, StringValue)};
		static const FNamePropertyParams NameValue = {"NameValue", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, NameValue)};
		static const FGuidPropertyParams GuidValue = {"GuidValue", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, GuidValue)};
		static const FInt32PropertyParams FixedValues = {"FixedValues", Durin::EPropertyFlags::None, 2, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, FixedValues)};
		static const FEnumPropertyParams EnumInt8 = {"EnumInt8", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumInt8), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::Int8, int8>};
		static const FEnumPropertyParams EnumInt16 = {"EnumInt16", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumInt16), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::Int16, int16>};
		static const FEnumPropertyParams EnumInt32 = {"EnumInt32", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumInt32), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::Int32, int32>};
		static const FEnumPropertyParams EnumInt64 = {"EnumInt64", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumInt64), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::Int64, int64>};
		static const FEnumPropertyParams EnumUInt8 = {"EnumUInt8", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumUInt8), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::UInt8, uint8>};
		static const FEnumPropertyParams EnumUInt16 = {"EnumUInt16", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumUInt16), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::UInt16, uint16>};
		static const FEnumPropertyParams EnumUInt32 = {"EnumUInt32", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumUInt32), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::UInt32, uint32>};
		static const FEnumPropertyParams EnumUInt64 = {"EnumUInt64", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, EnumUInt64), &GetBuiltInLeafEnumForTest<EEnumUnderlyingType::UInt64, uint64>};
		static const FObjectPropertyParams RawObject = FObjectPropertyParams::Raw<Durin::DObject>("RawObject", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, RawObject), &Durin::DObject::StaticClass);
		static const FObjectPropertyParams ObjectPtr = FObjectPropertyParams::ObjectPtr<Durin::DObject>("ObjectPtr", Durin::EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(FBuiltInLeafOwnerForTest, ObjectPtr), &Durin::DObject::StaticClass);
		static const FPropertyParamsBase* Properties[] = {
			&BoolValue, &Int8Value, &Int16Value, &Int32Value, &Int64Value,
			&UInt8Value, &UInt16Value, &UInt32Value, &UInt64Value,
			&FloatValue, &DoubleValue, &StringValue, &NameValue, &GuidValue,
			&FixedValues, &EnumInt8, &EnumInt16, &EnumInt32, &EnumInt64,
			&EnumUInt8, &EnumUInt16, &EnumUInt32, &EnumUInt64, &RawObject, &ObjectPtr
		};
		static const FStructParams Params = {
			&GetBuiltInLeafOwnerStructNoRegister,
			"Tests::FBuiltInLeafOwnerForTest",
			"FBuiltInLeafOwnerForTest",
			sizeof(FBuiltInLeafOwnerForTest),
			alignof(FBuiltInLeafOwnerForTest),
			Properties,
			std::size(Properties)
		};
		return ConstructDStruct(Params);
	}

	void EnsureDObjectInitialized()
	{
		Durin::Testing::InitializeDObjectSystemForTests();
	}

	struct FTypedStructPropertyOwnerForTest
	{
		Durin::FVector3 Direct;
		Durin::FVector3 Accessed;
	};

	struct FSoftObjectPropertyOwnerForTest
	{
		Durin::TSoftObjectPtr<Durin::DObject> Direct;
		Durin::TSoftObjectPtr<Durin::DObject> Fixed[2];
		Durin::TSoftObjectPtr<Durin::DObject> Accessed;
		std::vector<Durin::TSoftObjectPtr<Durin::DObject>> Array;
		std::unordered_map<std::string, Durin::TSoftObjectPtr<Durin::DObject>> Map;
	};

	struct FWeakObjectPropertyOwnerForTest
	{
		Durin::TWeakObjectPtr<Durin::DObject> Direct;
		Durin::TWeakObjectPtr<Durin::DObject> Fixed[2];
		std::vector<Durin::TWeakObjectPtr<Durin::DObject>> Array;
		std::unordered_map<std::string, Durin::TWeakObjectPtr<Durin::DObject>> Map;
	};

	struct FUnavailableStructPropertyOwnerForTest
	{
		StructOpsTest::FDeletedDefault DeletedDefault;
		StructOpsTest::FMoveOnly MoveOnly;
	};

	auto GetAccessedVector(void* Container, uint32 ArrayIndex) -> void*
	{
		return &static_cast<FTypedStructPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetAccessedVector(const void* Container, uint32 ArrayIndex) -> const void*
	{
		return &static_cast<const FTypedStructPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetAccessedSoftObject(void* Container, uint32 ArrayIndex) -> void*
	{
		return &static_cast<FSoftObjectPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetAccessedSoftObject(const void* Container, uint32 ArrayIndex) -> const void*
	{
		return &static_cast<const FSoftObjectPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetTypedStructPropertyOwnerNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = nullptr;
		if (!Struct)
		{
			Struct = new Durin::DStruct(
				Durin::EC_StaticConstructor,
				Durin::FName("FTypedStructPropertyOwnerForTest"),
				Durin::FName("FTypedStructPropertyOwnerForTest"),
				sizeof(FTypedStructPropertyOwnerForTest),
				alignof(FTypedStructPropertyOwnerForTest),
				Durin::EObjectFlags::Transient
			);
			Struct->Register(Durin::DStruct::StaticClass, "", "FTypedStructPropertyOwnerForTest");
		}
		return Struct;
	}

	auto GetTypedStructPropertyOwner() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FMetaDataPair DirectMetaData[] = {{"Category", "TypedStruct"}};
		static const Durin::DurinCodeGen::FStructPropertyParams Direct = {
			"Direct",
			Durin::EPropertyFlags::Edit,
			1,
			STRUCT_OFFSET_UINT16(FTypedStructPropertyOwnerForTest, Direct),
			&Durin::Z_Construct_DStruct_FVector3,
			DirectMetaData,
			std::size(DirectMetaData)
		};
		static constexpr Durin::DurinCodeGen::FStructPropertyParams Accessed =
			Durin::DurinCodeGen::FStructPropertyParams::WithAccessors(
				"Accessed",
				Durin::EPropertyFlags::ReadOnly,
				1,
				&Durin::Z_Construct_DStruct_FVector3,
				static_cast<Durin::DurinCodeGen::FStructPropertyParams::FMutableValueAccessor>(&GetAccessedVector),
				static_cast<Durin::DurinCodeGen::FStructPropertyParams::FConstValueAccessor>(&GetAccessedVector)
			);
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Direct, &Accessed};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetTypedStructPropertyOwnerNoRegister,
			"Tests::FTypedStructPropertyOwnerForTest",
			"FTypedStructPropertyOwnerForTest",
			sizeof(FTypedStructPropertyOwnerForTest),
			alignof(FTypedStructPropertyOwnerForTest),
			Properties,
			std::size(Properties)
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetSoftObjectPropertyOwnerNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = nullptr;
		if (!Struct)
		{
			Struct = new Durin::DStruct(
				Durin::EC_StaticConstructor,
				Durin::FName("FSoftObjectPropertyOwnerForTest"),
				Durin::FName("FSoftObjectPropertyOwnerForTest"),
				sizeof(FSoftObjectPropertyOwnerForTest),
				alignof(FSoftObjectPropertyOwnerForTest),
				Durin::EObjectFlags::Transient
			);
			Struct->Register(Durin::DStruct::StaticClass, "", "FSoftObjectPropertyOwnerForTest");
		}
		return Struct;
	}

	auto GetSoftObjectPropertyOwner() -> Durin::DStruct*
	{
		using FSoftPtr = Durin::TSoftObjectPtr<Durin::DObject>;
		static const Durin::DurinCodeGen::FMetaDataPair DirectMetaData[] = {{"Category", "SoftObject"}};
		static const auto Direct = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Direct", Durin::EPropertyFlags::Edit, 1,
			STRUCT_OFFSET_UINT16(FSoftObjectPropertyOwnerForTest, Direct),
			&Durin::DObject::StaticClass, DirectMetaData, std::size(DirectMetaData)
		);
		static const auto Fixed = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Fixed", Durin::EPropertyFlags::Edit, 2,
			STRUCT_OFFSET_UINT16(FSoftObjectPropertyOwnerForTest, Fixed),
			&Durin::DObject::StaticClass
		);
		static const auto Accessed = Durin::DurinCodeGen::FSoftObjectPropertyParams::WithAccessors<FSoftPtr>(
			"Accessed", Durin::EPropertyFlags::ReadOnly, 1, &Durin::DObject::StaticClass,
			static_cast<Durin::DurinCodeGen::FSoftObjectPropertyParams::FMutableValueAccessor>(&GetAccessedSoftObject),
			static_cast<Durin::DurinCodeGen::FSoftObjectPropertyParams::FConstValueAccessor>(&GetAccessedSoftObject)
		);
		static const auto ArrayInner = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Array_Inner", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass
		);
		static const Durin::DurinCodeGen::FArrayPropertyParams Array = {
			"Array", Durin::EPropertyFlags::Edit, 1,
			STRUCT_OFFSET_UINT16(FSoftObjectPropertyOwnerForTest, Array),
			&ArrayInner, &GVectorPropertyHelper<FSoftPtr>
		};
		static const Durin::DurinCodeGen::FStringPropertyParams MapKey = {
			"Map_Key", Durin::EPropertyFlags::None, 1, 0
		};
		static const auto MapValue = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Map_Value", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass
		);
		static const Durin::DurinCodeGen::FMapPropertyParams Map = {
			"Map", Durin::EPropertyFlags::Edit, 1,
			STRUCT_OFFSET_UINT16(FSoftObjectPropertyOwnerForTest, Map),
			&MapKey, &MapValue, &GMapPropertyHelper<std::string, FSoftPtr>
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
			&Direct, &Fixed, &Accessed, &Array, &Map
		};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetSoftObjectPropertyOwnerNoRegister,
			"Tests::FSoftObjectPropertyOwnerForTest",
			"FSoftObjectPropertyOwnerForTest",
			sizeof(FSoftObjectPropertyOwnerForTest),
			alignof(FSoftObjectPropertyOwnerForTest),
			Properties,
			std::size(Properties)
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetWeakObjectPropertyOwnerNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = nullptr;
		if (!Struct)
		{
			Struct = new Durin::DStruct(Durin::EC_StaticConstructor,
				Durin::FName("FWeakObjectPropertyOwnerForTest"),
				Durin::FName("FWeakObjectPropertyOwnerForTest"),
				sizeof(FWeakObjectPropertyOwnerForTest), alignof(FWeakObjectPropertyOwnerForTest),
				Durin::EObjectFlags::Transient);
			Struct->Register(Durin::DStruct::StaticClass, "", "FWeakObjectPropertyOwnerForTest");
		}
		return Struct;
	}

	auto GetWeakObjectPropertyOwner() -> Durin::DStruct*
	{
		using FWeakPtr = Durin::TWeakObjectPtr<Durin::DObject>;
		constexpr auto WeakFlags = Durin::EPropertyFlags::Transient;
		static const auto Direct = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<FWeakPtr>(
			"Direct", WeakFlags, 1, STRUCT_OFFSET_UINT16(FWeakObjectPropertyOwnerForTest, Direct), &Durin::DObject::StaticClass);
		static const auto Fixed = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<FWeakPtr>(
			"Fixed", WeakFlags, 2, STRUCT_OFFSET_UINT16(FWeakObjectPropertyOwnerForTest, Fixed), &Durin::DObject::StaticClass);
		static const auto ArrayInner = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<FWeakPtr>(
			"Array_Inner", WeakFlags, 1, 0, &Durin::DObject::StaticClass);
		static const Durin::DurinCodeGen::FArrayPropertyParams Array = {
			"Array", WeakFlags, 1, STRUCT_OFFSET_UINT16(FWeakObjectPropertyOwnerForTest, Array),
			&ArrayInner, &GVectorPropertyHelper<FWeakPtr>};
		static const Durin::DurinCodeGen::FStringPropertyParams MapKey = {
			"Map_Key", Durin::EPropertyFlags::None, 1, 0};
		static const auto MapValue = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<FWeakPtr>(
			"Map_Value", WeakFlags, 1, 0, &Durin::DObject::StaticClass);
		static const Durin::DurinCodeGen::FMapPropertyParams Map = {
			"Map", WeakFlags, 1, STRUCT_OFFSET_UINT16(FWeakObjectPropertyOwnerForTest, Map),
			&MapKey, &MapValue, &GMapPropertyHelper<std::string, FWeakPtr>};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Direct, &Fixed, &Array, &Map};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetWeakObjectPropertyOwnerNoRegister, "Tests::FWeakObjectPropertyOwnerForTest",
			"FWeakObjectPropertyOwnerForTest", sizeof(FWeakObjectPropertyOwnerForTest),
			alignof(FWeakObjectPropertyOwnerForTest), Properties, std::size(Properties)};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetDeletedDefaultStructNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FDeletedDefaultForTypedPropertyTest"),
			Durin::FName("FDeletedDefaultForTypedPropertyTest"),
			sizeof(StructOpsTest::FDeletedDefault),
			alignof(StructOpsTest::FDeletedDefault),
			Durin::EObjectFlags::Transient
		);
		return Struct;
	}

	auto GetDeletedDefaultStruct() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetDeletedDefaultStructNoRegister,
			"Tests::FDeletedDefaultForTypedPropertyTest",
			"FDeletedDefaultForTypedPropertyTest",
			sizeof(StructOpsTest::FDeletedDefault),
			alignof(StructOpsTest::FDeletedDefault),
			nullptr,
			0,
			&Durin::GetDStructOps<StructOpsTest::FDeletedDefault>()
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetMoveOnlyStructNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FMoveOnlyForTypedPropertyTest"),
			Durin::FName("FMoveOnlyForTypedPropertyTest"),
			sizeof(StructOpsTest::FMoveOnly),
			alignof(StructOpsTest::FMoveOnly),
			Durin::EObjectFlags::Transient
		);
		return Struct;
	}

	auto GetMoveOnlyStruct() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetMoveOnlyStructNoRegister,
			"Tests::FMoveOnlyForTypedPropertyTest",
			"FMoveOnlyForTypedPropertyTest",
			sizeof(StructOpsTest::FMoveOnly),
			alignof(StructOpsTest::FMoveOnly),
			nullptr,
			0,
			&Durin::GetDStructOps<StructOpsTest::FMoveOnly>()
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetUnavailableStructPropertyOwnerNoRegister() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = nullptr;
		if (!Struct)
		{
			Struct = new Durin::DStruct(
				Durin::EC_StaticConstructor,
				Durin::FName("FUnavailableStructPropertyOwnerForTest"),
				Durin::FName("FUnavailableStructPropertyOwnerForTest"),
				sizeof(FUnavailableStructPropertyOwnerForTest),
				alignof(FUnavailableStructPropertyOwnerForTest),
				Durin::EObjectFlags::Transient
			);
			Struct->Register(Durin::DStruct::StaticClass, "", "FUnavailableStructPropertyOwnerForTest");
		}
		return Struct;
	}

	auto GetUnavailableStructPropertyOwner() -> Durin::DStruct*
	{
		static const Durin::DurinCodeGen::FStructPropertyParams DeletedDefault = {
			"DeletedDefault",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FUnavailableStructPropertyOwnerForTest, DeletedDefault),
			&GetDeletedDefaultStruct
		};
		static const Durin::DurinCodeGen::FStructPropertyParams MoveOnly = {
			"MoveOnly",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FUnavailableStructPropertyOwnerForTest, MoveOnly),
			&GetMoveOnlyStruct
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&DeletedDefault, &MoveOnly};
		static const Durin::DurinCodeGen::FStructParams Params = {
			&GetUnavailableStructPropertyOwnerNoRegister,
			"Tests::FUnavailableStructPropertyOwnerForTest",
			"FUnavailableStructPropertyOwnerForTest",
			sizeof(FUnavailableStructPropertyOwnerForTest),
			alignof(FUnavailableStructPropertyOwnerForTest),
			Properties,
			std::size(Properties)
		};
		return Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	auto GetNullStructDescriptor() -> Durin::DStruct*
	{
		return nullptr;
	}

	auto GetZeroSizeStructDescriptor() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FZeroSizeStructDescriptorForTest"),
			Durin::FName("FZeroSizeStructDescriptorForTest"),
			0,
			1,
			Durin::EObjectFlags::Transient
		);
		return Struct;
	}

	auto GetInvalidAlignmentStructDescriptor() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FInvalidAlignmentStructDescriptorForTest"),
			Durin::FName("FInvalidAlignmentStructDescriptorForTest"),
			4,
			3,
			Durin::EObjectFlags::Transient
		);
		return Struct;
	}

	Durin::DStruct* GInvalidStructPropertyOwner = nullptr;

	auto GetInvalidStructPropertyOwnerNoRegister() -> Durin::DStruct*
	{
		return GInvalidStructPropertyOwner;
	}

	auto ConstructInvalidStructProperty(const Durin::DurinCodeGen::FPropertyParamsBase& Property) -> void
	{
		GInvalidStructPropertyOwner = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FInvalidStructPropertyOwnerForTest"),
			Durin::FName("FInvalidStructPropertyOwnerForTest"),
			16,
			8,
			Durin::EObjectFlags::Transient
		);
		GInvalidStructPropertyOwner->Register(
			Durin::DStruct::StaticClass,
			"",
			"FInvalidStructPropertyOwnerForTest"
		);
		const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&Property};
		const Durin::DurinCodeGen::FStructParams Params = {
			&GetInvalidStructPropertyOwnerNoRegister,
			"Tests::FInvalidStructPropertyOwnerForTest",
			"FInvalidStructPropertyOwnerForTest",
			16,
			8,
			Properties,
			std::size(Properties)
		};
		(void)Durin::DurinCodeGen::ConstructDStruct(Params);
	}

	void EnsurePackageTestMount()
	{
		static const bool bMounted = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::Testing::RegisterMountPointForTests(
				"/CoreTests/",
				(Durin::Testing::GetTestWorkDirectory() / "CoreTests").generic_string() + "/"
			);
			return true;
		}();
		(void)bMounted;
	}

	auto ObjectArrayContains(Durin::DObject* Object) -> bool
	{
		std::vector<Durin::DObject*> Objects = Durin::GDObjectArray.GetAll(Durin::EObjectQueryScope::IncludeTemplates);
		return std::find(Objects.begin(), Objects.end(), Object) != Objects.end();
	}


	TEST(FCoreDObjectReflectionTests, IntrinsicTypesUseTypeAndStructBaseHierarchy)
	{
		EnsureDObjectInitialized();
		EXPECT_TRUE(Durin::IsFNameInitialized());
		EXPECT_TRUE(Durin::FName("None").IsNone());
		EXPECT_EQ(Durin::FindClassByQualifiedName("Durin::DObject"), Durin::DObject::StaticClass());
		EXPECT_EQ(Durin::FindClassByQualifiedName(Durin::FName("Durin::DClass")), Durin::DClass::StaticClass());
		EXPECT_EQ(Durin::FindClassByQualifiedName("Durin::DPackage"), Durin::DPackage::StaticClass());
		EXPECT_EQ(Durin::FindClassByQualifiedName("Durin::MissingType"), nullptr);

		EXPECT_EQ(Durin::DObject::StaticClass()->GetSuperClass(), nullptr);
		EXPECT_EQ(Durin::DType::StaticClass()->GetSuperClass(), Durin::DObject::StaticClass());
		EXPECT_EQ(Durin::DStructBase::StaticClass()->GetSuperClass(), Durin::DType::StaticClass());
		EXPECT_EQ(Durin::DClass::StaticClass()->GetSuperClass(), Durin::DStructBase::StaticClass());
	}

	TEST(FCoreDObjectReflectionTests, PropertyLegacyNamesAreOwnerScopedSerializedAliases)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DOverrideContainerOwnerForTest::StaticClass();
		Durin::FProperty* Current = Class->FindPropertyByName("Values", false);
		ASSERT_NE(Current, nullptr);
		EXPECT_EQ(Class->FindPropertyByName("LegacyValues", false), nullptr);
		EXPECT_EQ(Class->FindPropertyBySerializedName("Values", false), Current);
		EXPECT_EQ(Class->FindPropertyBySerializedName("LegacyValues", false), Current);
		EXPECT_EQ(Class->FindPropertyBySerializedName("OlderValues", false), Current);

		const auto Aliases = Durin::CaptureSerializedPropertyAliases();
		const auto Alias = std::ranges::find_if(Aliases, [](const auto& Candidate) {
			return Candidate.DeclaringType == "DOverrideContainerOwnerForTest"
				&& Candidate.StoredName == "LegacyValues";
		});
		ASSERT_NE(Alias, Aliases.end());
		EXPECT_EQ(Alias->CurrentName, "Values");
	}

	TEST(FCoreDObjectReflectionTests, ClassDefaultObjectHasStableIdentityAndSurvivesGarbageCollection)
	{
		EnsureDObjectInitialized();
		EXPECT_EQ(Durin::DObject::StaticClass()->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Ineligible);
		EXPECT_EQ(Durin::DObject::StaticClass()->GetDefaultObjectReason(), Durin::EClassDefaultObjectReason::Intrinsic);
		EXPECT_EQ(Durin::DPackage::StaticClass()->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Ineligible);
		EXPECT_EQ(
			Durin::DPackage::StaticClass()->GetDefaultObjectReason(),
			Durin::EClassDefaultObjectReason::NoClassDefaultObject);

		Durin::DClass* Class = DLifecycleTestObject::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		ASSERT_EQ(Class->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Ready);
		auto* DefaultObject = static_cast<const DLifecycleTestObject*>(Class->GetDefaultObject());
		ASSERT_NE(DefaultObject, nullptr);
		EXPECT_EQ(DefaultObject->GetOuter(), Class);
		EXPECT_EQ(DefaultObject->GetName(), "Default__DLifecycleTestObject");
		EXPECT_TRUE(DefaultObject->IsClassDefaultObject());
		EXPECT_TRUE(DefaultObject->IsTemplateObject());
		EXPECT_TRUE(DefaultObject->HasAnyObjectFlags(Durin::EObjectFlags::Transient));
		const auto LiveObjects = Durin::GDObjectArray.GetAll(Durin::EObjectQueryScope::LiveOnly);
		const auto DiagnosticObjects = Durin::GDObjectArray.GetAll(Durin::EObjectQueryScope::IncludeTemplates);
		EXPECT_EQ(std::ranges::find(LiveObjects, DefaultObject), LiveObjects.end());
		EXPECT_NE(std::ranges::find(DiagnosticObjects, DefaultObject), DiagnosticObjects.end());

		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		EXPECT_EQ(Class->GetDefaultObject(), DefaultObject);
		const Durin::DObject* WorkerDefault = nullptr;
		std::thread Worker([&] { WorkerDefault = Class->GetDefaultObject(); });
		Worker.join();
		EXPECT_EQ(WorkerDefault, DefaultObject);

		auto* MutableDefault = const_cast<DLifecycleTestObject*>(DefaultObject);
		MutableDefault->Rename("RejectedRename");
		MutableDefault->SetOuterPrivate(nullptr);
		Durin::AddToRoot(MutableDefault);
		Durin::MarkAsGarbage(MutableDefault);
		EXPECT_EQ(DefaultObject->GetName(), "Default__DLifecycleTestObject");
		EXPECT_EQ(DefaultObject->GetOuter(), Class);
		EXPECT_FALSE(DefaultObject->IsGarbage());
		EXPECT_FALSE(DefaultObject->HasAnyInternalFlags(Durin::EObjectInternalFlags::RootSet));
		std::vector<std::byte> SerializedDefault;
		EXPECT_FALSE(Durin::SaveObjectGraphToMemory(MutableDefault, SerializedDefault));
		EXPECT_EQ(Durin::DuplicateObject(
			MutableDefault, nullptr, Durin::FName("RejectedDefaultDuplicate")), nullptr);
		Durin::CollectGarbage();
		EXPECT_TRUE(Durin::GDObjectArray.Contains(DefaultObject));
		EXPECT_EQ(Class->GetDefaultObject(), DefaultObject);

		DLifecycleTestObject::ResetLifecycleCounts();
		Durin::Private::ReleaseClassDefaultObjectForTests(Class);
		EXPECT_EQ(Class->GetDefaultObject(), nullptr);
		Durin::CollectGarbage();
		EXPECT_FALSE(Durin::GDObjectArray.Contains(DefaultObject));
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 1u);
	}

	TEST(FCoreDObjectReflectionTests, RecursiveDefaultObjectConstructionRollsBackTheWholeObject)
	{
		EnsureDObjectInitialized();
		Durin::CollectGarbage();
		Durin::DClass* Class = DRecursiveDefaultObjectForTest::StaticClass();
		(void)DLifecycleTestObject::StaticClass();
		const uint64 ObjectCountBeforeCreation = Durin::GDObjectArray.GetNum();
		DRecursiveDefaultObjectForTest::DestructorCount = 0;
		DLifecycleTestObject::ResetLifecycleCounts();

		const std::array Batch{Class};
		EXPECT_FALSE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		EXPECT_EQ(Class->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Failed);
		EXPECT_EQ(Class->GetDefaultObjectReason(), Durin::EClassDefaultObjectReason::RecursiveConstruction);
		EXPECT_EQ(Class->GetDefaultObject(), nullptr);
		EXPECT_EQ(Durin::GDObjectArray.GetNum(), ObjectCountBeforeCreation);
		EXPECT_EQ(DRecursiveDefaultObjectForTest::DestructorCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 1u);
	}

	TEST(FCoreDObjectReflectionTests, DefaultObjectGraphPairsStableSubobjectsAndResolvesClassBaselines)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DDefaultGraphOwnerForTest::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		const auto* DefaultObject = static_cast<const DDefaultGraphOwnerForTest*>(Class->GetDefaultObject());
		auto* Instance = Durin::NewObject<DDefaultGraphOwnerForTest>(nullptr, Durin::FName("DefaultGraphInstance"));
		ASSERT_NE(DefaultObject, nullptr);
		ASSERT_NE(DefaultObject->Child, nullptr);
		ASSERT_NE(Instance->Child, nullptr);
		EXPECT_TRUE(DefaultObject->Child->HasAnyObjectFlags(Durin::EObjectFlags::DefaultSubobject));
		EXPECT_FALSE(Instance->Child->IsTemplateObject());

		Durin::FDefaultObjectGraphMap Graph;
		Durin::FDefaultObjectGraphDiagnostic GraphDiagnostic;
		ASSERT_TRUE(Graph.Build(DefaultObject, Instance, &GraphDiagnostic));
		EXPECT_EQ(Graph.Num(), 2u);
		EXPECT_EQ(Graph.FindInstance(DefaultObject), Instance);
		EXPECT_EQ(Graph.FindInstance(DefaultObject->Child), Instance->Child);
		EXPECT_TRUE(Graph.AreReferencesEquivalent(DefaultObject->Child, Instance->Child));

		Durin::FProperty* ChildProperty = Class->FindPropertyByName(Durin::FName("Child"), false);
		ASSERT_NE(ChildProperty, nullptr);
		EXPECT_EQ(
			Durin::CompareObjectPropertyToClassDefault(ChildProperty, Instance, 0, Graph),
			Durin::EPropertyIdentityResult::Identical
		);
		auto* ClassSpecificProperty = static_cast<Durin::FStructProperty*>(
			Class->FindPropertyByName(Durin::FName("ClassSpecific"), false));
		ASSERT_NE(ClassSpecificProperty, nullptr);
		EXPECT_EQ(
			Durin::CompareObjectPropertyToClassDefault(ClassSpecificProperty, Instance, 0, Graph),
			Durin::EPropertyIdentityResult::Identical
		);
		EXPECT_EQ(
			Durin::CompareStructPropertyToTypeDefault(ClassSpecificProperty, Instance, 0),
			Durin::EPropertyIdentityResult::Different
		);
		Durin::DObject* OriginalChild = Instance->Child;
		Instance->Child = Durin::NewObject<DLifecycleTestObject>(Instance, Durin::FName("ExternalChild"));
		EXPECT_EQ(
			Durin::CompareObjectPropertyToClassDefault(ChildProperty, Instance, 0, Graph),
			Durin::EPropertyIdentityResult::Different
		);
		Instance->Child = OriginalChild;
		auto* ExtraChild = Durin::NewObject<DLifecycleTestObject>(
			Instance, Durin::FName("AllowedExtraChild"));
		EXPECT_TRUE(Graph.Build(DefaultObject, Instance, &GraphDiagnostic));

		auto* DuplicateChild = Durin::NewObject<DLifecycleTestObject>(
			Instance, Durin::FName("StableChild"));
		EXPECT_FALSE(Graph.Build(DefaultObject, Instance, &GraphDiagnostic));
		EXPECT_EQ(GraphDiagnostic.Reason, Durin::EDefaultObjectGraphFailureReason::DuplicateInstanceIdentity);
		Durin::MarkAsGarbage(DuplicateChild);

		OriginalChild->SetOuterPrivate(nullptr);
		EXPECT_FALSE(Graph.Build(DefaultObject, Instance, &GraphDiagnostic));
		EXPECT_EQ(GraphDiagnostic.Reason, Durin::EDefaultObjectGraphFailureReason::MissingInstanceNode);
		auto* WrongClassChild = Durin::NewObject<Durin::DObject>(
			Instance, Durin::FName("StableChild"));
		EXPECT_FALSE(Graph.Build(DefaultObject, Instance, &GraphDiagnostic));
		EXPECT_EQ(GraphDiagnostic.Reason, Durin::EDefaultObjectGraphFailureReason::ClassMismatch);
		Durin::MarkAsGarbage(WrongClassChild);
		OriginalChild->SetOuterPrivate(Instance);
		EXPECT_FALSE(Graph.Build(Instance, DefaultObject, &GraphDiagnostic));
		EXPECT_EQ(GraphDiagnostic.Reason, Durin::EDefaultObjectGraphFailureReason::InvalidTemplateRoot);

		Durin::MarkAsGarbage(ExtraChild);
		Durin::MarkObjectHierarchyAsGarbage(Instance);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, DefaultDeltaPlanCanonicalizesReflectedAndNativeClassDifferences)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DDefaultGraphOwnerForTest::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		auto* Instance = Durin::NewObject<DDefaultGraphOwnerForTest>(
			nullptr, Durin::FName("DefaultDeltaInstance"));
		Instance->bReverseNativeOrder = true;

		Durin::FDefaultDeltaPlan DefaultPlan;
		Durin::FDefaultDeltaDiagnostic Diagnostic;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, DefaultPlan, &Diagnostic))
			<< "reason=" << static_cast<int>(Diagnostic.Reason)
			<< " path=" << Diagnostic.LogicalPath;
		ASSERT_EQ(DefaultPlan.Objects.size(), 2u);
		EXPECT_EQ(DefaultPlan.EmittedFieldCount, 0u);
		EXPECT_GT(DefaultPlan.OmittedFieldCount, 0u);

		Instance->Blob.assign(Durin::DefaultDeltaMaxFields + 1, std::byte{0x5a});
		Durin::FDefaultDeltaPlan BlobPlan;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, BlobPlan, &Diagnostic))
			<< "reason=" << static_cast<int>(Diagnostic.Reason)
			<< " path=" << Diagnostic.LogicalPath;
		const auto BlobRoot = std::ranges::find_if(BlobPlan.Objects,
			[&](const Durin::FDefaultDeltaObjectPlan& Object) { return Object.Object == Instance; });
		ASSERT_NE(BlobRoot, BlobPlan.Objects.end());
		const auto BlobField = std::ranges::find_if(BlobRoot->Fields,
			[](const Durin::FDefaultDeltaFieldPlan& Field) {
				return Field.Descriptor.Name == Durin::FName("Blob");
			});
		ASSERT_NE(BlobField, BlobRoot->Fields.end());
		ASSERT_NE(BlobField->Value, nullptr);
		EXPECT_EQ(BlobField->Value->LogicalType.Kind,
			Durin::FArchiveLogicalTypeDescriptor::EKind::Bytes);
		EXPECT_EQ(BlobField->Value->ByteValue.size(), Instance->Blob.size() + sizeof(uint64));
		Durin::FProperty* ReflectedBlob = Instance->GetClass()->FindPropertyByName(
			Durin::FName("Blob"), true);
		ASSERT_NE(ReflectedBlob, nullptr);
		Durin::FPropertyValueSnapshot BlobSnapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(ReflectedBlob, Instance, 0, BlobSnapshot));
		Instance->Blob.clear();
		ASSERT_TRUE(Durin::RestorePropertyValue(ReflectedBlob, Instance, 0, BlobSnapshot));
		ASSERT_EQ(Instance->Blob.size(), Durin::DefaultDeltaMaxFields + 1);
		EXPECT_EQ(Instance->Blob.front(), std::byte{0x5a});
		Instance->Blob.clear();

		Instance->ClassSpecific = Durin::FVector3(0.0);
		Instance->Fixed[1] = 17;
		Instance->NativeSecond = 11;
		const Durin::FVector3 BeforeStruct = Instance->ClassSpecific;
		const int32 BeforeFixed = Instance->Fixed[1];
		Durin::FDefaultDeltaPlan ChangedPlan;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, ChangedPlan, &Diagnostic))
			<< "reason=" << static_cast<int>(Diagnostic.Reason)
			<< " path=" << Diagnostic.LogicalPath;
		EXPECT_EQ(Instance->ClassSpecific, BeforeStruct);
		EXPECT_EQ(Instance->Fixed[1], BeforeFixed);

		const auto ChangedRootIt = std::ranges::find_if(ChangedPlan.Objects,
			[&](const Durin::FDefaultDeltaObjectPlan& Object) { return Object.Object == Instance; });
		ASSERT_NE(ChangedRootIt, ChangedPlan.Objects.end());
		const auto& Root = *ChangedRootIt;
		auto FindField = [&](std::string_view Name) -> const Durin::FDefaultDeltaFieldPlan* {
			auto It = std::ranges::find_if(Root.Fields, [&](const Durin::FDefaultDeltaFieldPlan& Field) {
				return Field.Descriptor.Name.ToString() == Name;
			});
			return It == Root.Fields.end() ? nullptr : &*It;
		};
		const auto* Child = FindField("Child");
		const auto* ClassSpecific = FindField("ClassSpecific");
		const auto* Fixed = FindField("Fixed");
		const auto* ExactFloat = FindField("ExactFloat");
		const auto* NativeFirst = FindField("NativeFirst");
		const auto* NativeSecond = FindField("NativeSecond");
		ASSERT_NE(Child, nullptr);
		ASSERT_NE(ClassSpecific, nullptr);
		ASSERT_NE(Fixed, nullptr);
		ASSERT_NE(ExactFloat, nullptr);
		ASSERT_NE(NativeFirst, nullptr);
		ASSERT_NE(NativeSecond, nullptr);
		EXPECT_EQ(Child->Disposition, Durin::EDefaultDeltaDisposition::Omitted);
		EXPECT_EQ(NativeFirst->Disposition, Durin::EDefaultDeltaDisposition::Omitted);
		EXPECT_EQ(ExactFloat->Disposition, Durin::EDefaultDeltaDisposition::Omitted);
		EXPECT_EQ(NativeSecond->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		ASSERT_EQ(ClassSpecific->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		ASSERT_NE(ClassSpecific->Value, nullptr);
		EXPECT_EQ(ClassSpecific->Baseline, Durin::EDefaultDeltaBaselineKind::ClassDefault);
		EXPECT_EQ(ClassSpecific->Value->Fields.size(), 3u);
		EXPECT_TRUE(std::ranges::all_of(ClassSpecific->Value->Fields,
			[](const Durin::FDefaultDeltaFieldPlan& Field) {
				return Field.Baseline == Durin::EDefaultDeltaBaselineKind::StructTypeDefault
					&& Field.Disposition == Durin::EDefaultDeltaDisposition::Omitted;
			}));
		ASSERT_EQ(Fixed->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		ASSERT_NE(Fixed->Value, nullptr);
		EXPECT_EQ(Fixed->Value->Elements.size(), 2u);

		Instance->ClassSpecific = Durin::FVector3(-0.0, 0.0, 0.0);
		Durin::FDefaultDeltaPlan SignedZeroPlan;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, SignedZeroPlan, &Diagnostic));
		const auto SignedZeroRoot = std::ranges::find_if(SignedZeroPlan.Objects,
			[&](const Durin::FDefaultDeltaObjectPlan& Object) { return Object.Object == Instance; });
		ASSERT_NE(SignedZeroRoot, SignedZeroPlan.Objects.end());
		const auto SignedZeroStruct = std::ranges::find_if(SignedZeroRoot->Fields,
			[](const Durin::FDefaultDeltaFieldPlan& Field) {
				return Field.Descriptor.Name == Durin::FName("ClassSpecific");
			});
		ASSERT_NE(SignedZeroStruct, SignedZeroRoot->Fields.end());
		ASSERT_NE(SignedZeroStruct->Value, nullptr);
		ASSERT_EQ(SignedZeroStruct->Value->Fields.size(), 3u);
		EXPECT_EQ(SignedZeroStruct->Value->Fields[0].Disposition,
			Durin::EDefaultDeltaDisposition::Emitted);
		EXPECT_EQ(SignedZeroStruct->Value->Fields[1].Disposition,
			Durin::EDefaultDeltaDisposition::Omitted);
		EXPECT_EQ(SignedZeroStruct->Value->Fields[2].Disposition,
			Durin::EDefaultDeltaDisposition::Omitted);

		Instance->ExactFloat = std::bit_cast<double>(uint64{0x7FF8000000000043ull});
		Durin::FDefaultDeltaPlan NaNPlan;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, NaNPlan, &Diagnostic));
		const auto RootIt = std::ranges::find_if(NaNPlan.Objects,
			[&](const Durin::FDefaultDeltaObjectPlan& Object) { return Object.Object == Instance; });
		ASSERT_NE(RootIt, NaNPlan.Objects.end());
		const auto NaNField = std::ranges::find_if(RootIt->Fields,
			[](const Durin::FDefaultDeltaFieldPlan& Field) {
				return Field.Descriptor.Name == Durin::FName("ExactFloat");
			});
		ASSERT_NE(NaNField, RootIt->Fields.end());
		EXPECT_EQ(NaNField->Disposition, Durin::EDefaultDeltaDisposition::Emitted);

		Instance->bEmitLateField = true;
		Durin::FDefaultDeltaPlan FailedPlan = NaNPlan;
		EXPECT_FALSE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, FailedPlan, &Diagnostic));
		EXPECT_EQ(Diagnostic.Reason, Durin::EDefaultDeltaFailureReason::ManifestMismatch);
		EXPECT_TRUE(FailedPlan.Objects.empty());
		Instance->bEmitLateField = false;
		Instance->bEmitDeepField = true;
		EXPECT_FALSE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, FailedPlan, &Diagnostic));
		EXPECT_EQ(Diagnostic.Reason, Durin::EDefaultDeltaFailureReason::DepthLimit);
		EXPECT_TRUE(FailedPlan.Objects.empty());
		Instance->bEmitDeepField = false;
		Instance->bEmitOversizedArray = true;
		EXPECT_FALSE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, FailedPlan, &Diagnostic));
		EXPECT_EQ(Diagnostic.Reason, Durin::EDefaultDeltaFailureReason::ArchiveFailure);
		EXPECT_TRUE(FailedPlan.Objects.empty());
		Instance->bEmitOversizedArray = false;
		Instance->NativeOnlyStructValue = 4;
		EXPECT_FALSE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, FailedPlan, &Diagnostic));
		EXPECT_EQ(Diagnostic.Reason, Durin::EDefaultDeltaFailureReason::MissingStructDefault);
		EXPECT_TRUE(FailedPlan.Objects.empty());

		Durin::MarkObjectHierarchyAsGarbage(Instance);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, DefaultDeltaPlanUsesAuthoritativeStructIdentity)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DAuthoritativeDeltaOwnerForTest::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		auto* Instance = Durin::NewObject<DAuthoritativeDeltaOwnerForTest>(
			nullptr, Durin::FName("AuthoritativeDeltaInstance"));
		Instance->Value.Value = "DEFAULT";
		Durin::FDefaultDeltaPlan Plan;
		Durin::FDefaultDeltaDiagnostic Diagnostic;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic))
			<< "reason=" << static_cast<int>(Diagnostic.Reason)
			<< " path=" << Diagnostic.LogicalPath;
		ASSERT_EQ(Plan.Objects.size(), 1u);
		ASSERT_EQ(Plan.Objects[0].Fields.size(), 1u);
		EXPECT_EQ(Plan.Objects[0].Fields[0].Disposition,
			Durin::EDefaultDeltaDisposition::Omitted);

		Instance->Value.Value = "different";
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic))
			<< "reason=" << static_cast<int>(Diagnostic.Reason)
			<< " path=" << Diagnostic.LogicalPath;
		ASSERT_EQ(Plan.Objects[0].Fields[0].Disposition,
			Durin::EDefaultDeltaDisposition::Emitted);
		ASSERT_NE(Plan.Objects[0].Fields[0].Value, nullptr);
		ASSERT_EQ(Plan.Objects[0].Fields[0].Value->Fields.size(), 1u);
		EXPECT_EQ(Plan.Objects[0].Fields[0].Value->Fields[0].Disposition,
			Durin::EDefaultDeltaDisposition::Emitted);

		Durin::MarkAsGarbage(Instance);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, AuthoredOverrideLedgerIsCanonicalTransactionalAndCopied)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DDefaultGraphOwnerForTest::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		auto* Instance = Durin::NewObject<DDefaultGraphOwnerForTest>(
			nullptr, Durin::FName("AuthoredLedgerInstance"));
		ASSERT_FALSE(Instance->HasAllocatedAuthoredOverrideLedger());

		const Durin::FName Owner = Class->GetQualifiedName();
		const Durin::FName Vector = Durin::Z_Construct_DStruct_FVector3()->GetQualifiedName();
		const Durin::FAuthoredOverridePath StructPath{
			Durin::FAuthoredOverridePathToken::Field(Owner, Durin::FName("ClassSpecific"))};
		Durin::FAuthoredOverridePath NestedPath = StructPath;
		NestedPath.push_back(Durin::FAuthoredOverridePathToken::Field(Vector, Durin::FName("x")));
		const Durin::FAuthoredOverridePath FixedPath{
			Durin::FAuthoredOverridePathToken::Field(Owner, Durin::FName("Fixed")),
			Durin::FAuthoredOverridePathToken::FixedArrayElement(1)};
		Durin::FAuthoredOverrideDiagnostic LedgerDiagnostic;
		ASSERT_TRUE(Instance->SetAuthoredOverride(
			NestedPath, Durin::EAuthoredOverrideProvenance::LoadedExplicit, &LedgerDiagnostic));
		ASSERT_TRUE(Instance->SetAuthoredOverride(
			FixedPath, Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic));
		EXPECT_TRUE(Instance->HasAllocatedAuthoredOverrideLedger());
		ASSERT_EQ(Instance->GetAuthoredOverrideEntries().size(), 2u);

		Durin::FDefaultDeltaPlan Plan, Repeated;
		Durin::FDefaultDeltaDiagnostic Diagnostic;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic));
		const auto EqualStruct = std::ranges::find_if(Plan.Objects.front().Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("ClassSpecific");
		});
		ASSERT_NE(EqualStruct, Plan.Objects.front().Fields.end());
		EXPECT_EQ(EqualStruct->Identity, Durin::EPropertyIdentityResult::Identical);
		EXPECT_EQ(EqualStruct->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		EXPECT_EQ(EqualStruct->Provenance, Durin::EDefaultDeltaProvenance::Explicit);

		Instance->ClassSpecific = Durin::FVector3(0.0, 0.0, 0.0);
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic));
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Repeated, &Diagnostic));
		EXPECT_TRUE(Durin::AreDefaultDeltaPlansEquivalent(Plan, Repeated));
		const auto& Fields = Plan.Objects.front().Fields;
		const auto StructIt = std::ranges::find_if(Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("ClassSpecific");
		});
		const auto FixedIt = std::ranges::find_if(Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("Fixed");
		});
		ASSERT_NE(StructIt, Fields.end());
		ASSERT_NE(FixedIt, Fields.end());
		EXPECT_EQ(StructIt->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		ASSERT_NE(StructIt->Value, nullptr);
		const auto X = std::ranges::find_if(StructIt->Value->Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("x");
		});
		ASSERT_NE(X, StructIt->Value->Fields.end());
		EXPECT_EQ(X->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		EXPECT_EQ(X->Provenance, Durin::EDefaultDeltaProvenance::Explicit);
		EXPECT_EQ(FixedIt->Provenance, Durin::EDefaultDeltaProvenance::Forced);

		const auto BeforeDuplicateFailure = Instance->GetAuthoredOverrideEntries();
		const std::array DuplicateEntries{
			Durin::FAuthoredOverrideEntry{StructPath, Durin::EAuthoredOverrideProvenance::LoadedExplicit},
			Durin::FAuthoredOverrideEntry{StructPath, Durin::EAuthoredOverrideProvenance::Forced}};
		EXPECT_FALSE(Instance->ReplaceAuthoredOverrides(DuplicateEntries, &LedgerDiagnostic));
		EXPECT_EQ(LedgerDiagnostic.Reason, Durin::EAuthoredOverrideFailureReason::DuplicatePath);
		EXPECT_EQ(Instance->GetAuthoredOverrideEntries().size(), BeforeDuplicateFailure.size());

		const Durin::FAuthoredOverridePath InvalidPath{
			Durin::FAuthoredOverridePathToken::Field(Owner, Durin::FName("Missing"))};
		EXPECT_FALSE(Instance->SetAuthoredOverride(
			InvalidPath, Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic));
		EXPECT_EQ(LedgerDiagnostic.Reason, Durin::EAuthoredOverrideFailureReason::FieldNotFound);
		EXPECT_FALSE(Instance->SetAuthoredOverride(StructPath,
			static_cast<Durin::EAuthoredOverrideProvenance>(255), &LedgerDiagnostic));
		EXPECT_EQ(LedgerDiagnostic.Reason, Durin::EAuthoredOverrideFailureReason::InvalidProvenance);
		auto* DefaultObject = const_cast<Durin::DObject*>(Class->GetDefaultObject());
		ASSERT_NE(DefaultObject, nullptr);
		EXPECT_FALSE(DefaultObject->SetAuthoredOverride(
			StructPath, Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic));
		EXPECT_EQ(LedgerDiagnostic.Reason, Durin::EAuthoredOverrideFailureReason::TemplateObject);
		Durin::FAuthoredOverridePath Excessive = StructPath;
		for (uint32 Index = 0; Index < Durin::DefaultDeltaMaxDepth; ++Index)
			Excessive.push_back(Durin::FAuthoredOverridePathToken::Field(Vector, Durin::FName("x")));
		EXPECT_FALSE(Instance->SetAuthoredOverride(
			Excessive, Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic));
		EXPECT_EQ(LedgerDiagnostic.Reason, Durin::EAuthoredOverrideFailureReason::DepthLimit);
		ASSERT_TRUE(Instance->SetAuthoredOverride(
			StructPath, Durin::EAuthoredOverrideProvenance::LoadedExplicit, &LedgerDiagnostic));
		EXPECT_TRUE(Instance->ClearAuthoredOverride(StructPath));
		EXPECT_EQ(Instance->GetAuthoredOverrideEntries().size(), 2u);

		Instance->bEmitOptionalField = true;
		const Durin::FAuthoredOverridePath RemovedNativePath{
			Durin::FAuthoredOverridePathToken::Field(
				Durin::FName("Tests::DDefaultGraphOwnerForTest"), Durin::FName("OptionalField"))};
		ASSERT_TRUE(Instance->SetAuthoredOverride(RemovedNativePath,
			Durin::EAuthoredOverrideProvenance::LoadedExplicit, &LedgerDiagnostic));
		Instance->bEmitOptionalField = false;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic));
		EXPECT_TRUE(Instance->ClearAuthoredOverride(RemovedNativePath));

		auto* NewOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("LedgerDuplicateOuter"));
		const DDefaultGraphOwnerForTest* ConstInstance = Instance;
		auto* Duplicate = Durin::DuplicateObject(ConstInstance, NewOuter);
		ASSERT_NE(Duplicate, nullptr);
		EXPECT_EQ(Duplicate->GetFName(), Instance->GetFName());
		EXPECT_EQ(Duplicate->GetAuthoredOverrideEntries().size(), 2u);
		std::atomic<bool> WorkerReadsSucceeded = true;
		std::thread Worker([&] {
			for (int Iteration = 0; Iteration < 256; ++Iteration)
				if (Duplicate->GetAuthoredOverrideEntries().size() != 2u) WorkerReadsSucceeded = false;
		});
		Worker.join();
		EXPECT_TRUE(WorkerReadsSucceeded.load());

		EXPECT_EQ(Instance->ClearAuthoredOverrideSubtree(StructPath), 1u);
		EXPECT_FALSE(Instance->ClearAuthoredOverride(StructPath));
		Instance->ResetAuthoredOverrides();
		EXPECT_FALSE(Instance->HasAllocatedAuthoredOverrideLedger());
		EXPECT_TRUE(Instance->GetAuthoredOverrideEntries().empty());

		Durin::MarkObjectHierarchyAsGarbage(Instance);
		Durin::MarkObjectHierarchyAsGarbage(NewOuter);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, AuthoredOverridePathsTrackArrayPositionsAndCanonicalMapKeys)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DOverrideContainerOwnerForTest::StaticClass();
		const std::array ClassBatch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(ClassBatch));
		auto* Instance = Durin::NewObject<DOverrideContainerOwnerForTest>(
			nullptr, Durin::FName("ContainerLedgerInstance"));

		const Durin::FName Owner = Class->GetQualifiedName();
		Durin::FAuthoredOverridePath ArrayPath{
			Durin::FAuthoredOverridePathToken::Field(Owner, Durin::FName("Values")),
			Durin::FAuthoredOverridePathToken::ArrayElement(0)};
		auto* MapProperty = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("Lookup"));
		ASSERT_NE(MapProperty, nullptr);
		std::string StableKey = "Stable";
		std::vector<std::byte> StableToken;
		std::string TokenError;
		ASSERT_TRUE(Durin::BuildCanonicalMapKeyToken(
			MapProperty->GetKeyProp(), &StableKey, 0, StableToken, &TokenError)) << TokenError;
		Durin::FAuthoredOverridePath MapPath{
			Durin::FAuthoredOverridePathToken::Field(Owner, Durin::FName("Lookup")),
			Durin::FAuthoredOverridePathToken::MapValue(StableToken)};
		Durin::FAuthoredOverrideDiagnostic LedgerDiagnostic;
		ASSERT_TRUE(Instance->SetAuthoredOverride(
			ArrayPath, Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic))
			<< "reason=" << static_cast<int>(LedgerDiagnostic.Reason)
			<< " path=" << LedgerDiagnostic.LogicalPath;
		ASSERT_TRUE(Instance->SetAuthoredOverride(
			MapPath, Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic));

		Durin::FDefaultDeltaPlan Plan;
		Durin::FDefaultDeltaDiagnostic Diagnostic;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic));
		const auto& Fields = Plan.Objects.front().Fields;
		const auto Array = std::ranges::find_if(Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("Values");
		});
		const auto Map = std::ranges::find_if(Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("Lookup");
		});
		ASSERT_NE(Array, Fields.end());
		ASSERT_NE(Map, Fields.end());
		ASSERT_NE(Array->Value, nullptr);
		ASSERT_EQ(Array->Value->Elements.size(), 1u);
		EXPECT_EQ(Array->Value->Elements[0]->Provenance, Durin::EDefaultDeltaProvenance::Forced);
		ASSERT_NE(Map->Value, nullptr);
		ASSERT_EQ(Map->Value->Elements.size(), 2u);
		EXPECT_EQ(Map->Value->Elements[1]->Provenance, Durin::EDefaultDeltaProvenance::Forced);

		Instance->Lookup.emplace("Earlier", 0);
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic));
		const auto ReorderedMap = std::ranges::find_if(Plan.Objects.front().Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("Lookup");
		});
		ASSERT_NE(ReorderedMap, Plan.Objects.front().Fields.end());
		ASSERT_NE(ReorderedMap->Value, nullptr);
		const auto StableValue = std::ranges::find_if(ReorderedMap->Value->Elements,
			[&](const auto& Element) {
				return Element && Element->CanonicalMapKeyToken == StableToken
					&& Element->LogicalType.Kind == Durin::FArchiveLogicalTypeDescriptor::EKind::Scalar;
			});
		ASSERT_NE(StableValue, ReorderedMap->Value->Elements.end());
		EXPECT_EQ((*StableValue)->Provenance, Durin::EDefaultDeltaProvenance::Forced);

		Instance->Values.clear();
		Instance->Lookup.erase("Stable");
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::Enabled, Plan, &Diagnostic));
		const auto StaleArray = std::ranges::find_if(Plan.Objects.front().Fields, [](const auto& Field) {
			return Field.Descriptor.Name == Durin::FName("Values");
		});
		ASSERT_NE(StaleArray, Plan.Objects.front().Fields.end());
		EXPECT_EQ(StaleArray->Disposition, Durin::EDefaultDeltaDisposition::Emitted);
		EXPECT_EQ(StaleArray->Provenance, Durin::EDefaultDeltaProvenance::Explicit);

		Durin::DClass* NativeClass = DDefaultGraphOwnerForTest::StaticClass();
		const std::array NativeBatch{NativeClass};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(NativeBatch));
		auto* Native = Durin::NewObject<DDefaultGraphOwnerForTest>(
			nullptr, Durin::FName("NativeMapLedgerInstance"));
		const Durin::FAuthoredOverridePath UnavailableMapPath{
			Durin::FAuthoredOverridePathToken::Field(
				Durin::FName("Tests::DDefaultGraphOwnerForTest"), Durin::FName("NativeMap")),
			Durin::FAuthoredOverridePathToken::MapValue(
				{std::byte{1}, std::byte{2}, std::byte{3}})};
		EXPECT_FALSE(Native->SetAuthoredOverride(UnavailableMapPath,
			Durin::EAuthoredOverrideProvenance::Forced, &LedgerDiagnostic));
		EXPECT_EQ(LedgerDiagnostic.Reason, Durin::EAuthoredOverrideFailureReason::MapKeyUnavailable);

		Durin::MarkAsGarbage(Instance);
		Durin::MarkObjectHierarchyAsGarbage(Native);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, NoDeltaForcesCompleteLogicalGraphWithoutDefaults)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DDefaultGraphOwnerForTest::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		auto* Instance = Durin::NewObject<DDefaultGraphOwnerForTest>(
			nullptr, Durin::FName("NoDeltaInstance"));
		Instance->NativeOnlyStructValue = 99;
		Durin::FDefaultDeltaPlan Plan;
		Durin::FDefaultDeltaDiagnostic Diagnostic;
		ASSERT_TRUE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::NoDelta, Plan, &Diagnostic));
		EXPECT_EQ(Plan.Mode, Durin::EDefaultDeltaMode::NoDelta);
		EXPECT_EQ(Plan.Objects.size(), 2u);
		EXPECT_EQ(Plan.OmittedFieldCount, 0u);
		EXPECT_EQ(Plan.FieldCount, Plan.EmittedFieldCount);
		std::function<void(const Durin::FDefaultDeltaNode&)> ExpectForcedNode;
		ExpectForcedNode = [&](const Durin::FDefaultDeltaNode& Node) {
			EXPECT_EQ(Node.Baseline, Durin::EDefaultDeltaBaselineKind::None);
			EXPECT_EQ(Node.Disposition, Durin::EDefaultDeltaDisposition::Emitted);
			EXPECT_EQ(Node.Provenance, Durin::EDefaultDeltaProvenance::Forced);
			EXPECT_EQ(Node.SourceValue, nullptr);
			EXPECT_EQ(Node.SourceStruct, nullptr);
			for (const auto& Field : Node.Fields)
			{
				EXPECT_EQ(Field.Baseline, Durin::EDefaultDeltaBaselineKind::None);
				EXPECT_EQ(Field.Disposition, Durin::EDefaultDeltaDisposition::Emitted);
				EXPECT_EQ(Field.Provenance, Durin::EDefaultDeltaProvenance::Forced);
				ASSERT_NE(Field.Value, nullptr);
				ExpectForcedNode(*Field.Value);
			}
			for (const auto& Element : Node.Elements)
			{
				ASSERT_NE(Element, nullptr);
				ExpectForcedNode(*Element);
			}
		};
		for (const auto& Object : Plan.Objects)
		{
			EXPECT_EQ(Object.ClassDefaultObject, nullptr);
			for (const auto& Field : Object.Fields)
			{
				EXPECT_EQ(Field.Provenance, Durin::EDefaultDeltaProvenance::Forced);
				ASSERT_NE(Field.Value, nullptr);
				ExpectForcedNode(*Field.Value);
			}
		}
		EXPECT_FALSE(Instance->HasAllocatedAuthoredOverrideLedger());

		Instance->bEmitLateField = true;
		EXPECT_FALSE(Durin::BuildDefaultDeltaPlan(
			Instance, Durin::EDefaultDeltaMode::NoDelta, Plan, &Diagnostic));
		EXPECT_EQ(Diagnostic.Reason, Durin::EDefaultDeltaFailureReason::ManifestMismatch);
		EXPECT_TRUE(Plan.Objects.empty());

		Durin::MarkObjectHierarchyAsGarbage(Instance);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, ClassDefaultMatchesReflectedAndNativeArchiveDefaults)
	{
		EnsureDObjectInitialized();
		Durin::DClass* Class = DLifecycleReferenceOwnerForTest::StaticClass();
		const std::array Batch{Class};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		const auto* DefaultObject = static_cast<const DLifecycleReferenceOwnerForTest*>(
			Class->GetDefaultObject());
		auto* Instance = Durin::NewObject<DLifecycleReferenceOwnerForTest>(
			nullptr, Durin::FName("ArchiveParityInstance"));
		ASSERT_NE(DefaultObject, nullptr);
		ASSERT_NE(Instance, nullptr);

		Class->ForEachProperty([&](Durin::FProperty* Property) {
			if (Property->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				EXPECT_TRUE(Durin::ArePropertyValuesIdentical(
					Property, DefaultObject, Index, Instance, Index))
					<< Property->NamePrivate.ToString();
			}
		}, true);
		EXPECT_EQ(DefaultObject->NativeScalar, Instance->NativeScalar);
		EXPECT_EQ(DefaultObject->NativeStruct.Value, Instance->NativeStruct.Value);
		EXPECT_EQ(DefaultObject->NativeStruct.Label, Instance->NativeStruct.Label);
		EXPECT_EQ(DefaultObject->NativeValues, Instance->NativeValues);
		EXPECT_EQ(DefaultObject->SerializedNativeReference, Instance->SerializedNativeReference);

		Durin::MarkAsGarbage(Instance);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, ClassDefaultBatchIgnoresInputRegistrationOrder)
	{
		EnsureDObjectInitialized();
		auto MakeClass = [](const char* Name, Durin::DClass* SuperClass) {
			auto* Class = new Durin::DClass(
				Durin::EC_StaticConstructor,
				Durin::FName(Name),
				sizeof(DLifecycleTestObject),
				alignof(DLifecycleTestObject),
				Durin::EObjectFlags::NoFlags,
				Durin::EClassFlags::None,
				Durin::EClassCastFlags::DClass,
				&ConstructOrderedDefault);
			Class->SetSuperStructBase(SuperClass);
			Class->Register(Durin::DClass::StaticClass, "/Cpp/LateDefaultModuleForTest", Name);
			Durin::DObjectForceRegistration(Class);
			return Class;
		};

		Durin::DClass* BaseClass = MakeClass("DOrderedDefaultBaseForTest", Durin::DObject::StaticClass());
		Durin::DClass* DerivedClass = MakeClass("DOrderedDefaultDerivedForTest", BaseClass);
		GOrderedDefaultConstruction.clear();
		const std::array ReversedBatch{DerivedClass, BaseClass};
		ASSERT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(ReversedBatch));
		ASSERT_EQ(GOrderedDefaultConstruction.size(), 2u);
		EXPECT_EQ(GOrderedDefaultConstruction[0], "DOrderedDefaultBaseForTest");
		EXPECT_EQ(GOrderedDefaultConstruction[1], "DOrderedDefaultDerivedForTest");
		const Durin::DObject* BaseDefault = BaseClass->GetDefaultObject();
		const Durin::DObject* DerivedDefault = DerivedClass->GetDefaultObject();

		const std::array ForwardBatch{BaseClass, DerivedClass};
		EXPECT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(ForwardBatch));
		EXPECT_EQ(BaseClass->GetDefaultObject(), BaseDefault);
		EXPECT_EQ(DerivedClass->GetDefaultObject(), DerivedDefault);
		EXPECT_EQ(GOrderedDefaultConstruction.size(), 2u);

		auto* DeferredDefault = const_cast<DLifecycleTestObject*>(
			static_cast<const DLifecycleTestObject*>(BaseDefault));
		DeferredDefault->bReadyForFinishDestroy = false;
		EXPECT_FALSE(Durin::ReleaseClassDefaultObjectsForModule(
			Durin::FName("LateDefaultModuleForTest")));
		EXPECT_EQ(BaseClass->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Uninitialized);
		EXPECT_EQ(DerivedClass->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Uninitialized);
		EXPECT_TRUE(Durin::GDObjectArray.Contains(BaseDefault));
		EXPECT_FALSE(Durin::GDObjectArray.Contains(DerivedDefault));
		DeferredDefault->bReadyForFinishDestroy = true;
		EXPECT_TRUE(Durin::ReleaseClassDefaultObjectsForModule(
			Durin::FName("LateDefaultModuleForTest")));
		EXPECT_FALSE(Durin::GDObjectArray.Contains(BaseDefault));
	}

	TEST(FCoreDObjectReflectionTests, DeferredRegistrationOwnsTemporaryNames)
	{
		EnsureDObjectInitialized();

		auto* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("Durin::FTemporaryRegistrationTest"),
			Durin::FName("FTemporaryRegistrationTest"),
			1,
			1,
			Durin::EObjectFlags::NoFlags
		);
		{
			const std::string PackageName = "/Cpp/CoreDObjectTests";
			const std::string ObjectName = "Durin::FTemporaryRegistrationTest";
			Struct->Register(Durin::DStruct::StaticClass, PackageName.c_str(), ObjectName.c_str());
		}

		// Reuse freed small-string allocations before registration to expose borrowed buffers.
		const std::vector<std::string> AllocationChurn(64, std::string(64, 'x'));
		EXPECT_FALSE(AllocationChurn.empty());
		Durin::DObjectForceRegistration(Struct);

		EXPECT_EQ(Struct->GetName(), "Durin::FTemporaryRegistrationTest");
		ASSERT_NE(Struct->GetOuter(), nullptr);
		EXPECT_EQ(Struct->GetOuter()->GetName(), "CoreDObjectTests");
		Durin::MarkAsGarbage(Struct);
	}

	TEST(FCoreDObjectReflectionTests, NewObjectKeepsClassAndCastBehavior)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("CoreDObjectTestObject"));

		ASSERT_NE(Object, nullptr);
		EXPECT_EQ(Object->GetClass(), Durin::DObject::StaticClass());
		EXPECT_TRUE(Object->IsA(Durin::DObject::StaticClass()));
		EXPECT_EQ(Durin::Cast<Durin::DObject>(Object), Object);
	}

	TEST(FCoreDObjectReflectionTests, AbstractClassesRetainHierarchyButRejectConstruction)
	{
		EnsureDObjectInitialized();
		Durin::DClass AbstractClass(
			Durin::EC_StaticConstructor,
			Durin::FName("DAbstractObjectForTest"),
			sizeof(DLifecycleTestObject),
			alignof(DLifecycleTestObject),
			Durin::EObjectFlags::NoFlags,
			Durin::EClassFlags::Abstract,
			Durin::EClassCastFlags::DClass,
			(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DLifecycleTestObject>
		);
		AbstractClass.SetSuperStructBase(Durin::DObject::StaticClass());

		EXPECT_TRUE(AbstractClass.IsChildOf(Durin::DObject::StaticClass()));
		EXPECT_TRUE(AbstractClass.HasAnyClassFlags(Durin::EClassFlags::Abstract));
		const std::array Batch{&AbstractClass};
		EXPECT_TRUE(Durin::Private::CreateClassDefaultObjectsForBatch(Batch));
		EXPECT_EQ(AbstractClass.GetDefaultObjectState(), Durin::EClassDefaultObjectState::Ineligible);
		EXPECT_EQ(AbstractClass.GetDefaultObjectReason(), Durin::EClassDefaultObjectReason::Abstract);
		EXPECT_FALSE(Durin::CanConstructObjectOfClass(
			&AbstractClass, Durin::DObject::StaticClass()
		));
		EXPECT_EQ(Durin::NewObject(&AbstractClass, nullptr, Durin::FName("RejectedAbstractObject")), nullptr);
	}

	TEST(FCoreDObjectReflectionTests, OuterIndexTracksRegistrationRenameReparentGarbageAndRemoval)
	{
		EnsureDObjectInitialized();

		Durin::DObject* FirstOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("OuterIndexFirst"));
		Durin::DObject* SecondOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("OuterIndexSecond"));
		Durin::DObject* FirstChild = Durin::NewObject<Durin::DObject>(FirstOuter, Durin::FName("OuterIndexFirstChild"));
		Durin::DObject* MiddleChild = Durin::NewObject<Durin::DObject>(FirstOuter, Durin::FName("OuterIndexMiddleChild"));
		Durin::DObject* LastChild = Durin::NewObject<Durin::DObject>(FirstOuter, Durin::FName("OuterIndexLastChild"));
		auto Contains = [](const std::vector<Durin::DObject*>& Objects, const Durin::DObject* Object) {
			return std::find(Objects.begin(), Objects.end(), Object) != Objects.end();
		};

		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter, Durin::EObjectQueryScope::LiveOnly), MiddleChild));
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter, Durin::EObjectQueryScope::LiveOnly), MiddleChild));

		MiddleChild->Rename(Durin::FName("OuterIndexRenamedChild"));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter, Durin::EObjectQueryScope::LiveOnly), MiddleChild));

		// Removing the middle swaps LastChild into its slot and must update that back-pointer.
		MiddleChild->SetOuterPrivate(SecondOuter);
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter, Durin::EObjectQueryScope::LiveOnly), MiddleChild));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter, Durin::EObjectQueryScope::LiveOnly), MiddleChild));

		FirstChild->SetOuterPrivate(SecondOuter);
		LastChild->SetOuterPrivate(SecondOuter);
		EXPECT_TRUE(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter, Durin::EObjectQueryScope::LiveOnly).empty());
		EXPECT_EQ(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter, Durin::EObjectQueryScope::LiveOnly).size(), 3);

		MiddleChild->SetOuterPrivate(nullptr);
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter, Durin::EObjectQueryScope::LiveOnly), MiddleChild));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr, Durin::EObjectQueryScope::LiveOnly), MiddleChild));

		Durin::MarkAsGarbage(MiddleChild);
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr, Durin::EObjectQueryScope::LiveOnly), MiddleChild));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr, Durin::EObjectQueryScope::LiveOnly, true), MiddleChild));

		Durin::CollectGarbage();
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr, Durin::EObjectQueryScope::LiveOnly, true), MiddleChild));
	}

	TEST(FCoreDObjectReflectionTests, DestroyingWideOuterDetachesAllReachableChildren)
	{
		EnsureDObjectInitialized();

		constexpr size_t ChildCount = 1024;
		Durin::DObject* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WideOuter"));
		std::vector<Durin::DObject*> Children;
		Children.reserve(ChildCount);
		for (size_t Index = 0; Index < ChildCount; ++Index)
		{
			Durin::DObject* Child = Durin::NewObject<Durin::DObject>(Outer, Durin::FName("WideOuterChild"));
			Durin::AddToRoot(Child);
			Children.push_back(Child);
		}

		Durin::MarkAsGarbage(Outer);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Outer));
		for (Durin::DObject* Child : Children)
		{
			EXPECT_TRUE(ObjectArrayContains(Child));
			EXPECT_EQ(Child->GetOuter(), nullptr);
			Durin::RemoveFromRoot(Child);
		}
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, PackageRetainsAllTopLevelAssetsAndBuildsStructuralObjectPaths)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/Package", Path));
		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "Package");
		Package->InitializeAssetPackage(Path);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "Package");
		Durin::DObject* Inner = Durin::NewObject<Durin::DObject>(Asset, "Inner");
		Durin::DObject* Secondary = Durin::NewObject<Durin::DObject>(Package, "Secondary");

		EXPECT_EQ(Package->GetOuter(), nullptr);
		EXPECT_EQ(Asset->GetPackage(), Package);
		EXPECT_EQ(Asset->GetObjectPath(), "/CoreTests/Package.Package");
		EXPECT_EQ(Inner->GetObjectPath(), "/CoreTests/Package.Package:Inner");
		EXPECT_EQ(Secondary->GetObjectPath(), "/CoreTests/Package.Secondary");
		ASSERT_EQ(Package->GetTopLevelAssets().size(), 2u);
		EXPECT_EQ(Package->FindTopLevelAsset("Package"), Asset);
		EXPECT_EQ(Package->FindTopLevelAsset("Secondary"), Secondary);
		Secondary->Rename("Package");
		EXPECT_EQ(Secondary->GetFName(), Durin::FName("Secondary"));
		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Package));
		EXPECT_TRUE(ObjectArrayContains(Asset));
		EXPECT_TRUE(ObjectArrayContains(Secondary));
		EXPECT_FALSE(ObjectArrayContains(Inner));
		Durin::MarkAsGarbage(Secondary);
		Durin::CollectGarbage();
		EXPECT_EQ(Package->FindTopLevelAsset("Secondary"), nullptr);

		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
	}

	TEST(FCoreDObjectReflectionTests, CreatePackageInitializesStandaloneAndRejectsDuplicatePaths)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/CreatedPackage", Path));

		Durin::DPackage* Package = Durin::CreatePackage(Path);
		ASSERT_NE(Package, nullptr);
		EXPECT_TRUE(Package->IsAssetPackage());
		EXPECT_FALSE(Package->IsNewlyCreated());
		EXPECT_EQ(Package->GetPackagePath(), Path.ToString());
		EXPECT_EQ(Package->GetFName(), Durin::FName("CreatedPackage"));
		EXPECT_EQ(Durin::FindPackage(Path.GetView()), Package);
		EXPECT_TRUE(Package->HasAnyObjectFlags(Durin::EObjectFlags::Standalone));
		EXPECT_FALSE(Package->HasAnyInternalFlags(
			Durin::EObjectInternalFlags::RootSet));
		EXPECT_EQ(Durin::CreatePackage(Path), nullptr);
		EXPECT_EQ(Durin::CreatePackage({}), nullptr);
		Package->MarkAsNewlyCreated();
		EXPECT_TRUE(Package->IsNewlyCreated());
		Package->MarkAsPublished();
		EXPECT_FALSE(Package->IsNewlyCreated());
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::FindPackage(Path.GetView()), Package);

		Durin::MarkObjectHierarchyAsGarbage(Package);
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	}

	TEST(FCoreDObjectReflectionTests, NewObjectFlagsDrivePublicAndConfigurableStandaloneLifetime)
	{
		EnsureDObjectInitialized();
		Durin::DObject* Object = Durin::NewObject(
			Durin::DObject::StaticClass(), nullptr, "FlaggedObject",
			Durin::EObjectFlags::Public | Durin::EObjectFlags::Standalone);
		ASSERT_NE(Object, nullptr);
		EXPECT_TRUE(Object->HasAnyObjectFlags(Durin::EObjectFlags::Public));
		EXPECT_TRUE(Object->HasAnyObjectFlags(Durin::EObjectFlags::Standalone));
		EXPECT_FALSE(Object->HasAnyInternalFlags(Durin::EObjectInternalFlags::RootSet));

		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Object));
		Durin::CollectGarbage({.KeepFlags = Durin::EObjectFlags::NoFlags});
		EXPECT_FALSE(ObjectArrayContains(Object));

		Durin::DObject* ExplicitGarbage = Durin::NewObject(
			Durin::DObject::StaticClass(), nullptr, "StandaloneGarbageObject",
			Durin::EObjectFlags::Standalone);
		Durin::MarkAsGarbage(ExplicitGarbage);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(ExplicitGarbage));
	}

	TEST(FCoreDObjectReflectionTests, CppPackagesOwnReflectedTypesAndAreStableRoots)
	{
		EnsureDObjectInitialized();

		Durin::DPackage* CorePackage = Durin::FindPackage("/Cpp/CoreDObject");
		ASSERT_NE(CorePackage, nullptr);
		EXPECT_TRUE(CorePackage->IsCppPackage());
		EXPECT_EQ(CorePackage->GetOuter(), nullptr);
		EXPECT_TRUE(CorePackage->HasAnyInternalFlags(Durin::EObjectInternalFlags::RootSet));
		EXPECT_EQ(Durin::DPackage::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DObject::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DType::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DStructBase::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DClass::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DStruct::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DEnum::StaticClass()->GetOuter(), CorePackage);
		EXPECT_EQ(Durin::DPackage::StaticClass()->GetPackage(), CorePackage);
		EXPECT_EQ(Durin::DPackage::StaticClass()->GetObjectPath(), "/Cpp/CoreDObject.Durin::DPackage");
		EXPECT_EQ(Durin::FindClassByPath("/Cpp/CoreDObject.Durin::DPackage"), Durin::DPackage::StaticClass());
		EXPECT_EQ(Durin::FindOrCreateCppPackage("CoreDObject"), CorePackage);

		Durin::DPackage* OtherPackage = Durin::FindOrCreateCppPackage("CoreDObjectTests");
		ASSERT_NE(OtherPackage, nullptr);
		EXPECT_NE(OtherPackage, CorePackage);
		EXPECT_EQ(Durin::FindPackage("/Cpp/CoreDObjectTests"), OtherPackage);

		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(CorePackage));
		EXPECT_TRUE(ObjectArrayContains(OtherPackage));
	}

	TEST(FCoreDObjectReflectionTests, MarkAsGarbageDefersPhysicalDestructionUntilGarbageCollection)
	{
		EnsureDObjectInitialized();
		DLifecycleTestObject::ResetLifecycleCounts();

		auto* Object = Durin::NewObject<DLifecycleTestObject>(nullptr, Durin::FName("GarbageRequestTestObject"));
		const Durin::FObjectHandle Handle = Durin::MakeObjectHandle(Object);
		ASSERT_TRUE(ObjectArrayContains(Object));

		Durin::MarkAsGarbage(Object);

		EXPECT_FALSE(Durin::IsValid(Object));
		EXPECT_TRUE(ObjectArrayContains(Object));
		EXPECT_EQ(Durin::ResolveObjectHandle(Handle), Object);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 0u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 0u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 0u);

		Durin::CollectGarbage();

		EXPECT_FALSE(ObjectArrayContains(Object));
		EXPECT_EQ(Durin::ResolveObjectHandle(Handle), nullptr);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 1u);
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionRejectsRecursiveEntryFromLifecycleCallbacks)
	{
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				auto* Object = Durin::NewObject<DLifecycleTestObject>(
					nullptr, Durin::FName("RecursiveGarbageCollectionObject"));
				Object->bCollectGarbageInBeginDestroy = true;
				Durin::MarkAsGarbage(Object);
				Durin::CollectGarbage();
			}()),
			""
		);
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionWaitsForFinishReadinessWithoutRepeatingCallbacks)
	{
		EnsureDObjectInitialized();
		Durin::CollectGarbage();
		DLifecycleTestObject::ResetLifecycleCounts();
		auto* Object = Durin::NewObject<DLifecycleTestObject>(nullptr, Durin::FName("DeferredFinishTestObject"));
		Object->bReadyForFinishDestroy = false;
		Durin::MarkAsGarbage(Object);

		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Object));
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().CandidateObjectCount, 1u);
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().SweptObjectCount, 0u);
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().DeferredDestroyObjectCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 0u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 0u);

		Durin::CollectGarbage();
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().CandidateObjectCount, 1u);
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().DeferredDestroyObjectCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 0u);

		Object->bReadyForFinishDestroy = true;
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Object));
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().CandidateObjectCount, 1u);
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().SweptObjectCount, 1u);
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().DeferredDestroyObjectCount, 0u);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 1u);
	}

	TEST(FCoreDObjectReflectionTests, ShutdownDestructionCompletesAfterOneFlushAndSecondCollection)
	{
		EnsureDObjectInitialized();
		DLifecycleTestObject::ResetLifecycleCounts();
		FShutdownDestroyScheduler Scheduler;
		auto* Object = Durin::NewObject<DLifecycleTestObject>(
			nullptr, Durin::FName("ShutdownDeferredFinishTestObject")
		);
		Object->ShutdownDestroyScheduler = &Scheduler;
		Durin::MarkAsGarbage(Object);

		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Object));
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().DeferredDestroyObjectCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 0u);

		Scheduler.bFenceComplete = true;
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Object));
		EXPECT_EQ(Durin::GetLastGarbageCollectionStats().DeferredDestroyObjectCount, 0u);
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 1u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 1u);
		EXPECT_EQ(Scheduler.Checkpoints, (std::vector{EShutdownDestroyCheckpoint::BeforeRelease, EShutdownDestroyCheckpoint::FencePending, EShutdownDestroyCheckpoint::FinishDestroy, EShutdownDestroyCheckpoint::Destructor}));
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionKeepsRootAndCollectsUnreachableObjects)
	{
		EnsureDObjectInitialized();

		Durin::DObject* RootedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("RootedGCTestObject"));
		Durin::DObject* UnreachableObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("UnreachableGCTestObject"));
		Durin::AddToRoot(RootedObject);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(RootedObject));
		EXPECT_FALSE(ObjectArrayContains(UnreachableObject));

		Durin::RemoveFromRoot(RootedObject);
		Durin::MarkAsGarbage(RootedObject);
	}

	TEST(FCoreDObjectReflectionTests, GarbageObjectIsCollectedEvenWhenRooted)
	{
		EnsureDObjectInitialized();
		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("RootedGarbageObject"));
		Durin::TObjectPtr<Durin::DObject> ObjectPtr = Object;
		Durin::AddToRoot(Object);
		Durin::MarkAsGarbage(Object);

		EXPECT_EQ(ObjectPtr.Get(), Object);
		EXPECT_FALSE(ObjectPtr.IsValid());
		EXPECT_FALSE(Durin::IsValid(Object));
		Durin::CollectGarbage();
		EXPECT_EQ(ObjectPtr.Get(), nullptr);
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionSchedulerUsesTimeAndPressure)
	{
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.PendingKillThreshold = 128;
		Settings.ObjectGrowthThreshold = 1024;
		Durin::FGarbageCollectionScheduler Scheduler(Settings);
		Scheduler.Reset(100.0, 20);

		EXPECT_EQ(Scheduler.ShouldCollect(159.0, 20, 0), Durin::EGarbageCollectionTrigger::None);
		EXPECT_EQ(Scheduler.ShouldCollect(160.0, 20, 0), Durin::EGarbageCollectionTrigger::Interval);
		EXPECT_EQ(Scheduler.ShouldCollect(101.0, 20, 128), Durin::EGarbageCollectionTrigger::PendingKillPressure);
		EXPECT_EQ(Scheduler.ShouldCollect(101.0, 1044, 0), Durin::EGarbageCollectionTrigger::ObjectGrowthPressure);

		Settings.bEnabled = false;
		Durin::FGarbageCollectionScheduler DisabledScheduler(Settings);
		DisabledScheduler.Reset(100.0, 20);
		EXPECT_EQ(DisabledScheduler.ShouldCollect(1000.0, 10000, 10000), Durin::EGarbageCollectionTrigger::None);
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionSchedulerBacksOffEmptyCollectionsWithinBound)
	{
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.MaxIntervalSeconds = 600.0;
		Settings.IntervalBackoffMultiplier = 2.0;
		Settings.PendingKillThreshold = 128;
		Settings.ObjectGrowthThreshold = 1024;
		Durin::FGarbageCollectionScheduler Scheduler(Settings);
		Scheduler.Reset(0.0, 20);

		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 60.0);
		Scheduler.NotifyCollectionCompleted(60.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 120.0);
		EXPECT_EQ(Scheduler.ShouldCollect(179.0, 20, 0), Durin::EGarbageCollectionTrigger::None);
		EXPECT_EQ(Scheduler.ShouldCollect(180.0, 20, 0), Durin::EGarbageCollectionTrigger::Interval);

		Scheduler.NotifyCollectionCompleted(180.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 240.0);
		Scheduler.NotifyCollectionCompleted(420.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 480.0);
		Scheduler.NotifyCollectionCompleted(900.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 600.0);
		Scheduler.NotifyCollectionCompleted(1500.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 600.0);

		EXPECT_EQ(Scheduler.ShouldCollect(1501.0, 20, 128), Durin::EGarbageCollectionTrigger::PendingKillPressure);
		EXPECT_EQ(Scheduler.ShouldCollect(1501.0, 1044, 0), Durin::EGarbageCollectionTrigger::ObjectGrowthPressure);
		Scheduler.NotifyCollectionCompleted(1501.0, 20, 1);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 60.0);
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionSchedulerNormalizesAdaptiveSettings)
	{
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.MaxIntervalSeconds = 30.0;
		Settings.IntervalBackoffMultiplier = 0.5;
		Durin::FGarbageCollectionScheduler Scheduler(Settings);
		Scheduler.Reset(0.0, 0);
		Scheduler.NotifyCollectionCompleted(60.0, 0, 0);

		EXPECT_DOUBLE_EQ(Scheduler.GetSettings().MaxIntervalSeconds, 60.0);
		EXPECT_DOUBLE_EQ(Scheduler.GetSettings().IntervalBackoffMultiplier, 1.0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 60.0);

		Settings.IntervalSeconds = 0.0;
		Durin::FGarbageCollectionScheduler NoIntervalScheduler(Settings);
		NoIntervalScheduler.Reset(0.0, 0);
		NoIntervalScheduler.NotifyCollectionCompleted(100.0, 0, 0);
		EXPECT_EQ(NoIntervalScheduler.ShouldCollect(1000.0, 0, 0), Durin::EGarbageCollectionTrigger::None);
	}

	TEST(FCoreDObjectReflectionTests, OnlyTObjectPtrPropertiesKeepReferencedObjectsReachable)
	{
		EnsureDObjectInitialized();

		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, Durin::FName("GCReferenceOwner"));
		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCReferencedObject"));
		Durin::DObject* ObjectPtrReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCObjectPtrReferencedObject"));
		Owner->Reference = ReferencedObject;
		Owner->ObjectPtrReference = ObjectPtrReferencedObject;
		Durin::AddToRoot(Owner);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(Owner));
		EXPECT_FALSE(ObjectArrayContains(ReferencedObject));
		EXPECT_TRUE(ObjectArrayContains(ObjectPtrReferencedObject));

		Durin::RemoveFromRoot(Owner);
		Durin::MarkAsGarbage(Owner);
	}

	TEST(FCoreDObjectReflectionTests, RootedOuterDoesNotKeepChildReachable)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCOuterObject"));
		auto* Inner = Durin::NewObject<DLifecycleTestObject>(Outer, Durin::FName("GCInnerObject"));
		Durin::AddToRoot(Outer);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(Outer));
		EXPECT_FALSE(ObjectArrayContains(Inner));

		Durin::RemoveFromRoot(Outer);
		Durin::MarkAsGarbage(Outer);
	}

	TEST(FCoreDObjectReflectionTests, GarbageCollectionDestroysUnreachableOuterHierarchyOnce)
	{
		EnsureDObjectInitialized();
		DLifecycleTestObject::ResetLifecycleCounts();

		auto* Outer = Durin::NewObject<DLifecycleTestObject>(nullptr, Durin::FName("GCUnreachableOuter"));
		auto* Inner = Durin::NewObject<DLifecycleTestObject>(Outer, Durin::FName("GCUnreachableInner"));
		Durin::CollectGarbage();

		EXPECT_FALSE(ObjectArrayContains(Outer));
		EXPECT_FALSE(ObjectArrayContains(Inner));
		EXPECT_EQ(DLifecycleTestObject::BeginDestroyCount, 2u);
		EXPECT_EQ(DLifecycleTestObject::FinishDestroyCount, 2u);
		EXPECT_EQ(DLifecycleTestObject::DestructorCount, 2u);
	}

	TEST(FCoreDObjectReflectionTests, RootedChildKeepsOuterChainButNotSiblingReachable)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCReverseOuter"));
		Durin::DObject* Inner = Durin::NewObject<Durin::DObject>(Outer, Durin::FName("GCReverseInner"));
		Durin::DObject* Sibling = Durin::NewObject<Durin::DObject>(Outer, Durin::FName("GCReverseSibling"));
		Durin::AddToRoot(Inner);

		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Outer));
		EXPECT_TRUE(ObjectArrayContains(Inner));
		EXPECT_FALSE(ObjectArrayContains(Sibling));

		Durin::RemoveFromRoot(Inner);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Outer));
		EXPECT_FALSE(ObjectArrayContains(Inner));
		EXPECT_FALSE(ObjectArrayContains(Sibling));
	}

	TEST(FCoreDObjectReflectionTests, ExplicitNativeReferenceKeepsObjectReachable)
	{
		EnsureDObjectInitialized();

		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, Durin::FName("GCNativeReferenceOwner"));
		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCNativeReferencedObject"));
		Owner->NativeReference = ReferencedObject;
		Durin::AddToRoot(Owner);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(Owner));
		EXPECT_TRUE(ObjectArrayContains(ReferencedObject));

		Durin::RemoveFromRoot(Owner);
		Durin::MarkAsGarbage(Owner);
		Durin::MarkAsGarbage(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, ExplicitGarbageOuterDoesNotDestroyRootedChild)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCExplicitOuter"));
		Durin::DObject* Inner = Durin::NewObject<Durin::DObject>(Outer, Durin::FName("GCExplicitInner"));
		Durin::AddToRoot(Inner);
		Durin::MarkAsGarbage(Outer);
		Durin::CollectGarbage();

		EXPECT_FALSE(ObjectArrayContains(Outer));
		EXPECT_TRUE(ObjectArrayContains(Inner));
		EXPECT_EQ(Inner->GetOuter(), nullptr);
		Durin::RemoveFromRoot(Inner);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Inner));
	}

	TEST(FCoreDObjectReflectionTests, HierarchyGarbageMarkingUsesCurrentOuterTreeAndHonorsReparenting)
	{
		EnsureDObjectInitialized();

		Durin::DObject* OriginalOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("HierarchyOriginalOuter"));
		Durin::DObject* DoomedChild = Durin::NewObject<Durin::DObject>(OriginalOuter, Durin::FName("HierarchyDoomedChild"));
		Durin::DObject* DoomedGrandchild = Durin::NewObject<Durin::DObject>(DoomedChild, Durin::FName("HierarchyDoomedGrandchild"));
		Durin::DObject* NewOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("HierarchyNewOuter"));
		Durin::DObject* ReparentedChild = Durin::NewObject<Durin::DObject>(OriginalOuter, Durin::FName("HierarchyReparentedChild"));
		ReparentedChild->SetOuterPrivate(NewOuter);
		Durin::AddToRoot(ReparentedChild);

		Durin::MarkObjectHierarchyAsGarbage(OriginalOuter);

		EXPECT_FALSE(Durin::IsValid(OriginalOuter));
		EXPECT_FALSE(Durin::IsValid(DoomedChild));
		EXPECT_FALSE(Durin::IsValid(DoomedGrandchild));
		EXPECT_TRUE(Durin::IsValid(NewOuter));
		EXPECT_TRUE(Durin::IsValid(ReparentedChild));
		EXPECT_EQ(ReparentedChild->GetOuter(), NewOuter);

		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(OriginalOuter));
		EXPECT_FALSE(ObjectArrayContains(DoomedChild));
		EXPECT_FALSE(ObjectArrayContains(DoomedGrandchild));
		EXPECT_TRUE(ObjectArrayContains(NewOuter));
		EXPECT_TRUE(ObjectArrayContains(ReparentedChild));

		Durin::RemoveFromRoot(ReparentedChild);
		Durin::MarkObjectHierarchyAsGarbage(NewOuter);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, AutomaticGarbageCollectionPhysicallyRemovesMarkedHierarchy)
	{
		EnsureDObjectInitialized();
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 0.0;
		Settings.PendingKillThreshold = 1;
		Settings.ObjectGrowthThreshold = 0;
		Durin::ConfigureAutomaticGarbageCollection(Settings, 0.0);

		Durin::DObject* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("AutomaticHierarchyOuter"));
		Durin::DObject* Child = Durin::NewObject<Durin::DObject>(Outer, Durin::FName("AutomaticHierarchyChild"));
		Durin::MarkObjectHierarchyAsGarbage(Outer);

		EXPECT_EQ(Durin::TryCollectGarbage(1.0), Durin::EGarbageCollectionTrigger::PendingKillPressure);
		EXPECT_FALSE(ObjectArrayContains(Outer));
		EXPECT_FALSE(ObjectArrayContains(Child));

		Durin::ConfigureAutomaticGarbageCollection(Durin::FGarbageCollectionSettings{}, 1.0);
	}

	TEST(FCoreDObjectReflectionTests, AutomaticGarbageCollectionFindsReferenceRemovalAtMaximumInterval)
	{
		EnsureDObjectInitialized();
		Durin::CollectGarbage();
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.MaxIntervalSeconds = 600.0;
		Settings.IntervalBackoffMultiplier = 2.0;
		Settings.PendingKillThreshold = 0;
		Settings.ObjectGrowthThreshold = 0;
		Durin::ConfigureAutomaticGarbageCollection(Settings, Durin::FTime::Seconds());

		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, Durin::FName("AdaptiveGCReferenceOwner"));
		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("AdaptiveGCReferencedObject"));
		Durin::TObjectPtr<Durin::DObject> ReferencedObjectPtr = ReferencedObject;
		Owner->ObjectPtrReference = ReferencedObject;
		Durin::AddToRoot(Owner);

		for (uint32 Index = 0; Index < 4; ++Index)
		{
			Durin::CollectGarbage();
			EXPECT_EQ(Durin::GetLastGarbageCollectionStats().CandidateObjectCount, 0u);
		}
		EXPECT_DOUBLE_EQ(Durin::GetCurrentAutomaticGarbageCollectionIntervalSeconds(), 600.0);

		Owner->ObjectPtrReference = nullptr;
		const double ReferenceRemovalTime = Durin::FTime::Seconds();
		EXPECT_EQ(Durin::TryCollectGarbage(ReferenceRemovalTime + 500.0), Durin::EGarbageCollectionTrigger::None);
		EXPECT_EQ(Durin::TryCollectGarbage(ReferenceRemovalTime + 600.0), Durin::EGarbageCollectionTrigger::Interval);
		EXPECT_EQ(ReferencedObjectPtr.Get(), nullptr);

		Durin::RemoveFromRoot(Owner);
		Durin::CollectGarbage();
		Durin::ConfigureAutomaticGarbageCollection(Durin::FGarbageCollectionSettings{}, Durin::FTime::Seconds());
	}

	TEST(FCoreDObjectReflectionTests, RootSetIsIdempotentAndStrongPointersAreComposable)
	{
		EnsureDObjectInitialized();
		Durin::DObject* RootedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCIdempotentRoot"));

		Durin::AddToRoot(RootedObject);
		Durin::AddToRoot(RootedObject);
		Durin::RemoveFromRoot(RootedObject);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(RootedObject));

		Durin::DObject* StrongObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCStrongObject"));
		const Durin::FObjectHandle StrongHandle = Durin::MakeObjectHandle(StrongObject);
		Durin::TStrongObjectPtr<Durin::DObject> First(StrongObject);
		Durin::TStrongObjectPtr<Durin::DObject> Second(First);

		{
			Durin::TStrongObjectPtr<Durin::DObject> Moved(std::move(First));
			Second.Reset();
			Durin::CollectGarbage();
			EXPECT_TRUE(ObjectArrayContains(StrongObject));
		}

		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(StrongObject));
		EXPECT_FALSE(Durin::TStrongObjectPtr<Durin::DObject>(StrongHandle));
	}

	TEST(FCoreDObjectReflectionTests, DeepOuterChainUsesIterativeMarkAndDestroy)
	{
		EnsureDObjectInitialized();
		constexpr uint32 ChainLength = 10000;
		Durin::DObject* Outermost = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCDeepOuter0"));
		Durin::DObject* Innermost = Outermost;
		for (uint32 Index = 1; Index < ChainLength; ++Index)
		{
			Innermost = Durin::NewObject<Durin::DObject>(Innermost, Durin::FName("GCDeepOuter"));
		}
		Durin::AddToRoot(Innermost);

		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Outermost));
		EXPECT_TRUE(ObjectArrayContains(Innermost));

		Durin::RemoveFromRoot(Innermost);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Outermost));
		EXPECT_FALSE(ObjectArrayContains(Innermost));
	}

	TEST(FCoreDObjectReflectionTests, VectorTObjectPtrPropertiesKeepElementsReachable)
	{
		EnsureDObjectInitialized();

		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, Durin::FName("GCVectorReferenceOwner"));
		Durin::DObject* RawReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCRawVectorReferencedObject"));
		Durin::DObject* ObjectPtrReferencedObjectA = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCObjectPtrVectorReferencedObjectA"));
		Durin::DObject* ObjectPtrReferencedObjectB = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCObjectPtrVectorReferencedObjectB"));
		Owner->RawReferences.push_back(RawReferencedObject);
		Owner->ObjectPtrReferences.push_back(ObjectPtrReferencedObjectA);
		Owner->ObjectPtrReferences.push_back(ObjectPtrReferencedObjectB);
		Durin::AddToRoot(Owner);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(Owner));
		EXPECT_FALSE(ObjectArrayContains(RawReferencedObject));
		EXPECT_TRUE(ObjectArrayContains(ObjectPtrReferencedObjectA));
		EXPECT_TRUE(ObjectArrayContains(ObjectPtrReferencedObjectB));

		Durin::RemoveFromRoot(Owner);
		Durin::MarkAsGarbage(Owner);
	}

	TEST(FCoreDObjectReflectionTests, CompiledReferenceSchemaTraversesInheritanceStructsArraysAndMaps)
	{
		EnsureDObjectInitialized();
		// Match generated registration ordering where a derived class can first see
		// provisional superclass metadata, then verify superclass finalization rebuilds it.
		DGCReferenceSchemaDerivedForTest::StaticClass();
		DGCReferenceSchemaBaseForTest::StaticClass();

		auto* Owner = Durin::NewObject<DGCReferenceSchemaDerivedForTest>(nullptr, Durin::FName("GCReferenceSchemaOwner"));
		Durin::DObject* SharedReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaShared"));
		Durin::DObject* RawReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaRaw"));
		Durin::DObject* NestedReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaNested"));
		Durin::DObject* StaticReferenceA = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaStaticA"));
		Durin::DObject* StaticReferenceB = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaStaticB"));
		Durin::DObject* StructArrayReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaStructArray"));
		Durin::DObject* NestedArrayReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaNestedArray"));
		Durin::DObject* DirectMapReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaDirectMap"));
		Durin::DObject* ArrayMapReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCSchemaArrayMap"));
		EnsurePackageTestMount();
		Durin::FPackagePath SoftAssetPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/GCSchemaSoft", SoftAssetPath));
		Durin::DPackage* SoftPackage = Durin::NewObject<Durin::DPackage>(nullptr, Durin::FName("GCSchemaSoft"));
		SoftPackage->InitializeAssetPackage(SoftAssetPath);
		Durin::DObject* SoftReference =
			Durin::NewObject<Durin::DObject>(SoftPackage, Durin::FName("GCSchemaSoft"));

		Owner->BaseReference = SharedReference;
		Owner->DuplicateReference = SharedReference;
		Owner->RawReference = RawReference;
		Owner->Nested.Leaf.Reference = NestedReference;
		Owner->Nested.Leaf.StaticReferences[0] = StaticReferenceA;
		Owner->Nested.Leaf.StaticReferences[1] = StaticReferenceB;
		Owner->StructArray.resize(1);
		Owner->StructArray[0].Reference = StructArrayReference;
		Owner->NestedArrays = {{}, {NestedArrayReference}};
		Owner->DirectMap.emplace("Null", nullptr);
		Owner->DirectMap.emplace("Reference", DirectMapReference);
		Owner->ArrayMap.emplace("Empty", std::vector<Durin::TObjectPtr<Durin::DObject>>{});
		Owner->ArrayMap.emplace("Reference", std::vector<Durin::TObjectPtr<Durin::DObject>>{ArrayMapReference});
		ASSERT_TRUE(Owner->SoftReference.TrySetObject(SoftReference));
		Durin::AddToRoot(Owner);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(Owner));
		EXPECT_TRUE(ObjectArrayContains(SharedReference));
		EXPECT_FALSE(ObjectArrayContains(RawReference));
		EXPECT_TRUE(ObjectArrayContains(NestedReference));
		EXPECT_TRUE(ObjectArrayContains(StaticReferenceA));
		EXPECT_TRUE(ObjectArrayContains(StaticReferenceB));
		EXPECT_TRUE(ObjectArrayContains(StructArrayReference));
		EXPECT_TRUE(ObjectArrayContains(NestedArrayReference));
		EXPECT_TRUE(ObjectArrayContains(DirectMapReference));
		EXPECT_TRUE(ObjectArrayContains(ArrayMapReference));
		EXPECT_FALSE(ObjectArrayContains(SoftReference));
		EXPECT_FALSE(ObjectArrayContains(SoftPackage));

		Durin::RemoveFromRoot(Owner);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Owner));
		EXPECT_FALSE(ObjectArrayContains(SharedReference));
		EXPECT_FALSE(ObjectArrayContains(ArrayMapReference));
	}

	TEST(FCoreDObjectReflectionTests, MapPropertySupportsEditorMutationOperations)
	{
		EnsureDObjectInitialized();
		auto* Owner = Durin::NewObject<DGCReferenceSchemaDerivedForTest>(nullptr, Durin::FName("MapPropertyMutationOwner"));
		auto* Property = static_cast<Durin::FMapProperty*>(Owner->GetClass()->FindPropertyByName("DirectMap"));
		ASSERT_NE(Property, nullptr);
		ASSERT_TRUE(Property->HasMapOps());

		const std::string InitialKey = "Initial";
		Durin::TObjectPtr<Durin::DObject> InitialValue;
		Property->Insert(Owner, &InitialKey, &InitialValue);
		ASSERT_EQ(Property->Num(Owner), 1u);
		EXPECT_TRUE(Property->Contains(Owner, &InitialKey));

		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("MapPropertyMutationReference"));
		void* MutableValueAddress = nullptr;
		ASSERT_EQ(Property->FindMutableValue(Owner, &InitialKey, &MutableValueAddress), Durin::EContainerOpResult::Success);
		auto* MutableValue = static_cast<Durin::TObjectPtr<Durin::DObject>*>(MutableValueAddress);
		ASSERT_NE(MutableValue, nullptr);
		*MutableValue = ReferencedObject;
		EXPECT_EQ(Owner->DirectMap.at(InitialKey).Get(), ReferencedObject);

		const std::string RenamedKey = "Renamed";
		EXPECT_TRUE(Property->RenameKey(Owner, &InitialKey, &RenamedKey));
		EXPECT_FALSE(Property->Contains(Owner, &InitialKey));
		EXPECT_EQ(Owner->DirectMap.at(RenamedKey).Get(), ReferencedObject);
		EXPECT_TRUE(Property->Remove(Owner, &RenamedKey));
		EXPECT_TRUE(Owner->DirectMap.empty());

		const std::string ClearKey = "Clear";
		Property->Insert(Owner, &ClearKey, &InitialValue);
		ASSERT_EQ(Property->Num(Owner), 1u);
		Property->Clear(Owner);
		EXPECT_TRUE(Owner->DirectMap.empty());

		Durin::MarkAsGarbage(Owner);
		Durin::MarkAsGarbage(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, EditorOnlyFilteringRecursesThroughStructContainers)
	{
		EnsureDObjectInitialized();
		class FFieldRecordingArchive final : public Durin::FObjectArchive
		{
		public:
			explicit FFieldRecordingArchive(bool bFilterEditorOnly)
				: FObjectArchive({
					.Direction = Durin::EArchiveDirection::Save,
					.Purpose = Durin::EArchivePurpose::AuthoredPackage,
					.bFilterEditorOnly = bFilterEditorOnly})
			{
			}
			auto SerializeRawBytes(std::span<std::byte>) -> void override {}
			std::vector<Durin::FName> Fields;
		protected:
			auto OnEnterField(const Durin::FArchiveFieldDescriptor& Field) -> void override
			{
				Fields.push_back(Field.Name);
			}
		};

		auto* Owner = Durin::NewObject<DEditorOnlyArchiveOwnerForTest>(
			nullptr, "EditorOnlyArchiveOwner");
		auto Serialize = [&](FFieldRecordingArchive& Archive) {
			auto Scope = Archive.EnterObject(*Owner);
			Owner->Serialize(Archive);
			ASSERT_FALSE(Archive.HasError()) << Archive.GetError();
		};
		FFieldRecordingArchive Authored(false);
		Serialize(Authored);
		FFieldRecordingArchive Filtered(true);
		Serialize(Filtered);

		EXPECT_EQ(std::ranges::count(Authored.Fields, Durin::FName("EditorTop")), 1u);
		EXPECT_EQ(std::ranges::count(Filtered.Fields, Durin::FName("EditorTop")), 0u);
		EXPECT_EQ(std::ranges::count(Authored.Fields, Durin::FName("EditorValue")), 5u);
		EXPECT_EQ(std::ranges::count(Filtered.Fields, Durin::FName("EditorValue")), 0u);
		EXPECT_EQ(std::ranges::count(Authored.Fields, Durin::FName("RuntimeValue")), 5u);
		EXPECT_EQ(std::ranges::count(Filtered.Fields, Durin::FName("RuntimeValue")), 5u);
		Durin::MarkAsGarbage(Owner);
	}

	TEST(FCoreDObjectReflectionTests, ObjectGraphSerializationRoundTripsScalarStringAndObjectReference)
	{
		EnsureDObjectInitialized();

		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, Durin::FName("SerializedOwner"));
		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedReference"));
		Durin::DObject* RawVectorReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedRawVectorReference"));
		Durin::DObject* ObjectPtrVectorReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedObjectPtrVectorReference"));
		Durin::DObject* SerializedNativeReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedNativeReference"));
		Durin::DObject* HiddenGCReference = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("HiddenGCReference"));
		Owner->Value = 37;
		Owner->bEnabled = true;
		Owner->Label = "Serialized";
		Owner->Reference = ReferencedObject;
		Owner->ObjectPtrReference = ReferencedObject;
		Owner->WeakReference = ReferencedObject;
		Owner->WeakExternal = HiddenGCReference;
		Owner->WeakReferences = {ReferencedObject, HiddenGCReference};
		Owner->WeakMap.emplace("Internal", ReferencedObject);
		Owner->WeakMap.emplace("External", HiddenGCReference);
		Owner->RawReferences.push_back(RawVectorReferencedObject);
		Owner->ObjectPtrReferences.push_back(ObjectPtrVectorReferencedObject);
		Owner->Scores = {7, 11, 42};
		Owner->Tags = {"Alpha", "Beta"};
		Owner->Modes = {EReflectedEnumForTest::A, EReflectedEnumForTest::B};
		Owner->ScoreGroups = {{1, 2}, {3, 5, 8}};
		Owner->TransientValue = 99;
		Owner->NativeScalar = 73;
		Owner->NativeStruct = {19, Durin::FName("NativeStructLabel")};
		Owner->NativeValues = {2, 4, 8, 16};
		Owner->SerializedNativeReference = SerializedNativeReference;
		Owner->NativeReference = HiddenGCReference;
		ASSERT_EQ(Owner->GetClass(), DLifecycleReferenceOwnerForTest::StaticClass());
		ASSERT_EQ(Owner->GetClass()->GetName(), "DLifecycleReferenceOwnerForTest");
		ASSERT_EQ(ReferencedObject->GetClass(), Durin::DObject::StaticClass());

		std::vector<std::byte> Bytes;
		ASSERT_TRUE(Durin::SaveObjectGraphToMemory(Owner, Bytes));
		ASSERT_EQ(Owner->SerializePurposes.size(), 2u);
		EXPECT_EQ(Owner->SerializePurposes[0], Durin::EArchivePurpose::Discovery);
		EXPECT_EQ(Owner->SerializePurposes[1], Durin::EArchivePurpose::ObjectGraph);
		{
			Durin::FMemoryReader HeaderReader(Bytes);
			uint32 Magic = 0;
			uint32 Version = 0;
			uint64 RootId = 0;
			uint64 ObjectCount = 0;
			HeaderReader << Magic << Version << RootId << ObjectCount;
			EXPECT_EQ(Magic, 0x4E524F44u);
			EXPECT_EQ(Version, 2u);
			EXPECT_EQ(RootId, 1u);
			EXPECT_EQ(ObjectCount, 4u);
		}
		{
			auto V1Bytes = Bytes;
			V1Bytes[4] = std::byte{1};
			V1Bytes[5] = V1Bytes[6] = V1Bytes[7] = std::byte{0};
			EXPECT_EQ(Durin::LoadObjectGraphFromMemory(V1Bytes), nullptr);

			auto TrailingBytes = Bytes;
			TrailingBytes.push_back(std::byte{0x7F});
			EXPECT_EQ(Durin::LoadObjectGraphFromMemory(TrailingBytes), nullptr);

			auto InvalidReferenceBytes = Bytes;
			auto ReadUint64 = [&InvalidReferenceBytes](size_t Offset) {
				uint64 Value = 0;
				std::memcpy(&Value, InvalidReferenceBytes.data() + Offset, sizeof(Value));
				return Value;
			};
			size_t Offset = 24 + 16;
			for (int StringIndex = 0; StringIndex < 2; ++StringIndex)
			{
				const uint64 Length = ReadUint64(Offset);
				Offset += sizeof(uint64) + static_cast<size_t>(Length);
			}
			const uint64 PropertySize = ReadUint64(Offset);
			const size_t PropertyEnd = Offset + sizeof(uint64)
				+ static_cast<size_t>(PropertySize);
			ASSERT_GE(PropertySize, 9u);
			ASSERT_EQ(InvalidReferenceBytes[PropertyEnd - 9],
				static_cast<std::byte>(Durin::EArchiveObjectReferenceKind::Internal));
			std::fill(InvalidReferenceBytes.begin() + static_cast<ptrdiff_t>(PropertyEnd - 8),
				InvalidReferenceBytes.begin() + static_cast<ptrdiff_t>(PropertyEnd), std::byte{0});
			EXPECT_EQ(Durin::LoadObjectGraphFromMemory(InvalidReferenceBytes), nullptr);
		}
		Durin::MarkAsGarbage(Owner);
		Durin::MarkAsGarbage(ReferencedObject);
		Durin::MarkAsGarbage(RawVectorReferencedObject);
		Durin::MarkAsGarbage(ObjectPtrVectorReferencedObject);
		Durin::MarkAsGarbage(SerializedNativeReference);
		Durin::MarkAsGarbage(HiddenGCReference);

		Durin::DObject* LoadedRoot = Durin::LoadObjectGraphFromMemory(Bytes);
		ASSERT_NE(LoadedRoot, nullptr);
		EXPECT_EQ(LoadedRoot->GetClass(), DLifecycleReferenceOwnerForTest::StaticClass());
		auto* LoadedOwner = Durin::Cast<DLifecycleReferenceOwnerForTest>(LoadedRoot);
		ASSERT_NE(LoadedOwner, nullptr);
		ASSERT_NE(LoadedOwner->ObjectPtrReference.Get(), nullptr);
		EXPECT_EQ(LoadedOwner->Value, 37);
		EXPECT_TRUE(LoadedOwner->bEnabled);
		EXPECT_EQ(LoadedOwner->Label, "Serialized");
		EXPECT_EQ(LoadedOwner->Reference, nullptr);
		EXPECT_EQ(LoadedOwner->ObjectPtrReference.Get()->GetName(), "SerializedReference");
		EXPECT_EQ(LoadedOwner->WeakReference.Get(), LoadedOwner->ObjectPtrReference.Get());
		EXPECT_EQ(LoadedOwner->WeakExternal.Get(), nullptr);
		ASSERT_EQ(LoadedOwner->WeakReferences.size(), 2u);
		EXPECT_EQ(LoadedOwner->WeakReferences[0].Get(), LoadedOwner->ObjectPtrReference.Get());
		EXPECT_EQ(LoadedOwner->WeakReferences[1].Get(), nullptr);
		EXPECT_EQ(LoadedOwner->WeakMap.at("Internal").Get(), LoadedOwner->ObjectPtrReference.Get());
		EXPECT_EQ(LoadedOwner->WeakMap.at("External").Get(), nullptr);
		ASSERT_EQ(LoadedOwner->RawReferences.size(), 1u);
		EXPECT_EQ(LoadedOwner->RawReferences[0], nullptr);
		ASSERT_EQ(LoadedOwner->ObjectPtrReferences.size(), 1u);
		ASSERT_NE(LoadedOwner->ObjectPtrReferences[0].Get(), nullptr);
		EXPECT_EQ(LoadedOwner->ObjectPtrReferences[0].Get()->GetName(), "SerializedObjectPtrVectorReference");
		ASSERT_EQ(LoadedOwner->Scores.size(), 3u);
		EXPECT_EQ(LoadedOwner->Scores[0], 7);
		EXPECT_EQ(LoadedOwner->Scores[1], 11);
		EXPECT_EQ(LoadedOwner->Scores[2], 42);
		ASSERT_EQ(LoadedOwner->Tags.size(), 2u);
		EXPECT_EQ(LoadedOwner->Tags[0], "Alpha");
		EXPECT_EQ(LoadedOwner->Tags[1], "Beta");
		ASSERT_EQ(LoadedOwner->Modes.size(), 2u);
		EXPECT_EQ(LoadedOwner->Modes[0], EReflectedEnumForTest::A);
		EXPECT_EQ(LoadedOwner->Modes[1], EReflectedEnumForTest::B);
		ASSERT_EQ(LoadedOwner->ScoreGroups.size(), 2u);
		ASSERT_EQ(LoadedOwner->ScoreGroups[0].size(), 2u);
		EXPECT_EQ(LoadedOwner->ScoreGroups[0][0], 1);
		EXPECT_EQ(LoadedOwner->ScoreGroups[0][1], 2);
		ASSERT_EQ(LoadedOwner->ScoreGroups[1].size(), 3u);
		EXPECT_EQ(LoadedOwner->ScoreGroups[1][0], 3);
		EXPECT_EQ(LoadedOwner->ScoreGroups[1][1], 5);
		EXPECT_EQ(LoadedOwner->ScoreGroups[1][2], 8);
		EXPECT_EQ(LoadedOwner->TransientValue, 0);
		EXPECT_EQ(LoadedOwner->NativeScalar, 73);
		EXPECT_EQ(LoadedOwner->NativeStruct.Value, 19);
		EXPECT_EQ(LoadedOwner->NativeStruct.Label, Durin::FName("NativeStructLabel"));
		EXPECT_EQ(LoadedOwner->NativeValues, (std::vector<int32>{2, 4, 8, 16}));
		ASSERT_NE(LoadedOwner->SerializedNativeReference, nullptr);
		EXPECT_EQ(LoadedOwner->SerializedNativeReference->GetName(), "SerializedNativeReference");
		EXPECT_EQ(LoadedOwner->NativeReference, nullptr);
		ASSERT_EQ(LoadedOwner->SerializePurposes.size(), 1u);
		EXPECT_EQ(LoadedOwner->SerializePurposes[0], Durin::EArchivePurpose::ObjectGraph);

		Durin::MarkAsGarbage(LoadedOwner);
	}

	TEST(FCoreDObjectReflectionTests, ObjectGraphDiscoveryFreezesScopeAndPublishesAtomically)
	{
		EnsureDObjectInitialized();
		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(
			nullptr, Durin::FName("FrozenDiscoveryOwner"));
		Durin::DObject* LateReference = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("FrozenDiscoveryLateReference"));
		Owner->EmissionOnlyReference = LateReference;
		Owner->bEmitLateReference = true;

		std::vector<std::byte> Bytes = {std::byte{0xC0}, std::byte{0xDE}};
		EXPECT_FALSE(Durin::SaveObjectGraphToMemory(Owner, Bytes));
		EXPECT_EQ(Bytes, (std::vector<std::byte>{std::byte{0xC0}, std::byte{0xDE}}));
		ASSERT_EQ(Owner->SerializePurposes.size(), 2u);
		EXPECT_EQ(Owner->SerializePurposes[0], Durin::EArchivePurpose::Discovery);
		EXPECT_EQ(Owner->SerializePurposes[1], Durin::EArchivePurpose::ObjectGraph);

		Owner->bEmitLateReference = false;
		Owner->bSkipSuperSerialize = true;
		Owner->SerializePurposes.clear();
		EXPECT_FALSE(Durin::SaveObjectGraphToMemory(Owner, Bytes));
		EXPECT_EQ(Bytes, (std::vector<std::byte>{std::byte{0xC0}, std::byte{0xDE}}));
		ASSERT_EQ(Owner->SerializePurposes.size(), 1u);
		EXPECT_EQ(Owner->SerializePurposes[0], Durin::EArchivePurpose::Discovery);

		Durin::MarkAsGarbage(Owner);
		Durin::MarkAsGarbage(LateReference);
	}

	TEST(FCoreDObjectReflectionTests, DuplicateArchiveRemapsInternalReferencesSharesExternalAndCleansFailures)
	{
		EnsureDObjectInitialized();
		auto* Source = Durin::NewObject<DLifecycleReferenceOwnerForTest>(
			nullptr, Durin::FName("DuplicateArchiveSource"));
		auto* Inner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(
			Source, Durin::FName("DuplicateArchiveInner"));
		Durin::DObject* External = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("DuplicateArchiveExternal"));
		auto* NewOuter = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("DuplicateArchiveNewOuter"));
		Source->NativeScalar = 41;
		Source->SerializedNativeReference = Inner;
		Source->ObjectPtrReference = Inner;
		Source->WeakReference = Inner;
		Source->WeakExternal = External;
		Source->WeakReferences = {Inner, External};
		Source->WeakMap.emplace("Internal", Inner);
		Source->WeakMap.emplace("External", External);
		Source->WeakNested.Reference = Inner;
		Source->WeakNestedArray = {{Inner}, {External}};
		Inner->SerializedNativeReference = External;

		std::unordered_map<Durin::DObject*, Durin::DObject*> Duplicates;
		auto* Duplicate = Durin::Cast<DLifecycleReferenceOwnerForTest>(
			Durin::DuplicateObject(Source, NewOuter, Durin::FName("DuplicateArchiveResult"),
				&Duplicates));
		ASSERT_NE(Duplicate, nullptr);
		ASSERT_TRUE(Duplicates.contains(Inner));
		auto* DuplicateInner = Durin::Cast<DLifecycleReferenceOwnerForTest>(Duplicates[Inner]);
		ASSERT_NE(DuplicateInner, nullptr);
		EXPECT_EQ(Duplicate->NativeScalar, 41);
		EXPECT_EQ(Duplicate->SerializedNativeReference, DuplicateInner);
		EXPECT_EQ(Duplicate->WeakReference.Get(), DuplicateInner);
		EXPECT_EQ(Duplicate->WeakExternal.Get(), nullptr);
		ASSERT_EQ(Duplicate->WeakReferences.size(), 2u);
		EXPECT_EQ(Duplicate->WeakReferences[0].Get(), DuplicateInner);
		EXPECT_EQ(Duplicate->WeakReferences[1].Get(), nullptr);
		EXPECT_EQ(Duplicate->WeakMap.at("Internal").Get(), DuplicateInner);
		EXPECT_EQ(Duplicate->WeakMap.at("External").Get(), nullptr);
		EXPECT_EQ(Duplicate->WeakNested.Reference.Get(), DuplicateInner);
		ASSERT_EQ(Duplicate->WeakNestedArray.size(), 2u);
		EXPECT_EQ(Duplicate->WeakNestedArray[0].Reference.Get(), DuplicateInner);
		EXPECT_EQ(Duplicate->WeakNestedArray[1].Reference.Get(), nullptr);
		EXPECT_EQ(DuplicateInner->SerializedNativeReference, External);
		EXPECT_EQ(Duplicate->PostLoadCallCount, 1);
		EXPECT_EQ(DuplicateInner->PostLoadCallCount, 1);
		EXPECT_EQ(Source->SerializePurposes.back(), Durin::EArchivePurpose::Duplicate);
		EXPECT_EQ(Duplicate->SerializePurposes.back(), Durin::EArchivePurpose::Duplicate);

		auto* FailingSource = Durin::NewObject<DLifecycleReferenceOwnerForTest>(
			nullptr, Durin::FName("DuplicateArchiveFailingSource"));
		FailingSource->bInjectSerializeFailure = true;
		EXPECT_EQ(Durin::DuplicateObject(FailingSource, NewOuter,
			Durin::FName("DuplicateArchiveFailedResult")), nullptr);
		Durin::CollectGarbage();
		auto RemainingInners = Durin::GDObjectArray.GetObjectsWithOuter(NewOuter, Durin::EObjectQueryScope::LiveOnly);
		EXPECT_EQ(std::ranges::count_if(RemainingInners, [](Durin::DObject* Object) {
			return Object && Object->GetName() == "DuplicateArchiveFailedResult";
		}), 0);

		Durin::MarkAsGarbage(Source);
		Durin::MarkAsGarbage(External);
		Durin::MarkAsGarbage(NewOuter);
		Durin::MarkAsGarbage(FailingSource);
	}

	TEST(FCoreDObjectReflectionTests, GenericMemoryArchiveRejectsProcessObjectAddresses)
	{
		EnsureDObjectInitialized();
		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("GenericMemoryArchiveReference"));
		std::vector<std::byte> Bytes;
		Durin::FMemoryWriter Writer(Bytes);
		Writer.SerializeObjectReference(Object);
		ASSERT_NE(Writer.GetFailure(), nullptr);
		EXPECT_EQ(Writer.GetFailure()->Code, Durin::EArchiveFailureCode::UnsupportedCapability);
		EXPECT_TRUE(Bytes.empty());
		Durin::MarkAsGarbage(Object);
	}

	TEST(FCoreDObjectReflectionTests, TObjectPtrWrapsDObjectReferencesWithoutOwnership)
	{
		EnsureDObjectInitialized();

		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectPtrReferencedObject"));
		Durin::TObjectPtr<Durin::DObject> ObjectPtr;

		EXPECT_FALSE(ObjectPtr);
		EXPECT_EQ(ObjectPtr.Get(), nullptr);

		ObjectPtr = ReferencedObject;
		EXPECT_TRUE(ObjectPtr);
		EXPECT_EQ(ObjectPtr.Get(), ReferencedObject);
		EXPECT_FALSE(Durin::IsObjectHandleNull(ObjectPtr.GetHandle()));
		EXPECT_EQ(Durin::ResolveObjectHandle(ObjectPtr.GetHandle()), ReferencedObject);
		EXPECT_EQ(static_cast<Durin::DObject*>(ObjectPtr), ReferencedObject);

		Durin::MarkAsGarbage(ReferencedObject);
		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectPtr);
		EXPECT_EQ(ObjectPtr.Get(), nullptr);
		EXPECT_EQ(Durin::ResolveObjectHandle(ObjectPtr.GetHandle()), nullptr);

		ObjectPtr.Reset();
		EXPECT_FALSE(ObjectPtr);
		EXPECT_EQ(ObjectPtr.Get(), nullptr);
		EXPECT_TRUE(Durin::IsObjectHandleNull(ObjectPtr.GetHandle()));
	}

	TEST(FCoreDObjectReflectionTests, ObjectPtrUsesGenerationHandleInAllBuilds)
	{
		EXPECT_EQ(sizeof(Durin::FObjectHandle), sizeof(Durin::DObject*));
		EXPECT_EQ(sizeof(Durin::FObjectPtr), sizeof(Durin::DObject*));
		EXPECT_EQ(sizeof(Durin::TObjectPtr<Durin::DObject>), sizeof(Durin::DObject*));

		Durin::DObject* Object = nullptr;
		Durin::FObjectHandle NullHandle = Durin::MakeObjectHandle(Object);
		EXPECT_TRUE(Durin::IsObjectHandleNull(NullHandle));
		EXPECT_EQ(Durin::ResolveObjectHandle(NullHandle), nullptr);

		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectHandleStorageReferencedObject"));
		Durin::FObjectHandle ObjectHandle = Durin::MakeObjectHandle(ReferencedObject);
		EXPECT_FALSE(Durin::IsObjectHandleNull(ObjectHandle));
		EXPECT_EQ(Durin::ResolveObjectHandle(ObjectHandle), ReferencedObject);

		Durin::MarkAsGarbage(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPtrResolvesLiveObjectAndResets)
	{
		EnsureDObjectInitialized();

		Durin::TWeakObjectPtr<Durin::DObject> WeakObject;
		EXPECT_EQ(WeakObject.Get(), nullptr);
		EXPECT_FALSE(WeakObject.IsValid());
		EXPECT_TRUE(Durin::IsObjectHandleNull(WeakObject.GetHandle()));

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WeakObjectPtrLiveObject"));
		WeakObject = Object;
		EXPECT_EQ(WeakObject.Get(), Object);
		EXPECT_TRUE(WeakObject.IsValid());
		EXPECT_FALSE(Durin::IsObjectHandleNull(WeakObject.GetHandle()));

		Durin::TWeakObjectPtr<Durin::DObject> Copy = WeakObject;
		EXPECT_EQ(Copy.Get(), Object);

		WeakObject.Reset();
		EXPECT_EQ(WeakObject.Get(), nullptr);
		EXPECT_TRUE(Durin::IsObjectHandleNull(WeakObject.GetHandle()));
		EXPECT_EQ(Copy.Get(), Object);

		Durin::MarkAsGarbage(Object);
		EXPECT_EQ(Copy.Get(), nullptr);
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPtrRejectsGarbageBeforeObjectRemoval)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WeakObjectPtrGarbageObject"));
		Durin::TWeakObjectPtr<Durin::DObject> WeakObject = Object;
		const Durin::FObjectHandle Handle = WeakObject.GetHandle();

		Durin::MarkAsGarbage(Object);
		EXPECT_EQ(Durin::ResolveObjectHandle(Handle), Object);
		EXPECT_EQ(WeakObject.Get(), nullptr);
		EXPECT_FALSE(WeakObject.IsValid());

		Durin::MarkAsGarbage(Object);
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::ResolveObjectHandle(Handle), nullptr);
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPtrUsesGenerationToRejectReusedSlot)
	{
		EnsureDObjectInitialized();
		Durin::CollectGarbage();

		Durin::DObject* First = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WeakObjectPtrFirst"));
		Durin::TWeakObjectPtr<Durin::DObject> WeakFirst = First;
		const Durin::FObjectHandle FirstHandle = WeakFirst.GetHandle();
		Durin::MarkAsGarbage(First);
		Durin::CollectGarbage();

		Durin::DObject* Second = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WeakObjectPtrSecond"));
		Durin::TWeakObjectPtr<Durin::DObject> WeakSecond = Second;
		const Durin::FObjectHandle SecondHandle = WeakSecond.GetHandle();

		EXPECT_EQ(FirstHandle.Index, SecondHandle.Index);
		EXPECT_NE(FirstHandle.Generation, SecondHandle.Generation);
		EXPECT_EQ(WeakFirst.Get(), nullptr);
		EXPECT_EQ(WeakSecond.Get(), Second);

		Durin::MarkAsGarbage(Second);
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPtrRemainsPointerSizedAndTriviallyCopyable)
	{
		EXPECT_EQ(sizeof(Durin::FWeakObjectPtr), sizeof(Durin::FObjectHandle));
		EXPECT_EQ(sizeof(Durin::TWeakObjectPtr<Durin::DObject>), sizeof(Durin::FObjectHandle));
		EXPECT_TRUE(std::is_trivially_copyable_v<Durin::FWeakObjectPtr>);
		EXPECT_TRUE(std::is_trivially_copyable_v<Durin::TWeakObjectPtr<Durin::DObject>>);
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPtrCanBeCarriedAcrossWorkerAndResolvedOnGameThread)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WeakObjectPtrCarriedObject"));
		Durin::TWeakObjectPtr<Durin::DObject> WeakObject = Object;
		Durin::TWeakObjectPtr<Durin::DObject> ReturnedWeak;

		std::thread Worker([WeakObject, &ReturnedWeak]() {
			ReturnedWeak = WeakObject;
		});
		Worker.join();

		EXPECT_EQ(ReturnedWeak.Get(), Object);
		Durin::MarkAsGarbage(Object);
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPtrMayOutliveObjectWhileCarriedByWorker)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("WeakObjectPtrWorkerLifetimeObject"));
		Durin::TWeakObjectPtr<Durin::DObject> WeakObject = Object;
		Durin::TWeakObjectPtr<Durin::DObject> ReturnedWeak;
		std::mutex Mutex;
		std::condition_variable CV;
		bool bWorkerHasCopy = false;
		bool bMayReturnCopy = false;

		std::thread Worker([WeakObject, &ReturnedWeak, &Mutex, &CV, &bWorkerHasCopy, &bMayReturnCopy]() {
			{
				std::unique_lock Lock(Mutex);
				bWorkerHasCopy = true;
				CV.notify_all();
				CV.wait(Lock, [&bMayReturnCopy]() { return bMayReturnCopy; });
			}
			ReturnedWeak = WeakObject;
		});

		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [&bWorkerHasCopy]() { return bWorkerHasCopy; });
		}
		Durin::MarkAsGarbage(Object);
		Durin::CollectGarbage();
		{
			std::lock_guard Lock(Mutex);
			bMayReturnCopy = true;
		}
		CV.notify_all();
		Worker.join();

		EXPECT_EQ(ReturnedWeak.Get(), nullptr);
	}

	TEST(FCoreDObjectReflectionTests, SoftObjectPtrUsesPathIdentityAndWeakLoadedState)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/SoftObjectValue", Path));
		Durin::FObjectPath SoftPath;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(
			"/CoreTests/SoftObjectValue.SoftObjectValue", SoftPath));
		const Durin::FObjectPath ValidSoftPath = SoftPath;
		EXPECT_FALSE(Durin::FObjectPath::TryCreate("/Unknown/InvalidSoftObject", SoftPath));
		EXPECT_EQ(SoftPath, ValidSoftPath);

		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "SoftObjectValue");
		Package->InitializeAssetPackage(Path);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "SoftObjectValue");

		Durin::FSoftObjectPtr Reference(SoftPath);
		EXPECT_FALSE(Reference.IsNull());
		EXPECT_FALSE(Reference.IsLoaded());
		EXPECT_EQ(Reference.GetState(), Durin::ESoftObjectPtrState::Pending);
		ASSERT_TRUE(Reference.TrySetObject(Asset));
		EXPECT_EQ(Reference.Get(), Asset);
		EXPECT_EQ(Reference.GetState(), Durin::ESoftObjectPtrState::Valid);
		EXPECT_EQ(Reference.GetPath(), SoftPath);

		Durin::FSoftObjectPtr PathOnly(SoftPath);
		EXPECT_EQ(PathOnly.GetState(), Durin::ESoftObjectPtrState::Pending);
		EXPECT_EQ(Reference, PathOnly);
		EXPECT_EQ(std::hash<Durin::FSoftObjectPtr>{}(Reference), std::hash<Durin::FSoftObjectPtr>{}(PathOnly));

		Durin::FSoftObjectPtr Copy = Reference;
		EXPECT_EQ(Copy.Get(), Asset);
		Durin::FSoftObjectPtr Moved = std::move(Copy);
		EXPECT_TRUE(Copy.IsNull());
		EXPECT_EQ(Moved.Get(), Asset);

		Durin::FSoftObjectPtr WorkerCopy;
		std::thread Worker([PathOnly, &WorkerCopy]() {
			WorkerCopy = PathOnly;
		});
		Worker.join();
		EXPECT_EQ(WorkerCopy, PathOnly);
		EXPECT_FALSE(WorkerCopy.IsLoaded());

		Reference.SetPath(SoftPath);
		EXPECT_FALSE(Reference.IsLoaded());
		EXPECT_EQ(Reference.GetState(), Durin::ESoftObjectPtrState::Pending);
		EXPECT_TRUE(Reference.TrySetLoadedObject(Asset));
		EXPECT_EQ(Reference.Get(), Asset);
		Durin::InvalidateSoftObjectCaches();
		EXPECT_EQ(Reference.GetState(), Durin::ESoftObjectPtrState::Stale);
		EXPECT_EQ(Reference.Get(), nullptr);
		ASSERT_TRUE(Reference.TrySetLoadedObject(Asset));
		EXPECT_EQ(Reference.GetState(), Durin::ESoftObjectPtrState::Valid);

		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
		Durin::CollectGarbage();
		EXPECT_EQ(Reference.Get(), nullptr);
		EXPECT_EQ(Reference.GetState(), Durin::ESoftObjectPtrState::Stale);
		EXPECT_EQ(Moved.Get(), nullptr);
		EXPECT_EQ(Reference.GetPath(), SoftPath);
	}

	TEST(FCoreDObjectReflectionTests, SoftObjectPtrRejectsInvalidObjectsWithoutMutation)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/SoftObjectValidation", Path));

		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "SoftObjectValidation");
		Package->InitializeAssetPackage(Path);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "SoftObjectValidation");
		Durin::DObject* Inner = Durin::NewObject<Durin::DObject>(Asset, "Inner");

		Durin::FSoftObjectPtr Reference;
		ASSERT_TRUE(Reference.TrySetObject(Asset));
		const Durin::FObjectPath OriginalPath = Reference.GetPath();
		std::string Error;
		EXPECT_FALSE(Reference.TrySetObject(Package, nullptr, &Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_TRUE(Reference.TrySetObject(Inner, nullptr, &Error));
		EXPECT_EQ(Reference.GetPath().ToString(),
			"/CoreTests/SoftObjectValidation.SoftObjectValidation:Inner");
		ASSERT_TRUE(Reference.TrySetObject(Asset));
		EXPECT_FALSE(Reference.TrySetObject(Asset, Durin::DPackage::StaticClass(), &Error));
		Durin::DObject* Unpackaged = Durin::NewObject<Durin::DObject>(nullptr, "UnpackagedSoftObject");
		EXPECT_FALSE(Reference.TrySetObject(Unpackaged, nullptr, &Error));

		auto* TransientType = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FTransientSoftObjectForTest"),
			Durin::FName("FTransientSoftObjectForTest"),
			1,
			1,
			Durin::EObjectFlags::Transient
		);
		EXPECT_FALSE(Reference.TrySetObject(TransientType, nullptr, &Error));
		delete TransientType;

		EXPECT_EQ(Reference.GetPath(), OriginalPath);
		EXPECT_EQ(Reference.Get(), Asset);

		Durin::TSoftObjectPtr<Durin::DPackage> WrongType(OriginalPath);
		EXPECT_FALSE(WrongType.GetBase().TrySetObject(
			Asset, Durin::DPackage::StaticClass(), &Error));
		EXPECT_EQ(WrongType.GetPath(), OriginalPath);
		EXPECT_FALSE(WrongType.IsLoaded());

		EXPECT_TRUE(Reference.TrySetObject(nullptr));
		EXPECT_TRUE(Reference.IsNull());
		EXPECT_EQ(Reference.Get(), nullptr);

		Durin::MarkAsGarbage(Unpackaged);
		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, SoftObjectPropertySupportsTypedDirectNestedAndDetachedValues)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::DStruct* OwnerStruct = GetSoftObjectPropertyOwner();
		auto* Direct = static_cast<Durin::FSoftObjectProperty*>(OwnerStruct->FindPropertyByName("Direct", false));
		auto* Fixed = static_cast<Durin::FSoftObjectProperty*>(OwnerStruct->FindPropertyByName("Fixed", false));
		auto* Accessed = static_cast<Durin::FSoftObjectProperty*>(OwnerStruct->FindPropertyByName("Accessed", false));
		auto* Array = static_cast<Durin::FArrayProperty*>(OwnerStruct->FindPropertyByName("Array", false));
		auto* Map = static_cast<Durin::FMapProperty*>(OwnerStruct->FindPropertyByName("Map", false));
		ASSERT_NE(Direct, nullptr);
		ASSERT_NE(Fixed, nullptr);
		ASSERT_NE(Accessed, nullptr);
		ASSERT_NE(Array, nullptr);
		ASSERT_NE(Map, nullptr);
		EXPECT_EQ(Direct->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::SoftObject);
		EXPECT_EQ(Direct->GetExpectedClass(), Durin::DObject::StaticClass());
		EXPECT_TRUE(Direct->ClassPrivate->IsChildOf(Durin::FSoftObjectProperty::StaticClass()));
		EXPECT_EQ(Direct->GetMetaData(Durin::FName("Category")), "SoftObject");
		EXPECT_FALSE(Direct->HasValueAccessors());
		EXPECT_TRUE(Accessed->HasValueAccessors());
		EXPECT_EQ(Accessed->GetOffset(), 0);

		Durin::FPackagePath AssetPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/SoftObjectProperty", AssetPath));
		Durin::FObjectPath SoftPath;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(
			"/CoreTests/SoftObjectProperty.SoftObjectProperty", SoftPath));
		FSoftObjectPropertyOwnerForTest Owner;
		Owner.Direct.SetPath(SoftPath);
		Owner.Fixed[1].SetPath(SoftPath);
		Owner.Accessed.SetPath(SoftPath);
		ASSERT_EQ(Direct->GetSoftObjectPtr(&Owner), &Owner.Direct.GetBase());
		ASSERT_EQ(Fixed->GetSoftObjectPtr(&Owner, 1), &Owner.Fixed[1].GetBase());
		ASSERT_EQ(Accessed->GetSoftObjectPtr(&Owner), &Owner.Accessed.GetBase());
		EXPECT_EQ(Direct->GetSoftObjectPtr(&Owner)->GetPath(), SoftPath);

		Owner.Array.emplace_back(SoftPath);
		auto* ArrayInner = static_cast<Durin::FSoftObjectProperty*>(Array->GetInner());
		ASSERT_NE(ArrayInner, nullptr);
		ASSERT_NE(ArrayInner->GetSoftObjectPtr(Array->GetElementPtr(&Owner, 0)), nullptr);
		EXPECT_EQ(
			ArrayInner->GetSoftObjectPtr(Array->GetElementPtr(&Owner, 0))->GetPath(),
			SoftPath
		);

		Owner.Map.emplace("Target", Durin::TSoftObjectPtr<Durin::DObject>(SoftPath));
		auto* MapValue = static_cast<Durin::FSoftObjectProperty*>(Map->GetValueProp());
		const void* RawMapValue = nullptr;
		const std::string Key = "Target";
		ASSERT_EQ(Map->FindValue(&Owner, &Key, &RawMapValue), Durin::EContainerOpResult::Success);
		ASSERT_NE(MapValue, nullptr);
		ASSERT_NE(MapValue->GetSoftObjectPtr(RawMapValue), nullptr);
		EXPECT_EQ(MapValue->GetSoftObjectPtr(RawMapValue)->GetPath(), SoftPath);

		Durin::FReflectedValueStorage Detached;
		ASSERT_TRUE(Detached.CopyConstruct(Direct, Direct->GetValuePtr(&Owner)));
		auto* DetachedValue = static_cast<Durin::TSoftObjectPtr<Durin::DObject>*>(Detached.GetValue());
		ASSERT_NE(DetachedValue, nullptr);
		EXPECT_EQ(DetachedValue->GetPath(), SoftPath);
		Durin::FPackagePath ReplacementPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/CoreTests/SoftObjectPropertyReplacement", ReplacementPath));
		Durin::FObjectPath ReplacementObjectPath;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(
			"/CoreTests/SoftObjectPropertyReplacement.SoftObjectPropertyReplacement",
			ReplacementObjectPath));
		Durin::TSoftObjectPtr<Durin::DObject> Replacement(ReplacementObjectPath);
		ASSERT_TRUE(Detached.CopyAssign(&Replacement));
		EXPECT_EQ(DetachedValue->GetPath(), Replacement.GetPath());

		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "SoftObjectProperty");
		Package->InitializeAssetPackage(AssetPath);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "SoftObjectProperty");
		FSoftObjectPropertyOwnerForTest CachedOwner;
		ASSERT_TRUE(CachedOwner.Direct.TrySetObject(Asset));
		EXPECT_TRUE(Durin::ArePropertyValuesIdentical(Direct, &Owner, 0, &CachedOwner, 0));
		Durin::FPropertyValueSnapshot PathOnlySnapshot;
		Durin::FPropertyValueSnapshot CachedSnapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(Direct, &Owner, 0, PathOnlySnapshot));
		ASSERT_TRUE(Durin::CapturePropertyValue(Direct, &CachedOwner, 0, CachedSnapshot));
		EXPECT_EQ(PathOnlySnapshot, CachedSnapshot);
		EXPECT_TRUE(PathOnlySnapshot.GetReferencedObjects().empty());
		Owner.Direct = Replacement;
		ASSERT_TRUE(Durin::RestorePropertyValue(Direct, &Owner, 0, PathOnlySnapshot));
		EXPECT_EQ(Owner.Direct.GetPath(), SoftPath);

		Durin::FPropertyValueSnapshot ArraySnapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(Array, &Owner, 0, ArraySnapshot));
		Owner.Array.clear();
		ASSERT_TRUE(Durin::RestorePropertyValue(Array, &Owner, 0, ArraySnapshot));
		ASSERT_EQ(Owner.Array.size(), 1u);
		EXPECT_EQ(Owner.Array[0].GetPath(), SoftPath);

		Durin::FPropertyValueSnapshot MapSnapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(Map, &Owner, 0, MapSnapshot));
		Owner.Map.clear();
		ASSERT_TRUE(Durin::RestorePropertyValue(Map, &Owner, 0, MapSnapshot));
		ASSERT_EQ(Owner.Map.size(), 1u);
		EXPECT_EQ(Owner.Map.at("Target").GetPath(), SoftPath);

		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
		Durin::CollectGarbage();
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPropertySupportsTransientTypedContainersWithoutRootingTargets)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* OwnerStruct = GetWeakObjectPropertyOwner();
		auto* Direct = static_cast<Durin::FWeakObjectProperty*>(OwnerStruct->FindPropertyByName("Direct", false));
		auto* Fixed = static_cast<Durin::FWeakObjectProperty*>(OwnerStruct->FindPropertyByName("Fixed", false));
		auto* Array = static_cast<Durin::FArrayProperty*>(OwnerStruct->FindPropertyByName("Array", false));
		auto* Map = static_cast<Durin::FMapProperty*>(OwnerStruct->FindPropertyByName("Map", false));
		ASSERT_NE(Direct, nullptr);
		ASSERT_NE(Fixed, nullptr);
		ASSERT_NE(Array, nullptr);
		ASSERT_NE(Map, nullptr);
		EXPECT_EQ(Direct->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::WeakObject);
		EXPECT_EQ(Direct->GetExpectedClass(), Durin::DObject::StaticClass());
		EXPECT_TRUE(Direct->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));

		Durin::DObject* Target = Durin::NewObject<Durin::DObject>(nullptr, "WeakPropertyTarget");
		FWeakObjectPropertyOwnerForTest Owner;
		Owner.Direct = Target;
		Owner.Fixed[1] = Target;
		Owner.Array.emplace_back(Target);
		Owner.Map.emplace("Target", Target);
		EXPECT_EQ(Direct->GetWeakObjectPtr(&Owner)->Get(), Target);
		EXPECT_EQ(Fixed->GetWeakObjectPtr(&Owner, 1)->Get(), Target);
		EXPECT_EQ(static_cast<Durin::FWeakObjectProperty*>(Array->GetInner())->GetExpectedClass(), Durin::DObject::StaticClass());
		EXPECT_EQ(static_cast<Durin::FWeakObjectProperty*>(Map->GetValueProp())->GetExpectedClass(), Durin::DObject::StaticClass());

		Durin::FPropertyValueSnapshot Snapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(Direct, &Owner, 0, Snapshot));
		EXPECT_TRUE(Snapshot.GetReferencedObjects().empty());
		Owner.Direct.Reset();
		ASSERT_TRUE(Durin::RestorePropertyValue(Direct, &Owner, 0, Snapshot));
		EXPECT_EQ(Owner.Direct.Get(), Target);

		Durin::MarkAsGarbage(Target);
		Durin::CollectGarbage();
		EXPECT_EQ(Owner.Direct.Get(), nullptr);
		FWeakObjectPropertyOwnerForTest NullOwner;
		EXPECT_TRUE(Durin::ArePropertyValuesIdentical(Direct, &Owner, 0, &NullOwner, 0));
	}

	TEST(FCoreDObjectReflectionTests, WeakObjectPropertySchemaIgnoresDirectArrayAndMapTargets)
	{
		EnsureDObjectInitialized();
		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, "WeakSchemaOwner");
		Durin::DObject* Target = Durin::NewObject<Durin::DObject>(nullptr, "WeakSchemaTarget");
		Owner->WeakReference = Target;
		Owner->WeakReferences.emplace_back(Target);
		Owner->WeakMap.emplace("Target", Target);
		Durin::AddToRoot(Owner);

		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Target));
		EXPECT_EQ(Owner->WeakReference.Get(), nullptr);
		ASSERT_EQ(Owner->WeakReferences.size(), 1u);
		EXPECT_EQ(Owner->WeakReferences[0].Get(), nullptr);
		ASSERT_EQ(Owner->WeakMap.size(), 1u);
		EXPECT_EQ(Owner->WeakMap.at("Target").Get(), nullptr);

		Durin::RemoveFromRoot(Owner);
		Durin::CollectGarbage();
	}

#if DO_CHECK
	TEST(FCoreDObjectReflectionTests, SoftObjectPtrLoadedInspectionRequiresGameThread)
	{
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				Durin::FSoftObjectPtr Reference;
				std::thread Worker([&Reference]() {
					(void)Reference.Get();
				});
				Worker.join();
			}()),
			""
		);
	}
#endif

	TEST(FCoreDObjectReflectionTests, ReusedObjectSlotInvalidatesOldHandleGeneration)
	{
		EnsureDObjectInitialized();
		Durin::CollectGarbage();
		Durin::DObject* First = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectHandleFirst"));
		const Durin::FObjectHandle FirstHandle = Durin::MakeObjectHandle(First);
		Durin::MarkAsGarbage(First);
		Durin::CollectGarbage();

		Durin::DObject* Second = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectHandleSecond"));
		const Durin::FObjectHandle SecondHandle = Durin::MakeObjectHandle(Second);
		EXPECT_EQ(FirstHandle.Index, SecondHandle.Index);
		EXPECT_NE(FirstHandle.Generation, SecondHandle.Generation);
		EXPECT_EQ(Durin::ResolveObjectHandle(FirstHandle), nullptr);
		EXPECT_EQ(Durin::ResolveObjectHandle(SecondHandle), Second);
		Durin::MarkAsGarbage(Second);
	}

	TEST(FCoreDObjectReflectionTests, ObjectAndGarbageCountsTrackLiveObjectsWithoutCompaction)
	{
		EnsureDObjectInitialized();
		Durin::CollectGarbage();
		const uint64 InitialCount = Durin::GDObjectArray.GetNum();
		Durin::DObject* A = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCCountA"));
		Durin::DObject* B = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCCountB"));
		EXPECT_EQ(Durin::GDObjectArray.GetNum(), InitialCount + 2);
		EXPECT_EQ(Durin::GetGarbageObjectCount(), 0u);

		Durin::MarkAsGarbage(A);
		Durin::MarkAsGarbage(A);
		EXPECT_EQ(Durin::GetGarbageObjectCount(), 1u);
		Durin::MarkAsGarbage(A);
		EXPECT_EQ(Durin::GDObjectArray.GetNum(), InitialCount + 2);
		EXPECT_EQ(Durin::GetGarbageObjectCount(), 1u);
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::GDObjectArray.GetNum(), InitialCount);
		EXPECT_EQ(Durin::GetGarbageObjectCount(), 0u);

		EXPECT_FALSE(ObjectArrayContains(B));
	}

	TEST(FCoreDObjectReflectionTests, ConstructDEnumCreatesRuntimeEnumMetadata)
	{
		static const Durin::DurinCodeGen::FEnumValueParams Values[] = {
			{"A", 0, "Alpha"},
			{"B", 4, nullptr},
			{"AliasB", 4, "Second B"},
		};
		static const Durin::DurinCodeGen::FEnumParams EnumParams = {
			&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister,
			"EReflectedEnumForTest",
			"EReflectedEnumForTest",
			"Reflected Enum For Test",
			true,
			Durin::DurinCodeGen::EEnumUnderlyingType::UInt8,
			static_cast<uint16>(sizeof(EReflectedEnumForTest)),
			Values,
			3
		};

		Durin::DEnum* Enum = Durin::DurinCodeGen::ConstructDEnum(EnumParams);

		ASSERT_NE(Enum, nullptr);
		EXPECT_EQ(Enum->GetClass(), Durin::DEnum::StaticClass());
		EXPECT_TRUE(Enum->IsA(Durin::DType::StaticClass()));
		EXPECT_TRUE(Enum->IsScoped());
		EXPECT_EQ(Enum->GetUnderlyingType(), Durin::DurinCodeGen::EEnumUnderlyingType::UInt8);
		EXPECT_EQ(Enum->GetDisplayName(), "Reflected Enum For Test");
		ASSERT_EQ(Enum->GetValues().size(), 3u);
		EXPECT_EQ(Enum->GetValues()[0].DisplayName, "Alpha");
		EXPECT_EQ(Enum->GetValues()[1].DisplayName, "B");
		EXPECT_EQ(Durin::FindEnumByQualifiedName("EReflectedEnumForTest"), Enum);

		uint64 Value = std::numeric_limits<uint64>::max();
		EXPECT_TRUE(Enum->FindValueByName(Durin::FName("A"), Value));
		EXPECT_EQ(Value, 0);

		Durin::FName Name;
		EXPECT_TRUE(Enum->FindNameByValue(4, Name));
		EXPECT_EQ(Name.ToString(), "B");
		ASSERT_NE(Enum->FindValueRecordByName(Durin::FName("AliasB")), nullptr);
		EXPECT_EQ(Enum->FindValueRecordByName(Durin::FName("AliasB"))->DisplayName, "Second B");
		ASSERT_NE(Enum->FindValueRecordByValue(4), nullptr);
		EXPECT_EQ(Enum->FindValueRecordByValue(4)->Name.ToString(), "B");
	}

	TEST(FCoreDObjectReflectionTests, ConstructDClassAttachesEnumPropertyMetadata)
	{
		static const Durin::DurinCodeGen::FEnumPropertyParams ModePropertyParams = {
			"Mode",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedEnumPropertyOwnerForTest, Mode),
			&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[] = {
			&ModePropertyParams
		};
		static const Durin::DurinCodeGen::FClassParams ClassParams = {
			&Z_Construct_DClass_FReflectedEnumPropertyOwnerForTest_NoRegister,
			"FReflectedEnumPropertyOwnerForTest",
			"FReflectedEnumPropertyOwnerForTest",
			PropertyParams,
			1
		};

		Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(ClassParams);
		auto* EnumProperty = static_cast<Durin::FEnumProperty*>(Class->FindPropertyByName("Mode"));

		ASSERT_NE(EnumProperty, nullptr);
		ASSERT_NE(EnumProperty->GetEnum(), nullptr);
		EXPECT_EQ(EnumProperty->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Enum);
		EXPECT_EQ(EnumProperty->GetEnum()->GetUnderlyingType(), Durin::DurinCodeGen::EEnumUnderlyingType::UInt8);

		FReflectedEnumPropertyOwnerForTest Instance;
		*EnumProperty->GetEnumValuePtr<EReflectedEnumForTest>(&Instance) = EReflectedEnumForTest::B;
		EXPECT_EQ(Instance.Mode, EReflectedEnumForTest::B);
	}

	TEST(FCoreDObjectReflectionTests, EnumPropertyUsesUnsigned64BitValueChannel)
	{
		auto SignedEnum = std::make_unique<Durin::DEnum>(
			Durin::EC_StaticConstructor,
			Durin::FName("ESignedEnumValueForTest"),
			Durin::FName("ESignedEnumValueForTest"),
			Durin::FName("ESignedEnumValueForTest"),
			"",
			true,
			Durin::DurinCodeGen::EEnumUnderlyingType::Int8,
			static_cast<uint16>(sizeof(ESignedEnumValueForTest)),
			std::vector<Durin::FEnumValue>{
				{Durin::FName("Negative"), std::numeric_limits<uint64>::max()},
				{Durin::FName("Positive"), 1}
			},
			Durin::EObjectFlags::NoFlags
		);
		auto UnsignedEnum = std::make_unique<Durin::DEnum>(
			Durin::EC_StaticConstructor,
			Durin::FName("EUnsignedEnumValueForTest"),
			Durin::FName("EUnsignedEnumValueForTest"),
			Durin::FName("EUnsignedEnumValueForTest"),
			"",
			true,
			Durin::DurinCodeGen::EEnumUnderlyingType::UInt64,
			static_cast<uint16>(sizeof(EUnsignedEnumValueForTest)),
			std::vector<Durin::FEnumValue>{
				{Durin::FName("Low"), 0},
				{Durin::FName("High"), std::numeric_limits<uint64>::max()}
			},
			Durin::EObjectFlags::NoFlags
		);

		Durin::FEnumProperty SignedProperty(
			{}, Durin::FName("Signed"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(FWideEnumPropertyOwnerForTest, Signed),
			static_cast<uint16>(sizeof(ESignedEnumValueForTest)), Durin::DurinCodeGen::EPropertyGenFlags::Enum, nullptr, SignedEnum.get()
		);
		Durin::FEnumProperty UnsignedProperty(
			{}, Durin::FName("Unsigned"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(FWideEnumPropertyOwnerForTest, Unsigned),
			static_cast<uint16>(sizeof(EUnsignedEnumValueForTest)), Durin::DurinCodeGen::EPropertyGenFlags::Enum, nullptr, UnsignedEnum.get()
		);
		FWideEnumPropertyOwnerForTest Instance;
		const uint64 MaxValue = std::numeric_limits<uint64>::max();

		EXPECT_EQ(SignedProperty.GetValueAsUInt64(&Instance), MaxValue);
		EXPECT_EQ(UnsignedProperty.GetValueAsUInt64(&Instance), MaxValue);
		Durin::FName Name;
		EXPECT_TRUE(SignedEnum->FindNameByValue(MaxValue, Name));
		EXPECT_EQ(Name.ToString(), "Negative");
		EXPECT_TRUE(UnsignedEnum->FindNameByValue(MaxValue, Name));
		EXPECT_EQ(Name.ToString(), "High");

		SignedProperty.SetValueFromUInt64(&Instance, 1);
		UnsignedProperty.SetValueFromUInt64(&Instance, 0);
		EXPECT_EQ(Instance.Signed, ESignedEnumValueForTest::Positive);
		EXPECT_EQ(Instance.Unsigned, EUnsignedEnumValueForTest::Low);
	}

	TEST(FCoreDObjectReflectionTests, ConstructDClassAttachesGeneratedPropertiesToStructBase)
	{
		EnsureDObjectInitialized();
		static const Durin::DurinCodeGen::FInt32PropertyParams ValuePropertyParams = {
			"Value",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, Value)
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPropertyParams =
			Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
				"ObjectValue",
				Durin::EPropertyFlags::Edit,
				1,
				STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, ObjectValue),
				&Durin::DObject::StaticClass
			);
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrPropertyParams =
			Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
				"ObjectPtrValue",
				Durin::EPropertyFlags::None,
				1,
				STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, ObjectPtrValue),
				&Durin::DObject::StaticClass
			);
		static const Durin::DurinCodeGen::FStringPropertyParams StringPropertyParams = {
			"StringValue",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, StringValue)
		};
		static const Durin::DurinCodeGen::FNamePropertyParams NamePropertyParams = {
			"NameValue",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, NameValue)
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectArrayInnerPropertyParams =
			Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
				"ObjectArray_Inner",
				Durin::EPropertyFlags::None,
				1,
				0,
				&Durin::DObject::StaticClass
			);
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectArrayPropertyParams = {
			"ObjectArray",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, ObjectArray),
			&ObjectArrayInnerPropertyParams,
			&GVectorPropertyHelper<Durin::DObject*>
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrArrayInnerPropertyParams =
			Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
				"ObjectPtrArray_Inner",
				Durin::EPropertyFlags::None,
				1,
				0,
				&Durin::DObject::StaticClass
			);
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectPtrArrayPropertyParams = {
			"ObjectPtrArray",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, ObjectPtrArray),
			&ObjectPtrArrayInnerPropertyParams,
			&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
		};
		static const Durin::DurinCodeGen::FStringPropertyParams StringToIntKeyPropertyParams = {
			"StringToInt_Key",
			Durin::EPropertyFlags::None,
			1,
			0
		};
		static const Durin::DurinCodeGen::FInt32PropertyParams StringToIntValuePropertyParams = {
			"StringToInt_Value",
			Durin::EPropertyFlags::None,
			1,
			0
		};
		static const Durin::DurinCodeGen::FMapPropertyParams StringToIntPropertyParams = {
			"StringToInt",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, StringToInt),
			&StringToIntKeyPropertyParams,
			&StringToIntValuePropertyParams,
			&GMapPropertyHelper<std::string, int32>
		};
		static const Durin::DurinCodeGen::FInt32PropertyParams NestedScoresInnerInnerPropertyParams = {
			"NestedScores_Inner_Inner",
			Durin::EPropertyFlags::None,
			1,
			0
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresInnerPropertyParams = {
			"NestedScores_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			&NestedScoresInnerInnerPropertyParams,
			&GVectorPropertyHelper<int32>
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresPropertyParams = {
			"NestedScores",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, NestedScores),
			&NestedScoresInnerPropertyParams,
			&GVectorPropertyHelper<std::vector<int32>>
		};
		static const Durin::DurinCodeGen::FStringPropertyParams ObjectListsKeyPropertyParams = {
			"ObjectLists_Key",
			Durin::EPropertyFlags::None,
			1,
			0
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectListsValueInnerPropertyParams =
			Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
				"ObjectLists_Value_Inner",
				Durin::EPropertyFlags::None,
				1,
				0,
				&Durin::DObject::StaticClass
			);
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectListsValuePropertyParams = {
			"ObjectLists_Value",
			Durin::EPropertyFlags::None,
			1,
			0,
			&ObjectListsValueInnerPropertyParams,
			&GVectorPropertyHelper<Durin::DObject*>
		};
		static const Durin::DurinCodeGen::FMapPropertyParams ObjectListsPropertyParams = {
			"ObjectLists",
			Durin::EPropertyFlags::None,
			1,
			STRUCT_OFFSET_UINT16(FReflectedPropertyOwnerForTest, ObjectLists),
			&ObjectListsKeyPropertyParams,
			&ObjectListsValuePropertyParams,
			&GMapPropertyHelper<std::string, std::vector<Durin::DObject*>>
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[] = {
			&ValuePropertyParams,
			&ObjectPropertyParams,
			&ObjectPtrPropertyParams,
			&StringPropertyParams,
			&NamePropertyParams,
			&ObjectArrayPropertyParams,
			&ObjectPtrArrayPropertyParams,
			&StringToIntPropertyParams,
			&NestedScoresPropertyParams,
			&ObjectListsPropertyParams
		};
		static const Durin::DurinCodeGen::FClassParams ClassParams = {
			&Z_Construct_DClass_FReflectedPropertyOwnerForTest_NoRegister,
			"FReflectedPropertyOwnerForTest",
			"FReflectedPropertyOwnerForTest",
			PropertyParams,
			10
		};

		Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(ClassParams);

		ASSERT_NE(Class, nullptr);
		ASSERT_NE(Class->ChildProperties, nullptr);
		EXPECT_EQ(Class->ChildProperties->NamePrivate.ToString(), "Value");
		EXPECT_NE(Class->ChildProperties->Next, nullptr);
		EXPECT_EQ(Class->PropertiesSize, sizeof(FReflectedPropertyOwnerForTest));
		EXPECT_EQ(Class->MinAlignment, alignof(FReflectedPropertyOwnerForTest));

		auto* ValueProperty = Class->FindPropertyByName("Value");
		ASSERT_NE(ValueProperty, nullptr);
		EXPECT_EQ(ValueProperty->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Int32);
		EXPECT_EQ(ValueProperty->GetElementSize(), sizeof(int32));
		EXPECT_TRUE(ValueProperty->ClassPrivate->IsChildOf(Durin::FNumericProperty::StaticClass()));

		FReflectedPropertyOwnerForTest Instance;
		*ValueProperty->ContainerPtrToValuePtr<int32>(&Instance) = 42;
		EXPECT_EQ(Instance.Value, 42);

		auto* ObjectProperty = static_cast<Durin::FObjectProperty*>(Class->FindPropertyByName("ObjectValue"));
		ASSERT_NE(ObjectProperty, nullptr);
		EXPECT_EQ(ObjectProperty->GetReferencedClass(), Durin::DObject::StaticClass());
		EXPECT_TRUE(ObjectProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::Edit));

		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PropertyReferencedObject"));
		Instance.ObjectValue = ReferencedObject;
		EXPECT_EQ(ObjectProperty->GetObjectPropertyValue(&Instance), ReferencedObject);
		Instance.ObjectValue = nullptr;
		ObjectProperty->SetObjectPropertyValue(&Instance, ReferencedObject);
		EXPECT_EQ(Instance.ObjectValue, ReferencedObject);

		auto* ObjectPtrProperty = static_cast<Durin::FObjectProperty*>(Class->FindPropertyByName("ObjectPtrValue"));
		ASSERT_NE(ObjectPtrProperty, nullptr);
		EXPECT_TRUE(ObjectPtrProperty->IsObjectPtrWrapper());
		Instance.ObjectPtrValue = ReferencedObject;
		EXPECT_EQ(ObjectPtrProperty->GetObjectPropertyValue(&Instance), ReferencedObject);
		Instance.ObjectPtrValue.Reset();
		ObjectPtrProperty->SetObjectPropertyValue(&Instance, ReferencedObject);
		EXPECT_EQ(Instance.ObjectPtrValue.Get(), ReferencedObject);

		auto* StringProperty = static_cast<Durin::FStringProperty*>(Class->FindPropertyByName("StringValue"));
		ASSERT_NE(StringProperty, nullptr);
		*StringProperty->GetStringValuePtr(&Instance) = "Durin";
		EXPECT_EQ(Instance.StringValue, "Durin");

		auto* NameProperty = static_cast<Durin::FNameProperty*>(Class->FindPropertyByName("NameValue"));
		ASSERT_NE(NameProperty, nullptr);
		EXPECT_TRUE(NameProperty->ClassPrivate->IsChildOf(Durin::FNameProperty::StaticClass()));
		*NameProperty->GetNameValuePtr(&Instance) = Durin::FName("ReflectedName_7");
		EXPECT_EQ(Instance.NameValue.ToString(), "ReflectedName_7");

		auto* ArrayProperty = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("ObjectArray"));
		ASSERT_NE(ArrayProperty, nullptr);
		ASSERT_NE(ArrayProperty->GetInner(), nullptr);
		EXPECT_TRUE(ArrayProperty->HasArrayOps());
		EXPECT_EQ(ArrayProperty->GetContainerPtr(&Instance), &Instance.ObjectArray);
		EXPECT_EQ(ArrayProperty->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Object);
		EXPECT_EQ(ArrayProperty->GetInner()->GetReferencedClass(), Durin::DObject::StaticClass());
		EXPECT_EQ(Class->FindPropertyByName("ObjectArray_Inner"), nullptr);
		Instance.ObjectArray.push_back(ReferencedObject);
		ASSERT_EQ(ArrayProperty->Num(&Instance), 1u);
		EXPECT_EQ(*static_cast<Durin::DObject* const*>(ArrayProperty->GetElementPtr(&Instance, 0)), ReferencedObject);

		auto* ObjectPtrArrayProperty = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("ObjectPtrArray"));
		ASSERT_NE(ObjectPtrArrayProperty, nullptr);
		ASSERT_NE(ObjectPtrArrayProperty->GetInner(), nullptr);
		EXPECT_TRUE(ObjectPtrArrayProperty->HasArrayOps());
		EXPECT_TRUE(ObjectPtrArrayProperty->GetInner()->IsObjectPtrWrapper());
		EXPECT_EQ(ObjectPtrArrayProperty->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Object);
		EXPECT_EQ(ObjectPtrArrayProperty->GetInner()->GetReferencedClass(), Durin::DObject::StaticClass());
		ObjectPtrArrayProperty->Resize(&Instance, 1);
		static_cast<Durin::TObjectPtr<Durin::DObject>*>(ObjectPtrArrayProperty->GetMutableElementPtr(&Instance, 0))->operator=(ReferencedObject);
		EXPECT_EQ(Instance.ObjectPtrArray[0].Get(), ReferencedObject);

		auto* MapProperty = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("StringToInt"));
		ASSERT_NE(MapProperty, nullptr);
		ASSERT_NE(MapProperty->GetKeyProp(), nullptr);
		ASSERT_NE(MapProperty->GetValueProp(), nullptr);
		EXPECT_EQ(MapProperty->GetContainerPtr(&Instance), &Instance.StringToInt);
		EXPECT_EQ(MapProperty->GetKeyProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::String);
		EXPECT_EQ(MapProperty->GetValueProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Int32);
		EXPECT_EQ(Class->FindPropertyByName("StringToInt_Key"), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("StringToInt_Value"), nullptr);

		auto* NestedArrayProperty = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("NestedScores"));
		ASSERT_NE(NestedArrayProperty, nullptr);
		auto* NestedArrayInner = static_cast<Durin::FArrayProperty*>(NestedArrayProperty->GetInner());
		ASSERT_NE(NestedArrayInner, nullptr);
		ASSERT_NE(NestedArrayInner->GetInner(), nullptr);
		EXPECT_TRUE(NestedArrayProperty->HasArrayOps());
		EXPECT_TRUE(NestedArrayInner->HasArrayOps());
		EXPECT_EQ(NestedArrayInner->GetOwnerProperty(), NestedArrayProperty);
		EXPECT_EQ(NestedArrayInner->GetInner()->GetOwnerProperty(), NestedArrayInner);
		EXPECT_EQ(NestedArrayInner->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Int32);
		EXPECT_EQ(Class->FindPropertyByName("NestedScores_Inner"), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("NestedScores_Inner_Inner"), nullptr);

		std::vector<std::string> NestedNames;
		Durin::ForEachNestedProperty(
			NestedArrayProperty,
			[&NestedNames](Durin::FProperty* Property) {
				NestedNames.push_back(Property->NamePrivate.ToString());
			}
		);
		ASSERT_EQ(NestedNames.size(), 2u);
		EXPECT_EQ(NestedNames[0], "NestedScores_Inner");
		EXPECT_EQ(NestedNames[1], "NestedScores_Inner_Inner");

		auto* ObjectListsProperty = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("ObjectLists"));
		ASSERT_NE(ObjectListsProperty, nullptr);
		ASSERT_NE(ObjectListsProperty->GetKeyProp(), nullptr);
		auto* ObjectListsValue = static_cast<Durin::FArrayProperty*>(ObjectListsProperty->GetValueProp());
		ASSERT_NE(ObjectListsValue, nullptr);
		ASSERT_NE(ObjectListsValue->GetInner(), nullptr);
		EXPECT_EQ(ObjectListsProperty->GetKeyProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::String);
		EXPECT_EQ(ObjectListsValue->GetOwnerProperty(), ObjectListsProperty);
		EXPECT_EQ(ObjectListsValue->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Object);
		EXPECT_EQ(ObjectListsValue->GetInner()->GetReferencedClass(), Durin::DObject::StaticClass());

		Durin::MarkAsGarbage(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, TypedBuiltInLeavesSupportDetachedDefaultDestroyAndCopy)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* Struct = GetBuiltInLeafOwnerStructForTest();
		ASSERT_NE(Struct, nullptr);

		FBuiltInLeafOwnerForTest DefaultOwner;
		FBuiltInLeafOwnerForTest Source;
		Source.BoolValue = true;
		Source.Int8Value = -8;
		Source.Int16Value = -16;
		Source.Int32Value = -32;
		Source.Int64Value = -64;
		Source.UInt8Value = 8;
		Source.UInt16Value = 16;
		Source.UInt32Value = 32;
		Source.UInt64Value = 64;
		Source.FloatValue = 1.25f;
		Source.DoubleValue = 2.5;
		Source.StringValue = "TypedLeaf";
		Source.NameValue = Durin::FName("TypedLeafName");
		Source.GuidValue = Durin::FGuid(1, 2, 3, 4);
		Source.FixedValues[0] = 31;
		Source.FixedValues[1] = 32;
		Source.EnumInt8 = 1;
		Source.EnumInt16 = 2;
		Source.EnumInt32 = 3;
		Source.EnumInt64 = 4;
		Source.EnumUInt8 = 5;
		Source.EnumUInt16 = 6;
		Source.EnumUInt32 = 7;
		Source.EnumUInt64 = 8;
		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("TypedLeafReferencedObject")
		);
		Source.RawObject = ReferencedObject;
		Source.ObjectPtr = ReferencedObject;

		size_t NumProperties = 0;
		for (Durin::FProperty* Property = static_cast<Durin::FProperty*>(Struct->ChildProperties);
			 Property;
			 Property = static_cast<Durin::FProperty*>(Property->Next))
		{
			++NumProperties;
			EXPECT_TRUE(Property->CanDefaultConstructValue()) << Property->NamePrivate.ToString();
			EXPECT_TRUE(Property->CanDestroyValue()) << Property->NamePrivate.ToString();
			EXPECT_TRUE(Property->CanCopyConstructValue()) << Property->NamePrivate.ToString();
			EXPECT_TRUE(Property->CanCopyAssignValue()) << Property->NamePrivate.ToString();
			EXPECT_GT(Property->GetValueSize(), 0u);
			EXPECT_GT(Property->GetValueAlignment(), 0u);

			const uint32 ArrayIndex = Property->NamePrivate.ToString() == "FixedValues" ? 1u : 0u;
			Durin::FReflectedValueStorage DefaultStorage;
			ASSERT_TRUE(DefaultStorage.DefaultConstruct(Property, ArrayIndex))
				<< Property->NamePrivate.ToString();
			EXPECT_TRUE(Durin::ArePropertyValuesIdentical(
				Property, DefaultStorage.GetContainer(), ArrayIndex, &DefaultOwner, ArrayIndex
			));

			Durin::FReflectedValueStorage CopyStorage;
			ASSERT_TRUE(CopyStorage.CopyConstruct(
				Property, Property->GetValuePtr(&Source, ArrayIndex), ArrayIndex
			))
				<< Property->NamePrivate.ToString();
			EXPECT_TRUE(Durin::ArePropertyValuesIdentical(
				Property, CopyStorage.GetContainer(), ArrayIndex, &Source, ArrayIndex
			));
			ASSERT_TRUE(CopyStorage.CopyAssign(Property->GetValuePtr(&DefaultOwner, ArrayIndex)))
				<< Property->NamePrivate.ToString();
			EXPECT_TRUE(Durin::ArePropertyValuesIdentical(
				Property, CopyStorage.GetContainer(), ArrayIndex, &DefaultOwner, ArrayIndex
			));
		}

		EXPECT_EQ(NumProperties, 25u);
		Durin::MarkAsGarbage(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, DeclarativeStructOpsMatchCompilerAndTraitCapabilities)
	{
		const Durin::FDStructOps& Ordinary = Durin::GetDStructOps<StructOpsTest::FOrdinary>();
		EXPECT_TRUE(Durin::EnumHasAnyFlags(Ordinary.Flags, Durin::EDStructOpsFlags::DefaultConstruct));
		EXPECT_TRUE(Durin::EnumHasAnyFlags(Ordinary.Flags, Durin::EDStructOpsFlags::TriviallyDestructible));
		EXPECT_TRUE(Durin::EnumHasAnyFlags(Ordinary.Flags, Durin::EDStructOpsFlags::CopyConstruct));
		EXPECT_TRUE(Durin::EnumHasAnyFlags(Ordinary.Flags, Durin::EDStructOpsFlags::CopyAssign));
		EXPECT_TRUE(Durin::EnumHasAnyFlags(Ordinary.Flags, Durin::EDStructOpsFlags::AuthoredFieldsComplete));
		EXPECT_EQ(Ordinary.Destroy, nullptr);
		EXPECT_EQ(Ordinary.ZeroConstruct, nullptr);
		EXPECT_EQ(Ordinary.Identical, nullptr);
		EXPECT_EQ(Ordinary.Serialize, nullptr);
		EXPECT_EQ(Ordinary.PostDeserialize, nullptr);
		EXPECT_EQ(Ordinary.CollectReferences, nullptr);

		alignas(StructOpsTest::FOrdinary) std::byte OrdinaryStorage[sizeof(StructOpsTest::FOrdinary)];
		Ordinary.DefaultConstruct(OrdinaryStorage);
		auto* OrdinaryValue = std::launder(reinterpret_cast<StructOpsTest::FOrdinary*>(OrdinaryStorage));
		EXPECT_EQ(OrdinaryValue->Value, 7);

		const Durin::FDStructOps& MoveOnly = Durin::GetDStructOps<StructOpsTest::FMoveOnly>();
		EXPECT_NE(MoveOnly.DefaultConstruct, nullptr);
		EXPECT_EQ(MoveOnly.CopyConstruct, nullptr);
		EXPECT_EQ(MoveOnly.CopyAssign, nullptr);

		const Durin::FDStructOps& DeletedDefault = Durin::GetDStructOps<StructOpsTest::FDeletedDefault>();
		EXPECT_EQ(DeletedDefault.DefaultConstruct, nullptr);
		EXPECT_NE(DeletedDefault.CopyConstruct, nullptr);
		EXPECT_NE(DeletedDefault.CopyAssign, nullptr);

		const Durin::FDStructOps& NonTrivial = Durin::GetDStructOps<StructOpsTest::FNonTrivial>();
		EXPECT_FALSE(Durin::EnumHasAnyFlags(NonTrivial.Flags, Durin::EDStructOpsFlags::TriviallyDestructible));
		ASSERT_NE(NonTrivial.DefaultConstruct, nullptr);
		ASSERT_NE(NonTrivial.Destroy, nullptr);
		alignas(StructOpsTest::FNonTrivial) std::byte NonTrivialStorage[sizeof(StructOpsTest::FNonTrivial)];
		NonTrivial.DefaultConstruct(NonTrivialStorage);
		NonTrivial.Destroy(NonTrivialStorage);

		const Durin::FDStructOps& Custom = Durin::GetDStructOps<StructOpsTest::FCustomOps>();
		EXPECT_NE(Custom.ZeroConstruct, nullptr);
		EXPECT_NE(Custom.Identical, nullptr);
		EXPECT_NE(Custom.Serialize, nullptr);
		EXPECT_NE(Custom.PostDeserialize, nullptr);
		EXPECT_NE(Custom.CollectReferences, nullptr);
		EXPECT_FALSE(Durin::EnumHasAnyFlags(Custom.Flags, Durin::EDStructOpsFlags::AuthoredFieldsComplete));
		const StructOpsTest::FCustomOps EqualLeft{42};
		const StructOpsTest::FCustomOps EqualRight{42};
		EXPECT_TRUE(Custom.Identical(&EqualLeft, &EqualRight));
	}

	TEST(FCoreDObjectReflectionTests, SemanticArchiveCapabilitiesScopesAndFailuresAreSticky)
	{
		int32 Sentinel = 73;
		Durin::FArchive Unsupported({
			Durin::EArchiveDirection::Load,
			Durin::EArchivePurpose::ObjectGraph,
			Durin::EArchiveCapability::None});
		Unsupported << Sentinel;
		ASSERT_TRUE(Unsupported.HasError());
		ASSERT_NE(Unsupported.GetFailure(), nullptr);
		EXPECT_EQ(Unsupported.GetFailure()->Code, Durin::EArchiveFailureCode::UnsupportedCapability);
		EXPECT_EQ(Sentinel, 73);
		Unsupported.Fail(Durin::EArchiveFailureCode::InvalidData, "replacement failure");
		EXPECT_EQ(Unsupported.GetFailure()->Code, Durin::EArchiveFailureCode::UnsupportedCapability);

		EnsureDObjectInitialized();
		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("SemanticArchiveScopeObject"));
		ASSERT_NE(Object, nullptr);

		std::vector<std::byte> MissingBaseBytes;
		Durin::FMemoryWriter MissingBaseWriter(MissingBaseBytes);
		{
			auto ObjectScope = MissingBaseWriter.EnterObject(*Object);
		}
		ASSERT_NE(MissingBaseWriter.GetFailure(), nullptr);
		EXPECT_EQ(MissingBaseWriter.GetFailure()->Code,
			Durin::EArchiveFailureCode::MissingBaseReflectedFields);

		std::vector<std::byte> DuplicateBaseBytes;
		Durin::FMemoryWriter DuplicateBaseWriter(DuplicateBaseBytes);
		{
			auto ObjectScope = DuplicateBaseWriter.EnterObject(*Object);
			DuplicateBaseWriter.MarkBaseReflectedFieldsSerialized();
			DuplicateBaseWriter.MarkBaseReflectedFieldsSerialized();
		}
		ASSERT_NE(DuplicateBaseWriter.GetFailure(), nullptr);
		EXPECT_EQ(DuplicateBaseWriter.GetFailure()->Code,
			Durin::EArchiveFailureCode::DuplicateBaseReflectedFields);

		std::vector<std::byte> DuplicateBytes;
		Durin::FMemoryWriter DuplicateWriter(DuplicateBytes);
		{
			auto ObjectScope = DuplicateWriter.EnterObject(*Object);
			DuplicateWriter.MarkBaseReflectedFieldsSerialized();
			Durin::FArchiveFieldDescriptor Field{
				.DeclaringType = Durin::FName("Tests::SemanticArchiveScopeObject"),
				.Name = Durin::FName("Value"),
				.LogicalType = Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)};
			{ auto First = DuplicateWriter.EnterField(Field); }
			{ auto Second = DuplicateWriter.EnterField(Field); }
		}
		ASSERT_NE(DuplicateWriter.GetFailure(), nullptr);
		EXPECT_EQ(DuplicateWriter.GetFailure()->Code, Durin::EArchiveFailureCode::DuplicateField);

		std::vector<std::byte> UnbalancedBytes;
		Durin::FMemoryWriter UnbalancedWriter(UnbalancedBytes);
		{
			auto ObjectScope = UnbalancedWriter.EnterObject(*Object);
			UnbalancedWriter.MarkBaseReflectedFieldsSerialized();
			Durin::FArchiveFieldDescriptor Field{
				.Name = Durin::FName("OpenField"),
				.LogicalType = Durin::FArchiveLogicalTypeDescriptor::Guid()};
			auto FieldScope = UnbalancedWriter.EnterField(Field);
			ObjectScope = {};
		}
		ASSERT_NE(UnbalancedWriter.GetFailure(), nullptr);
		EXPECT_EQ(UnbalancedWriter.GetFailure()->Code, Durin::EArchiveFailureCode::UnbalancedScope);

		Durin::DStruct IncompleteStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("Tests::FIncompleteAuthoredStruct"),
			Durin::FName("FIncompleteAuthoredStruct"),
			sizeof(StructOpsTest::FIncompleteAuthoredStruct),
			alignof(StructOpsTest::FIncompleteAuthoredStruct),
			Durin::EObjectFlags::Transient);
		Durin::FDStructOps IncompleteOps;
		IncompleteStruct.InitializeOps(&IncompleteOps);
		Durin::FNumericProperty ReflectedValue(
			Durin::FFieldVariant(&IncompleteStruct), Durin::FName("Value"),
			Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(StructOpsTest::FIncompleteAuthoredStruct, Value),
			sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
		IncompleteStruct.ChildProperties = &ReflectedValue;
		Durin::FStructProperty IncompleteProperty(
			Durin::FFieldVariant(), Durin::FName("Incomplete"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, &IncompleteStruct);
		StructOpsTest::FIncompleteAuthoredStruct IncompleteValue{41};
		std::vector<std::byte> MalformedBytes;
		Durin::FMemoryWriter AuthoredWriter(
			MalformedBytes, Durin::EArchivePurpose::AuthoredPackage);
		Durin::SerializeReflectedPropertyValue(
			AuthoredWriter, IncompleteProperty, &IncompleteValue);
		ASSERT_NE(AuthoredWriter.GetFailure(), nullptr);
		EXPECT_EQ(AuthoredWriter.GetFailure()->Code, Durin::EArchiveFailureCode::MalformedSerializer);
		EXPECT_TRUE(MalformedBytes.empty());

		Durin::MarkAsGarbage(Object);
	}

	TEST(FCoreDObjectReflectionTests, BuiltInMathStructsExposeNestedFieldMetadataAndOperations)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* FloatVector2Struct = Durin::Z_Construct_DStruct_FVector2f();
		Durin::DStruct* FloatVectorStruct = Durin::Z_Construct_DStruct_FVector3f();
		Durin::DStruct* FloatVector4Struct = Durin::Z_Construct_DStruct_FVector4f();
		Durin::DStruct* Vector2Struct = Durin::Z_Construct_DStruct_FVector2();
		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_FVector3();
		Durin::DStruct* Vector4Struct = Durin::Z_Construct_DStruct_FVector4();
		Durin::DStruct* QuatStruct = Durin::Z_Construct_DStruct_FQuat();
		Durin::DStruct* TransformStruct = Durin::Z_Construct_DStruct_FTransform();
		Durin::DStruct* ColorStruct = Durin::Z_Construct_DStruct_FLinearColor();
		ASSERT_NE(FloatVector2Struct, nullptr);
		ASSERT_NE(FloatVectorStruct, nullptr);
		ASSERT_NE(FloatVector4Struct, nullptr);
		ASSERT_NE(Vector2Struct, nullptr);
		ASSERT_NE(VectorStruct, nullptr);
		ASSERT_NE(Vector4Struct, nullptr);
		ASSERT_NE(QuatStruct, nullptr);
		ASSERT_NE(TransformStruct, nullptr);
		ASSERT_NE(ColorStruct, nullptr);
		EXPECT_EQ(FloatVector2Struct->GetQualifiedName().ToString(), "Durin::FVector2f");
		EXPECT_EQ(FloatVectorStruct->GetQualifiedName().ToString(), "Durin::FVector3f");
		EXPECT_EQ(FloatVector4Struct->GetQualifiedName().ToString(), "Durin::FVector4f");
		EXPECT_EQ(Vector2Struct->GetQualifiedName().ToString(), "Durin::FVector2");
		EXPECT_EQ(VectorStruct->GetQualifiedName().ToString(), "Durin::FVector3");
		EXPECT_EQ(Vector4Struct->GetQualifiedName().ToString(), "Durin::FVector4");
		EXPECT_EQ(QuatStruct->GetQualifiedName().ToString(), "Durin::FQuat");
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector2f"), FloatVector2Struct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector3f"), FloatVectorStruct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector4f"), FloatVector4Struct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector2"), Vector2Struct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector3"), VectorStruct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector4"), Vector4Struct);
		EXPECT_EQ(Durin::FindStructByQualifiedName(Durin::FName("Durin::FTransform")), TransformStruct);
		EXPECT_EQ(ColorStruct->GetQualifiedName().ToString(), "Durin::FLinearColor");
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FLinearColor"), ColorStruct);
		for (Durin::DStruct* Struct : {FloatVector2Struct, FloatVectorStruct, FloatVector4Struct,
				 Vector2Struct, VectorStruct, Vector4Struct, QuatStruct, TransformStruct, ColorStruct})
		{
			EXPECT_EQ(Struct->GetDefaultState(), Durin::EDStructDefaultState::Ready);
			EXPECT_EQ(Struct->GetDefaultReason(), Durin::EDStructDefaultReason::None);
			EXPECT_NE(Struct->GetDefaultValue(), nullptr);
			EXPECT_TRUE(Struct->CanDefaultConstruct());
			EXPECT_TRUE(Struct->CanDestroy());
			EXPECT_FALSE(Struct->NeedsDestroy());
			EXPECT_TRUE(Struct->CanCopyConstruct());
			EXPECT_TRUE(Struct->CanCopyAssign());
			EXPECT_FALSE(Struct->CanZeroConstruct());
			EXPECT_FALSE(Struct->HasIdentical());
			EXPECT_FALSE(Struct->HasSerializer());
			EXPECT_FALSE(Struct->HasPostDeserialize());
			EXPECT_FALSE(Struct->HasReferenceCollector());
			EXPECT_TRUE(Struct->HasCompleteAuthoredFields());
		}
		EXPECT_EQ(VectorStruct->PropertiesSize, sizeof(Durin::FVector3));
		EXPECT_EQ(VectorStruct->MinAlignment, alignof(Durin::FVector3));
		EXPECT_EQ(FloatVectorStruct->PropertiesSize, sizeof(Durin::FVector3f));
		EXPECT_EQ(FloatVectorStruct->MinAlignment, alignof(Durin::FVector3f));
		for (const auto& [Struct, Components] : std::array{
				 std::pair{FloatVector2Struct, std::array<const char*, 4>{"x", "y", nullptr, nullptr}},
				 std::pair{FloatVectorStruct, std::array<const char*, 4>{"x", "y", "z", nullptr}},
				 std::pair{FloatVector4Struct, std::array<const char*, 4>{"x", "y", "z", "w"}}
			 })
		{
			for (const char* Component : Components)
			{
				if (!Component) continue;
				Durin::FProperty* Field = Struct->FindPropertyByName(Component, false);
				ASSERT_NE(Field, nullptr);
				EXPECT_EQ(Field->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Float);
				EXPECT_EQ(Field->GetElementSize(), sizeof(float));
			}
		}
		for (const auto& [Struct, Components] : std::array{
				 std::pair{Vector2Struct, std::array<const char*, 4>{"x", "y", nullptr, nullptr}},
				 std::pair{VectorStruct, std::array<const char*, 4>{"x", "y", "z", nullptr}},
				 std::pair{Vector4Struct, std::array<const char*, 4>{"x", "y", "z", "w"}}
			 })
		{
			for (const char* Component : Components)
			{
				if (!Component) continue;
				Durin::FProperty* Field = Struct->FindPropertyByName(Component, false);
				ASSERT_NE(Field, nullptr);
				EXPECT_EQ(Field->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Double);
				EXPECT_EQ(Field->GetElementSize(), sizeof(double));
			}
		}
		for (const char* Channel : {"R", "G", "B", "A"})
		{
			Durin::FProperty* Field = ColorStruct->FindPropertyByName(Channel, false);
			ASSERT_NE(Field, nullptr);
			EXPECT_EQ(Field->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Float);
			EXPECT_EQ(Field->GetElementSize(), sizeof(float));
		}
		Durin::FProperty* Rotation = TransformStruct->FindPropertyByName("Rotation", false);
		Durin::FProperty* Translation = TransformStruct->FindPropertyByName("Translation", false);
		Durin::FProperty* Scale = TransformStruct->FindPropertyByName("Scale3D", false);
		ASSERT_NE(Rotation, nullptr);
		ASSERT_NE(Translation, nullptr);
		ASSERT_NE(Scale, nullptr);
		EXPECT_EQ(TransformStruct->ChildProperties, Rotation);
		EXPECT_EQ(Rotation->Next, Translation);
		EXPECT_EQ(Translation->Next, Scale);
		EXPECT_EQ(Scale->Next, nullptr);
		EXPECT_EQ(Rotation->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Struct);
		auto* RotationStructProperty = static_cast<Durin::FStructProperty*>(Rotation);
		auto* TranslationStructProperty = static_cast<Durin::FStructProperty*>(Translation);
		auto* ScaleStructProperty = static_cast<Durin::FStructProperty*>(Scale);
		EXPECT_EQ(RotationStructProperty->GetStruct(), QuatStruct);
		EXPECT_EQ(TranslationStructProperty->GetStruct(), VectorStruct);
		EXPECT_EQ(ScaleStructProperty->GetStruct(), VectorStruct);
		for (const auto& [Property, Offset] : std::array{
				 std::pair{RotationStructProperty, static_cast<uint16>(STRUCT_OFFSET(Durin::FTransform, Rotation))},
				 std::pair{TranslationStructProperty, static_cast<uint16>(STRUCT_OFFSET(Durin::FTransform, Translation))},
				 std::pair{ScaleStructProperty, static_cast<uint16>(STRUCT_OFFSET(Durin::FTransform, Scale3D))}
			 })
		{
			EXPECT_EQ(Property->GetPropertyFlags(), Durin::EPropertyFlags::None);
			EXPECT_EQ(Property->GetArrayDim(), 1);
			EXPECT_EQ(Property->GetOffset(), Offset);
			EXPECT_FALSE(Property->HasValueAccessors());
			EXPECT_TRUE(Property->HasValueLifecycle());
		}
		EXPECT_EQ(RotationStructProperty->GetElementSize(), QuatStruct->PropertiesSize);
		EXPECT_EQ(TranslationStructProperty->GetElementSize(), VectorStruct->PropertiesSize);
		EXPECT_EQ(ScaleStructProperty->GetElementSize(), VectorStruct->PropertiesSize);
		EXPECT_EQ(TranslationStructProperty->GetValueAlignment(), VectorStruct->MinAlignment);
		Durin::FTransform TransformValue;
		EXPECT_EQ(RotationStructProperty->GetValuePtr(&TransformValue), &TransformValue.Rotation);
		EXPECT_EQ(TranslationStructProperty->GetValuePtr(&TransformValue), &TransformValue.Translation);
		EXPECT_EQ(ScaleStructProperty->GetValuePtr(&TransformValue), &TransformValue.Scale3D);

		alignas(Durin::FVector3) std::byte ZeroStorage[sizeof(Durin::FVector3)];
		alignas(Durin::FVector3) std::byte CopyStorage[sizeof(Durin::FVector3)];
		const Durin::FDStructOps& VectorOps = VectorStruct->GetOps();
		VectorOps.DefaultConstruct(ZeroStorage);
		auto* Zero = std::launder(reinterpret_cast<Durin::FVector3*>(ZeroStorage));
		EXPECT_EQ(*Zero, Durin::FVector3(0.0));
		const Durin::FVector3 Source(1.0, 2.0, 3.0);
		VectorOps.CopyConstruct(CopyStorage, &Source);
		auto* Copy = std::launder(reinterpret_cast<Durin::FVector3*>(CopyStorage));
		EXPECT_EQ(*Copy, Source);
		VectorOps.CopyAssign(Zero, Copy);
		EXPECT_EQ(*Zero, Source);

		Durin::FProperty* X = VectorStruct->FindPropertyByName("x", false);
		Durin::FProperty* Y = VectorStruct->FindPropertyByName("y", false);
		Durin::FProperty* Z = VectorStruct->FindPropertyByName("z", false);
		ASSERT_NE(X, nullptr);
		ASSERT_NE(Y, nullptr);
		ASSERT_NE(Z, nullptr);
		EXPECT_EQ(VectorStruct->ChildProperties, X);
		EXPECT_EQ(X->Next, Y);
		EXPECT_EQ(Y->Next, Z);
		EXPECT_EQ(Z->Next, nullptr);
		EXPECT_EQ(X->GetValuePtr(&Source), &Source.x);
		EXPECT_EQ(Y->GetValuePtr(&Source), &Source.y);
		EXPECT_EQ(Z->GetValuePtr(&Source), &Source.z);
	}

	TEST(FCoreDObjectReflectionTests, PrecisionSpecificQuaternionAndMatrixStructsExposeCanonicalSchemas)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* FloatVector4Struct = Durin::Z_Construct_DStruct_FVector4f();
		Durin::DStruct* FloatQuatStruct = Durin::Z_Construct_DStruct_FQuatf();
		Durin::DStruct* FloatMatrixStruct = Durin::Z_Construct_DStruct_FMatrix4f();
		ASSERT_NE(FloatVector4Struct, nullptr);
		ASSERT_NE(FloatQuatStruct, nullptr);
		ASSERT_NE(FloatMatrixStruct, nullptr);
		EXPECT_EQ(FloatQuatStruct->GetQualifiedName().ToString(), "Durin::FQuatf");
		EXPECT_EQ(FloatMatrixStruct->GetQualifiedName().ToString(), "Durin::FMatrix4f");
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FQuatf"), FloatQuatStruct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FMatrix4f"), FloatMatrixStruct);

		for (const char* Component : {"w", "x", "y", "z"})
		{
			Durin::FProperty* Field = FloatQuatStruct->FindPropertyByName(Component, false);
			ASSERT_NE(Field, nullptr);
			EXPECT_EQ(Field->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Float);
			EXPECT_EQ(Field->GetElementSize(), sizeof(float));
			EXPECT_TRUE(Field->HasValueAccessors());
		}

		Durin::FMatrix4f Matrix(1.0f);
		for (uint32 ColumnIndex = 0; ColumnIndex < 4; ++ColumnIndex)
		{
			const std::string ColumnName = "Column" + std::to_string(ColumnIndex);
			auto* Column = static_cast<Durin::FStructProperty*>(
				FloatMatrixStruct->FindPropertyByName(ColumnName.c_str(), false));
			ASSERT_NE(Column, nullptr);
			EXPECT_EQ(Column->GetStruct(), FloatVector4Struct);
			EXPECT_TRUE(Column->HasValueAccessors());
			EXPECT_EQ(Column->GetValuePtr(&Matrix), &Matrix[ColumnIndex]);
			EXPECT_EQ(Column->GetTypedMetadata().Category, "Matrix");
		}
		auto* TransformStruct = Durin::Z_Construct_DStruct_FTransform();
		ASSERT_NE(TransformStruct, nullptr);
		auto* Rotation = TransformStruct->FindPropertyByName("Rotation", false);
		auto* Translation = TransformStruct->FindPropertyByName("Translation", false);
		ASSERT_NE(Rotation, nullptr);
		ASSERT_NE(Translation, nullptr);
		EXPECT_EQ(Rotation->GetTypedMetadata().Category, "Transform");
		EXPECT_EQ(Translation->GetTypedMetadata().Category, "Transform");

		const auto* DefaultQuat = static_cast<const Durin::FQuatf*>(FloatQuatStruct->GetDefaultValue());
		const auto* DefaultMatrix = static_cast<const Durin::FMatrix4f*>(FloatMatrixStruct->GetDefaultValue());
		ASSERT_NE(DefaultQuat, nullptr);
		ASSERT_NE(DefaultMatrix, nullptr);
		EXPECT_EQ(*DefaultQuat, Durin::FQuatf(1.0f, 0.0f, 0.0f, 0.0f));
		EXPECT_EQ(*DefaultMatrix, Durin::FMatrix4f(1.0f));
	}

	TEST(FCoreDObjectReflectionTests, TypedMetadataPreservesExactNumericChannelsAndValidatesPropertyEditBounds)
	{
		Durin::FNumericProperty UnsignedProperty(
			Durin::FFieldVariant(), Durin::FName("Unsigned"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, 0, sizeof(uint64),
			Durin::DurinCodeGen::EPropertyGenFlags::UInt64, nullptr);
		const Durin::FPropertyMetadataParams UnsignedMetadata{
			"Counter", "Exact unsigned value", "Numbers", Durin::EPropertyUnit::Unitless,
			Durin::FPropertyMetadataNumber::FromUnsigned(1), -1,
			Durin::FPropertyMetadataNumber::FromUnsigned(9'007'199'254'740'993ULL),
			Durin::FPropertyMetadataNumber::FromUnsigned(std::numeric_limits<uint64>::max())};
		UnsignedProperty.SetTypedMetadata(&UnsignedMetadata);
		EXPECT_EQ(UnsignedProperty.GetTypedMetadata().ClampMin.Unsigned, 9'007'199'254'740'993ULL);
		EXPECT_EQ(UnsignedProperty.GetTypedMetadata().ClampMax.Unsigned,
			std::numeric_limits<uint64>::max());
		EXPECT_EQ(UnsignedProperty.GetMetaData(Durin::FName("DisplayName")), "Counter");

		std::string Error;
		uint64 Value = 9'007'199'254'740'993ULL;
		EXPECT_TRUE(Durin::ValidatePropertyEditValue(&UnsignedProperty, &Value, 0, &Error));
		Value = 9'007'199'254'740'992ULL;
		EXPECT_FALSE(Durin::ValidatePropertyEditValue(&UnsignedProperty, &Value, 0, &Error));
		EXPECT_EQ(Error, "The proposed value is below ClampMin.");

		Durin::FNumericProperty FloatProperty(
			Durin::FFieldVariant(), Durin::FName("Float"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, 0, sizeof(float),
			Durin::DurinCodeGen::EPropertyGenFlags::Float, nullptr);
		const Durin::FPropertyMetadataParams FloatMetadata{
			nullptr, nullptr, nullptr, Durin::EPropertyUnit::None, {}, 3,
			Durin::FPropertyMetadataNumber::FromFloat(-1.0f),
			Durin::FPropertyMetadataNumber::FromFloat(1.0f)};
		FloatProperty.SetTypedMetadata(&FloatMetadata);
		float FloatValue = std::numeric_limits<float>::quiet_NaN();
		EXPECT_FALSE(Durin::ValidatePropertyEditValue(&FloatProperty, &FloatValue, 0, &Error));
		FloatValue = 1.0f;
		EXPECT_TRUE(Durin::ValidatePropertyEditValue(&FloatProperty, &FloatValue, 0, &Error));
	}

	TEST(FCoreDObjectReflectionTests, StructDefaultsPublishAtomicallyAndRejectUnstableOrReentrantConstruction)
	{
		auto MakeOps = [](Durin::FDStructOps::FDefaultConstruct Construct) {
			Durin::FDStructOps Ops;
			Ops.Flags = Durin::EDStructOpsFlags::DefaultConstruct
				| Durin::EDStructOpsFlags::TriviallyDestructible
				| Durin::EDStructOpsFlags::AuthoredFieldsComplete;
			Ops.DefaultConstruct = Construct;
			return Ops;
		};
		auto AttachValue = [](Durin::DStruct& Struct, Durin::FNumericProperty& Property) {
			Struct.ChildProperties = &Property;
		};

		Durin::FDStructOps StableOps = MakeOps(&StructOpsTest::ConstructStableDefault);
		Durin::DStruct Stable(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FStableDefault"), Durin::FName("FStableDefault"),
			sizeof(int32), alignof(int32), Durin::EObjectFlags::Transient
		);
		Stable.InitializeOps(&StableOps);
		Durin::FNumericProperty StableValue(
			Durin::FFieldVariant(&Stable), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		AttachValue(Stable, StableValue);
		const std::array StableBatch{&Stable};
		ASSERT_TRUE(Durin::Private::CreateDStructDefaultsForBatch(StableBatch));
		ASSERT_EQ(Stable.GetDefaultState(), Durin::EDStructDefaultState::Ready);
		EXPECT_EQ(*static_cast<const int32*>(Stable.GetDefaultValue()), 7);
		const void* WorkerDefault = nullptr;
		std::thread Worker([&] { WorkerDefault = Stable.GetDefaultValue(); });
		Worker.join();
		EXPECT_EQ(WorkerDefault, Stable.GetDefaultValue());

		StructOpsTest::DefaultSequence = 0;
		Durin::FDStructOps ChangingOps = MakeOps(&StructOpsTest::ConstructChangingDefault);
		Durin::DStruct AtomicStable(
			Durin::EC_StaticConstructor, Durin::FName("Tests::AAtomicStableDefault"), Durin::FName("AAtomicStableDefault"),
			sizeof(int32), alignof(int32), Durin::EObjectFlags::Transient
		);
		AtomicStable.InitializeOps(&StableOps);
		Durin::FNumericProperty AtomicStableValue(
			Durin::FFieldVariant(&AtomicStable), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		AttachValue(AtomicStable, AtomicStableValue);
		Durin::DStruct Changing(
			Durin::EC_StaticConstructor, Durin::FName("Tests::ZChangingDefault"), Durin::FName("ZChangingDefault"),
			sizeof(int32), alignof(int32), Durin::EObjectFlags::Transient
		);
		Changing.InitializeOps(&ChangingOps);
		Durin::FNumericProperty ChangingValue(
			Durin::FFieldVariant(&Changing), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		AttachValue(Changing, ChangingValue);
		const std::array ChangingBatch{&Changing, &AtomicStable};
		EXPECT_FALSE(Durin::Private::CreateDStructDefaultsForBatch(ChangingBatch));
		EXPECT_EQ(AtomicStable.GetDefaultState(), Durin::EDStructDefaultState::Failed);
		EXPECT_EQ(AtomicStable.GetDefaultReason(), Durin::EDStructDefaultReason::ConstructionFailed);
		EXPECT_EQ(AtomicStable.GetDefaultValue(), nullptr);
		EXPECT_EQ(Changing.GetDefaultState(), Durin::EDStructDefaultState::Failed);
		EXPECT_EQ(Changing.GetDefaultReason(), Durin::EDStructDefaultReason::NonDeterministicConstruction);
		EXPECT_EQ(Changing.GetDefaultValue(), nullptr);

		Durin::FDStructOps ReentrantOps = MakeOps(&StructOpsTest::ConstructReentrantDefault);
		Durin::DStruct Reentrant(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FReentrantDefault"), Durin::FName("FReentrantDefault"),
			sizeof(int32), alignof(int32), Durin::EObjectFlags::Transient
		);
		Reentrant.InitializeOps(&ReentrantOps);
		Durin::FNumericProperty ReentrantValue(
			Durin::FFieldVariant(&Reentrant), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		AttachValue(Reentrant, ReentrantValue);
		StructOpsTest::ReentrantDefaultStruct = &Reentrant;
		const std::array ReentrantBatch{&Reentrant};
		EXPECT_FALSE(Durin::Private::CreateDStructDefaultsForBatch(ReentrantBatch));
		EXPECT_EQ(Reentrant.GetDefaultState(), Durin::EDStructDefaultState::Failed);
		EXPECT_EQ(Reentrant.GetDefaultReason(), Durin::EDStructDefaultReason::RecursiveConstruction);
		StructOpsTest::ReentrantDefaultStruct = nullptr;

		Durin::FDStructOps SideEffectOps = MakeOps(&StructOpsTest::ConstructSideEffectDefault);
		Durin::DStruct SideEffect(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FSideEffectDefault"),
			Durin::FName("FSideEffectDefault"), sizeof(int32), alignof(int32),
			Durin::EObjectFlags::Transient);
		SideEffect.InitializeOps(&SideEffectOps);
		Durin::FNumericProperty SideEffectValue(
			Durin::FFieldVariant(&SideEffect), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
		AttachValue(SideEffect, SideEffectValue);
		StructOpsTest::SideEffectObjects.clear();
		const uint64 ObjectCountBefore = Durin::GDObjectArray.GetNum();
		const std::array SideEffectBatch{&SideEffect};
		EXPECT_FALSE(Durin::Private::CreateDStructDefaultsForBatch(SideEffectBatch));
		EXPECT_EQ(SideEffect.GetDefaultReason(), Durin::EDStructDefaultReason::PublicationSideEffect);
		EXPECT_LE(Durin::GDObjectArray.GetNum(), ObjectCountBefore);
		for (Durin::DObject* Object : StructOpsTest::SideEffectObjects)
			EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
		StructOpsTest::SideEffectObjects.clear();
	}

	TEST(FCoreDObjectReflectionTests, PublishedStructDefaultsRootReferencesUntilModuleRelease)
	{
		EnsureDObjectInitialized();
		StructOpsTest::DefaultReferenceTarget = Durin::NewObject<Durin::DObject>(
			nullptr, Durin::FName("StructDefaultReferenceTarget"));
		Durin::DObject* Target = StructOpsTest::DefaultReferenceTarget;

		static Durin::FDStructOps ReferenceOps;
		ReferenceOps.Flags = Durin::EDStructOpsFlags::DefaultConstruct
			| Durin::EDStructOpsFlags::Destroy
			| Durin::EDStructOpsFlags::CollectReferences
			| Durin::EDStructOpsFlags::AuthoredFieldsComplete;
		ReferenceOps.DefaultConstruct = &StructOpsTest::ConstructReferenceDefault;
		ReferenceOps.Destroy = &StructOpsTest::DestroyReferenceDefault;
		ReferenceOps.CollectReferences = &StructOpsTest::CollectReferenceDefault;

		auto* Struct = new Durin::DStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FReferenceDefault"),
			Durin::FName("FReferenceDefault"), sizeof(StructOpsTest::FReferenceDefault),
			alignof(StructOpsTest::FReferenceDefault), Durin::EObjectFlags::NoFlags);
		Struct->InitializeOps(&ReferenceOps);
		const auto Params = Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
			"Value", Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(StructOpsTest::FReferenceDefault, Value),
			&Durin::DObject::StaticClass);
		auto* ValueProperty = new Durin::FObjectProperty(
			Durin::FFieldVariant(Struct), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(StructOpsTest::FReferenceDefault, Value),
			static_cast<uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(),
			false, Params.ReadObjectValue, Params.WriteObjectValue);
		Struct->ChildProperties = ValueProperty;
		Struct->Register(
			Durin::DStruct::StaticClass, "/Cpp/StructDefaultLifecycleTest", "Tests::FReferenceDefault");
		Durin::DObjectForceRegistration(Struct);

		const std::array Batch{Struct};
		ASSERT_TRUE(Durin::Private::CreateDStructDefaultsForBatch(Batch));
		ASSERT_EQ(Struct->GetDefaultState(), Durin::EDStructDefaultState::Ready);
		StructOpsTest::DefaultReferenceTarget = nullptr;
		Durin::CollectGarbage();
		EXPECT_TRUE(Durin::GDObjectArray.Contains(Target));

		Durin::ReleaseDStructDefaultsForModule(Durin::FName("StructDefaultLifecycleTest"));
		EXPECT_EQ(Struct->GetDefaultState(), Durin::EDStructDefaultState::Released);
		Durin::CollectGarbage();
		EXPECT_FALSE(Durin::GDObjectArray.Contains(Target));
	}

	TEST(FCoreDObjectReflectionTests, StructDefaultEligibilityReportsStableReasons)
	{
		auto ExpectReason = [](Durin::DStruct& Struct, Durin::EDStructDefaultReason Expected) {
			const std::array Batch{&Struct};
			EXPECT_TRUE(Durin::Private::CreateDStructDefaultsForBatch(Batch));
			EXPECT_EQ(Struct.GetDefaultState(), Durin::EDStructDefaultState::Unavailable);
			EXPECT_EQ(Struct.GetDefaultReason(), Expected);
			EXPECT_EQ(Struct.GetDefaultValue(), nullptr);
		};
		auto MakeStruct = [](const char* Name) {
			return Durin::DStruct(
				Durin::EC_StaticConstructor, Durin::FName(Name), Durin::FName(Name),
				sizeof(int32), alignof(int32), Durin::EObjectFlags::Transient);
		};

		auto MissingOps = MakeStruct("Tests::FMissingOpsDefault");
		ExpectReason(MissingOps, Durin::EDStructDefaultReason::MissingInitializedOps);

		auto MissingConstructor = MakeStruct("Tests::FMissingConstructorDefault");
		Durin::FDStructOps MissingConstructorOps;
		MissingConstructorOps.Flags = Durin::EDStructOpsFlags::TriviallyDestructible
			| Durin::EDStructOpsFlags::AuthoredFieldsComplete;
		MissingConstructor.InitializeOps(&MissingConstructorOps);
		ExpectReason(MissingConstructor, Durin::EDStructDefaultReason::MissingDefaultConstructor);

		auto MissingDestructor = MakeStruct("Tests::FMissingDestructorDefault");
		Durin::FDStructOps MissingDestructorOps;
		MissingDestructorOps.Flags = Durin::EDStructOpsFlags::DefaultConstruct
			| Durin::EDStructOpsFlags::AuthoredFieldsComplete;
		MissingDestructorOps.DefaultConstruct = &StructOpsTest::ConstructStableDefault;
		MissingDestructor.InitializeOps(&MissingDestructorOps);
		ExpectReason(MissingDestructor, Durin::EDStructDefaultReason::MissingDestructor);

		auto Incomplete = MakeStruct("Tests::FIncompleteDefault");
		Durin::FDStructOps IncompleteOps;
		IncompleteOps.Flags = Durin::EDStructOpsFlags::DefaultConstruct
			| Durin::EDStructOpsFlags::TriviallyDestructible;
		IncompleteOps.DefaultConstruct = &StructOpsTest::ConstructStableDefault;
		Incomplete.InitializeOps(&IncompleteOps);
		ExpectReason(Incomplete, Durin::EDStructDefaultReason::IncompleteAuthoredFields);

		auto CustomSerializer = MakeStruct("Tests::FCustomSerializerDefault");
		Durin::FDStructOps CustomSerializerOps;
		CustomSerializerOps.Flags = Durin::EDStructOpsFlags::DefaultConstruct
			| Durin::EDStructOpsFlags::TriviallyDestructible
			| Durin::EDStructOpsFlags::AuthoredFieldsComplete
			| Durin::EDStructOpsFlags::Serialize;
		CustomSerializerOps.DefaultConstruct = &StructOpsTest::ConstructStableDefault;
		CustomSerializerOps.Serialize = &StructOpsTest::SerializeDefault;
		CustomSerializer.InitializeOps(&CustomSerializerOps);
		ExpectReason(CustomSerializer, Durin::EDStructDefaultReason::CustomSerializer);
	}

	TEST(FCoreDObjectReflectionTests, TypedStructPropertyCompiledMetadataFootprintIsRecorded)
	{
		RecordProperty(
			"FPropertyParamsBaseBytes",
			static_cast<int>(sizeof(Durin::DurinCodeGen::FPropertyParamsBase))
		);
		RecordProperty(
			"FStructPropertyParamsBytes",
			static_cast<int>(sizeof(Durin::DurinCodeGen::FStructPropertyParams))
		);
		EXPECT_TRUE((std::is_base_of_v<
					 Durin::DurinCodeGen::FPropertyParamsBase,
					 Durin::DurinCodeGen::FStructPropertyParams>));
	}

	TEST(FCoreDObjectReflectionTests, TypedStructPropertyRegistrationSupportsOffsetsAccessorsAndMetadata)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* OwnerStruct = GetTypedStructPropertyOwner();
		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_FVector3();
		auto* Direct = static_cast<Durin::FStructProperty*>(OwnerStruct->FindPropertyByName("Direct", false));
		auto* Accessed = static_cast<Durin::FStructProperty*>(OwnerStruct->FindPropertyByName("Accessed", false));
		ASSERT_NE(Direct, nullptr);
		ASSERT_NE(Accessed, nullptr);
		EXPECT_EQ(Direct->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Struct);
		EXPECT_EQ(Direct->GetStruct(), VectorStruct);
		EXPECT_EQ(Accessed->GetStruct(), VectorStruct);
		EXPECT_EQ(Direct->GetElementSize(), VectorStruct->PropertiesSize);
		EXPECT_EQ(Direct->GetValueSize(), VectorStruct->PropertiesSize);
		EXPECT_EQ(Direct->GetValueAlignment(), VectorStruct->MinAlignment);
		EXPECT_EQ(Direct->GetMetaData(Durin::FName("Category")), "TypedStruct");
		EXPECT_FALSE(Direct->HasValueAccessors());
		EXPECT_TRUE(Accessed->HasValueAccessors());

		FTypedStructPropertyOwnerForTest Owner;
		EXPECT_EQ(Direct->GetValuePtr(&Owner), &Owner.Direct);
		EXPECT_EQ(Accessed->GetValuePtr(&Owner), &Owner.Accessed);
		EXPECT_EQ(Accessed->GetOffset(), 0);
	}

	TEST(FCoreDObjectReflectionTests, TypedStructPropertyRegistrationDefersUnavailableCapabilitiesToUse)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* OwnerStruct = GetUnavailableStructPropertyOwner();
		auto* DeletedDefault = static_cast<Durin::FStructProperty*>(OwnerStruct->FindPropertyByName("DeletedDefault", false));
		auto* MoveOnly = static_cast<Durin::FStructProperty*>(OwnerStruct->FindPropertyByName("MoveOnly", false));
		ASSERT_NE(DeletedDefault, nullptr);
		ASSERT_NE(MoveOnly, nullptr);
		EXPECT_FALSE(DeletedDefault->CanDefaultConstructValue());
		EXPECT_TRUE(DeletedDefault->CanCopyAssignValue());
		EXPECT_TRUE(MoveOnly->CanDefaultConstructValue());
		EXPECT_FALSE(MoveOnly->CanCopyAssignValue());

		alignas(StructOpsTest::FDeletedDefault)
			std::array<std::byte, sizeof(StructOpsTest::FDeletedDefault)>
				UnchangedStorage;
		UnchangedStorage.fill(std::byte{0x5a});
		const auto OriginalStorage = UnchangedStorage;
		std::string Error;
		EXPECT_FALSE(DeletedDefault->InitializeValue(UnchangedStorage.data(), &Error));
		EXPECT_EQ(UnchangedStorage, OriginalStorage);
		EXPECT_NE(Error.find("DefaultConstruct"), std::string::npos);

		StructOpsTest::FMoveOnly Destination;
		Destination.Value = 7;
		StructOpsTest::FMoveOnly Source;
		Source.Value = 11;
		Error.clear();
		EXPECT_FALSE(MoveOnly->CopyAssignValue(&Destination, &Source, &Error));
		EXPECT_EQ(Destination.Value, 7);
		EXPECT_NE(Error.find("CopyAssign"), std::string::npos);
	}

#if DO_CHECK
	TEST(FCoreDObjectReflectionTests, TypedStructPropertyRegistrationRejectsInvalidMetadata)
	{
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				static constexpr Durin::FGuid Guid{1, 2, 3, 4};
				static const char* const Targets[] = {"Current"};
				static const Durin::FPropertyDeprecationParams Deprecation{
					Guid, 1, 1, "Historical", Targets, std::size(Targets)};
				auto Params = Durin::DurinCodeGen::WithDeprecation(
					Durin::DurinCodeGen::FInt32PropertyParams{
						"Historical", Durin::EPropertyFlags::Deprecated, 1, 0},
					&Deprecation);
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.InvalidDeprecation"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				static const char* const Targets[] = {"Current"};
				static const Durin::FPropertyDeprecationParams Deprecation{
					{}, 1, 1, "Historical", Targets, std::size(Targets)};
				auto Params = Durin::DurinCodeGen::WithDeprecation(
					Durin::DurinCodeGen::FInt32PropertyParams{
						"Historical_DEPRECATED", Durin::EPropertyFlags::Deprecated, 1, 0},
					&Deprecation);
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.InvalidDeprecation"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				Durin::DurinCodeGen::FInt32PropertyParams Params{
					"Historical_DEPRECATED", Durin::EPropertyFlags::Deprecated, 1, 0};
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.InvalidDeprecation"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				constexpr Durin::FPropertyMetadataParams Metadata{
					.Step = Durin::FPropertyMetadataNumber::FromUnsigned(1)
				};
				auto Params = Durin::DurinCodeGen::WithTypedMetadata(
					Durin::DurinCodeGen::FInt32PropertyParams{
						"WrongNumericChannel", Durin::EPropertyFlags::Edit, 1, 0
					}, &Metadata);
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.InvalidTypedMetadata"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				Durin::DurinCodeGen::FInt32PropertyParams Params = {
					"KindMismatch", Durin::EPropertyFlags::None, 1, 0
				};
				Params.Kind = Durin::DurinCodeGen::EPropertyGenFlags::Struct;
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.KindLayoutMismatch"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FStructPropertyParams Params = {
					"MissingResolver", Durin::EPropertyFlags::None, 1, 0, nullptr
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"StructPropertyRegistration.MissingResolver"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FStructPropertyParams Params = {
					"NullDescriptor", Durin::EPropertyFlags::None, 1, 0, &GetNullStructDescriptor
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"StructPropertyRegistration.NullDescriptor"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FStructPropertyParams Params = {
					"InvalidSize", Durin::EPropertyFlags::None, 1, 0, &GetZeroSizeStructDescriptor
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"StructPropertyRegistration.InvalidSize"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FStructPropertyParams Params = {
					"InvalidAlignment", Durin::EPropertyFlags::None, 1, 0, &GetInvalidAlignmentStructDescriptor
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"StructPropertyRegistration.InvalidAlignment"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FStructPropertyParams Params =
					Durin::DurinCodeGen::FStructPropertyParams::WithAccessors(
						"AccessorPairMismatch",
						Durin::EPropertyFlags::None,
						1,
						&Durin::Z_Construct_DStruct_FVector3,
						static_cast<Durin::DurinCodeGen::FStructPropertyParams::FMutableValueAccessor>(&GetAccessedVector),
						nullptr
					);
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.AccessorPairMismatch"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				Durin::DurinCodeGen::FStructPropertyParams Params = {
					"MetadataMismatch",
					Durin::EPropertyFlags::None,
					1,
					0,
					&Durin::Z_Construct_DStruct_FVector3
				};
				Params.NumMetaData = 1;
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.MetadataMismatch"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FEnumPropertyParams Params = {
					"MissingEnumResolver", Durin::EPropertyFlags::None, 1, 0, nullptr
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"EnumPropertyRegistration.InvalidDescriptor"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FEnumPropertyParams Params = {
					"InvalidEnumSize", Durin::EPropertyFlags::None, 1, 0,
					&GetInvalidBuiltInLeafEnumForTest
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"EnumPropertyRegistration.InvalidDescriptor"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				auto Params = Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>(
					"MissingObjectAccess", Durin::EPropertyFlags::None, 1, 0,
					&Durin::DObject::StaticClass
				);
				Params.ReadObjectValue = nullptr;
				ConstructInvalidStructProperty(Params);
			}()),
			"ObjectPropertyRegistration.InvalidDescriptor"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const Durin::DurinCodeGen::FGenericPropertyParams Params = {
					"InvalidGeneric", Durin::EPropertyFlags::None, 1, 0, 0,
					Durin::DurinCodeGen::MakePropertyValueOps<int32>()
				};
				ConstructInvalidStructProperty(Params);
			}()),
			"GenericPropertyRegistration.InvalidDescriptor"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				const auto Params = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<Durin::TWeakObjectPtr<Durin::DObject>>(
					"PersistentWeak", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass);
				ConstructInvalidStructProperty(Params);
			}()),
			"WeakObjectPropertyRegistration.NonTransient"
		);
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				using FWeakPtr = Durin::TWeakObjectPtr<Durin::DObject>;
				const auto Key = Durin::DurinCodeGen::FWeakObjectPropertyParams::Create<FWeakPtr>(
					"WeakKey", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass);
				const Durin::DurinCodeGen::FInt32PropertyParams Value{
					"Value", Durin::EPropertyFlags::None, 1, 0};
				const Durin::DurinCodeGen::FMapPropertyParams Params = {
					"WeakKeyMap", Durin::EPropertyFlags::Transient, 1, 0,
					&Key, &Value, &GMapPropertyHelper<std::string, int32>};
				ConstructInvalidStructProperty(Params);
			}()),
			"MapPropertyRegistration.WeakKeyUnsupported"
		);
	}
#endif
} // namespace

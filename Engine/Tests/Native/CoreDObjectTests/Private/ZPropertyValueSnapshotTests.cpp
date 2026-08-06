#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>

#include <bit>

namespace StructConsumerTest
{
	struct FLifetimeTracked
	{
		static inline int DefaultConstructCount = 0;
		static inline int CopyConstructCount = 0;
		static inline int CopyAssignCount = 0;
		static inline int DestroyCount = 0;

		FLifetimeTracked() { ++DefaultConstructCount; }
		FLifetimeTracked(const FLifetimeTracked& Other)
			: Value(Other.Value)
		{
			++CopyConstructCount;
		}
		auto operator=(const FLifetimeTracked& Other) -> FLifetimeTracked&
		{
			Value = Other.Value;
			++CopyAssignCount;
			return *this;
		}
		~FLifetimeTracked() { ++DestroyCount; }

		static auto ResetCounts() -> void
		{
			DefaultConstructCount = 0;
			CopyConstructCount = 0;
			CopyAssignCount = 0;
			DestroyCount = 0;
		}

		Durin::int32 Value = 11;
	};

	struct FNoDefault
	{
		FNoDefault() = delete;
		explicit FNoDefault(Durin::int32 InValue)
			: Value(InValue)
		{
		}
		Durin::int32 Value = 0;
	};

	struct FMoveOnly
	{
		FMoveOnly() = default;
		FMoveOnly(const FMoveOnly&) = delete;
		auto operator=(const FMoveOnly&) -> FMoveOnly& = delete;
		FMoveOnly(FMoveOnly&&) = default;
		auto operator=(FMoveOnly&&) -> FMoveOnly& = default;
	};

	struct FImmovable
	{
		FImmovable() = default;
		FImmovable(const FImmovable&) = delete;
		auto operator=(const FImmovable&) -> FImmovable& = delete;
		FImmovable(FImmovable&&) = delete;
		auto operator=(FImmovable&&) -> FImmovable& = delete;
	};

	struct FCustomEquality
	{
		Durin::int32 Reflected = 0;
		Durin::int32 Semantic = 0;
	};

	struct FHiddenReference
	{
		Durin::TObjectPtr<Durin::DObject> Hidden;
	};

	struct FCustomArchiveValue
	{
		Durin::int32 Value = 0;
		Durin::int32 Derived = 0;
	};
} // namespace StructConsumerTest

namespace Durin
{
	template<>
	struct TDStructOpsTraits<StructConsumerTest::FCustomEquality>
		: TDStructOpsTraitsBase<StructConsumerTest::FCustomEquality>
	{
		static constexpr bool bWithIdentical = true;
		static auto Identical(
			const StructConsumerTest::FCustomEquality& Left,
			const StructConsumerTest::FCustomEquality& Right
		) -> bool
		{
			return Left.Semantic == Right.Semantic;
		}
	};

	template<>
	struct TDStructOpsTraits<StructConsumerTest::FHiddenReference>
		: TDStructOpsTraitsBase<StructConsumerTest::FHiddenReference>
	{
		static inline int CollectCount = 0;
		static constexpr bool bWithReferenceCollector = true;
		static auto CollectReferences(
			StructConsumerTest::FHiddenReference& Value,
			FReferenceCollector& Collector
		) -> void
		{
			++CollectCount;
			DObject* Object = Value.Hidden.Get();
			Collector.AddReferencedObject(Object);
		}
	};

	template<>
	struct TDStructOpsTraits<StructConsumerTest::FCustomArchiveValue>
		: TDStructOpsTraitsBase<StructConsumerTest::FCustomArchiveValue>
	{
		static inline int SerializeCount = 0;
		static inline int PostDeserializeCount = 0;
		static inline EDStructDeserializeSource LastSource =
			EDStructDeserializeSource::AuthoredAsset;
		static constexpr bool bWithSerializer = true;
		static constexpr bool bWithPostDeserialize = true;

		static auto Serialize(
			FArchive& Archive,
			StructConsumerTest::FCustomArchiveValue& Value
		) -> void
		{
			++SerializeCount;
			Archive << Value.Value;
		}

		static auto PostDeserialize(
			StructConsumerTest::FCustomArchiveValue& Value,
			FDStructPostDeserializeContext& Context
		) -> bool
		{
			++PostDeserializeCount;
			LastSource = Context.Source;
			if (Value.Value < 0) return Context.Fail("Custom archive value must be non-negative.");
			Value.Derived = Value.Value * 2;
			return true;
		}
	};
} // namespace Durin

namespace
{
	void EnsureSnapshotTestsInitialized()
	{
		static const bool bInitialized = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			if (!Durin::IsFNameInitialized()) Durin::FNameInit();
			if (!Durin::FindClassByQualifiedName("Durin::DObject")) Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}

	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64
	{
		return static_cast<Durin::uint64>(static_cast<const std::vector<T>*>(Container)->size());
	}

	template<typename T>
	auto VectorElement(const void* Container, Durin::uint64 Index) -> const void*
	{
		return &(*static_cast<const std::vector<T>*>(Container))[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto MutableVectorElement(void* Container, Durin::uint64 Index) -> void*
	{
		return &(*static_cast<std::vector<T>*>(Container))[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto ResizeVector(void* Container, Durin::uint64 Num) -> bool
	{
		auto* Value = static_cast<std::vector<T>*>(Container);
		while (Value->size() > Num)
			Value->pop_back();
		if constexpr (std::is_default_constructible_v<T>)
		{
			Value->resize(static_cast<size_t>(Num));
			return true;
		}
		return Value->size() == Num;
	}

	template<typename T>
	auto GSnapshotVectorHelper() -> const Durin::FArrayOps*
	{
		return Durin::ResolveArrayOps<std::vector<T>>();
	}

	using FEqualityMap = std::unordered_map<Durin::int32, std::string>;
	auto GEqualityMapHelper() -> const Durin::FMapOps*
	{
		return Durin::ResolveMapOps<FEqualityMap>();
	}

	struct FSnapshotOwner
	{
		Durin::int32 Value = 0;
		std::string Label;
		Durin::FName Name;
		Durin::FGuid Guid;
		std::vector<Durin::FGuid> Guids;
		std::vector<Durin::TObjectPtr<Durin::DObject>> References;
	};

	struct FMathSnapshotOwner
	{
		Durin::FVector3 Vector{0.0};
		Durin::FTransform Transform;
		std::vector<Durin::FVector3> Vectors;
	};

	struct FNestedGuid
	{
		Durin::FGuid Value;
	};

	struct FNestedGuidOwner
	{
		FNestedGuid Nested;
	};

	auto ContainsObject(const Durin::DObject* Object) -> bool
	{
		return Durin::GDObjectArray.Contains(Object);
	}

	TEST(FPropertyValueSnapshotTests, RestoresScalarStringAndNameValues)
	{
		EnsureSnapshotTestsInitialized();
		Durin::FNumericProperty ValueProperty(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FStringProperty LabelProperty(
			Durin::FFieldVariant(), Durin::FName("Label"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Label)),
			static_cast<Durin::uint16>(sizeof(std::string)), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
		);
		Durin::FNameProperty NameProperty(
			Durin::FFieldVariant(), Durin::FName("Name"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Name)),
			static_cast<Durin::uint16>(sizeof(Durin::FName)), Durin::DurinCodeGen::EPropertyGenFlags::Name, nullptr
		);
		FSnapshotOwner Owner;
		Owner.Value = 17;
		Owner.Label = "before";
		Owner.Name = Durin::FName("BeforeName_3");
		Durin::FPropertyValueSnapshot ValueSnapshot;
		Durin::FPropertyValueSnapshot LabelSnapshot;
		Durin::FPropertyValueSnapshot NameSnapshot;
		std::string Error;

		ASSERT_TRUE(Durin::CapturePropertyValue(&ValueProperty, &Owner, 0, ValueSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&LabelProperty, &Owner, 0, LabelSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&NameProperty, &Owner, 0, NameSnapshot, &Error)) << Error;
		Owner.Value = 91;
		Owner.Label = "after";
		Owner.Name = Durin::FName("AfterName");
		ASSERT_TRUE(Durin::RestorePropertyValue(&ValueProperty, &Owner, 0, ValueSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&LabelProperty, &Owner, 0, LabelSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&NameProperty, &Owner, 0, NameSnapshot, &Error)) << Error;

		EXPECT_EQ(Owner.Value, 17);
		EXPECT_EQ(Owner.Label, "before");
		EXPECT_EQ(Owner.Name.ToString(), "BeforeName_3");
		Durin::FPropertyValueSnapshot Duplicate = LabelSnapshot;
		EXPECT_EQ(Duplicate, LabelSnapshot);
		EXPECT_FALSE(Durin::RestorePropertyValue(&ValueProperty, &Owner, 0, LabelSnapshot, &Error));
		EXPECT_FALSE(Error.empty());
	}

	TEST(FPropertyValueSnapshotTests, RestoresDirectNestedAndArrayGuidValuesByteForByte)
	{
		EnsureSnapshotTestsInitialized();
		Durin::FGuidProperty GuidProperty(
			Durin::FFieldVariant(), Durin::FName("Guid"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Guid)),
			static_cast<Durin::uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		Durin::FArrayProperty GuidArrayProperty(
			Durin::FFieldVariant(), Durin::FName("Guids"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Guids)),
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::FGuid>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, GSnapshotVectorHelper<Durin::FGuid>()
		);
		Durin::FGuidProperty GuidArrayInner(
			Durin::FFieldVariant(&GuidArrayProperty), Durin::FName("Guids_Inner"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, static_cast<Durin::uint16>(sizeof(Durin::FGuid)),
			Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		GuidArrayProperty.SetInner(&GuidArrayInner);

		Durin::DStruct NestedStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FNestedGuid"), Durin::FName("FNestedGuid"),
			sizeof(FNestedGuid), alignof(FNestedGuid), Durin::EObjectFlags::Transient
		);
		NestedStruct.InitializeOps(&Durin::GetDStructOps<FNestedGuid>());
		Durin::FGuidProperty NestedValueProperty(
			Durin::FFieldVariant(&NestedStruct), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FNestedGuid, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		NestedStruct.ChildProperties = &NestedValueProperty;
		Durin::FStructProperty NestedProperty(
			Durin::FFieldVariant(), Durin::FName("Nested"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FNestedGuidOwner, Nested)),
			&NestedStruct
		);

		const Durin::FGuid Direct(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
		const std::vector<Durin::FGuid> Array{
			Durin::FGuid(1, 2, 3, 4), Durin::FGuid(0xffffffff, 0xabcdef01, 0x23456789, 0x87654321)
		};
		const Durin::FGuid Nested(0xdeadbeef, 0xcafebabe, 0x10203040, 0x50607080);
		FSnapshotOwner Owner;
		Owner.Guid = Direct;
		Owner.Guids = Array;
		FNestedGuidOwner NestedOwner{{Nested}};
		Durin::FPropertyValueSnapshot DirectSnapshot, ArraySnapshot, NestedSnapshot;
		std::string Error;

		ASSERT_TRUE(Durin::CapturePropertyValue(&GuidProperty, &Owner, 0, DirectSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&GuidArrayProperty, &Owner, 0, ArraySnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&NestedProperty, &NestedOwner, 0, NestedSnapshot, &Error)) << Error;
		EXPECT_EQ(DirectSnapshot.GetBytes().size(), sizeof(Durin::FGuid));
		Owner.Guid = {};
		Owner.Guids = {Durin::FGuid(9, 9, 9, 9)};
		NestedOwner.Nested.Value = {};
		ASSERT_TRUE(Durin::RestorePropertyValue(&GuidProperty, &Owner, 0, DirectSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&GuidArrayProperty, &Owner, 0, ArraySnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&NestedProperty, &NestedOwner, 0, NestedSnapshot, &Error)) << Error;

		EXPECT_EQ(Owner.Guid, Direct);
		EXPECT_EQ(Owner.Guids, Array);
		EXPECT_EQ(NestedOwner.Nested.Value, Nested);
	}

	TEST(FPropertyValueSnapshotTests, RestoresIntrinsicStructsDirectlyAndThroughTransformAndArray)
	{
		EnsureSnapshotTestsInitialized();
		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_Durin_FVector3();
		Durin::DStruct* TransformStruct = Durin::Z_Construct_DStruct_Durin_FTransform();
		ASSERT_NE(VectorStruct, nullptr);
		ASSERT_NE(TransformStruct, nullptr);
		Durin::FStructProperty VectorProperty(
			Durin::FFieldVariant(), Durin::FName("Vector"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(FMathSnapshotOwner, Vector)), VectorStruct
		);
		Durin::FStructProperty TransformProperty(
			Durin::FFieldVariant(), Durin::FName("Transform"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(FMathSnapshotOwner, Transform)), TransformStruct
		);
		Durin::FArrayProperty VectorsProperty(
			Durin::FFieldVariant(), Durin::FName("Vectors"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(FMathSnapshotOwner, Vectors)),
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::FVector3>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr,
			GSnapshotVectorHelper<Durin::FVector3>()
		);
		Durin::FStructProperty VectorsInner(
			Durin::FFieldVariant(&VectorsProperty), Durin::FName("Vectors_Inner"),
			Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1, 0, VectorStruct
		);
		VectorsProperty.SetInner(&VectorsInner);

		const double PayloadNaN = std::bit_cast<double>(Durin::uint64{0x7ff8000000000042ull});
		FMathSnapshotOwner Expected;
		Expected.Vector = {PayloadNaN, -0.0, 3.0};
		Expected.Transform.Translation = {-0.0, PayloadNaN, 9.0};
		Expected.Transform.Scale3D = {1.0, -0.0, PayloadNaN};
		Expected.Vectors = {{0.0, -0.0, PayloadNaN}, {4.0, 5.0, 6.0}};
		FMathSnapshotOwner Owner = Expected;
		Durin::FPropertyValueSnapshot VectorSnapshot;
		Durin::FPropertyValueSnapshot TransformSnapshot;
		Durin::FPropertyValueSnapshot VectorsSnapshot;
		std::string Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&VectorProperty, &Owner, 0, VectorSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&TransformProperty, &Owner, 0, TransformSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&VectorsProperty, &Owner, 0, VectorsSnapshot, &Error)) << Error;

		Owner.Vector = Durin::FVector3(0.0);
		Owner.Transform = Durin::FTransform();
		Owner.Vectors = {Durin::FVector3(99.0)};
		ASSERT_TRUE(Durin::RestorePropertyValue(&VectorProperty, &Owner, 0, VectorSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&TransformProperty, &Owner, 0, TransformSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&VectorsProperty, &Owner, 0, VectorsSnapshot, &Error)) << Error;

		auto ExpectVectorBits = [](const Durin::FVector3& Actual, const Durin::FVector3& ExpectedValue) {
			EXPECT_EQ(std::bit_cast<Durin::uint64>(Actual.x), std::bit_cast<Durin::uint64>(ExpectedValue.x));
			EXPECT_EQ(std::bit_cast<Durin::uint64>(Actual.y), std::bit_cast<Durin::uint64>(ExpectedValue.y));
			EXPECT_EQ(std::bit_cast<Durin::uint64>(Actual.z), std::bit_cast<Durin::uint64>(ExpectedValue.z));
		};
		ExpectVectorBits(Owner.Vector, Expected.Vector);
		EXPECT_EQ(Owner.Transform.Rotation, Expected.Transform.Rotation);
		ExpectVectorBits(Owner.Transform.Translation, Expected.Transform.Translation);
		ExpectVectorBits(Owner.Transform.Scale3D, Expected.Transform.Scale3D);
		ASSERT_EQ(Owner.Vectors.size(), Expected.Vectors.size());
		for (size_t Index = 0; Index < Owner.Vectors.size(); ++Index)
			ExpectVectorBits(Owner.Vectors[Index], Expected.Vectors[Index]);
	}

	TEST(FPropertyValueSnapshotTests, KeepsNestedObjectReferencesAliveUntilReleased)
	{
		EnsureSnapshotTestsInitialized();
		Durin::FArrayProperty ReferencesProperty(
			Durin::FFieldVariant(), Durin::FName("References"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, References)),
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::TObjectPtr<Durin::DObject>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr,
			GSnapshotVectorHelper<Durin::TObjectPtr<Durin::DObject>>()
		);
		Durin::FObjectProperty InnerProperty(
			Durin::FFieldVariant(&ReferencesProperty), Durin::FName("References_Inner"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true,
			[](const void* Value) -> Durin::DObject* {
				return static_cast<const Durin::TObjectPtr<Durin::DObject>*>(Value)->Get();
			},
			[](void* Value, Durin::DObject* Object) {
				*static_cast<Durin::TObjectPtr<Durin::DObject>*>(Value) = Object;
			}
		);
		ReferencesProperty.SetInner(&InnerProperty);

		Durin::DObject* First = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SnapshotReferenceA"));
		Durin::DObject* Second = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SnapshotReferenceB"));
		FSnapshotOwner Owner;
		Owner.References = {First, Second, First};
		Durin::FPropertyValueSnapshot Snapshot;
		std::string Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&ReferencesProperty, &Owner, 0, Snapshot, &Error)) << Error;
		ASSERT_EQ(Snapshot.GetReferencedObjects().size(), 2u);
		Durin::FPropertyValueSnapshot SnapshotCopy = Snapshot;
		Snapshot = {};

		Owner.References.clear();
		Durin::CollectGarbage();
		ASSERT_TRUE(ContainsObject(First));
		ASSERT_TRUE(ContainsObject(Second));
		ASSERT_TRUE(Durin::RestorePropertyValue(&ReferencesProperty, &Owner, 0, SnapshotCopy, &Error)) << Error;
		ASSERT_EQ(Owner.References.size(), 3u);
		EXPECT_EQ(Owner.References[0].Get(), First);
		EXPECT_EQ(Owner.References[1].Get(), Second);
		EXPECT_EQ(Owner.References[2].Get(), First);

		Owner.References.clear();
		SnapshotCopy = {};
		Durin::CollectGarbage();
		EXPECT_FALSE(ContainsObject(First));
		EXPECT_FALSE(ContainsObject(Second));
	}

	TEST(FReflectedStructConsumerTests, ManagedStoragePairsLifetimesAndSeparatesCopyModes)
	{
		using StructConsumerTest::FLifetimeTracked;
		FLifetimeTracked::ResetCounts();
		FLifetimeTracked Source;
		Source.Value = 37;

		Durin::DStruct Struct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FLifetimeTracked"), Durin::FName("FLifetimeTracked"),
			sizeof(FLifetimeTracked), alignof(FLifetimeTracked), Durin::EObjectFlags::Transient
		);
		Struct.InitializeOps(&Durin::GetDStructOps<FLifetimeTracked>());
		Durin::FStructProperty Property(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, &Struct
		);

		std::string Error;
		Durin::FReflectedValueStorage DefaultStorage;
		ASSERT_TRUE(DefaultStorage.DefaultConstruct(&Property, 0, &Error)) << Error;
		EXPECT_TRUE(DefaultStorage.IsLive());
		EXPECT_EQ(reinterpret_cast<uintptr_t>(DefaultStorage.GetValue()) % alignof(FLifetimeTracked), 0u);
		EXPECT_EQ(FLifetimeTracked::DefaultConstructCount, 2);
		EXPECT_FALSE(DefaultStorage.DefaultConstruct(&Property, 0, &Error));
		EXPECT_NE(Error.find("DStructOperationUnavailable"), std::string::npos);
		EXPECT_EQ(FLifetimeTracked::DefaultConstructCount, 2);

		Durin::FReflectedValueStorage CopyStorage;
		ASSERT_TRUE(CopyStorage.CopyConstruct(&Property, &Source, 0, &Error)) << Error;
		EXPECT_EQ(FLifetimeTracked::CopyConstructCount, 1);
		EXPECT_EQ(static_cast<FLifetimeTracked*>(CopyStorage.GetValue())->Value, 37);
		Source.Value = 91;
		ASSERT_TRUE(DefaultStorage.CopyAssign(&Source, &Error)) << Error;
		EXPECT_EQ(FLifetimeTracked::CopyAssignCount, 1);
		EXPECT_EQ(static_cast<FLifetimeTracked*>(DefaultStorage.GetValue())->Value, 91);

		DefaultStorage.Reset();
		CopyStorage.Reset();
		EXPECT_EQ(FLifetimeTracked::DestroyCount, 2);
	}

	TEST(FReflectedStructConsumerTests, UnavailableConstructionLeavesNestedArrayUnchanged)
	{
		using StructConsumerTest::FNoDefault;
		Durin::DStruct Struct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FNoDefault"), Durin::FName("FNoDefault"),
			sizeof(FNoDefault), alignof(FNoDefault), Durin::EObjectFlags::Transient
		);
		Struct.InitializeOps(&Durin::GetDStructOps<FNoDefault>());
		Durin::FStructProperty Inner(
			Durin::FFieldVariant(), Durin::FName("Values_Inner"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, &Struct
		);
		Durin::FArrayProperty Array(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::vector<FNoDefault>),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, GSnapshotVectorHelper<FNoDefault>()
		);
		Array.SetInner(&Inner);
		std::vector<FNoDefault> Values;
		Values.emplace_back(7);
		std::string Error;

		EXPECT_FALSE(Array.Resize(&Values, 2, 0, &Error));
		EXPECT_EQ(Values.size(), 1u);
		EXPECT_EQ(Values[0].Value, 7);
		EXPECT_NE(Error.find("DStructOperationUnavailable"), std::string::npos);
		EXPECT_TRUE(Array.Resize(&Values, 0, 0, &Error)) << Error;
		EXPECT_TRUE(Values.empty());

		Durin::FReflectedValueStorage Storage;
		EXPECT_FALSE(Storage.DefaultConstruct(&Inner, 0, &Error));
		EXPECT_EQ(Storage.GetContainer(), nullptr);
		EXPECT_NE(Error.find("DefaultConstruct"), std::string::npos);

		Durin::DStruct MoveOnlyStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FMoveOnly"), Durin::FName("FMoveOnly"),
			sizeof(StructConsumerTest::FMoveOnly), alignof(StructConsumerTest::FMoveOnly),
			Durin::EObjectFlags::Transient
		);
		MoveOnlyStruct.InitializeOps(&Durin::GetDStructOps<StructConsumerTest::FMoveOnly>());
		Durin::FMapProperty Map(
			Durin::FFieldVariant(), Durin::FName("Map"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(FEqualityMap),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, GEqualityMapHelper()
		);
		Durin::FNumericProperty Key(
			Durin::FFieldVariant(&Map), Durin::FName("Map_Key"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FStructProperty MoveOnlyValue(
			Durin::FFieldVariant(&Map), Durin::FName("Map_Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, &MoveOnlyStruct
		);
		Map.SetKeyProp(&Key);
		Map.SetValueProp(&MoveOnlyValue);
		FEqualityMap MapValue;
		const Durin::int32 MapKey = 3;
		const StructConsumerTest::FMoveOnly ProposedValue;
		EXPECT_FALSE(Map.Insert(&MapValue, &MapKey, &ProposedValue, 0, &Error));
		EXPECT_TRUE(MapValue.empty());
		EXPECT_NE(Error.find("CopyConstruct/CopyAssign"), std::string::npos);
	}

	TEST(FReflectedContainerOpsTests, ArchiveLoadingRollsBackFailureAndRejectsDuplicateKeys)
	{
		Durin::FNumericProperty ArrayInner(
			Durin::FFieldVariant(), Durin::FName("Values_Inner"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FArrayProperty ArrayProperty(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::vector<Durin::int32>),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr,
			GSnapshotVectorHelper<Durin::int32>()
		);
		ArrayProperty.SetInner(&ArrayInner);

		std::vector<Durin::int32> SourceArray{11, 22};
		std::vector<Durin::uint8> ArrayBytes;
		Durin::FMemoryWriter ArrayWriter(ArrayBytes);
		Durin::SerializeReflectedPropertyValue(ArrayWriter, &ArrayProperty, &SourceArray);
		ASSERT_FALSE(ArrayWriter.HasError()) << ArrayWriter.GetError();
		ASSERT_GT(ArrayBytes.size(), 1u);
		ArrayBytes.pop_back();

		std::vector<Durin::int32> DestinationArray{7, 8, 9};
		Durin::FMemoryReader ArrayReader(ArrayBytes);
		Durin::SerializeReflectedPropertyValue(ArrayReader, &ArrayProperty, &DestinationArray);
		ASSERT_TRUE(ArrayReader.HasError());
		EXPECT_EQ(DestinationArray, (std::vector<Durin::int32>{7, 8, 9}));

		std::vector<Durin::uint8> OversizedBytes;
		Durin::FMemoryWriter OversizedWriter(OversizedBytes);
		Durin::uint64 OversizedCount = 10000001;
		OversizedWriter << OversizedCount;
		std::vector<Durin::int32> OversizedDestination{31, 32};
		Durin::FMemoryReader OversizedReader(OversizedBytes);
		Durin::SerializeReflectedPropertyValue(
			OversizedReader, &ArrayProperty, &OversizedDestination
		);
		EXPECT_TRUE(OversizedReader.HasError());
		EXPECT_EQ(OversizedDestination, (std::vector<Durin::int32>{31, 32}));

		std::vector<Durin::uint8> ValidArrayBytes;
		Durin::FMemoryWriter ValidArrayWriter(ValidArrayBytes);
		Durin::SerializeReflectedPropertyValue(ValidArrayWriter, &ArrayProperty, &SourceArray);
		ASSERT_FALSE(ValidArrayWriter.HasError());
		Durin::FArrayOps CommitFailureOps = *Durin::ResolveArrayOps<std::vector<Durin::int32>>();
		CommitFailureOps.Commit = [](void*, void*) { return Durin::EContainerOpResult::BackendRejected; };
		Durin::FArrayProperty CommitFailureProperty(
			Durin::FFieldVariant(), Durin::FName("CommitFailureValues"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::vector<Durin::int32>),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, &CommitFailureOps
		);
		CommitFailureProperty.SetInner(&ArrayInner);
		std::vector<Durin::int32> CommitFailureDestination{41, 42};
		Durin::FMemoryReader CommitFailureReader(ValidArrayBytes);
		Durin::SerializeReflectedPropertyValue(
			CommitFailureReader, &CommitFailureProperty, &CommitFailureDestination
		);
		EXPECT_TRUE(CommitFailureReader.HasError());
		EXPECT_NE(CommitFailureReader.GetError().find("Commit"), std::string_view::npos);
		EXPECT_EQ(CommitFailureDestination, (std::vector<Durin::int32>{41, 42}));

		Durin::FArrayOps ConstructionFailureOps = *Durin::ResolveArrayOps<std::vector<Durin::int32>>();
		ConstructionFailureOps.CreateDetached = [](void**) { return Durin::EContainerOpResult::ConstructionFailure; };
		Durin::FArrayProperty ConstructionFailureProperty(
			Durin::FFieldVariant(), Durin::FName("ConstructionFailureValues"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::vector<Durin::int32>),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, &ConstructionFailureOps
		);
		ConstructionFailureProperty.SetInner(&ArrayInner);
		std::vector<Durin::int32> ConstructionFailureDestination{51, 52};
		Durin::FMemoryReader ConstructionFailureReader(ValidArrayBytes);
		Durin::SerializeReflectedPropertyValue(
			ConstructionFailureReader, &ConstructionFailureProperty, &ConstructionFailureDestination
		);
		EXPECT_TRUE(ConstructionFailureReader.HasError());
		EXPECT_EQ(ConstructionFailureDestination, (std::vector<Durin::int32>{51, 52}));

		Durin::FMapProperty MapProperty(
			Durin::FFieldVariant(), Durin::FName("Map"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(FEqualityMap),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, GEqualityMapHelper()
		);
		Durin::FNumericProperty MapKey(
			Durin::FFieldVariant(&MapProperty), Durin::FName("Map_Key"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FStringProperty MapValue(
			Durin::FFieldVariant(&MapProperty), Durin::FName("Map_Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
			Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
		);
		MapProperty.SetKeyProp(&MapKey);
		MapProperty.SetValueProp(&MapValue);

		std::vector<Durin::uint8> MapBytes;
		Durin::FMemoryWriter MapWriter(MapBytes);
		Durin::uint64 MapCount = 2;
		Durin::int32 DuplicateKey = 1;
		std::string FirstValue = "first";
		std::string SecondValue = "second";
		MapWriter << MapCount << DuplicateKey;
		MapWriter.SerializeString(FirstValue);
		MapWriter << DuplicateKey;
		MapWriter.SerializeString(SecondValue);
		ASSERT_FALSE(MapWriter.HasError()) << MapWriter.GetError();

		FEqualityMap DestinationMap{{9, "sentinel"}};
		Durin::FMemoryReader MapReader(MapBytes);
		Durin::SerializeReflectedPropertyValue(MapReader, &MapProperty, &DestinationMap);
		ASSERT_TRUE(MapReader.HasError());
		EXPECT_NE(MapReader.GetError().find("MapDuplicateKey"), std::string_view::npos);
		ASSERT_EQ(DestinationMap.size(), 1u);
		EXPECT_EQ(DestinationMap.at(9), "sentinel");
	}

	TEST(FReflectedContainerOpsTests, MapVisitorTraversesEachEntryExactlyOnce)
	{
		Durin::FMapProperty MapProperty(
			Durin::FFieldVariant(), Durin::FName("Map"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(FEqualityMap),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, GEqualityMapHelper()
		);
		Durin::FNumericProperty KeyProperty(
			Durin::FFieldVariant(&MapProperty), Durin::FName("Map_Key"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FStringProperty ValueProperty(
			Durin::FFieldVariant(&MapProperty), Durin::FName("Map_Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
			Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
		);
		MapProperty.SetKeyProp(&KeyProperty);
		MapProperty.SetValueProp(&ValueProperty);

		for (const Durin::uint64 Count : {Durin::uint64{0}, Durin::uint64{1}, Durin::uint64{4}, Durin::uint64{16}})
		{
			FEqualityMap Value;
			for (Durin::uint64 Index = 0; Index < Count; ++Index)
				Value.emplace(static_cast<Durin::int32>(Index), std::to_string(Index));

			Durin::uint64 Visits = 0;
			EXPECT_EQ(MapProperty.VisitEntries(&Value, [](void* Context, const void*, const void*) -> bool {
					++*static_cast<Durin::uint64*>(Context);
					return true; }, &Visits), Durin::EContainerOpResult::Success);
			EXPECT_EQ(Visits, Count);

			Durin::uint64 MutableVisits = 0;
			EXPECT_EQ(MapProperty.VisitMutableEntries(&Value, [](void* Context, const void*, void* Mapped) -> bool {
					++*static_cast<Durin::uint64*>(Context);
					static_cast<std::string*>(Mapped)->append("!");
					return true; }, &MutableVisits), Durin::EContainerOpResult::Success);
			EXPECT_EQ(MutableVisits, Count);

			Durin::uint64 EarlyStopVisits = 0;
			EXPECT_EQ(MapProperty.VisitEntries(&Value, [](void* Context, const void*, const void*) -> bool {
					++*static_cast<Durin::uint64*>(Context);
					return false; }, &EarlyStopVisits), Durin::EContainerOpResult::Success);
			EXPECT_EQ(EarlyStopVisits, Count == 0 ? 0u : 1u);
		}
	}

	TEST(FReflectedContainerOpsTests, DescriptorValidationAndUnsupportedCapabilitiesAreStable)
	{
		const Durin::FArrayOps* IntOps = Durin::ResolveArrayOps<std::vector<Durin::int32>>();
		ASSERT_TRUE(Durin::IsValidArrayOps(IntOps));
		EXPECT_TRUE(Durin::EnumHasAllFlags(IntOps->Flags, Durin::EArrayOpsFlags::DefaultGrow | Durin::EArrayOpsFlags::Reserve));

		Durin::FArrayOps InvalidVersion = *IntOps;
		++InvalidVersion.Version;
		EXPECT_FALSE(Durin::IsValidArrayOps(&InvalidVersion));
		Durin::FArrayOps MissingCallback = *IntOps;
		MissingCallback.VisitConst = nullptr;
		EXPECT_FALSE(Durin::IsValidArrayOps(&MissingCallback));

		const Durin::FArrayOps* ImmovableOps = Durin::ResolveArrayOps<std::vector<StructConsumerTest::FImmovable>>();
		ASSERT_TRUE(Durin::IsValidArrayOps(ImmovableOps));
		EXPECT_FALSE(Durin::EnumHasAnyFlags(ImmovableOps->Flags, Durin::EArrayOpsFlags::Reserve));
		EXPECT_FALSE(Durin::EnumHasAnyFlags(ImmovableOps->Flags, Durin::EArrayOpsFlags::DefaultGrow));
		std::vector<StructConsumerTest::FImmovable> Values;
		EXPECT_EQ(ImmovableOps->Resize(&Values, 1), Durin::EContainerOpResult::Unsupported);
		EXPECT_TRUE(Values.empty());
	}

	TEST(FReflectedContainerOpsTests, DetachedCommitOwnsOldStorageAndRollbackLeavesDestinationUntouched)
	{
		using FTracked = StructConsumerTest::FLifetimeTracked;
		using FTrackedArray = std::vector<FTracked>;
		const Durin::FArrayOps& ArrayOps = *Durin::ResolveArrayOps<FTrackedArray>();
		FTracked::ResetCounts();
		{
			FTrackedArray Destination(1);
			Destination[0].Value = 7;
			Durin::FDetachedContainerStorage Detached;
			ASSERT_EQ(Detached.Create(ArrayOps), Durin::EContainerOpResult::Success);
			ASSERT_EQ(ArrayOps.Resize(Detached.Get(), 2), Durin::EContainerOpResult::Success);
			EXPECT_EQ(Destination.size(), 1u);
			EXPECT_EQ(Destination[0].Value, 7);
			ASSERT_EQ(ArrayOps.Commit(&Destination, Detached.Get()), Durin::EContainerOpResult::Success);
			EXPECT_EQ(Destination.size(), 2u);
			Detached.Reset();
			EXPECT_EQ(FTracked::DestroyCount, 2);
		}
		EXPECT_EQ(FTracked::DefaultConstructCount, 3);
		EXPECT_EQ(FTracked::CopyConstructCount, 1);
		EXPECT_EQ(FTracked::DestroyCount, 4);

		using FMap = std::unordered_map<Durin::int32, std::string>;
		const Durin::FMapOps& MapOps = *Durin::ResolveMapOps<FMap>();
		FMap Destination{{9, "sentinel"}};
		Durin::FDetachedContainerStorage Detached;
		ASSERT_EQ(Detached.Create(MapOps), Durin::EContainerOpResult::Success);
		const Durin::int32 Key = 1;
		const std::string First = "first";
		const std::string Duplicate = "duplicate";
		EXPECT_EQ(MapOps.InsertCopy(Detached.Get(), &Key, &First), Durin::EContainerOpResult::Success);
		EXPECT_EQ(MapOps.InsertCopy(Detached.Get(), &Key, &Duplicate), Durin::EContainerOpResult::DuplicateKey);
		EXPECT_EQ(Destination, (FMap{{9, "sentinel"}}));
		Detached.Reset();
		EXPECT_EQ(Destination, (FMap{{9, "sentinel"}}));
	}

	TEST(FReflectedStructConsumerTests, LogicalEqualityUsesFieldsAssociationsAndExactFloatingBits)
	{
		Durin::FNumericProperty DoubleProperty(
			Durin::FFieldVariant(), Durin::FName("Double"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(double),
			Durin::DurinCodeGen::EPropertyGenFlags::Double, nullptr
		);
		double PositiveZero = 0.0;
		double NegativeZero = -0.0;
		EXPECT_FALSE(Durin::ArePropertyValuesIdentical(
			&DoubleProperty, &PositiveZero, 0, &NegativeZero, 0
		));
		const double NaN = std::bit_cast<double>(Durin::uint64{0x7ff8000000000042ull});
		const double SameNaN = std::bit_cast<double>(Durin::uint64{0x7ff8000000000042ull});
		const double OtherNaN = std::bit_cast<double>(Durin::uint64{0x7ff8000000000043ull});
		EXPECT_TRUE(Durin::ArePropertyValuesIdentical(&DoubleProperty, &NaN, 0, &SameNaN, 0));
		EXPECT_FALSE(Durin::ArePropertyValuesIdentical(&DoubleProperty, &NaN, 0, &OtherNaN, 0));

		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_Durin_FVector3();
		Durin::FStructProperty VectorProperty(
			Durin::FFieldVariant(), Durin::FName("Vector"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, VectorStruct
		);
		Durin::FVector3 LeftVector(0.0);
		Durin::FVector3 RightVector(0.0);
		RightVector.x = -0.0;
		EXPECT_FALSE(Durin::ArePropertyValuesIdentical(&VectorProperty, &LeftVector, 0, &RightVector, 0));
		LeftVector.x = NaN;
		RightVector.x = SameNaN;
		EXPECT_TRUE(Durin::ArePropertyValuesIdentical(&VectorProperty, &LeftVector, 0, &RightVector, 0));
		RightVector.x = OtherNaN;
		EXPECT_FALSE(Durin::ArePropertyValuesIdentical(&VectorProperty, &LeftVector, 0, &RightVector, 0));

		Durin::DStruct CustomStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FCustomEquality"), Durin::FName("FCustomEquality"),
			sizeof(StructConsumerTest::FCustomEquality), alignof(StructConsumerTest::FCustomEquality),
			Durin::EObjectFlags::Transient
		);
		CustomStruct.InitializeOps(&Durin::GetDStructOps<StructConsumerTest::FCustomEquality>());
		Durin::FStructProperty CustomProperty(
			Durin::FFieldVariant(), Durin::FName("Custom"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, &CustomStruct
		);
		StructConsumerTest::FCustomEquality LeftCustom{1, 7};
		StructConsumerTest::FCustomEquality RightCustom{99, 7};
		EXPECT_TRUE(Durin::ArePropertyValuesIdentical(&CustomProperty, &LeftCustom, 0, &RightCustom, 0));
		RightCustom.Semantic = 8;
		EXPECT_FALSE(Durin::ArePropertyValuesIdentical(&CustomProperty, &LeftCustom, 0, &RightCustom, 0));

		Durin::FMapProperty MapProperty(
			Durin::FFieldVariant(), Durin::FName("Map"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(FEqualityMap),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, GEqualityMapHelper()
		);
		Durin::FNumericProperty KeyProperty(
			Durin::FFieldVariant(&MapProperty), Durin::FName("Map_Key"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FStringProperty ValueProperty(
			Durin::FFieldVariant(&MapProperty), Durin::FName("Map_Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
			Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
		);
		MapProperty.SetKeyProp(&KeyProperty);
		MapProperty.SetValueProp(&ValueProperty);
		MapProperty.SetValueLifecycle(
			sizeof(FEqualityMap), alignof(FEqualityMap),
			&Durin::DurinCodeGen::InitializePropertyValue<FEqualityMap>,
			&Durin::DurinCodeGen::DestroyPropertyValue<FEqualityMap>
		);
		FEqualityMap LeftMap;
		LeftMap.emplace(1, "one");
		LeftMap.emplace(2, "two");
		FEqualityMap RightMap;
		RightMap.emplace(2, "two");
		RightMap.emplace(1, "one");
		EXPECT_TRUE(Durin::ArePropertyValuesIdentical(&MapProperty, &LeftMap, 0, &RightMap, 0));
		RightMap[2] = "changed";
		EXPECT_FALSE(Durin::ArePropertyValuesIdentical(&MapProperty, &LeftMap, 0, &RightMap, 0));
		RightMap[2] = "two";
		Durin::FPropertyValueSnapshot LeftSnapshot;
		Durin::FPropertyValueSnapshot RightSnapshot;
		std::string Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&MapProperty, &LeftMap, 0, LeftSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&MapProperty, &RightMap, 0, RightSnapshot, &Error)) << Error;
		EXPECT_EQ(LeftSnapshot, RightSnapshot);
		EXPECT_EQ(LeftSnapshot.GetBytes(), RightSnapshot.GetBytes());
	}

	TEST(FReflectedStructConsumerTests, HiddenReferencesAreCollectedAndRootedBySnapshots)
	{
		EnsureSnapshotTestsInitialized();
		using FHiddenReference = StructConsumerTest::FHiddenReference;
		Durin::DStruct Struct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FHiddenReference"), Durin::FName("FHiddenReference"),
			sizeof(FHiddenReference), alignof(FHiddenReference), Durin::EObjectFlags::Transient
		);
		Struct.InitializeOps(&Durin::GetDStructOps<FHiddenReference>());
		Durin::FStructProperty Property(
			Durin::FFieldVariant(), Durin::FName("Hidden"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, &Struct
		);
		Durin::DObject* Referenced = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("HiddenSnapshotReference"));
		FHiddenReference Value{Referenced};
		Durin::TDStructOpsTraits<FHiddenReference>::CollectCount = 0;
		Durin::FPropertyValueSnapshot Snapshot;
		std::string Error;

		ASSERT_TRUE(Durin::CapturePropertyValue(&Property, &Value, 0, Snapshot, &Error)) << Error;
		ASSERT_EQ(Snapshot.GetReferencedObjects().size(), 1u);
		EXPECT_EQ(Snapshot.GetReferencedObjects()[0], Referenced);
		EXPECT_EQ(Durin::TDStructOpsTraits<FHiddenReference>::CollectCount, 1);
		Value.Hidden.Reset();
		Durin::CollectGarbage();
		EXPECT_TRUE(ContainsObject(Referenced));

		Snapshot = {};
		Durin::CollectGarbage();
		EXPECT_FALSE(ContainsObject(Referenced));
	}

	TEST(FReflectedStructConsumerTests, ArchiveStructDispatchIsTransactionalAndSticky)
	{
		using FValue = StructConsumerTest::FCustomArchiveValue;
		using FTraits = Durin::TDStructOpsTraits<FValue>;
		Durin::DStruct Struct(
			Durin::EC_StaticConstructor,
			Durin::FName("Tests::FCustomArchiveValue"),
			Durin::FName("FCustomArchiveValue"),
			sizeof(FValue), alignof(FValue), Durin::EObjectFlags::Transient
		);
		Struct.InitializeOps(&Durin::GetDStructOps<FValue>());
		Durin::FNumericProperty ReflectedValue(
			Durin::FFieldVariant(&Struct), Durin::FName("Value"),
			Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(FValue, Value)), sizeof(Durin::int32),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Struct.ChildProperties = &ReflectedValue;
		Durin::FStructProperty Property(
			Durin::FFieldVariant(), Durin::FName("Custom"),
			Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1, 0,
			&Struct
		);

		FTraits::SerializeCount = 0;
		FTraits::PostDeserializeCount = 0;
		FValue Source{21, 999};
		std::vector<Durin::uint8> Bytes;
		Durin::FMemoryWriter Writer(Bytes);
		Durin::SerializeReflectedPropertyValue(Writer, &Property, &Source);
		ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
		EXPECT_EQ(FTraits::SerializeCount, 1);
		ASSERT_EQ(Bytes.size(), sizeof(Durin::int32));

		FValue Destination{7, 14};
		Durin::FMemoryReader Reader(Bytes);
		Durin::SerializeReflectedPropertyValue(Reader, &Property, &Destination);
		ASSERT_FALSE(Reader.HasError()) << Reader.GetError();
		EXPECT_EQ(FTraits::SerializeCount, 2);
		EXPECT_EQ(FTraits::PostDeserializeCount, 1);
		EXPECT_EQ(FTraits::LastSource, Durin::EDStructDeserializeSource::RuntimeArchive);
		EXPECT_EQ(Destination.Value, 21);
		EXPECT_EQ(Destination.Derived, 42);

		std::vector<Durin::uint8> Truncated(Bytes.begin(), Bytes.end() - 1);
		Destination = {7, 14};
		Durin::FMemoryReader TruncatedReader(Truncated);
		Durin::SerializeReflectedPropertyValue(TruncatedReader, &Property, &Destination);
		EXPECT_TRUE(TruncatedReader.HasError());
		EXPECT_NE(TruncatedReader.GetError().find("ArchiveFailure"), std::string_view::npos);
		EXPECT_EQ(Destination.Value, 7);
		EXPECT_EQ(Destination.Derived, 14);

		Source = {-1, 0};
		Bytes.clear();
		Durin::FMemoryWriter RejectionWriter(Bytes);
		Durin::SerializeReflectedPropertyValue(RejectionWriter, &Property, &Source);
		ASSERT_FALSE(RejectionWriter.HasError());
		Destination = {7, 14};
		Durin::FMemoryReader RejectionReader(Bytes);
		Durin::SerializeReflectedPropertyValue(RejectionReader, &Property, &Destination);
		EXPECT_TRUE(RejectionReader.HasError());
		EXPECT_NE(
			RejectionReader.GetError().find("PostDeserializeRejected"),
			std::string_view::npos
		);
		EXPECT_EQ(Destination.Value, 7);
		EXPECT_EQ(Destination.Derived, 14);
	}
} // namespace

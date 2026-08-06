#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/GarbageCollectionScheduler.h"
#include "DObject/AssetPath.h"
#include "DObject/Package.h"
#include "CoreGlobals.h"
#include "Misc/Paths.h"
#include "Math/Color.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>
#include <cstddef>

namespace StructOpsTest
{
	struct FOrdinary
	{
		Durin::int32 Value = 7;
	};

	struct FMoveOnly
	{
		Durin::int32 Value = 0;

		FMoveOnly() = default;
		FMoveOnly(const FMoveOnly&) = delete;
		auto operator=(const FMoveOnly&) -> FMoveOnly& = delete;
		FMoveOnly(FMoveOnly&&) = default;
		auto operator=(FMoveOnly&&) -> FMoveOnly& = default;
	};

	struct FDeletedDefault
	{
		FDeletedDefault() = delete;
		explicit FDeletedDefault(Durin::int32 InValue) : Value(InValue) {}
		Durin::int32 Value = 0;
	};

	struct FNonTrivial
	{
		std::string Value;
	};

	struct FCustomOps
	{
		Durin::int32 Value = 0;
	};

	struct FMalformedIdentical
	{
	};
}

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
			const StructOpsTest::FCustomOps& Right) -> bool
		{
			return Left.Value == Right.Value;
		}

		static auto Serialize(FArchive&, StructOpsTest::FCustomOps&) -> void {}
		static auto PostDeserialize(
			StructOpsTest::FCustomOps&,
			FDStructPostDeserializeContext&) -> bool { return true; }
		static auto CollectReferences(
			StructOpsTest::FCustomOps&,
			FReferenceCollector&) -> void {}
	};

	template<>
	struct TDStructOpsTraits<StructOpsTest::FMalformedIdentical>
		: TDStructOpsTraitsBase<StructOpsTest::FMalformedIdentical>
	{
		static constexpr bool bWithIdentical = true;
		static auto Identical(
			const StructOpsTest::FMalformedIdentical&,
			const StructOpsTest::FMalformedIdentical&) -> void {}
	};
}

static_assert(!Durin::Private::CValidDStructIdenticalTrait<
	StructOpsTest::FMalformedIdentical,
	Durin::TDStructOpsTraits<StructOpsTest::FMalformedIdentical>>);

namespace
{
	struct FReflectedPropertyOwnerForTest
	{
		Durin::int32 Value = 0;
		Durin::DObject* ObjectValue = nullptr;
		Durin::TObjectPtr<Durin::DObject> ObjectPtrValue;
		std::string StringValue;
		Durin::FName NameValue;
		std::vector<Durin::DObject*> ObjectArray;
		std::vector<Durin::TObjectPtr<Durin::DObject>> ObjectPtrArray;
		std::unordered_map<std::string, Durin::int32> StringToInt;
		std::vector<std::vector<Durin::int32>> NestedScores;
		std::unordered_map<std::string, std::vector<Durin::DObject*>> ObjectLists;
	};

	enum class EReflectedEnumForTest : Durin::uint8
	{
		A,
		B = 4
	};

	enum class ESignedEnumValueForTest : Durin::int8
	{
		Negative = -1,
		Positive = 1
	};

	enum class EUnsignedEnumValueForTest : Durin::uint64
	{
		Low = 0,
		High = std::numeric_limits<Durin::uint64>::max()
	};

	template<typename T>
	auto VectorPropertyNum(const void* Container) -> Durin::uint64
	{
		const auto* Value = static_cast<const std::vector<T>*>(Container);
		return static_cast<Durin::uint64>(Value->size());
	}

	template<typename T>
	auto VectorPropertyGetElement(const void* Container, Durin::uint64 Index) -> const void*
	{
		const auto* Value = static_cast<const std::vector<T>*>(Container);
		return &(*Value)[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto VectorPropertyGetMutableElement(void* Container, Durin::uint64 Index) -> void*
	{
		auto* Value = static_cast<std::vector<T>*>(Container);
		return &(*Value)[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto VectorPropertyResize(void* Container, Durin::uint64 Num) -> bool
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
	auto MapPropertyNum(const void* Container) -> Durin::uint64
	{
		return static_cast<Durin::uint64>(static_cast<const TTestMap<K, V>*>(Container)->size());
	}

	template<typename K, typename V>
	auto MapPropertyGetKey(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const TTestMap<K, V>*>(Container)->begin();
		std::advance(It, static_cast<size_t>(Index));
		return &It->first;
	}

	template<typename K, typename V>
	auto MapPropertyGetValue(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const TTestMap<K, V>*>(Container)->begin();
		std::advance(It, static_cast<size_t>(Index));
		return &It->second;
	}

	template<typename K, typename V>
	auto MapPropertyGetMutableValue(void* Container, Durin::uint64 Index) -> void*
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
		inline static Durin::uint64 BeginDestroyCount = 0;
		inline static Durin::uint64 FinishDestroyCount = 0;
		inline static Durin::uint64 DestructorCount = 0;
		bool bReadyForFinishDestroy = true;
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

	class DLifecycleReferenceOwnerForTest : public Durin::DObject
	{
	public:
		explicit DLifecycleReferenceOwnerForTest(const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DLifecycleReferenceOwnerForTest(X);
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
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, Value)),
					static_cast<Durin::uint16>(sizeof(Durin::int32)),
					Durin::DurinCodeGen::EPropertyGenFlags::Int32,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FBoolPropertyParams BoolPropertyParams = {
					"bEnabled",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, bEnabled)),
					static_cast<Durin::uint16>(sizeof(bool)),
					Durin::DurinCodeGen::EPropertyGenFlags::Bool,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FStringPropertyParams NamePropertyParams = {
					"Label",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, Label)),
					static_cast<Durin::uint16>(sizeof(std::string)),
					Durin::DurinCodeGen::EPropertyGenFlags::String,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams ReferencePropertyParams = {
					"Reference",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, Reference)),
					static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object,
					&Durin::DObject::StaticClass,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrReferencePropertyParams = {
					"ObjectPtrReference",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, ObjectPtrReference)),
					static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object,
					&Durin::DObject::StaticClass,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					true
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams RawReferencesInnerPropertyParams = {
					"RawReferences_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object,
					&Durin::DObject::StaticClass,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams RawReferencesPropertyParams = {
					"RawReferences",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, RawReferences)),
					&RawReferencesInnerPropertyParams,
					&GVectorPropertyHelper<Durin::DObject*>
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrReferencesInnerPropertyParams = {
					"ObjectPtrReferences_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object,
					&Durin::DObject::StaticClass,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					true
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ObjectPtrReferencesPropertyParams = {
					"ObjectPtrReferences",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, ObjectPtrReferences)),
					&ObjectPtrReferencesInnerPropertyParams,
					&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
				};
				static const Durin::DurinCodeGen::FInt32PropertyParams ScoresInnerPropertyParams = {
					"Scores_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					static_cast<Durin::uint16>(sizeof(Durin::int32)),
					Durin::DurinCodeGen::EPropertyGenFlags::Int32,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoresPropertyParams = {
					"Scores",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, Scores)),
					&ScoresInnerPropertyParams,
					&GVectorPropertyHelper<Durin::int32>
				};
				static const Durin::DurinCodeGen::FStringPropertyParams TagsInnerPropertyParams = {
					"Tags_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					static_cast<Durin::uint16>(sizeof(std::string)),
					Durin::DurinCodeGen::EPropertyGenFlags::String,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams TagsPropertyParams = {
					"Tags",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, Tags)),
					&TagsInnerPropertyParams,
					&GVectorPropertyHelper<std::string>
				};
				static const Durin::DurinCodeGen::FEnumPropertyParams ModesInnerPropertyParams = {
					"Modes_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					static_cast<Durin::uint16>(sizeof(EReflectedEnumForTest)),
					Durin::DurinCodeGen::EPropertyGenFlags::Enum,
					nullptr,
					&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ModesPropertyParams = {
					"Modes",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, Modes)),
					&ModesInnerPropertyParams,
					&GVectorPropertyHelper<EReflectedEnumForTest>
				};
				static const Durin::DurinCodeGen::FInt32PropertyParams ScoreGroupsInnerInnerPropertyParams = {
					"ScoreGroups_Inner_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					static_cast<Durin::uint16>(sizeof(Durin::int32)),
					Durin::DurinCodeGen::EPropertyGenFlags::Int32,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoreGroupsInnerPropertyParams = {
					"ScoreGroups_Inner",
					Durin::EPropertyFlags::None,
					1,
					0,
					&ScoreGroupsInnerInnerPropertyParams,
					&GVectorPropertyHelper<Durin::int32>
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoreGroupsPropertyParams = {
					"ScoreGroups",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, ScoreGroups)),
					&ScoreGroupsInnerPropertyParams,
					&GVectorPropertyHelper<std::vector<Durin::int32>>
				};
				static const Durin::DurinCodeGen::FInt32PropertyParams TransientPropertyParams = {
					"TransientValue",
					Durin::EPropertyFlags::Transient,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, TransientValue)),
					static_cast<Durin::uint16>(sizeof(Durin::int32)),
					Durin::DurinCodeGen::EPropertyGenFlags::Int32,
					nullptr,
					nullptr,
					nullptr,
					nullptr,
					nullptr
				};
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
					&TransientPropertyParams
				};
				static const Durin::DurinCodeGen::FClassParams ClassParams = {
					&DLifecycleReferenceOwnerForTest::StaticClassNoRegister,
					"DLifecycleReferenceOwnerForTest",
					"DLifecycleReferenceOwnerForTest",
					PropertyParams,
					12
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

		Durin::int32 Value = 0;
		bool bEnabled = false;
		std::string Label;
		Durin::DObject* Reference = nullptr;
		Durin::TObjectPtr<Durin::DObject> ObjectPtrReference;
		std::vector<Durin::DObject*> RawReferences;
		std::vector<Durin::TObjectPtr<Durin::DObject>> ObjectPtrReferences;
		std::vector<Durin::int32> Scores;
		std::vector<std::string> Tags;
		std::vector<EReflectedEnumForTest> Modes;
		std::vector<std::vector<Durin::int32>> ScoreGroups;
		Durin::int32 TransientValue = 0;
		Durin::DObject* NativeReference = nullptr;
	};

	struct FGCReferenceLeafForTest
	{
		Durin::TObjectPtr<Durin::DObject> Reference;
		Durin::TObjectPtr<Durin::DObject> StaticReferences[2];
		Durin::int32 NonReferenceValue = 0;
	};

	struct FGCReferenceNestedForTest
	{
		FGCReferenceLeafForTest Leaf;
	};

	auto GetGCReferenceLeafStructForTest() -> Durin::DStruct*
	{
		static Durin::DStruct* Struct = [] {
			static const Durin::DurinCodeGen::FObjectPropertyParams Reference = {
				"Reference", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(FGCReferenceLeafForTest, Reference)),
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const Durin::DurinCodeGen::FObjectPropertyParams StaticReferences = {
				"StaticReferences", Durin::EPropertyFlags::None, 2,
				static_cast<Durin::uint16>(offsetof(FGCReferenceLeafForTest, StaticReferences)),
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const Durin::DurinCodeGen::FInt32PropertyParams NonReferenceValue = {
				"NonReferenceValue", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(FGCReferenceLeafForTest, NonReferenceValue)),
				static_cast<Durin::uint16>(sizeof(Durin::int32)),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32,
				nullptr, nullptr, nullptr, nullptr, nullptr
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
				static_cast<Durin::uint16>(offsetof(FGCReferenceNestedForTest, Leaf)),
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
				static const Durin::DurinCodeGen::FObjectPropertyParams BaseReference = {
					"BaseReference", Durin::EPropertyFlags::None, 1,
					static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaBaseForTest, BaseReference)),
					static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
					nullptr, nullptr, nullptr, nullptr, true
				};
				static const Durin::DurinCodeGen::FObjectPropertyParams RawReference = {
					"RawReference", Durin::EPropertyFlags::None, 1,
					static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaBaseForTest, RawReference)),
					static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
					Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
					nullptr, nullptr, nullptr, nullptr, false
				};
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
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, Nested)),
				&GetGCReferenceNestedStructForTest
			};

			static const Durin::DurinCodeGen::FStructPropertyParams StructArrayInner = {
				"StructArray_Inner", Durin::EPropertyFlags::None, 1, 0,
				&GetGCReferenceLeafStructForTest
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams StructArray = {
				"StructArray", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, StructArray)),
				&StructArrayInner, &GVectorPropertyHelper<FGCReferenceLeafForTest>
			};

			static const Durin::DurinCodeGen::FObjectPropertyParams NestedArraysInnerInner = {
				"NestedArrays_Inner_Inner", Durin::EPropertyFlags::None, 1, 0,
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams NestedArraysInner = {
				"NestedArrays_Inner", Durin::EPropertyFlags::None, 1, 0,
				&NestedArraysInnerInner,
				&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams NestedArrays = {
				"NestedArrays", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, NestedArrays)),
				&NestedArraysInner,
				&GVectorPropertyHelper<std::vector<Durin::TObjectPtr<Durin::DObject>>>
			};

			static const Durin::DurinCodeGen::FStringPropertyParams DirectMapKey = {
				"DirectMap_Key", Durin::EPropertyFlags::None, 1, 0, static_cast<Durin::uint16>(sizeof(std::string)),
				Durin::DurinCodeGen::EPropertyGenFlags::String,
				nullptr, nullptr, nullptr, nullptr, nullptr
			};
			static const Durin::DurinCodeGen::FObjectPropertyParams DirectMapValue = {
				"DirectMap_Value", Durin::EPropertyFlags::None, 1, 0,
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const Durin::DurinCodeGen::FMapPropertyParams DirectMap = {
				"DirectMap", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, DirectMap)),
				&DirectMapKey, &DirectMapValue,
				&GMapPropertyHelper<std::string, Durin::TObjectPtr<Durin::DObject>>
			};

			static const Durin::DurinCodeGen::FStringPropertyParams ArrayMapKey = {
				"ArrayMap_Key", Durin::EPropertyFlags::None, 1, 0, static_cast<Durin::uint16>(sizeof(std::string)),
				Durin::DurinCodeGen::EPropertyGenFlags::String,
				nullptr, nullptr, nullptr, nullptr, nullptr
			};
			static const Durin::DurinCodeGen::FObjectPropertyParams ArrayMapValueInner = {
				"ArrayMap_Value_Inner", Durin::EPropertyFlags::None, 1, 0,
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams ArrayMapValue = {
				"ArrayMap_Value", Durin::EPropertyFlags::None, 1, 0,
				&ArrayMapValueInner,
				&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
			};
			static const Durin::DurinCodeGen::FMapPropertyParams ArrayMap = {
				"ArrayMap", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, ArrayMap)),
				&ArrayMapKey, &ArrayMapValue,
				&GMapPropertyHelper<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>>
			};

			static const Durin::DurinCodeGen::FObjectPropertyParams DuplicateReference = {
				"DuplicateReference", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, DuplicateReference)),
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const auto SoftReference =
				Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<Durin::TSoftObjectPtr<Durin::DObject>>(
					"SoftReference", Durin::EPropertyFlags::None, 1,
					static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, SoftReference)),
					&Durin::DObject::StaticClass);

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
				{ Durin::FName("A"), 0, "Alpha" },
				{ Durin::FName("B"), 4 },
				{ Durin::FName("AliasB"), 4, "Second B" },
			};
			Enum = new Durin::DEnum(
				Durin::EC_StaticConstructor,
				Durin::FName("EReflectedEnumForTest"),
				Durin::FName("EReflectedEnumForTest"),
				Durin::FName("EReflectedEnumForTest"),
				"Reflected Enum For Test",
				true,
				Durin::DurinCodeGen::EEnumUnderlyingType::UInt8,
				static_cast<Durin::uint16>(sizeof(EReflectedEnumForTest)),
				std::move(Values),
				Durin::EObjectFlags::NoFlags
			);
			Enum->Register(Durin::DEnum::StaticClass, "", "EReflectedEnumForTest");
		}
		return Enum;
	}

	void EnsureDObjectInitialized()
	{
		static const bool bInitialized = []()
		{
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::FNameInit();
			Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
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

	struct FUnavailableStructPropertyOwnerForTest
	{
		StructOpsTest::FDeletedDefault DeletedDefault;
		StructOpsTest::FMoveOnly MoveOnly;
	};

	auto GetAccessedVector(void* Container, Durin::uint32 ArrayIndex) -> void*
	{
		return &static_cast<FTypedStructPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetAccessedVector(const void* Container, Durin::uint32 ArrayIndex) -> const void*
	{
		return &static_cast<const FTypedStructPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetAccessedSoftObject(void* Container, Durin::uint32 ArrayIndex) -> void*
	{
		return &static_cast<FSoftObjectPropertyOwnerForTest*>(Container)->Accessed + ArrayIndex;
	}

	auto GetAccessedSoftObject(const void* Container, Durin::uint32 ArrayIndex) -> const void*
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
			static_cast<Durin::uint16>(offsetof(FTypedStructPropertyOwnerForTest, Direct)),
			&Durin::Z_Construct_DStruct_Durin_FVector3,
			DirectMetaData,
			std::size(DirectMetaData)
		};
		static constexpr Durin::DurinCodeGen::FStructPropertyParams Accessed =
			Durin::DurinCodeGen::FStructPropertyParams::WithAccessors(
				"Accessed",
				Durin::EPropertyFlags::ReadOnly,
				1,
				&Durin::Z_Construct_DStruct_Durin_FVector3,
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
			static_cast<Durin::uint16>(offsetof(FSoftObjectPropertyOwnerForTest, Direct)),
			&Durin::DObject::StaticClass, DirectMetaData, std::size(DirectMetaData));
		static const auto Fixed = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Fixed", Durin::EPropertyFlags::Edit, 2,
			static_cast<Durin::uint16>(offsetof(FSoftObjectPropertyOwnerForTest, Fixed)),
			&Durin::DObject::StaticClass);
		static const auto Accessed = Durin::DurinCodeGen::FSoftObjectPropertyParams::WithAccessors<FSoftPtr>(
			"Accessed", Durin::EPropertyFlags::ReadOnly, 1, &Durin::DObject::StaticClass,
			static_cast<Durin::DurinCodeGen::FSoftObjectPropertyParams::FMutableValueAccessor>(&GetAccessedSoftObject),
			static_cast<Durin::DurinCodeGen::FSoftObjectPropertyParams::FConstValueAccessor>(&GetAccessedSoftObject));
		static const auto ArrayInner = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Array_Inner", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass);
		static const Durin::DurinCodeGen::FArrayPropertyParams Array = {
			"Array", Durin::EPropertyFlags::Edit, 1,
			static_cast<Durin::uint16>(offsetof(FSoftObjectPropertyOwnerForTest, Array)),
			&ArrayInner, &GVectorPropertyHelper<FSoftPtr>
		};
		static const Durin::DurinCodeGen::FStringPropertyParams MapKey = {
			"Map_Key", Durin::EPropertyFlags::None, 1, 0,
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String
		};
		static const auto MapValue = Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<FSoftPtr>(
			"Map_Value", Durin::EPropertyFlags::None, 1, 0, &Durin::DObject::StaticClass);
		static const Durin::DurinCodeGen::FMapPropertyParams Map = {
			"Map", Durin::EPropertyFlags::Edit, 1,
			static_cast<Durin::uint16>(offsetof(FSoftObjectPropertyOwnerForTest, Map)),
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
			static_cast<Durin::uint16>(offsetof(FUnavailableStructPropertyOwnerForTest, DeletedDefault)),
			&GetDeletedDefaultStruct
		};
		static const Durin::DurinCodeGen::FStructPropertyParams MoveOnly = {
			"MoveOnly",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FUnavailableStructPropertyOwnerForTest, MoveOnly)),
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
			Durin::PathUtilities::RegisterMountPointForTests(
				"/CoreTests/",
				(Durin::Testing::GetTestWorkDirectory() / "CoreTests").generic_string() + "/");
			return true;
		}();
		(void)bMounted;
	}

	auto ObjectArrayContains(Durin::DObject* Object) -> bool
	{
		std::vector<Durin::DObject*> Objects = Durin::GDObjectArray.GetAll();
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
			(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DLifecycleTestObject>);
		AbstractClass.SetSuperStructBase(Durin::DObject::StaticClass());

		EXPECT_TRUE(AbstractClass.IsChildOf(Durin::DObject::StaticClass()));
		EXPECT_TRUE(AbstractClass.HasAnyClassFlags(Durin::EClassFlags::Abstract));
		EXPECT_FALSE(Durin::CanConstructObjectOfClass(
			&AbstractClass, Durin::DObject::StaticClass()));
		EXPECT_EQ(Durin::NewObject(
			&AbstractClass, nullptr, Durin::FName("RejectedAbstractObject")), nullptr);
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

		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter), MiddleChild));
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter), MiddleChild));

		MiddleChild->Rename(Durin::FName("OuterIndexRenamedChild"));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter), MiddleChild));

		// Removing the middle swaps LastChild into its slot and must update that back-pointer.
		MiddleChild->SetOuterPrivate(SecondOuter);
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter), MiddleChild));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter), MiddleChild));

		FirstChild->SetOuterPrivate(SecondOuter);
		LastChild->SetOuterPrivate(SecondOuter);
		EXPECT_TRUE(Durin::GDObjectArray.GetObjectsWithOuter(FirstOuter).empty());
		EXPECT_EQ(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter).size(), 3);

		MiddleChild->SetOuterPrivate(nullptr);
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(SecondOuter), MiddleChild));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr), MiddleChild));

		Durin::MarkAsGarbage(MiddleChild);
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr), MiddleChild));
		EXPECT_TRUE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr, true), MiddleChild));

		Durin::CollectGarbage();
		EXPECT_FALSE(Contains(Durin::GDObjectArray.GetObjectsWithOuter(nullptr, true), MiddleChild));
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

	TEST(FCoreDObjectReflectionTests, PackageKeepsAssetReferenceAndBuildsStableObjectPaths)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/CoreTests/Package", Path));
		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "Package");
		Package->InitializeAssetPackage(Path);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "Package");
		Durin::DObject* Inner = Durin::NewObject<Durin::DObject>(Asset, "Inner");
		ASSERT_TRUE(Package->SetAsset(Asset));

		EXPECT_EQ(Package->GetOuter(), nullptr);
		EXPECT_EQ(Asset->GetPackage(), Package);
		EXPECT_EQ(Asset->GetObjectPath(), "/CoreTests/Package");
		EXPECT_EQ(Inner->GetObjectPath(), "/CoreTests/Package:Inner");
		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Package));
		EXPECT_TRUE(ObjectArrayContains(Asset));
		EXPECT_FALSE(ObjectArrayContains(Inner));

		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
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

	TEST(FCoreDObjectReflectionTests, GarbageCollectionWaitsForFinishReadinessWithoutRepeatingCallbacks)
	{
		EnsureDObjectInitialized();
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
			nullptr, Durin::FName("ShutdownDeferredFinishTestObject"));
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
		EXPECT_EQ(Scheduler.Checkpoints, (std::vector{
			EShutdownDestroyCheckpoint::BeforeRelease,
			EShutdownDestroyCheckpoint::FencePending,
			EShutdownDestroyCheckpoint::FinishDestroy,
			EShutdownDestroyCheckpoint::Destructor}));
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

		for (Durin::uint32 Index = 0; Index < 4; ++Index)
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

	TEST(FCoreDObjectReflectionTests, RootReferencesAreCountedAndScopedRootsAreMovable)
	{
		EnsureDObjectInitialized();
		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCCountedRoot"));

		Durin::AddToRoot(Object);
		Durin::AddToRoot(Object);
		Durin::RemoveFromRoot(Object);
		Durin::CollectGarbage();
		EXPECT_TRUE(ObjectArrayContains(Object));
		Durin::RemoveFromRoot(Object);

		{
			Durin::FScopedObjectRoot First(Object);
			Durin::FScopedObjectRoot Second(std::move(First));
			Durin::CollectGarbage();
			EXPECT_TRUE(ObjectArrayContains(Object));
		}

		Durin::CollectGarbage();
		EXPECT_FALSE(ObjectArrayContains(Object));
	}

	TEST(FCoreDObjectReflectionTests, DeepOuterChainUsesIterativeMarkAndDestroy)
	{
		EnsureDObjectInitialized();
		constexpr Durin::uint32 ChainLength = 10000;
		Durin::DObject* Outermost = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCDeepOuter0"));
		Durin::DObject* Innermost = Outermost;
		for (Durin::uint32 Index = 1; Index < ChainLength; ++Index)
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
		Durin::FAssetPath SoftAssetPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/CoreTests/GCSchemaSoft", SoftAssetPath));
		Durin::DPackage* SoftPackage = Durin::NewObject<Durin::DPackage>(nullptr, Durin::FName("GCSchemaSoft"));
		SoftPackage->InitializeAssetPackage(SoftAssetPath);
		Durin::DObject* SoftReference =
			Durin::NewObject<Durin::DObject>(SoftPackage, Durin::FName("GCSchemaSoft"));
		ASSERT_TRUE(SoftPackage->SetAsset(SoftReference));

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

		Durin::MarkAsGarbage(Owner);
		Durin::MarkAsGarbage(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, ObjectGraphSerializationRoundTripsScalarStringAndObjectReference)
	{
		EnsureDObjectInitialized();

		auto* Owner = Durin::NewObject<DLifecycleReferenceOwnerForTest>(nullptr, Durin::FName("SerializedOwner"));
		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedReference"));
		Durin::DObject* RawVectorReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedRawVectorReference"));
		Durin::DObject* ObjectPtrVectorReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SerializedObjectPtrVectorReference"));
		Owner->Value = 37;
		Owner->bEnabled = true;
		Owner->Label = "Serialized";
		Owner->Reference = ReferencedObject;
		Owner->ObjectPtrReference = ReferencedObject;
		Owner->RawReferences.push_back(RawVectorReferencedObject);
		Owner->ObjectPtrReferences.push_back(ObjectPtrVectorReferencedObject);
		Owner->Scores = { 7, 11, 42 };
		Owner->Tags = { "Alpha", "Beta" };
		Owner->Modes = { EReflectedEnumForTest::A, EReflectedEnumForTest::B };
		Owner->ScoreGroups = { { 1, 2 }, { 3, 5, 8 } };
		Owner->TransientValue = 99;
		ASSERT_EQ(Owner->GetClass(), DLifecycleReferenceOwnerForTest::StaticClass());
		ASSERT_EQ(Owner->GetClass()->GetName(), "DLifecycleReferenceOwnerForTest");
		ASSERT_EQ(ReferencedObject->GetClass(), Durin::DObject::StaticClass());

		std::vector<Durin::uint8> Bytes;
		ASSERT_TRUE(Durin::SaveObjectGraphToMemory(Owner, Bytes));
		Durin::MarkAsGarbage(Owner);
		Durin::MarkAsGarbage(ReferencedObject);
		Durin::MarkAsGarbage(RawVectorReferencedObject);
		Durin::MarkAsGarbage(ObjectPtrVectorReferencedObject);

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

		Durin::MarkAsGarbage(LoadedOwner);
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
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/CoreTests/SoftObjectValue", Path));
		Durin::FSoftObjectPath SoftPath;
		ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(Path.GetView(), SoftPath));
		const Durin::FSoftObjectPath ValidSoftPath = SoftPath;
		EXPECT_FALSE(Durin::FSoftObjectPath::TryCreate("/Unknown/InvalidSoftObject", SoftPath));
		EXPECT_EQ(SoftPath, ValidSoftPath);

		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "SoftObjectValue");
		Package->InitializeAssetPackage(Path);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "SoftObjectValue");
		ASSERT_TRUE(Package->SetAsset(Asset));

		Durin::FSoftObjectPtr Reference(SoftPath);
		EXPECT_FALSE(Reference.IsNull());
		EXPECT_FALSE(Reference.IsLoaded());
		ASSERT_TRUE(Reference.TrySetObject(Asset));
		EXPECT_EQ(Reference.Get(), Asset);
		EXPECT_EQ(Reference.GetSoftObjectPath(), SoftPath);

		Durin::FSoftObjectPtr PathOnly(SoftPath);
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
		EXPECT_TRUE(Reference.TrySetLoadedObject(Asset));
		EXPECT_EQ(Reference.Get(), Asset);

		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
		Durin::CollectGarbage();
		EXPECT_EQ(Reference.Get(), nullptr);
		EXPECT_EQ(Moved.Get(), nullptr);
		EXPECT_EQ(Reference.GetSoftObjectPath(), SoftPath);
	}

	TEST(FCoreDObjectReflectionTests, SoftObjectPtrRejectsInvalidObjectsWithoutMutation)
	{
		EnsureDObjectInitialized();
		EnsurePackageTestMount();
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/CoreTests/SoftObjectValidation", Path));

		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "SoftObjectValidation");
		Package->InitializeAssetPackage(Path);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "SoftObjectValidation");
		Durin::DObject* Inner = Durin::NewObject<Durin::DObject>(Asset, "Inner");
		ASSERT_TRUE(Package->SetAsset(Asset));

		Durin::FSoftObjectPtr Reference;
		ASSERT_TRUE(Reference.TrySetObject(Asset));
		const Durin::FSoftObjectPath OriginalPath = Reference.GetSoftObjectPath();
		std::string Error;
		EXPECT_FALSE(Reference.TrySetObject(Package, nullptr, &Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_FALSE(Reference.TrySetObject(Inner, nullptr, &Error));
		EXPECT_FALSE(Reference.TrySetObject(Asset, Durin::DPackage::StaticClass(), &Error));
		Durin::DObject* Unpackaged = Durin::NewObject<Durin::DObject>(nullptr, "UnpackagedSoftObject");
		EXPECT_FALSE(Reference.TrySetObject(Unpackaged, nullptr, &Error));

		auto* TransientType = new Durin::DStruct(
			Durin::EC_StaticConstructor,
			Durin::FName("FTransientSoftObjectForTest"),
			Durin::FName("FTransientSoftObjectForTest"),
			1,
			1,
			Durin::EObjectFlags::Transient);
		EXPECT_FALSE(Reference.TrySetObject(TransientType, nullptr, &Error));
		delete TransientType;

		EXPECT_EQ(Reference.GetSoftObjectPath(), OriginalPath);
		EXPECT_EQ(Reference.Get(), Asset);

		Durin::TSoftObjectPtr<Durin::DPackage> WrongType(Path);
		EXPECT_FALSE(WrongType.TrySetObject(Asset, &Error));
		EXPECT_EQ(WrongType.GetSoftObjectPath(), OriginalPath);
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

		Durin::FAssetPath AssetPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/CoreTests/SoftObjectProperty", AssetPath));
		Durin::FSoftObjectPath SoftPath(AssetPath);
		FSoftObjectPropertyOwnerForTest Owner;
		Owner.Direct.SetPath(SoftPath);
		Owner.Fixed[1].SetPath(SoftPath);
		Owner.Accessed.SetPath(SoftPath);
		ASSERT_EQ(Direct->GetSoftObjectPtr(&Owner), &Owner.Direct.GetBase());
		ASSERT_EQ(Fixed->GetSoftObjectPtr(&Owner, 1), &Owner.Fixed[1].GetBase());
		ASSERT_EQ(Accessed->GetSoftObjectPtr(&Owner), &Owner.Accessed.GetBase());
		EXPECT_EQ(Direct->GetSoftObjectPtr(&Owner)->GetSoftObjectPath(), SoftPath);

		Owner.Array.emplace_back(SoftPath);
		auto* ArrayInner = static_cast<Durin::FSoftObjectProperty*>(Array->GetInner());
		ASSERT_NE(ArrayInner, nullptr);
		ASSERT_NE(ArrayInner->GetSoftObjectPtr(Array->GetElementPtr(&Owner, 0)), nullptr);
		EXPECT_EQ(
			ArrayInner->GetSoftObjectPtr(Array->GetElementPtr(&Owner, 0))->GetSoftObjectPath(),
			SoftPath);

		Owner.Map.emplace("Target", Durin::TSoftObjectPtr<Durin::DObject>(SoftPath));
		auto* MapValue = static_cast<Durin::FSoftObjectProperty*>(Map->GetValueProp());
		const void* RawMapValue = nullptr;
		const std::string Key = "Target";
		ASSERT_EQ(Map->FindValue(&Owner, &Key, &RawMapValue), Durin::EContainerOpResult::Success);
		ASSERT_NE(MapValue, nullptr);
		ASSERT_NE(MapValue->GetSoftObjectPtr(RawMapValue), nullptr);
		EXPECT_EQ(MapValue->GetSoftObjectPtr(RawMapValue)->GetSoftObjectPath(), SoftPath);

		Durin::FReflectedValueStorage Detached;
		ASSERT_TRUE(Detached.CopyConstruct(Direct, Direct->GetValuePtr(&Owner)));
		auto* DetachedValue = static_cast<Durin::TSoftObjectPtr<Durin::DObject>*>(Detached.GetValue());
		ASSERT_NE(DetachedValue, nullptr);
		EXPECT_EQ(DetachedValue->GetSoftObjectPath(), SoftPath);
		Durin::FAssetPath ReplacementPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/CoreTests/SoftObjectPropertyReplacement", ReplacementPath));
		Durin::TSoftObjectPtr<Durin::DObject> Replacement(ReplacementPath);
		ASSERT_TRUE(Detached.CopyAssign(&Replacement));
		EXPECT_EQ(DetachedValue->GetSoftObjectPath(), Replacement.GetSoftObjectPath());

		auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "SoftObjectProperty");
		Package->InitializeAssetPackage(AssetPath);
		Durin::AddToRoot(Package);
		Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "SoftObjectProperty");
		ASSERT_TRUE(Package->SetAsset(Asset));
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
		EXPECT_EQ(Owner.Direct.GetSoftObjectPath(), SoftPath);

		Durin::FPropertyValueSnapshot ArraySnapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(Array, &Owner, 0, ArraySnapshot));
		Owner.Array.clear();
		ASSERT_TRUE(Durin::RestorePropertyValue(Array, &Owner, 0, ArraySnapshot));
		ASSERT_EQ(Owner.Array.size(), 1u);
		EXPECT_EQ(Owner.Array[0].GetSoftObjectPath(), SoftPath);

		Durin::FPropertyValueSnapshot MapSnapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(Map, &Owner, 0, MapSnapshot));
		Owner.Map.clear();
		ASSERT_TRUE(Durin::RestorePropertyValue(Map, &Owner, 0, MapSnapshot));
		ASSERT_EQ(Owner.Map.size(), 1u);
		EXPECT_EQ(Owner.Map.at("Target").GetSoftObjectPath(), SoftPath);

		Durin::RemoveFromRoot(Package);
		Durin::MarkObjectHierarchyAsGarbage(Package);
		Durin::CollectGarbage();
	}

#ifdef DO_CHECK
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
			"");
	}
#endif

	TEST(FCoreDObjectReflectionTests, ReusedObjectSlotInvalidatesOldHandleGeneration)
	{
		EnsureDObjectInitialized();
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
		const Durin::uint64 InitialCount = Durin::GDObjectArray.GetNum();
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
			{ "A", 0, "Alpha" },
			{ "B", 4, nullptr },
			{ "AliasB", 4, "Second B" },
		};
		static const Durin::DurinCodeGen::FEnumParams EnumParams = {
			&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister,
			"EReflectedEnumForTest",
			"EReflectedEnumForTest",
			"Reflected Enum For Test",
			true,
			Durin::DurinCodeGen::EEnumUnderlyingType::UInt8,
			static_cast<Durin::uint16>(sizeof(EReflectedEnumForTest)),
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

		Durin::uint64 Value = std::numeric_limits<Durin::uint64>::max();
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
			static_cast<Durin::uint16>(offsetof(FReflectedEnumPropertyOwnerForTest, Mode)),
			static_cast<Durin::uint16>(sizeof(EReflectedEnumForTest)),
			Durin::DurinCodeGen::EPropertyGenFlags::Enum,
			nullptr,
			&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister,
			nullptr,
			nullptr,
			nullptr
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
			static_cast<Durin::uint16>(sizeof(ESignedEnumValueForTest)),
			std::vector<Durin::FEnumValue>{
				{ Durin::FName("Negative"), std::numeric_limits<Durin::uint64>::max() },
				{ Durin::FName("Positive"), 1 }
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
			static_cast<Durin::uint16>(sizeof(EUnsignedEnumValueForTest)),
			std::vector<Durin::FEnumValue>{
				{ Durin::FName("Low"), 0 },
				{ Durin::FName("High"), std::numeric_limits<Durin::uint64>::max() }
			},
			Durin::EObjectFlags::NoFlags
		);

		Durin::FEnumProperty SignedProperty(
			{}, Durin::FName("Signed"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(FWideEnumPropertyOwnerForTest, Signed)),
			static_cast<Durin::uint16>(sizeof(ESignedEnumValueForTest)), Durin::DurinCodeGen::EPropertyGenFlags::Enum, nullptr, SignedEnum.get()
		);
		Durin::FEnumProperty UnsignedProperty(
			{}, Durin::FName("Unsigned"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::None, 1,
			static_cast<Durin::uint16>(offsetof(FWideEnumPropertyOwnerForTest, Unsigned)),
			static_cast<Durin::uint16>(sizeof(EUnsignedEnumValueForTest)), Durin::DurinCodeGen::EPropertyGenFlags::Enum, nullptr, UnsignedEnum.get()
		);
		FWideEnumPropertyOwnerForTest Instance;
		const Durin::uint64 MaxValue = std::numeric_limits<Durin::uint64>::max();

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
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPropertyParams = {
			"ObjectValue",
			Durin::EPropertyFlags::Edit,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectValue)),
			static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrPropertyParams = {
			"ObjectPtrValue",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectPtrValue)),
			static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			true
		};
		static const Durin::DurinCodeGen::FStringPropertyParams StringPropertyParams = {
			"StringValue",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, StringValue)),
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FNamePropertyParams NamePropertyParams = {
			"NameValue",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, NameValue)),
			static_cast<Durin::uint16>(sizeof(Durin::FName)),
			Durin::DurinCodeGen::EPropertyGenFlags::Name,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectArrayInnerPropertyParams = {
			"ObjectArray_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectArrayPropertyParams = {
			"ObjectArray",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectArray)),
			&ObjectArrayInnerPropertyParams,
			&GVectorPropertyHelper<Durin::DObject*>
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPtrArrayInnerPropertyParams = {
			"ObjectPtrArray_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			true
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectPtrArrayPropertyParams = {
			"ObjectPtrArray",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectPtrArray)),
			&ObjectPtrArrayInnerPropertyParams,
			&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
		};
		static const Durin::DurinCodeGen::FStringPropertyParams StringToIntKeyPropertyParams = {
			"StringToInt_Key",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FInt32PropertyParams StringToIntValuePropertyParams = {
			"StringToInt_Value",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::int32)),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FMapPropertyParams StringToIntPropertyParams = {
			"StringToInt",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, StringToInt)),
			&StringToIntKeyPropertyParams,
			&StringToIntValuePropertyParams,
			&GMapPropertyHelper<std::string, Durin::int32>
		};
		static const Durin::DurinCodeGen::FInt32PropertyParams NestedScoresInnerInnerPropertyParams = {
			"NestedScores_Inner_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::int32)),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresInnerPropertyParams = {
			"NestedScores_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			&NestedScoresInnerInnerPropertyParams,
			&GVectorPropertyHelper<Durin::int32>
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresPropertyParams = {
			"NestedScores",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, NestedScores)),
			&NestedScoresInnerPropertyParams,
			&GVectorPropertyHelper<std::vector<Durin::int32>>
		};
		static const Durin::DurinCodeGen::FStringPropertyParams ObjectListsKeyPropertyParams = {
			"ObjectLists_Key",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectListsValueInnerPropertyParams = {
			"ObjectLists_Value_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
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
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectLists)),
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
		EXPECT_EQ(ValueProperty->GetElementSize(), sizeof(Durin::int32));
		EXPECT_TRUE(ValueProperty->ClassPrivate->IsChildOf(Durin::FNumericProperty::StaticClass()));

		FReflectedPropertyOwnerForTest Instance;
		*ValueProperty->ContainerPtrToValuePtr<Durin::int32>(&Instance) = 42;
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
			[&NestedNames](Durin::FProperty* Property)
			{
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

	TEST(FCoreDObjectReflectionTests, BuiltInMathStructsExposeNestedFieldMetadataAndOperations)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* Vector2Struct = Durin::Z_Construct_DStruct_Durin_FVector2();
		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_Durin_FVector3();
		Durin::DStruct* Vector4Struct = Durin::Z_Construct_DStruct_Durin_FVector4();
		Durin::DStruct* QuatStruct = Durin::Z_Construct_DStruct_Durin_FQuat();
		Durin::DStruct* TransformStruct = Durin::Z_Construct_DStruct_Durin_FTransform();
		Durin::DStruct* ColorStruct = Durin::Z_Construct_DStruct_Durin_FLinearColor();
		ASSERT_NE(Vector2Struct, nullptr);
		ASSERT_NE(VectorStruct, nullptr);
		ASSERT_NE(Vector4Struct, nullptr);
		ASSERT_NE(QuatStruct, nullptr);
		ASSERT_NE(TransformStruct, nullptr);
		ASSERT_NE(ColorStruct, nullptr);
		EXPECT_EQ(Vector2Struct->GetQualifiedName().ToString(), "Durin::FVector2");
		EXPECT_EQ(VectorStruct->GetQualifiedName().ToString(), "Durin::FVector3");
		EXPECT_EQ(Vector4Struct->GetQualifiedName().ToString(), "Durin::FVector4");
		EXPECT_EQ(QuatStruct->GetQualifiedName().ToString(), "Durin::FQuat");
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector2"), Vector2Struct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector3"), VectorStruct);
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FVector4"), Vector4Struct);
		EXPECT_EQ(Durin::FindStructByQualifiedName(Durin::FName("Durin::FTransform")), TransformStruct);
		EXPECT_EQ(ColorStruct->GetQualifiedName().ToString(), "Durin::FLinearColor");
		EXPECT_EQ(Durin::FindStructByQualifiedName("Durin::FLinearColor"), ColorStruct);
		for (Durin::DStruct* Struct : {Vector2Struct, VectorStruct, Vector4Struct, QuatStruct, TransformStruct, ColorStruct})
		{
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
			std::pair{RotationStructProperty, static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::FTransform, Rotation))},
			std::pair{TranslationStructProperty, static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::FTransform, Translation))},
			std::pair{ScaleStructProperty, static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::FTransform, Scale3D))}
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

	TEST(FCoreDObjectReflectionTests, TypedStructPropertyCompiledMetadataFootprintIsRecorded)
	{
		RecordProperty(
			"FPropertyParamsBaseBytes",
			static_cast<int>(sizeof(Durin::DurinCodeGen::FPropertyParamsBase)));
		RecordProperty(
			"FStructPropertyParamsBytes",
			static_cast<int>(sizeof(Durin::DurinCodeGen::FStructPropertyParams)));
		EXPECT_TRUE((std::is_base_of_v<
			Durin::DurinCodeGen::FPropertyParamsBase,
			Durin::DurinCodeGen::FStructPropertyParams>));
	}

	TEST(FCoreDObjectReflectionTests, TypedStructPropertyRegistrationSupportsOffsetsAccessorsAndMetadata)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* OwnerStruct = GetTypedStructPropertyOwner();
		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_Durin_FVector3();
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
		std::array<std::byte, sizeof(StructOpsTest::FDeletedDefault)> UnchangedStorage;
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

#ifdef DO_CHECK
	TEST(FCoreDObjectReflectionTests, TypedStructPropertyRegistrationRejectsInvalidMetadata)
	{
		EXPECT_DEATH(
			([] {
				EnsureDObjectInitialized();
				Durin::DurinCodeGen::FPropertyParamsBase Params = {
					"KindMismatch", Durin::EPropertyFlags::None, 1, 0, 0,
					Durin::DurinCodeGen::EPropertyGenFlags::Struct
				};
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
						&Durin::Z_Construct_DStruct_Durin_FVector3,
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
					&Durin::Z_Construct_DStruct_Durin_FVector3
				};
				Params.NumMetaData = 1;
				ConstructInvalidStructProperty(Params);
			}()),
			"PropertyRegistration.MetadataMismatch"
		);
	}
#endif
}

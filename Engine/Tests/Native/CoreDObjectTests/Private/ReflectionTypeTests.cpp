#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
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
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>
#include <cstddef>

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
	auto VectorPropertyResize(void* Container, Durin::uint64 Num) -> void
	{
		auto* Value = static_cast<std::vector<T>*>(Container);
		Value->resize(static_cast<size_t>(Num));
	}

	template<typename T>
	const Durin::DurinCodeGen::FArrayPropertyHelper GVectorPropertyHelper = {
		&VectorPropertyNum<T>,
		&VectorPropertyGetElement<T>,
		&VectorPropertyGetMutableElement<T>,
		&VectorPropertyResize<T>
	};

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
	auto MapPropertyInsert(void* Container, const void* Key, const void* Value) -> void
	{
		static_cast<TTestMap<K, V>*>(Container)->insert_or_assign(*static_cast<const K*>(Key), *static_cast<const V*>(Value));
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
	const Durin::DurinCodeGen::FMapPropertyHelper GMapPropertyHelper = {
		&MapPropertyNum<K, V>,
		&MapPropertyGetKey<K, V>,
		&MapPropertyGetValue<K, V>,
		&MapPropertyGetMutableValue<K, V>,
		&MapPropertyClear<K, V>,
		&MapPropertyCreateValue<K>,
		&MapPropertyCreateValueCopy<K>,
		&MapPropertyDestroyValue<K>,
		&MapPropertyCreateValue<V>,
		&MapPropertyDestroyValue<V>,
		&MapPropertyInsert<K, V>,
		&MapPropertyContains<K, V>,
		&MapPropertyRenameKey<K, V>,
		&MapPropertyRemove<K, V>
	};

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

	class DLifecycleTestObject : public Durin::DObject
	{
	public:
		inline static Durin::uint64 BeginDestroyCount = 0;
		inline static Durin::uint64 FinishDestroyCount = 0;
		inline static Durin::uint64 DestructorCount = 0;
		bool bReadyForFinishDestroy = true;

		explicit DLifecycleTestObject(const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
		}

		~DLifecycleTestObject() override { ++DestructorCount; }
		auto BeginDestroy() -> void override { ++BeginDestroyCount; }
		auto IsReadyForFinishDestroy() -> bool override { return bReadyForFinishDestroy; }
		auto FinishDestroy() -> void override { ++FinishDestroyCount; }

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
					static_cast<Durin::uint16>(sizeof(std::vector<Durin::DObject*>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&RawReferencesInnerPropertyParams,
					nullptr,
					nullptr,
					false,
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
					static_cast<Durin::uint16>(sizeof(std::vector<Durin::TObjectPtr<Durin::DObject>>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&ObjectPtrReferencesInnerPropertyParams,
					nullptr,
					nullptr,
					false,
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
					static_cast<Durin::uint16>(sizeof(std::vector<Durin::int32>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&ScoresInnerPropertyParams,
					nullptr,
					nullptr,
					false,
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
					static_cast<Durin::uint16>(sizeof(std::vector<std::string>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&TagsInnerPropertyParams,
					nullptr,
					nullptr,
					false,
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
					static_cast<Durin::uint16>(sizeof(std::vector<EReflectedEnumForTest>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&ModesInnerPropertyParams,
					nullptr,
					nullptr,
					false,
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
					static_cast<Durin::uint16>(sizeof(std::vector<Durin::int32>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&ScoreGroupsInnerInnerPropertyParams,
					nullptr,
					nullptr,
					false,
					&GVectorPropertyHelper<Durin::int32>
				};
				static const Durin::DurinCodeGen::FArrayPropertyParams ScoreGroupsPropertyParams = {
					"ScoreGroups",
					Durin::EPropertyFlags::None,
					1,
					static_cast<Durin::uint16>(offsetof(DLifecycleReferenceOwnerForTest, ScoreGroups)),
					static_cast<Durin::uint16>(sizeof(std::vector<std::vector<Durin::int32>>)),
					Durin::DurinCodeGen::EPropertyGenFlags::Array,
					nullptr,
					nullptr,
					&ScoreGroupsInnerPropertyParams,
					nullptr,
					nullptr,
					false,
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
				static_cast<Durin::uint16>(sizeof(FGCReferenceLeafForTest)),
				Durin::DurinCodeGen::EPropertyGenFlags::Struct,
				nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &GetGCReferenceLeafStructForTest
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
	};

	auto DGCReferenceSchemaDerivedForTest::StaticClass() -> Durin::DClass*
	{
		static Durin::DClass* Class = [] {
			static const Durin::DurinCodeGen::FStructPropertyParams Nested = {
				"Nested", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, Nested)),
				static_cast<Durin::uint16>(sizeof(FGCReferenceNestedForTest)),
				Durin::DurinCodeGen::EPropertyGenFlags::Struct,
				nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &GetGCReferenceNestedStructForTest
			};

			static const Durin::DurinCodeGen::FStructPropertyParams StructArrayInner = {
				"StructArray_Inner", Durin::EPropertyFlags::None, 1, 0,
				static_cast<Durin::uint16>(sizeof(FGCReferenceLeafForTest)),
				Durin::DurinCodeGen::EPropertyGenFlags::Struct,
				nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &GetGCReferenceLeafStructForTest
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams StructArray = {
				"StructArray", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, StructArray)),
				static_cast<Durin::uint16>(sizeof(std::vector<FGCReferenceLeafForTest>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, nullptr, &StructArrayInner, nullptr, nullptr, false, &GVectorPropertyHelper<FGCReferenceLeafForTest>
			};

			static const Durin::DurinCodeGen::FObjectPropertyParams NestedArraysInnerInner = {
				"NestedArrays_Inner_Inner", Durin::EPropertyFlags::None, 1, 0,
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams NestedArraysInner = {
				"NestedArrays_Inner", Durin::EPropertyFlags::None, 1, 0,
				static_cast<Durin::uint16>(sizeof(std::vector<Durin::TObjectPtr<Durin::DObject>>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, nullptr, &NestedArraysInnerInner, nullptr, nullptr, false,
				&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
			};
			static const Durin::DurinCodeGen::FArrayPropertyParams NestedArrays = {
				"NestedArrays", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, NestedArrays)),
				static_cast<Durin::uint16>(sizeof(std::vector<std::vector<Durin::TObjectPtr<Durin::DObject>>>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, nullptr, &NestedArraysInner, nullptr, nullptr, false,
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
				static_cast<Durin::uint16>(sizeof(TTestMap<std::string, Durin::TObjectPtr<Durin::DObject>>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Map,
				nullptr, nullptr, nullptr, &DirectMapKey, &DirectMapValue, false, nullptr,
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
				static_cast<Durin::uint16>(sizeof(std::vector<Durin::TObjectPtr<Durin::DObject>>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, nullptr, &ArrayMapValueInner, nullptr, nullptr, false,
				&GVectorPropertyHelper<Durin::TObjectPtr<Durin::DObject>>
			};
			static const Durin::DurinCodeGen::FMapPropertyParams ArrayMap = {
				"ArrayMap", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, ArrayMap)),
				static_cast<Durin::uint16>(sizeof(TTestMap<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Map,
				nullptr, nullptr, nullptr, &ArrayMapKey, &ArrayMapValue, false, nullptr,
				&GMapPropertyHelper<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>>
			};

			static const Durin::DurinCodeGen::FObjectPropertyParams DuplicateReference = {
				"DuplicateReference", Durin::EPropertyFlags::None, 1,
				static_cast<Durin::uint16>(offsetof(DGCReferenceSchemaDerivedForTest, DuplicateReference)),
				static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass,
				nullptr, nullptr, nullptr, nullptr, true
			};

			static const Durin::DurinCodeGen::FPropertyParamsBase* const Properties[] = {
				&Nested, &StructArray, &NestedArrays, &DirectMap, &ArrayMap, &DuplicateReference
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

	void EnsurePackageTestMount()
	{
		static const bool bMounted = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::PathUtilities::RegisterMountPoint("/CoreTests/", std::filesystem::path(DURIN_TEST_WORK_DIR).generic_string() + "/");
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
		ASSERT_TRUE(Property->HasMapHelper());

		const std::string InitialKey = "Initial";
		Durin::TObjectPtr<Durin::DObject> InitialValue;
		Property->Insert(Owner, &InitialKey, &InitialValue);
		ASSERT_EQ(Property->Num(Owner), 1u);
		EXPECT_TRUE(Property->Contains(Owner, &InitialKey));

		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("MapPropertyMutationReference"));
		auto* MutableValue = static_cast<Durin::TObjectPtr<Durin::DObject>*>(Property->GetMutableMappedValuePtr(Owner, 0));
		ASSERT_NE(MutableValue, nullptr);
		*MutableValue = ReferencedObject;
		EXPECT_EQ(Owner->DirectMap.at(InitialKey).Get(), ReferencedObject);

		void* KeyCopy = Property->CreateKeyCopy(Property->GetKeyPtr(Owner, 0));
		ASSERT_NE(KeyCopy, nullptr);
		EXPECT_EQ(*static_cast<std::string*>(KeyCopy), InitialKey);
		const std::string RenamedKey = "Renamed";
		EXPECT_TRUE(Property->RenameKey(Owner, KeyCopy, &RenamedKey));
		Property->DestroyKey(KeyCopy);
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
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::DObject*>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			nullptr,
			&ObjectArrayInnerPropertyParams,
			nullptr,
			nullptr,
			false,
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
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::TObjectPtr<Durin::DObject>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			nullptr,
			&ObjectPtrArrayInnerPropertyParams,
			nullptr,
			nullptr,
			false,
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
			static_cast<Durin::uint16>(sizeof(std::unordered_map<std::string, Durin::int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map,
			nullptr,
			nullptr,
			nullptr,
			&StringToIntKeyPropertyParams,
			&StringToIntValuePropertyParams
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
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			nullptr,
			&NestedScoresInnerInnerPropertyParams,
			nullptr,
			nullptr,
			false,
			&GVectorPropertyHelper<Durin::int32>
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresPropertyParams = {
			"NestedScores",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, NestedScores)),
			static_cast<Durin::uint16>(sizeof(std::vector<std::vector<Durin::int32>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			nullptr,
			&NestedScoresInnerPropertyParams,
			nullptr,
			nullptr,
			false,
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
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::DObject*>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			nullptr,
			&ObjectListsValueInnerPropertyParams,
			nullptr,
			nullptr,
			false,
			&GVectorPropertyHelper<Durin::DObject*>
		};
		static const Durin::DurinCodeGen::FMapPropertyParams ObjectListsPropertyParams = {
			"ObjectLists",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectLists)),
			static_cast<Durin::uint16>(sizeof(std::unordered_map<std::string, std::vector<Durin::DObject*>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map,
			nullptr,
			nullptr,
			nullptr,
			&ObjectListsKeyPropertyParams,
			&ObjectListsValuePropertyParams
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
		EXPECT_TRUE(ArrayProperty->HasArrayHelper());
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
		EXPECT_TRUE(ObjectPtrArrayProperty->HasArrayHelper());
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
		EXPECT_TRUE(NestedArrayProperty->HasArrayHelper());
		EXPECT_TRUE(NestedArrayInner->HasArrayHelper());
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

	TEST(FCoreDObjectReflectionTests, BuiltInMathStructsExposeNestedFieldMetadata)
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
		EXPECT_EQ(Rotation->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Struct);
		EXPECT_EQ(static_cast<Durin::FStructProperty*>(Rotation)->GetStruct(), QuatStruct);
		EXPECT_EQ(static_cast<Durin::FStructProperty*>(Translation)->GetStruct(), VectorStruct);
		EXPECT_EQ(static_cast<Durin::FStructProperty*>(Scale)->GetStruct(), VectorStruct);
	}
}

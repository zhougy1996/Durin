#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
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

	Durin::DEnum* Z_Construct_DEnum_EReflectedEnumForTest_NoRegister();

	struct FReflectedEnumPropertyOwnerForTest
	{
		EReflectedEnumForTest Mode = EReflectedEnumForTest::A;
	};

	class DLifecycleTestObject : public Durin::DObject
	{
	public:
		explicit DLifecycleTestObject(const Durin::FObjectInitializer& ObjectInitializer = Durin::FObjectInitializer::Get())
			: DObject(ObjectInitializer)
		{
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
	};

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
				{ Durin::FName("A"), 0 },
				{ Durin::FName("B"), 4 },
			};
			Enum = new Durin::DEnum(
				Durin::EC_StaticConstructor,
				Durin::FName("EReflectedEnumForTest"),
				Durin::FName("EReflectedEnumForTest"),
				Durin::FName("EReflectedEnumForTest"),
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

		EXPECT_EQ(Durin::DObject::StaticClass()->GetSuperClass(), nullptr);
		EXPECT_EQ(Durin::DType::StaticClass()->GetSuperClass(), Durin::DObject::StaticClass());
		EXPECT_EQ(Durin::DStructBase::StaticClass()->GetSuperClass(), Durin::DType::StaticClass());
		EXPECT_EQ(Durin::DClass::StaticClass()->GetSuperClass(), Durin::DStructBase::StaticClass());
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

	TEST(FCoreDObjectReflectionTests, PackageOwnsAssetGraphAndBuildsStableObjectPaths)
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
		EXPECT_TRUE(ObjectArrayContains(Inner));

		Durin::RemoveFromRoot(Package);
		Durin::DestroyObject(Package);
		Durin::GDObjectArray.Compact();
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

	TEST(FCoreDObjectReflectionTests, DestroyObjectRemovesRuntimeObject)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("DestroyObjectTestObject"));
		ASSERT_TRUE(ObjectArrayContains(Object));

		Durin::DestroyObject(Object);
		Durin::GDObjectArray.Compact();

		EXPECT_FALSE(ObjectArrayContains(Object));
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
		Durin::DestroyObject(RootedObject);
		Durin::GDObjectArray.Compact();
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
		Durin::DestroyObject(Owner);
		Durin::GDObjectArray.Compact();
	}

	TEST(FCoreDObjectReflectionTests, OuterKeepsInnerObjectReachable)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("GCOuterObject"));
		auto* Inner = Durin::NewObject<DLifecycleTestObject>(Outer, Durin::FName("GCInnerObject"));
		Durin::AddToRoot(Outer);

		Durin::CollectGarbage();

		EXPECT_TRUE(ObjectArrayContains(Outer));
		EXPECT_TRUE(ObjectArrayContains(Inner));
		EXPECT_EQ(Inner->GetOuter(), Outer);

		Durin::RemoveFromRoot(Outer);
		Durin::DestroyObject(Outer);
		Durin::GDObjectArray.Compact();
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
		Durin::DestroyObject(Owner);
		Durin::GDObjectArray.Compact();
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
		Durin::DestroyObject(Owner);
		Durin::DestroyObject(ReferencedObject);
		Durin::DestroyObject(RawVectorReferencedObject);
		Durin::DestroyObject(ObjectPtrVectorReferencedObject);
		Durin::GDObjectArray.Compact();

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

		Durin::DestroyObject(LoadedOwner);
		Durin::GDObjectArray.Compact();
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

		Durin::DestroyObject(ReferencedObject);
		EXPECT_FALSE(ObjectPtr);
		EXPECT_EQ(ObjectPtr.Get(), nullptr);
		EXPECT_EQ(Durin::ResolveObjectHandle(ObjectPtr.GetHandle()), nullptr);

		ObjectPtr.Reset();
		EXPECT_FALSE(ObjectPtr);
		EXPECT_EQ(ObjectPtr.Get(), nullptr);
		EXPECT_TRUE(Durin::IsObjectHandleNull(ObjectPtr.GetHandle()));
	}

	TEST(FCoreDObjectReflectionTests, ObjectPtrStorageMatchesBuildConfiguration)
	{
#if DURIN_WITH_OBJECT_HANDLE
		EXPECT_EQ(DURIN_WITH_OBJECT_HANDLE, 1);
#else
		EXPECT_EQ(DURIN_WITH_OBJECT_HANDLE, 0);
		EXPECT_EQ(sizeof(Durin::FObjectHandle), sizeof(Durin::DObject*));
		EXPECT_EQ(sizeof(Durin::FObjectPtr), sizeof(Durin::DObject*));
		EXPECT_EQ(sizeof(Durin::TObjectPtr<Durin::DObject>), sizeof(Durin::DObject*));
#endif

#if defined(DURIN_BUILD_SHIPPING) && DURIN_BUILD_SHIPPING
		EXPECT_EQ(DURIN_WITH_OBJECT_HANDLE, 0);
#else
		EXPECT_EQ(DURIN_WITH_OBJECT_HANDLE, 1);
#endif

		Durin::DObject* Object = nullptr;
		Durin::FObjectHandle NullHandle = Durin::MakeObjectHandle(Object);
		EXPECT_TRUE(Durin::IsObjectHandleNull(NullHandle));
		EXPECT_EQ(Durin::ResolveObjectHandle(NullHandle), nullptr);

		Durin::DObject* ReferencedObject = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectHandleStorageReferencedObject"));
		Durin::FObjectHandle ObjectHandle = Durin::MakeObjectHandle(ReferencedObject);
		EXPECT_FALSE(Durin::IsObjectHandleNull(ObjectHandle));
		EXPECT_EQ(Durin::ResolveObjectHandle(ObjectHandle), ReferencedObject);

		Durin::DestroyObject(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, ConstructDEnumCreatesRuntimeEnumMetadata)
	{
		static const Durin::DurinCodeGen::FEnumValueParams Values[] = {
			{ "A", 0 },
			{ "B", 4 },
		};
		static const Durin::DurinCodeGen::FEnumParams EnumParams = {
			&Z_Construct_DEnum_EReflectedEnumForTest_NoRegister,
			"EReflectedEnumForTest",
			"EReflectedEnumForTest",
			true,
			Durin::DurinCodeGen::EEnumUnderlyingType::UInt8,
			static_cast<Durin::uint16>(sizeof(EReflectedEnumForTest)),
			Values,
			2
		};

		Durin::DEnum* Enum = Durin::DurinCodeGen::ConstructDEnum(EnumParams);

		ASSERT_NE(Enum, nullptr);
		EXPECT_EQ(Enum->GetClass(), Durin::DEnum::StaticClass());
		EXPECT_TRUE(Enum->IsA(Durin::DType::StaticClass()));
		EXPECT_TRUE(Enum->IsScoped());
		EXPECT_EQ(Enum->GetUnderlyingType(), Durin::DurinCodeGen::EEnumUnderlyingType::UInt8);
		EXPECT_EQ(Enum->GetValues().size(), 2u);

		Durin::int64 Value = -1;
		EXPECT_TRUE(Enum->FindValueByName(Durin::FName("A"), Value));
		EXPECT_EQ(Value, 0);

		Durin::FName Name;
		EXPECT_TRUE(Enum->FindNameByValue(4, Name));
		EXPECT_EQ(Name.ToString(), "B");
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

	TEST(FCoreDObjectReflectionTests, ConstructDClassAttachesGeneratedPropertiesToStructBase)
	{
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
			9
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

		Durin::DestroyObject(ReferencedObject);
	}

	TEST(FCoreDObjectReflectionTests, BuiltInMathStructsExposeNestedFieldMetadata)
	{
		EnsureDObjectInitialized();
		Durin::DStruct* VectorStruct = Durin::Z_Construct_DStruct_Durin_FVector3();
		Durin::DStruct* QuatStruct = Durin::Z_Construct_DStruct_Durin_FQuat();
		Durin::DStruct* TransformStruct = Durin::Z_Construct_DStruct_Durin_FTransform();
		ASSERT_NE(VectorStruct, nullptr);
		ASSERT_NE(QuatStruct, nullptr);
		ASSERT_NE(TransformStruct, nullptr);
		EXPECT_EQ(VectorStruct->GetQualifiedName().ToString(), "Durin::FVector3");
		EXPECT_EQ(QuatStruct->GetQualifiedName().ToString(), "Durin::FQuat");
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

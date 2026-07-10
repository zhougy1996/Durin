#include <gtest/gtest.h>

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

namespace
{
	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64 { return static_cast<const std::vector<T>*>(Container)->size(); }
	template<typename T>
	auto VectorGet(const void* Container, Durin::uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorGetMutable(void* Container, Durin::uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[Index]; }
	template<typename T>
	auto VectorResize(void* Container, Durin::uint64 Num) -> void { static_cast<std::vector<T>*>(Container)->resize(Num); }

	const Durin::DurinCodeGen::FArrayPropertyHelper GIntVectorHelper = {
		&VectorNum<Durin::int32>, &VectorGet<Durin::int32>, &VectorGetMutable<Durin::int32>, &VectorResize<Durin::int32>
	};

	using FScoreMap = std::unordered_map<std::string, Durin::int32>;
	auto MapNum(const void* Container) -> Durin::uint64 { return static_cast<const FScoreMap*>(Container)->size(); }
	auto MapKey(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->first;
	}
	auto MapValue(const void* Container, Durin::uint64 Index) -> const void*
	{
		auto It = static_cast<const FScoreMap*>(Container)->begin(); std::advance(It, Index); return &It->second;
	}
	auto MapClear(void* Container) -> void { static_cast<FScoreMap*>(Container)->clear(); }
	auto CreateString() -> void* { return new std::string(); }
	auto DestroyString(void* Value) -> void { delete static_cast<std::string*>(Value); }
	auto CreateInt() -> void* { return new Durin::int32(); }
	auto DestroyInt(void* Value) -> void { delete static_cast<Durin::int32*>(Value); }
	auto MapInsert(void* Container, const void* Key, const void* Value) -> void
	{
		static_cast<FScoreMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const Durin::int32*>(Value));
	}
	const Durin::DurinCodeGen::FMapPropertyHelper GScoreMapHelper = {
		&MapNum, &MapKey, &MapValue, &MapClear, &CreateString, &DestroyString, &CreateInt, &DestroyInt, &MapInsert
	};

	class DPackageAssetForTest : public Durin::DObject
	{
	public:
		explicit DPackageAssetForTest(const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
			DefaultChild = Durin::NewObject<Durin::DObject>(this, "DefaultChild");
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X) { new (X.GetObj()) DPackageAssetForTest(X); }

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(Durin::EC_StaticConstructor, "DPackageAssetForTest", sizeof(DPackageAssetForTest), alignof(DPackageAssetForTest), Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None, Durin::EClassCastFlags::DClass, (Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DPackageAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DPackageAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FPropertyParamsBase ValueProp = {"Value", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Value)), sizeof(Value), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase LabelProp = {"Label", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Label)), sizeof(Label), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoreInner = {"Scores_Inner", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase ScoresProp = {"Scores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, Scores)), sizeof(Scores), Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &ScoreInner, nullptr, nullptr, false, &GIntVectorHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapKeyProp = {"NamedScores_Key", Durin::EPropertyFlags::None, 1, 0, sizeof(std::string), Durin::DurinCodeGen::EPropertyGenFlags::String};
			static const Durin::DurinCodeGen::FPropertyParamsBase MapValueProp = {"NamedScores_Value", Durin::EPropertyFlags::None, 1, 0, sizeof(Durin::int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32};
			static const Durin::DurinCodeGen::FPropertyParamsBase NamedScoresProp = {"NamedScores", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, NamedScores)), sizeof(NamedScores), Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, nullptr, &MapKeyProp, &MapValueProp, false, nullptr, &GScoreMapHelper};
			static const Durin::DurinCodeGen::FPropertyParamsBase ChildProp = {"DefaultChild", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, DefaultChild)), sizeof(DefaultChild), Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass, nullptr, nullptr, nullptr, nullptr, true};
			static const Durin::DurinCodeGen::FPropertyParamsBase ExternalProp = {"ExternalReference", Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(DPackageAssetForTest, ExternalReference)), sizeof(ExternalReference), Durin::DurinCodeGen::EPropertyGenFlags::Object, &Durin::DObject::StaticClass, nullptr, nullptr, nullptr, nullptr, true};
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {&ValueProp, &LabelProp, &ScoresProp, &NamedScoresProp, &ChildProp, &ExternalProp};
			static const Durin::DurinCodeGen::FClassParams Params = {&StaticClassNoRegister, "Tests::DPackageAssetForTest", "DPackageAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::int32 Value = 0;
		std::string Label;
		std::vector<Durin::int32> Scores;
		FScoreMap NamedScores;
		Durin::TObjectPtr<Durin::DObject> DefaultChild;
		Durin::TObjectPtr<Durin::DObject> ExternalReference;
	};

	auto InitializeAssetTests() -> void
	{
		static const bool Initialized = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::FNameInit();
			Durin::DObjectInit();
			const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "Assets";
			std::filesystem::remove_all(Root);
			Durin::PathUtilities::RegisterMountPoint("/TestAssets/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
	}
}

TEST(FPackageAssetTests, SavesLoadsContainersReferencesAndRegistryMetadata)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/RoundTrip", Path));

	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	Asset->Value = 42;
	Asset->Label = "RoundTrip";
	Asset->Scores = {3, 5, 8};
	Asset->NamedScores = {{"Alpha", 11}, {"Beta", 17}};
	ASSERT_NE(Asset->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Asset->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");

	Durin::DPackage* Package = Asset->GetPackage();
	ASSERT_NE(Package, nullptr);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Package));
	ASSERT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(Path), nullptr);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));

	DPackageAssetForTest* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->Value, 42);
	EXPECT_EQ(Loaded->Label, "RoundTrip");
	EXPECT_EQ(Loaded->Scores, (std::vector<Durin::int32>{3, 5, 8}));
	EXPECT_EQ(Loaded->NamedScores.at("Alpha"), 11);
	EXPECT_EQ(Loaded->NamedScores.at("Beta"), 17);
	ASSERT_NE(Loaded->DefaultChild.Get(), nullptr);
	EXPECT_EQ(Loaded->GetInnerObjects().size(), 1u);
	EXPECT_EQ(Loaded->DefaultChild->GetObjectPath(), "/TestAssets/RoundTrip:DefaultChild");
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().FindLoadedPackage(Path), Loaded->GetPackage());

	EXPECT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(Path));
}

TEST(FPackageAssetTests, RejectsInvalidPaths)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("TestAssets/Relative", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/TestAssets/../Escape", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/Unknown/Asset", Path));
	EXPECT_FALSE(Durin::FAssetPath::TryCreate("/TestAssets/With.dasset", Path));
}

TEST(FPackageAssetTests, LoadsExternalDependenciesAndPreventsPrematureUnload)
{
	InitializeAssetTests();
	Durin::FAssetPath DependencyPath;
	Durin::FAssetPath OwnerPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Dependency", DependencyPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Owner", OwnerPath));

	DPackageAssetForTest* Dependency = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(DependencyPath, Dependency));
	Dependency->Label = "Dependency";
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Dependency->GetPackage()));

	DPackageAssetForTest* Owner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OwnerPath, Owner));
	Owner->ExternalReference = Dependency;
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().SavePackage(Owner->GetPackage()));
	ASSERT_EQ(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(OwnerPath)->Dependencies.size(), 1u);
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath).Error, Durin::Asset::EAssetError::InUse);
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath));

	DPackageAssetForTest* LoadedOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(OwnerPath, LoadedOwner));
	ASSERT_NE(LoadedOwner->ExternalReference.Get(), nullptr);
	EXPECT_EQ(LoadedOwner->ExternalReference->GetObjectPath(), "/TestAssets/Dependency");
	EXPECT_EQ(Durin::Asset::FAssetManager::Get().FindLoadedPackage(DependencyPath)->GetAsset(), LoadedOwner->ExternalReference.Get());

	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(OwnerPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().UnloadPackage(DependencyPath));
	ASSERT_TRUE(Durin::Asset::FAssetManager::Get().GetRegistry().ScanMountedContent());
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(OwnerPath), nullptr);
	EXPECT_NE(Durin::Asset::FAssetManager::Get().GetRegistry().FindAsset(DependencyPath), nullptr);
}

TEST(FPackageAssetTests, RejectsTruncatedPackagesWithoutCachingPartialObjects)
{
	InitializeAssetTests();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TestAssets/Corrupt", Path));
	DPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Asset));
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	const std::filesystem::path File = std::filesystem::path(DURIN_TEST_WORK_DIR) / "Assets" / "Corrupt.dasset";
	std::filesystem::resize_file(File, 12);
	Durin::DObject* Loaded = nullptr;
	const Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Path, Loaded);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::CorruptFile);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
}

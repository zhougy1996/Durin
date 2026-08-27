#include "Factories/Factory.h"
#include "AssetTools/IAssetTools.h"

#include "Asset/AssetOperations.h"
#include "Asset/Load.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "HAL/PlatformLTS.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	class DFactoryAssetForTest : public Durin::DObject
	{
	public:
		explicit DFactoryAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DFactoryAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DFactoryAssetForTest",
					sizeof(DFactoryAssetForTest), alignof(DFactoryAssetForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DFactoryAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DFactoryAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin::DurinCodeGen;
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DFactoryAssetForTest",
				"DFactoryAssetForTest", nullptr, 0};
			static Durin::DClass* Class = ConstructDClass(Params);
			return Class;
		}
	};

	class DAssetToolsFactoryForTest : public Durin::DFactory
	{
	public:
		enum class EMode { Success, Fail, WrongClass, WrongOuter };

		explicit DAssetToolsFactoryForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DFactory(Initializer)
		{
			SupportedClass = DFactoryAssetForTest::StaticClass();
			Formats = {"factorytest"};
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DAssetToolsFactoryForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DAssetToolsFactoryForTest",
					sizeof(DAssetToolsFactoryForTest), alignof(DAssetToolsFactoryForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DAssetToolsFactoryForTest>);
				Class->SetSuperStructBase(Durin::DFactory::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DAssetToolsFactoryForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin::DurinCodeGen;
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DAssetToolsFactoryForTest",
				"DAssetToolsFactoryForTest", nullptr, 0};
			static Durin::DClass* Class = ConstructDClass(Params);
			return Class;
		}

		auto FactoryCreateNew(
			Durin::DClass* InClass,
			Durin::DObject* InParent,
			Durin::FName InName,
			Durin::EObjectFlags Flags,
			Durin::DObject*,
			Durin::FFactoryDiagnostics* Diagnostics) const -> Durin::DObject* override
		{
			if (Mode == EMode::Fail)
			{
				if (Diagnostics) Diagnostics->Report("test factory failure");
				return nullptr;
			}
			if (Mode == EMode::WrongClass)
				return Durin::NewObject(
					Durin::DObject::StaticClass(), InParent, InName, Flags);
			return Durin::NewObject(
				InClass, Mode == EMode::WrongOuter ? nullptr : InParent, InName, Flags);
		}

		auto FactoryCreateFromFile(
			Durin::DClass* InClass,
			Durin::DObject* InParent,
			Durin::FName InName,
			Durin::EObjectFlags Flags,
			std::string_view,
			Durin::DObject* Context,
			Durin::FFactoryDiagnostics* Diagnostics) const -> Durin::DObject* override
		{
			return FactoryCreateNew(
				InClass, InParent, InName, Flags, Context, Diagnostics);
		}

		EMode Mode = EMode::Success;
	};

	using FFactoryCreateNewSignature = Durin::DObject* (Durin::DFactory::*)(
		Durin::DClass*,
		Durin::DObject*,
		Durin::FName,
		Durin::EObjectFlags,
		Durin::DObject*,
		Durin::FFactoryDiagnostics*) const;
	using FFactoryCreateFromFileSignature = Durin::DObject* (Durin::DFactory::*)(
		Durin::DClass*,
		Durin::DObject*,
		Durin::FName,
		Durin::EObjectFlags,
		std::string_view,
		Durin::DObject*,
		Durin::FFactoryDiagnostics*) const;

	static_assert(std::is_same_v<
		decltype(&Durin::DFactory::FactoryCreateNew),
		FFactoryCreateNewSignature>);
	static_assert(std::is_same_v<
		decltype(&Durin::DFactory::FactoryCreateFromFile),
		FFactoryCreateFromFileSignature>);

	auto InitializeFactoryTestGameThread() -> void
	{
		if (Durin::GIsGameThreadIdInitialized) return;
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}

	auto EnsureAssetToolsTestMount() -> void
	{
		static const bool bMounted = [] {
			const std::filesystem::path Root =
				Durin::Testing::GetTestWorkDirectory() / "AssetTools";
			std::filesystem::create_directories(Root);
			Durin::PathUtilities::RegisterMountPointForTests(
				"/AssetToolsTests/", Root.generic_string() + "/");
			return true;
		}();
		(void)bMounted;
	}

	auto MakeFactory(DAssetToolsFactoryForTest::EMode Mode)
		-> DAssetToolsFactoryForTest*
	{
		auto* Factory = Durin::NewObject<DAssetToolsFactoryForTest>(
			nullptr, Durin::FName("ConfiguredAssetToolsFactory"),
			Durin::EObjectFlags::Transient);
		Factory->Mode = Mode;
		return Factory;
	}
}

TEST(DFactoryTests, ExposesAbstractReflectedFactoryContract)
{
	Durin::DClass* FactoryClass = Durin::DFactory::StaticClass();
	ASSERT_NE(FactoryClass, nullptr);
	EXPECT_EQ(FactoryClass->GetSuperClass(), Durin::DObject::StaticClass());
	EXPECT_TRUE(FactoryClass->HasAnyClassFlags(Durin::EClassFlags::Abstract));
}

TEST(DFactoryTests, DiscoversOnlyConcreteFactoryDefaultObjects)
{
	InitializeFactoryTestGameThread();
	for (const Durin::DFactory* Factory :
		Durin::DFactory::GetAvailableFactories())
	{
		ASSERT_NE(Factory, nullptr);
		ASSERT_NE(Factory->GetClass(), nullptr);
		EXPECT_FALSE(Factory->GetClass()->HasAnyClassFlags(
			Durin::EClassFlags::Abstract));
		EXPECT_EQ(Factory->GetClass()->GetDefaultObject(), Factory);
	}
}

TEST(DFactoryTests, RejectsEmptyFactoryLookups)
{
	InitializeFactoryTestGameThread();
	Durin::DFactory::InvalidateFactoryCache();
	EXPECT_EQ(Durin::DFactory::FindFactory(nullptr), nullptr);
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension({}), nullptr);
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension("."), nullptr);
	Durin::DFactory::InvalidateFactoryCache();
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension({}), nullptr);
}

TEST(DFactoryTests, AssetToolsRejectUnsupportedCreationWithoutLeakingPackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/UnsupportedFactoryAsset", Path));
	ASSERT_EQ(Durin::FindPackage(Path.GetView()), nullptr);

	Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, Durin::DObject::StaticClass());
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Asset, nullptr);
	EXPECT_EQ(Result.Package, nullptr);
	EXPECT_FALSE(Result.Message.empty());
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(DFactoryTests, AssetToolsRequireSourceFilenameBeforeCreatingPackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/MissingFactorySource", Path));
	ASSERT_EQ(Durin::FindPackage(Path.GetView()), nullptr);

	Durin::FAssetToolsResult Result = Durin::GetAssetTools().ImportAsset(
		Path, Durin::DObject::StaticClass(), {});
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Message, "A source filename is required for import.");
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(DFactoryTests, KeepsConfiguredStateOnTransientInvocationInstance)
{
	InitializeFactoryTestGameThread();
	auto* Configured = MakeFactory(DAssetToolsFactoryForTest::EMode::Fail);
	EXPECT_FALSE(Configured->IsClassDefaultObject());
	EXPECT_EQ(Configured->Mode, DAssetToolsFactoryForTest::EMode::Fail);
}

TEST(DFactoryTests, FactoryFailureReportsDiagnosticAndDiscardsPackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/FactoryFailure", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Fail);
	const Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, DFactoryAssetForTest::StaticClass(), Factory);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Message, "test factory failure");
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
}

TEST(DFactoryTests, WrongOuterIsRejectedAndPackageIsDiscarded)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/WrongOuter", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::WrongOuter);
	const Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, DFactoryAssetForTest::StaticClass(), Factory);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
}

TEST(DFactoryTests, WrongClassIsRejectedAndPackageIsDiscarded)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/WrongClass", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::WrongClass);
	const Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, DFactoryAssetForTest::StaticClass(), Factory);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
}

TEST(DFactoryTests, CreatedPackageSurvivesGcAndCanBeExplicitlyDiscarded)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/ExplicitDiscard", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, DFactoryAssetForTest::StaticClass(), Factory);
	ASSERT_TRUE(Result);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), Result.Package);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Result.Package);
	EXPECT_TRUE(Durin::GetAssetTools().DiscardPackage(Result.Package));
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(DFactoryTests, SaveFailureLeavesCreatedPackageAvailableForDiscard)
{
	InitializeFactoryTestGameThread();
	const std::filesystem::path InvalidRoot =
		Durin::Testing::GetTestWorkDirectory() / "AssetToolsSaveFailureRoot";
	Durin::PathUtilities::RegisterMountPointForTests(
		"/AssetToolsSaveFailure/", InvalidRoot.generic_string() + "/");
	Durin::Testing::RemoveTestWorkDirectory(InvalidRoot);
	const std::array InvalidRootBytes{std::byte{1}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(InvalidRootBytes, InvalidRoot));
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsSaveFailure/SaveFailure", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, DFactoryAssetForTest::StaticClass(), Factory);
	ASSERT_TRUE(Result);
	EXPECT_FALSE(Durin::Asset::SavePackage(Result.Package));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Result.Package);
	EXPECT_TRUE(Durin::GetAssetTools().DiscardPackage(Result.Package));
}

TEST(DFactoryTests, SavedFactoryPackageReloadsWithoutDuplicateLivePackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/AssetToolsTests/SaveReload", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const Durin::FAssetToolsResult Result = Durin::GetAssetTools().CreateAsset(
		Path, DFactoryAssetForTest::StaticClass(), Factory);
	ASSERT_TRUE(Result);
	ASSERT_TRUE(Durin::Asset::SavePackage(Result.Package));
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Result.Package);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	Durin::DObject* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, Reloaded));
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_NE(Reloaded->GetPackage(), Result.Package);
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), Reloaded->GetPackage());
	EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), Reloaded->GetPackage());
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

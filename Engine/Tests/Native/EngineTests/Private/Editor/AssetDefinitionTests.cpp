#include "NativeAssetRuntimeTestSupport.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
	class FTestAssetDefinition : public DAssetDefinition
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FTestAssetDefinition, DAssetDefinition, FTestAssetDefinition::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FTestAssetDefinition)
	public:
		explicit FTestAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetClassPath() const -> std::string override { return ClassPath; }
		auto CanRegisterStatically() const -> bool override { return false; }
		std::string ClassPath;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FTestAssetDefinition)

	class FAutomaticTestAssetDefinition : public DAssetDefinition
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FAutomaticTestAssetDefinition, DAssetDefinition, FAutomaticTestAssetDefinition::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FAutomaticTestAssetDefinition)
	public:
		explicit FAutomaticTestAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetClassPath() const -> std::string override { return "Test.AutomaticAsset"; }
	};
	auto FAutomaticTestAssetDefinition::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FAutomaticTestAssetDefinition", Class, nullptr,
				sizeof(FAutomaticTestAssetDefinition), alignof(FAutomaticTestAssetDefinition),
				EClassFlags::None, InternalConstructor<FAutomaticTestAssetDefinition>,
				&DAssetDefinition::StaticClass);
		return Class;
	}
}

namespace
{
	using namespace Durin;
	class FAssetDefinitionTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override { Testing::InitializeAssetSystemForTests(); }
		auto TearDown() -> void override { CollectGarbage(Durin::EObjectFlags::Standalone); }
		auto Definition(std::string Path) -> FTestAssetDefinition*
		{
			auto* Object = NewObject<FTestAssetDefinition>(nullptr, {});
			Object->ClassPath = std::move(Path);
			return Object;
		}
	};

	TEST_F(FAssetDefinitionTests, ExactDefinitionOverridesAncestorAndRemovalImmediatelyRestoresFallback)
	{
		FAssetDefinitionRegistry Registry;
		auto* Base = Definition(DObject::StaticClass()->GetQualifiedName().ToString());
		auto BaseRegistration = Registry.RegisterAssetDefinition(*Base);
		ASSERT_TRUE(BaseRegistration);
		const auto Path = DPackage::StaticClass()->GetQualifiedName().ToString();
		EXPECT_EQ(Base, Registry.GetAssetDefinitionForClass(Path));
		EXPECT_EQ(Base, Registry.GetAssetDefinitionForClass(DPackage::StaticClass()));
		const auto Revision = Registry.GetAssetDefinitionVersion();
		auto* Exact = Definition(Path);
		auto ExactRegistration = Registry.RegisterAssetDefinition(*Exact);
		ASSERT_TRUE(ExactRegistration);
		EXPECT_GT(Registry.GetAssetDefinitionVersion(), Revision);
		EXPECT_EQ(Exact, Registry.GetAssetDefinitionForClass(Path));
		EXPECT_EQ(Exact, Registry.GetAssetDefinitionForClass(DPackage::StaticClass()));
		EXPECT_EQ(nullptr, Registry.GetAssetDefinitionForClass(nullptr));
		ExactRegistration.Reset();
		EXPECT_EQ(Base, Registry.GetAssetDefinitionForClass(Path));
		EXPECT_EQ(Base, Registry.GetAssetDefinitionForClass(DPackage::StaticClass()));
		BaseRegistration.Reset();
		EXPECT_EQ(nullptr, Registry.GetAssetDefinitionForClass(Path));
	}

	TEST_F(FAssetDefinitionTests, DuplicateAndInvalidDefinitionsLeaveMembershipAndRevisionUnchanged)
	{
		FAssetDefinitionRegistry Registry;
		auto* First = Definition("Test.Asset");
		auto Registration = Registry.RegisterAssetDefinition(*First);
		ASSERT_TRUE(Registration);
		const auto Revision = Registry.GetAssetDefinitionVersion();
		std::string Error;
		EXPECT_FALSE(Registry.RegisterAssetDefinition(*Definition("Test.Asset"), &Error));
		EXPECT_NE(std::string::npos, Error.find("Test.Asset"));
		EXPECT_EQ(Revision, Registry.GetAssetDefinitionVersion());
		EXPECT_FALSE(Registry.RegisterAssetDefinition(*Definition(""), &Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_EQ(First, Registry.GetAssetDefinitionForClass("Test.Asset"));
		EXPECT_EQ(Revision, Registry.GetAssetDefinitionVersion());
	}

	TEST_F(FAssetDefinitionTests, DynamicLeaseRetainsDefinitionAcrossCollectionAndReleasesItOnReset)
	{
		FAssetDefinitionRegistry Registry;
		auto* Object = Definition("Test.Asset");
		const FObjectKey Key(Object);
		auto Registration = Registry.RegisterAssetDefinition(*Object);
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(Object, Key.ResolveObjectPtr());
		EXPECT_EQ(Object, Registry.GetAssetDefinitionForClass("Test.Asset"));
		Registration.Reset();
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Key.ResolveObjectPtr());
		EXPECT_EQ(nullptr, Registry.GetAssetDefinitionForClass("Test.Asset"));
	}

	TEST_F(FAssetDefinitionTests, OldLeaseCannotRemoveAReplacementDefinition)
	{
		FAssetDefinitionRegistry Registry;
		auto* First = Definition("Test.Asset");
		auto Old = Registry.RegisterAssetDefinition(*First);
		Registry.UnregisterAssetDefinition(FObjectKey(First));
		auto* Replacement = Definition("Test.Asset");
		auto Current = Registry.RegisterAssetDefinition(*Replacement);
		ASSERT_TRUE(Current);
		Old.Reset();
		EXPECT_EQ(Replacement, Registry.GetAssetDefinitionForClass("Test.Asset"));
	}

	TEST_F(FAssetDefinitionTests, LeaseMayOutliveRegistryWithoutKeepingDefinitionAlive)
	{
		FAssetDefinitionRegistration Registration;
		FObjectKey Key;
		{
			FAssetDefinitionRegistry Registry;
			auto* Object = Definition("Test.Asset");
			Key = FObjectKey(Object);
			Registration = Registry.RegisterAssetDefinition(*Object);
			ASSERT_TRUE(Registration);
		}
		EXPECT_FALSE(Registration.IsValid());
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Key.ResolveObjectPtr());
		Registration.Reset();
	}

	TEST_F(FAssetDefinitionTests, LateClassDefaultObjectRegistersAndRetiresAutomatically)
	{
		auto& Registry = FAssetDefinitionRegistry::Get();
		EXPECT_EQ(nullptr, Registry.GetAssetDefinitionForClass("Test.AutomaticAsset"));
		DClass* Class = FAutomaticTestAssetDefinition::StaticClass();
		const std::array<DClass*, 1> Classes{Class};
		ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
		const auto* Default = Cast<DAssetDefinition>(Class->GetDefaultObject());
		ASSERT_NE(nullptr, Default);
		EXPECT_EQ(Default, Registry.GetAssetDefinitionForClass("Test.AutomaticAsset"));
		Private::ReleaseClassDefaultObjectForTests(Class);
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(nullptr, Registry.GetAssetDefinitionForClass("Test.AutomaticAsset"));
	}

	TEST_F(FAssetDefinitionTests, MetadataQueriesDoNotConstructOrLoadObjects)
	{
		FAssetDefinitionRegistry Registry;
		auto* Object = Definition("UnloadedProject.Asset");
		auto Registration = Registry.RegisterAssetDefinition(*Object);
		const auto ObjectRevision = GDObjectArray.GetRevision();
		EXPECT_EQ(Object, Registry.GetAssetDefinitionForClass("UnloadedProject.Asset"));
		EXPECT_EQ(nullptr, Registry.GetAssetDefinitionForClass("UnknownProject.Asset"));
		EXPECT_TRUE(Object->GetAssetCategories().empty());
		EXPECT_TRUE(Object->GetAssetOpenSupport({EAssetOpenMethod::Edit}).IsSupported);
		EXPECT_FALSE(Object->GetAssetOpenSupport({EAssetOpenMethod::View}).IsSupported);
		EXPECT_EQ(ObjectRevision, GDObjectArray.GetRevision());
	}
}

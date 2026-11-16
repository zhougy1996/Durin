#include "AssetTools/AssetImportAdapter.h"
#include "FactoryImportTestSupport.h"
#include "AssetImportTask.h"
#include "AssetRegistry/Publication.h"
#include "Editor/TransactionRecordTestSupport.h"
#include "Asset/Redirector.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Misc/FeedbackContext.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/UnifiedError.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "NativePackageUnloadTestSupport.h"
#include "AssetTools/IAssetTools.h"
#include "Import/EditorReimportHandler.h"

#include "Asset/PackageSerialization.h"
#include "Asset/RegistryOperations.h"
#include "Asset/Testing/PackageSaveTesting.h"
#include "Asset/Load.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "HAL/PlatformLTS.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/MountPaths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Materials/Material.h"
#include "AssetRegistry/Scan.h"

namespace
{
	auto SavePackagesForTest(std::vector<Durin::FPackagePath> Paths, Durin::FFeedbackContext* Feedback = nullptr,
		std::function<void(const Durin::FPackagePath&)> OnSaved = {}) -> bool
	{
		Durin::FScopedFeedbackContext Scope(Feedback ? *Feedback : Durin::FFeedbackContext::Get());
		Durin::FScopedPackageSaveObserver Observer(std::move(OnSaved));
		std::vector<Durin::DPackage*> Packages;
		for (const auto& Path : Paths) Packages.push_back(Durin::FindResidentPackage(Path));
		return Durin::DEditorLoadingAndSavingUtils::SavePackages(Packages, true);
	}

	auto CreateAssetForTest(const Durin::FPackagePath& Path, Durin::DClass* Class,
		Durin::DFactory* Factory, Durin::FFeedbackContext* Feedback) -> Durin::DObject*
	{
		Durin::FScopedFeedbackContext Scope(*Feedback);
		const auto Text = Path.ToString();
		return Durin::IAssetTools::Get().CreateAsset(Path.GetPackageName(),
			Text.substr(0, Text.find_last_of('/')), Class, Factory);
	}

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

	class DAssetRenameReferencesForTest : public Durin::DObject
	{
	public:
		explicit DAssetRenameReferencesForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DAssetRenameReferencesForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, "DAssetRenameReferencesForTest",
					sizeof(DAssetRenameReferencesForTest), alignof(DAssetRenameReferencesForTest),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DAssetRenameReferencesForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				auto* ValueProperty = new Durin::FNumericProperty(
					Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DAssetRenameReferencesForTest, Value),
					sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
				Durin::Tests::ObjectState::SetValueLifecycle<int32>(*ValueProperty);

				auto* HardProperty = new Durin::FObjectProperty(
					Durin::FFieldVariant(Class), Durin::FName("Hard"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DAssetRenameReferencesForTest, Hard),
					sizeof(Durin::TObjectPtr<Durin::DObject>),
					Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true,
					[](const void* Value) -> Durin::DObject* {
						return static_cast<const Durin::TObjectPtr<Durin::DObject>*>(Value)->Get();
					},
					[](void* Value, Durin::DObject* Object) {
						*static_cast<Durin::TObjectPtr<Durin::DObject>*>(Value) = Object;
					});
				Durin::Tests::ObjectState::SetValueLifecycle<Durin::TObjectPtr<Durin::DObject>>(*HardProperty);

				auto* SoftProperty = new Durin::FSoftObjectProperty(
					Durin::FFieldVariant(Class), Durin::FName("Soft"), Durin::EObjectFlags::NoFlags,
					Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DAssetRenameReferencesForTest, Soft),
					sizeof(Durin::TSoftObjectPtr<Durin::DObject>), Durin::DObject::StaticClass(),
					[](void* Value) -> Durin::FSoftObjectPtr* {
						return &static_cast<Durin::TSoftObjectPtr<Durin::DObject>*>(Value)->GetBase();
					},
					[](const void* Value) -> const Durin::FSoftObjectPtr* {
						return &static_cast<const Durin::TSoftObjectPtr<Durin::DObject>*>(Value)->GetBase();
					});
				Durin::Tests::ObjectState::SetValueLifecycle<Durin::TSoftObjectPtr<Durin::DObject>>(*SoftProperty);

				ValueProperty->SetNext(HardProperty); HardProperty->SetNext(SoftProperty);
				Class->SetChildProperties(ValueProperty);

				Class->Register(Durin::DClass::StaticClass, "", "DAssetRenameReferencesForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			using namespace Durin::DurinCodeGen;
			static const FClassParams Params{
				&StaticClassNoRegister, "Tests::DAssetRenameReferencesForTest",
				"DAssetRenameReferencesForTest", nullptr, 0};
			static Durin::DClass* Class = ConstructDClass(Params);
			return Class;
		}
		int32 Value = 0;
		Durin::TObjectPtr<Durin::DObject> Hard;
		Durin::TSoftObjectPtr<Durin::DObject> Soft;

	};

	class DAssetToolsFactoryForTest : public Durin::DFactory
	{
	public:
		enum class EMode { Success, Fail, TypedFail, WrongClass, WrongOuter, WrongName };

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
			Durin::FFeedbackContext* Warn, Durin::FName CallingContext) -> Durin::DObject* override
		{
			++Calls;
			if (OnCreate) OnCreate();
			LastCallingContext = CallingContext;
		Durin::FFactoryDiagnostics DiagnosticStorage(Warn);
		auto* Diagnostics = &DiagnosticStorage;
			if (Mode == EMode::TypedFail)
			{
				if (Diagnostics) Diagnostics->ReportFailure({
					.Code = Durin::EFactoryError::ObjectCreation,
					.ExpectedClass = InClass->GetName(), .RequestedClass = InClass->GetName()});
				return nullptr;
			}
			if (Mode == EMode::Fail)
			{
				if (Diagnostics) Diagnostics->Report("test factory failure");
				return nullptr;
			}
			if (Mode == EMode::WrongClass)
				return Durin::NewObject(
					Durin::DObject::StaticClass(), InParent, InName, Flags);
			return Durin::NewObject(
				InClass, Mode == EMode::WrongOuter ? nullptr : InParent,
				Mode == EMode::WrongName ? Durin::FName("UnexpectedName") : InName, Flags);
		}

		auto FactoryCreateFile(
			Durin::DClass* InClass,
			Durin::DObject* InParent,
			Durin::FName InName,
			Durin::EObjectFlags Flags,
			std::string_view,
			std::string_view Parms,
			Durin::FFeedbackContext* Warn, bool& bOutOperationCanceled) -> Durin::DObject* override
		{
		Durin::FFactoryDiagnostics DiagnosticStorage(Warn);
		auto* Diagnostics = &DiagnosticStorage;
		bOutOperationCanceled = false;
			return FactoryCreateNew(
				InClass, InParent, InName, Flags, nullptr, Warn, {});
		}

		auto ConfigureImportTask(const Durin::DAssetImportTask& Task,
			const Durin::DObject* Existing, Durin::FFeedbackContext* Warn, std::function<void()>& Restore) -> bool override
		{
			LastOptions = Task.Options.Get();
			bSawExisting = Existing != nullptr;
			bReplacedSettings = Task.bReplaceExistingSettings;
			if (Task.Options && Task.Options->GetClass() != DFactoryAssetForTest::StaticClass())
				return Durin::DFactory::ConfigureImportTask(Task, Existing, Warn, Restore);
			return true;
		}
		std::function<void()> OnCreate;
		Durin::DObject* LastOptions = nullptr;
		bool bSawExisting = false, bReplacedSettings = false;
		EMode Mode = EMode::Success;
		uint32 Calls = 0;
		Durin::FName LastCallingContext;
	};

	class FStandaloneReimportHandlerForTest final : public Durin::FReimportHandler
	{
	public:
		explicit FStandaloneReimportHandlerForTest(int32 InPriority)
			: Priority(InPriority)
		{
		}

		auto GetPriority() const -> int32 override { return Priority; }

		auto QueryReimportActions(std::string_view ClassName) const
			-> Durin::FReimportActions override
		{
			if (ClassName != "MetadataOnlyAsset") return {};
			return {.bSupportsReimport = true, .bSupportsReimportFromFile = true};
		}

		auto GetSourceFileDialogs(const Durin::DObject&) const
			-> std::vector<Durin::FReimportSourceFileDialog> override
		{
			return {{std::to_string(Priority), "Test source", "*.test"}};
		}

		mutable int ObjectQueryCount = 0;
		bool bSupportsObject = true;
		Durin::EReimportResult Outcome = Durin::EReimportResult::Succeeded;

		auto GetReimportCapabilities(const Durin::DObject& Object) const
			-> Durin::FReimportCapabilities override
		{
			++ObjectQueryCount;
			if (!bSupportsObject) return {};

			if (Object.GetClass() != DFactoryAssetForTest::StaticClass()) return {};
			return {.bCanReimport = true, .bCanReimportFromFile = true};
		}

		auto Reimport(Durin::DObject&, Durin::FReimportCompletion Completion) const
			-> void override
		{
			if (Completion) Completion({Outcome,
				std::to_string(Priority)});
		}

		auto ReimportFromFiles(Durin::DObject&, std::span<const std::string>,
			Durin::FReimportCompletion Completion) const -> void override
		{
			if (Completion) Completion({Outcome,
				std::to_string(Priority)});
		}

	private:
		int32 Priority = 0;
	};

	using FFactoryCreateNewSignature = Durin::DObject* (Durin::DFactory::*)(
		Durin::DClass*, Durin::DObject*, Durin::FName, Durin::EObjectFlags,
		Durin::DObject*, Durin::FFeedbackContext*, Durin::FName);
	using FFactoryCreateFileSignature = Durin::DObject* (Durin::DFactory::*)(
		Durin::DClass*, Durin::DObject*, Durin::FName, Durin::EObjectFlags,
		std::string_view, std::string_view, Durin::FFeedbackContext*, bool&);

	static_assert(std::is_same_v<
		decltype(&Durin::DFactory::FactoryCreateNew),
		FFactoryCreateNewSignature>);
	static_assert(std::is_same_v<
		decltype(&Durin::DFactory::FactoryCreateFile),
		FFactoryCreateFileSignature>);

	auto InitializeFactoryTestGameThread() -> void
	{
		Durin::Testing::InitializeAssetSystemForTests();
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
			Durin::Testing::RegisterMountPointForTests(
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

TEST(DFactoryTests, FileListKeepsFirstCollisionSuccessAndReturnsNoNullSlots)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const std::array<std::string, 2> Files{"ListDuplicate.factorytest", "other/ListDuplicate.factorytest"};
	const auto Objects = IAssetTools::Get().ImportAssets(Files, "/AssetToolsTests", Factory, false);
	ASSERT_EQ(Objects.size(), 1u); EXPECT_NE(Objects.front(), nullptr);
	EXPECT_EQ(Factory->Calls, 1u); EXPECT_TRUE(Objects.front()->GetPackage()->IsDirty());
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Objects.front()->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DFactoryTests, FileListMappingsAndFailuresRetainSuccessfulPeers)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const std::array<std::string, 3> Files{"ListFirst.factorytest", "", "ListLast.factorytest"};
	std::vector<std::pair<std::string,std::string>> Mappings{{Files.back(), "/AssetToolsTests/Mapped"}};
	const auto Objects = IAssetTools::Get().ImportAssets(Files, "/AssetToolsTests", Factory, false, &Mappings);
	ASSERT_EQ(Objects.size(), 2u);
	EXPECT_EQ(Objects[0]->GetObjectPath(), "/AssetToolsTests/ListFirst.ListFirst");
	EXPECT_EQ(Objects[1]->GetObjectPath(), "/AssetToolsTests/Mapped/ListLast.ListLast");
	for (auto* Object : Objects) EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DFactoryTests, FileListRejectsUncatalogedBytesAndInvalidFactoryProduct)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	const auto File = Testing::GetTestWorkDirectory() / "AssetTools/ListOccupied.dasset";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::array{std::byte{42}}, File));
	const std::array<std::string, 1> Files{"ListOccupied.factorytest"};
	EXPECT_TRUE(IAssetTools::Get().ImportAssets(Files, "/AssetToolsTests", MakeFactory(DAssetToolsFactoryForTest::EMode::Success), false).empty());
	EXPECT_EQ(std::filesystem::file_size(File), 1u); std::filesystem::remove(File);
	const std::array<std::string, 1> Wrong{"ListWrongName.factorytest"};
	EXPECT_TRUE(IAssetTools::Get().ImportAssets(Wrong, "/AssetToolsTests", MakeFactory(DAssetToolsFactoryForTest::EMode::WrongName), false).empty());
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

TEST(DFactoryTests, AssetToolsCreatesConcreteAssetsWithoutSpecializedFactory)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsTests/UnsupportedFactoryAsset", Path));
	ASSERT_EQ(Durin::FindPackage(Path.GetView()), nullptr);

	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	auto Result = CreateAssetForTest(Path, Durin::DObject::StaticClass(), nullptr, &ResultFeedback);
	ASSERT_TRUE(Result) << ResultError;
	ASSERT_NE(Result, nullptr);
	ASSERT_NE(Result->GetPackage(), nullptr);
	EXPECT_EQ(Result->GetOuter(), Result->GetPackage());
	EXPECT_EQ(Result->GetPackage()->FindTopLevelAsset(Result->GetFName()), Result);
	EXPECT_TRUE(([&] { std::string Error; return Durin::DPackageTools::UnloadPackages(std::array{Result->GetPackage()}, Error, true) && Error.empty(); }()));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(DFactoryTests, AssetToolsRequireSourceFilenameBeforeCreatingPackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsTests/MissingFactorySource", Path));
	ASSERT_EQ(Durin::FindPackage(Path.GetView()), nullptr);

	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	auto Result = Durin::Testing::ImportFactoryAssetForTest(Path, Durin::DObject::StaticClass(), {}, nullptr, &ResultFeedback);
	EXPECT_FALSE(Result);
	EXPECT_EQ(ResultError, "A source filename is required for import.");
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
}

TEST(DFactoryTests, KeepsConfiguredStateOnTransientInvocationInstance)
{
	InitializeFactoryTestGameThread();
	auto* Configured = MakeFactory(DAssetToolsFactoryForTest::EMode::Fail);
	EXPECT_FALSE(Configured->IsClassDefaultObject());
	EXPECT_EQ(Configured->Mode, DAssetToolsFactoryForTest::EMode::Fail);
}

TEST(DFactoryTests, MetadataQueriesDoNotInspectObjectsAndCommandsRevalidate)
{
	InitializeFactoryTestGameThread();
	FStandaloneReimportHandlerForTest Handler(100);
	Handler.bSupportsObject = false;
	for (int Frame = 0; Frame < 100; ++Frame)
	{
		const auto Actions = Durin::FReimportManager::QueryReimportActions("MetadataOnlyAsset");
		EXPECT_TRUE(Actions.bSupportsReimport);
		EXPECT_TRUE(Actions.bSupportsReimportFromFile);
	}
	EXPECT_EQ(Handler.ObjectQueryCount, 0);
	EXPECT_FALSE(Durin::FReimportManager::QueryReimportActions("UnknownAsset").bSupportsReimport);
	auto* Object = Durin::NewObject<DFactoryAssetForTest>(
		nullptr, Durin::FName("RevalidatedReimportObject"), Durin::EObjectFlags::Transient);
	std::string Error;
	EXPECT_TRUE(Durin::FReimportManager::GetSourceFileDialogs(*Object, Error).empty());
	EXPECT_FALSE(Error.empty());
	Durin::FReimportResult Result;
	Durin::FReimportManager::Reimport(*Object, {.bSave = false},
		[&](Durin::FReimportResult Completed) { Result = std::move(Completed); });
	EXPECT_EQ(Result.Status, Durin::EReimportResult::Failed);
	EXPECT_GT(Handler.ObjectQueryCount, 0);
}

TEST(DFactoryTests, ReimportHandlersRegisterIndependentlyAndUsePriority)
{
	InitializeFactoryTestGameThread();
	auto* Object = Durin::NewObject<DFactoryAssetForTest>(
		nullptr, Durin::FName("StandaloneReimportObject"),
		Durin::EObjectFlags::Transient);

	{
		FStandaloneReimportHandlerForTest LowerPriority(10);
		FStandaloneReimportHandlerForTest HigherPriority(20);
		const Durin::FReimportCapabilities Capabilities =
			Durin::FReimportManager::GetCapabilities(*Object);
		EXPECT_TRUE(Capabilities.bCanReimport);

		std::string Error;
		const auto Dialogs = Durin::FReimportManager::GetSourceFileDialogs(*Object, Error);
		ASSERT_EQ(Dialogs.size(), 1u);
		EXPECT_EQ(Dialogs.front().Title, "20");
		EXPECT_TRUE(Error.empty());

		Durin::FReimportResult Result;
		Durin::FReimportManager::Reimport(*Object, {.bSave = false},
			[&Result](Durin::FReimportResult Completed) {
				Result = std::move(Completed);
			});
		EXPECT_TRUE(Result);
		EXPECT_EQ(Result.Message, "20");
	}

	const Durin::FReimportCapabilities Capabilities =
		Durin::FReimportManager::GetCapabilities(*Object);
	EXPECT_FALSE(Capabilities.bCanReimport);
	EXPECT_FALSE(Capabilities.bCanReimportFromFile);
}

TEST(DFactoryTests, FactoryFailureReportsDiagnosticAndDiscardsPackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsTests/FactoryFailure", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Fail);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), Factory, &ResultFeedback);
	EXPECT_FALSE(Result);
	EXPECT_NE(ResultError.find("test factory failure"), std::string::npos);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
}

TEST(DFactoryTests, WrongOuterIsRejectedAndPackageIsDiscarded)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsTests/WrongOuter", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::WrongOuter);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), Factory, &ResultFeedback);
	EXPECT_FALSE(Result);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
}

TEST(DFactoryTests, WrongClassIsRejectedAndPackageIsDiscarded)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsTests/WrongClass", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::WrongClass);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), Factory, &ResultFeedback);
	EXPECT_FALSE(Result);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
}

TEST(DFactoryTests, SaveFailureLeavesCreatedPackageAvailableForDiscard)
{
	InitializeFactoryTestGameThread();
	const std::filesystem::path InvalidRoot =
		Durin::Testing::GetTestWorkDirectory() / "AssetToolsSaveFailureRoot";
	Durin::Testing::RegisterMountPointForTests(
		"/AssetToolsSaveFailure/", InvalidRoot.generic_string() + "/");
	Durin::Testing::RemoveTestWorkDirectory(InvalidRoot);
	ASSERT_TRUE(std::filesystem::create_directories(InvalidRoot));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsSaveFailure/SaveFailure", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), Factory, &ResultFeedback);
	ASSERT_TRUE(Result) << ResultError;
	const auto Blocker = InvalidRoot / "SaveFailure.dasset";
	ASSERT_TRUE(std::filesystem::create_directory(Blocker));
	EXPECT_FALSE(Durin::SavePackage(Result->GetPackage()));
	EXPECT_TRUE(std::filesystem::remove(Blocker));
	EXPECT_EQ(Durin::FindResidentPackage(Path), Result->GetPackage());
	EXPECT_TRUE(([&] { std::string Error; return Durin::DPackageTools::UnloadPackages(std::array{Result->GetPackage()}, Error, true) && Error.empty(); }()));
}

TEST(DFactoryTests, SavedFactoryPackageReloadsWithoutDuplicateLivePackage)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsTests/SaveReload", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), Factory, &ResultFeedback);
	ASSERT_TRUE(Result);
	EXPECT_TRUE(Result->GetPackage()->IsNewlyCreated());
	ASSERT_TRUE(Durin::SavePackage(Result->GetPackage()));
	EXPECT_FALSE(Result->GetPackage()->IsNewlyCreated());
	EXPECT_EQ(Durin::FindResidentPackage(Path), Result->GetPackage());
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	Durin::DObject* Reloaded = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Reloaded = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), Reloaded->GetPackage());
	EXPECT_EQ(Durin::FindResidentPackage(Path), Reloaded->GetPackage());
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(DFactoryTests, SaveNotifiesEachSavedPackageAndDuplicateRequiresExplicitSave)
{
	using namespace Durin;
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	FPackagePath SourcePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetToolsTests/OperationSource", SourcePath));
	auto* Created = CreateAssetForTest(SourcePath, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), &(Durin::FFeedbackContext::Get()));
	ASSERT_NE(Created, nullptr);
	std::vector<FPackagePath> Saved;
	ASSERT_TRUE(SavePackagesForTest({SourcePath}, nullptr, [&](const FPackagePath& Path) { Saved.push_back(Path); }));
	EXPECT_EQ(Saved, std::vector{SourcePath});
	auto* Copy = ([&] { Durin::FScopedFeedbackContext Scope(Durin::FFeedbackContext::Get()); auto* Source = Durin::LoadObject(Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath), nullptr); std::vector<Durin::DObject*> Copies; Durin::ObjectTools::DuplicateObjects(std::span{&Source, 1}, {}, "/AssetToolsTests/", false, &Copies); return Copies.empty() ? nullptr : Copies.front(); }());
	ASSERT_NE(Copy, nullptr);
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	const auto CopyPath = Copy->GetPackage()->GetPackagePathIdentity();
	ASSERT_TRUE(SavePackagesForTest({CopyPath}, nullptr, {}));
	EXPECT_TRUE(UnloadPackage(CopyPath));
	if (FindResidentPackage(SourcePath)) EXPECT_TRUE(UnloadPackage(SourcePath));
	EXPECT_EQ(FindResidentPackage(SourcePath), nullptr);
}

TEST(DFactoryTests, BatchSaveContinuesAfterFailureAndPublishesOnlySavedPackages)
{
	using namespace Durin;
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	std::vector<FPackagePath> Paths;
	std::vector<DPackage*> Packages;
	for (const auto* Name : {"BatchFirst", "BatchBlocked", "BatchLast"})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::string("/AssetToolsTests/") + Name, Path));
		std::string CreatedError;
		Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
			if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
		});
		const auto Created = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), &CreatedFeedback);
		ASSERT_TRUE(Created);
		Paths.push_back(Path);
		Packages.push_back(Created->GetPackage());
	}
	const auto Blocker = Testing::GetTestWorkDirectory() / "AssetTools" / "BatchBlocked.dasset";
	ASSERT_TRUE(std::filesystem::create_directory(Blocker));
	std::vector<FPackagePath> Notifications;
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = SavePackagesForTest({Paths[0], Paths[1], Paths[2], Paths[0]}, &ResultFeedback, [&](const FPackagePath& Path) { Notifications.push_back(Path); });
	EXPECT_FALSE(Result);
	EXPECT_NE(ResultError.find(Paths[1].ToString()), std::string::npos);
	EXPECT_EQ(Notifications, (std::vector{Paths[0], Paths[2]}));
	EXPECT_FALSE(Packages[0]->IsDirty());
	EXPECT_TRUE(Packages[1]->IsDirty());
	EXPECT_FALSE(Packages[2]->IsDirty());
	std::string FailedOnlyError;
	Durin::FFeedbackContext FailedOnlyFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!FailedOnlyError.empty()) FailedOnlyError += "\n"; FailedOnlyError += Message; }
	});
	const auto FailedOnly = SavePackagesForTest({Paths[1]}, &FailedOnlyFeedback, [&](const FPackagePath& Path) { Notifications.push_back(Path); });
	EXPECT_FALSE(FailedOnly);
	EXPECT_EQ(Notifications, (std::vector{Paths[0], Paths[2]}));
	std::filesystem::remove(Blocker);
	std::string RetryError;
	Durin::FFeedbackContext RetryFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!RetryError.empty()) RetryError += "\n"; RetryError += Message; }
	});
	const auto Retry = SavePackagesForTest({Paths[1]}, &RetryFeedback, {});
	EXPECT_TRUE(Retry) << RetryError;
	for (const auto& Path : Paths)
	{
		if (FindResidentPackage(Path)) EXPECT_TRUE(UnloadPackage(Path));
		EXPECT_EQ(FindResidentPackage(Path), nullptr);
	}
}

TEST(DFactoryTests, DuplicateDefaultsToDirtyMemoryAndResolvesItsOwnDestination)
{
	using namespace Durin;
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	FPackagePath SourcePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetToolsTests/LiveDuplicateSource", SourcePath));
	std::string CreatedError;
	Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
	});
	const auto Created = CreateAssetForTest(SourcePath, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), &CreatedFeedback);
	ASSERT_TRUE(Created);
	EXPECT_FALSE(FindPackageExact(SourcePath));
	std::string CopyError;
	Durin::FFeedbackContext CopyFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CopyError.empty()) CopyError += "\n"; CopyError += Message; }
	});
	const auto Copy = ([&] { Durin::FScopedFeedbackContext Scope(*(&CopyFeedback)); auto* Source = Durin::LoadObject(Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath), nullptr); std::vector<Durin::DObject*> Copies; Durin::ObjectTools::DuplicateObjects(std::span{&Source, 1}, {}, "/AssetToolsTests/LiveCopies", false, &Copies); return Copies.empty() ? nullptr : Copies.front(); }());
	ASSERT_TRUE(Copy) << CopyError;
	ASSERT_NE(Copy, nullptr);
	EXPECT_NE(Copy, Created);
	const auto Path = Copy->GetPackage()->GetPackagePathIdentity();
	EXPECT_EQ(FindResidentPackage(Path), Copy->GetPackage());
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	EXPECT_TRUE(Copy->GetPackage()->IsNewlyCreated());
	const auto Resolved = FMountPaths::ResolveAssetPath(Path.GetView());
	ASSERT_TRUE(Resolved);
	EXPECT_FALSE(std::filesystem::exists(Resolved->PhysicalPath.generic_string() + ".dasset"));
	std::string RepeatedError;
	Durin::FFeedbackContext RepeatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!RepeatedError.empty()) RepeatedError += "\n"; RepeatedError += Message; }
	});
	const auto Repeated = ([&] { Durin::FScopedFeedbackContext Scope(*(&RepeatedFeedback)); auto* Source = Durin::LoadObject(Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath), nullptr); std::vector<Durin::DObject*> Copies; Durin::ObjectTools::DuplicateObjects(std::span{&Source, 1}, {}, "/AssetToolsTests/LiveCopies", false, &Copies); return Copies.empty() ? nullptr : Copies.front(); }());
	ASSERT_TRUE(Repeated);
	EXPECT_EQ(Repeated->GetPackage()->GetPackagePathIdentity().GetPackageName(), "LiveDuplicateSource1");
	const auto RepeatedPath = Repeated->GetPackage()->GetPackagePathIdentity();
	EXPECT_TRUE(Durin::Testing::EnsurePackageNotResident(RepeatedPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Durin::Testing::EnsurePackageNotResident(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Durin::Testing::EnsurePackageNotResident(SourcePath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DFactoryTests, DuplicateSaveFailureRetainsLiveCopyAndAllowsOrdinarySaveRetry)
{
	using namespace Durin;
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	FPackagePath SourcePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetToolsTests/DuplicateSaveFailureSource", SourcePath));
	std::string CreatedError;
	Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
	});
	const auto Created = CreateAssetForTest(SourcePath, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), &CreatedFeedback);
	ASSERT_TRUE(Created);
	TStrongObjectPtr<DObject> SourceOwner(Created);
	ASSERT_TRUE(SavePackage(Created->GetPackage()));
	for (auto Phase : {Testing::EPackageSaveFaultPoint::PublishPackage, Testing::EPackageSaveFaultPoint::PublishRegistry})
	{
		SCOPED_TRACE(static_cast<int>(Phase));
		auto* Copy = ([&] { Durin::FScopedFeedbackContext Scope(Durin::FFeedbackContext::Get()); auto* Source = Durin::LoadObject(Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath), nullptr); std::vector<Durin::DObject*> Copies; Durin::ObjectTools::DuplicateObjects(std::span{&Source, 1}, {}, "/AssetToolsTests/FailedCopies", false, &Copies); return Copies.empty() ? nullptr : Copies.front(); }());
		ASSERT_NE(Copy, nullptr);
		FAssetWriteResult Saved;
		{
			Testing::FScopedPackageSaveFault Fault([Phase](auto Current, size_t) { return Current == Phase; });
			SavePackage(Copy->GetPackage(), {}, &Saved);
		}
		EXPECT_FALSE(Saved);
		EXPECT_EQ(Saved.Effect, Phase == Testing::EPackageSaveFaultPoint::PublishRegistry
			? EAssetWriteEffect::ContentCommittedProjectionPending : EAssetWriteEffect::None);
		const auto Path = Copy->GetPackage()->GetPackagePathIdentity();
		EXPECT_EQ(FindResidentPackage(Path), Copy->GetPackage());
		EXPECT_EQ(Copy->GetPackage()->IsDirty(), Phase != Testing::EPackageSaveFaultPoint::PublishRegistry);
		EXPECT_EQ(FindResidentPackage(SourcePath), Created->GetPackage());
		FAssetWriteResult Retry;
		SavePackage(Copy->GetPackage(), {}, &Retry);
		ASSERT_TRUE(Retry) << (Retry.Error ? Retry.Error->ToString() : std::string{});
		EXPECT_FALSE(Copy->GetPackage()->IsDirty());
		EXPECT_TRUE(UnloadPackage(Path));
	}
	SourceOwner.Reset();
	if (FindResidentPackage(SourcePath)) EXPECT_TRUE(UnloadPackage(SourcePath));
	EXPECT_EQ(FindResidentPackage(SourcePath), nullptr);
}

TEST(DFactoryTests, TypedDiagnosticsRetainContextAndMixedReportOrder)
{
	Durin::FFactoryDiagnostics Diagnostics;
	Diagnostics.Report("first");
	std::string Filename = "source.mesh";
	Diagnostics.ReportFailure({.Code = Durin::EFactoryError::ExactClass,
		.ExpectedClass = "StaticMesh", .RequestedClass = "Texture2D", .Filename = Filename});
	Filename.clear();
	Diagnostics.Report("last");
	ASSERT_EQ(Diagnostics.GetEntries().size(), 3u);
	ASSERT_TRUE(Diagnostics.GetEntries()[1].Failure.has_value());
	// Appending diagnostics can reallocate Entries; retain an owned error value.
	const auto Error = *Diagnostics.GetEntries()[1].Failure;
	EXPECT_EQ(Error.Code, Durin::EFactoryError::ExactClass);
	EXPECT_EQ(Error.RequestedClass, "Texture2D");
	EXPECT_EQ(Error.Filename, "source.mesh");
	EXPECT_EQ(Diagnostics.ToString(), "first\n" + Durin::FormatFactoryError(Error) + "\nlast");
	for (size_t Index = 0; Index < 20; ++Index) Diagnostics.ReportFailure(Error);
	EXPECT_EQ(Diagnostics.GetEntries().size(), Durin::FFactoryDiagnostics::MaximumMessageCount);
}

TEST(DFactoryTests, TypedFactoryFailureSurvivesPackageDiscard)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetToolsTests/TypedFactoryFailure", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::TypedFail);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), Factory, &ResultFeedback);
	EXPECT_FALSE(Result);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Path.GetView()), nullptr);
	EXPECT_EQ(ResultError, Durin::FormatFactoryError({
		.Code = Durin::EFactoryError::ObjectCreation,
		.ExpectedClass = DFactoryAssetForTest::StaticClass()->GetName(),
		.RequestedClass = DFactoryAssetForTest::StaticClass()->GetName()}));
}

TEST(DFactoryTests, ReimportSaveFailurePreservesImportResultAndCompletesOnce)
{
	InitializeFactoryTestGameThread();
	FStandaloneReimportHandlerForTest Handler(100);
	auto* Object = Durin::NewObject<DFactoryAssetForTest>(
		nullptr, Durin::FName("UnpackagedReimportObject"), Durin::EObjectFlags::Transient);
	Durin::FReimportResult Result;
	int CompletionCount = 0;
	Durin::FReimportManager::Reimport(*Object, {.bSave = true},
		[&](Durin::FReimportResult Completed) {
			++CompletionCount;
			Result = std::move(Completed);
		});
	EXPECT_EQ(CompletionCount, 1);
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Status, Durin::EReimportResult::Succeeded);
	EXPECT_EQ(Result.Message, "100");
	EXPECT_FALSE(Result.FactoryCause);
	ASSERT_TRUE(Result.SaveResult);
	EXPECT_FALSE(*Result.SaveResult);
	EXPECT_TRUE(Result.SaveResult->Error && Result.SaveResult->Error->Is(Durin::AssetWriteErrors::InvalidPath));
}

TEST(DFactoryTests, ReimportWithoutSaveHasNoPersistenceResult)
{
	InitializeFactoryTestGameThread();
	FStandaloneReimportHandlerForTest Handler(100);
	auto* Object = Durin::NewObject<DFactoryAssetForTest>(nullptr, "UnsavedReimportResult", Durin::EObjectFlags::Transient);
	Durin::FReimportResult Result;
	Durin::FReimportManager::Reimport(*Object, {.bSave = false},
		[&](Durin::FReimportResult Completed) { Result = std::move(Completed); });
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Message, "100");
	EXPECT_FALSE(Result.SaveResult);
}

TEST(DFactoryTests, FailedAndCanceledReimportsDoNotAttemptSaving)
{
	InitializeFactoryTestGameThread();
	FStandaloneReimportHandlerForTest Handler(100);
	auto* Object = Durin::NewObject<DFactoryAssetForTest>(nullptr, "RejectedReimportResult", Durin::EObjectFlags::Transient);
	for (const auto Outcome : {Durin::EReimportResult::Failed, Durin::EReimportResult::Canceled})
		for (const bool bFromFile : {false, true})
		{
			Handler.Outcome = Outcome;
			Durin::FReimportResult Result;
			int CompletionCount = 0;
			auto Completion = [&](Durin::FReimportResult Completed) { ++CompletionCount; Result = std::move(Completed); };
			if (bFromFile)
			{
				const std::array<std::string, 1> Files{"source.test"};
				Durin::FReimportManager::ReimportFromFiles(*Object, Files, {.bSave = true}, Completion);
			}
			else Durin::FReimportManager::Reimport(*Object, {.bSave = true}, Completion);
			EXPECT_EQ(CompletionCount, 1);
			EXPECT_EQ(Result.Status, Outcome);
			EXPECT_EQ(Result.Message, "100");
			EXPECT_FALSE(Result.SaveResult);
		}
}

TEST(DFactoryTests, SuccessfulReimportSaveReportsOwnedPersistenceResult)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	FStandaloneReimportHandlerForTest Handler(100);
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetToolsTests/ReimportSavedResult", Path));
	std::string CreatedError;
	Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
	});
	const auto Created = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), &CreatedFeedback);
	ASSERT_TRUE(Created) << CreatedError;
	Durin::FReimportResult Result;
	Durin::FReimportManager::Reimport(*Created, {.bSave = true},
		[&](Durin::FReimportResult Completed) { Result = std::move(Completed); });
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Message, "100");
	ASSERT_TRUE(Result.SaveResult);
	EXPECT_TRUE(*Result.SaveResult) << (Result.SaveResult->Error ? Result.SaveResult->Error->ToString() : std::string{});
	EXPECT_FALSE(Result.FactoryCause);
	EXPECT_FALSE(Created->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(Path));
}

TEST(DFactoryTests, ReimportProjectionFailureRetainsSaveEffectAndImportSuccess)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	FStandaloneReimportHandlerForTest Handler(100);
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetToolsTests/ReimportProjectionResult", Path));
	std::string CreatedError;
	Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
	});
	const auto Created = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), &CreatedFeedback);
	ASSERT_TRUE(Created) << CreatedError;
	Durin::FReimportResult Result;
	{
		Durin::Testing::FScopedPackageSaveFault Fault([](Durin::Testing::EPackageSaveFaultPoint Point, size_t) {
			return Point == Durin::Testing::EPackageSaveFaultPoint::PublishRegistry;
		});
		Durin::FReimportManager::Reimport(*Created, {.bSave = true},
			[&](Durin::FReimportResult Completed) { Result = std::move(Completed); });
	}
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Message, "100");
	ASSERT_TRUE(Result.SaveResult);
	EXPECT_FALSE(*Result.SaveResult);
	EXPECT_EQ(Result.SaveResult->Effect, Durin::EAssetWriteEffect::ContentCommittedProjectionPending);
	EXPECT_FALSE(Result.FactoryCause);
	EXPECT_FALSE(Created->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(Path));
}

TEST(DFactoryTests, FailedCandidateRequestsCollectionBeforeHostSafePoint)
{
	using namespace Durin;
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetToolsTests/ProtectedFailure", Path));
	{
		auto* Result = CreateAssetForTest(Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Fail), &(Durin::FFeedbackContext::Get()));
		EXPECT_EQ(Result, nullptr);
		EXPECT_EQ(FindResidentPackage(Path), nullptr);
		EXPECT_TRUE(IsGarbageCollectionRequested());
	}
	EXPECT_TRUE(TryCollectGarbage(Durin::EObjectFlags::Standalone));
	EXPECT_EQ(FindPackage(Path.GetView()), nullptr);
}

TEST(DFactoryTests, CreationInfersClassRoutesCallingContextAndDoesNotExecuteTheDefaultFactory)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	auto* Created = IAssetTools::Get().CreateAsset("CallingContext", "/AssetToolsTests", nullptr, Factory, FName("Creator"));
	ASSERT_NE(Created, nullptr);
	EXPECT_EQ(Factory->Calls, 1u);
	EXPECT_EQ(Factory->LastCallingContext, FName("Creator"));
	const auto CreationFlags = EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional;
	EXPECT_EQ(Created->GetObjectFlags() & CreationFlags, CreationFlags);
	const std::array Classes{DAssetToolsFactoryForTest::StaticClass()};
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	DFactory::InvalidateFactoryCache();
	auto* Default = Cast<DAssetToolsFactoryForTest>(Classes[0]->GetDefaultObject());
	ASSERT_NE(Default, nullptr);
	const auto Calls = Default->Calls;
	auto* Other = IAssetTools::Get().CreateAsset("DefaultFactory", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	ASSERT_NE(Other, nullptr);
	EXPECT_EQ(Default->Calls, Calls);
	const auto CreatedPath = Created->GetPackage()->GetPackagePathIdentity();
	const auto OtherPath = Other->GetPackage()->GetPackagePathIdentity();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(CreatedPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(OtherPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DFactoryTests, OverwritePreservesTheOriginalOnFactoryFailureAndRejectsUnclaimedOwners)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	auto* Original = IAssetTools::Get().CreateAsset("Overwrite", "/AssetToolsTests", nullptr, Factory);
	ASSERT_NE(Original, nullptr);
	const auto Path = Original->GetPackage()->GetPackagePathIdentity();
	Factory->Mode = DAssetToolsFactoryForTest::EMode::Fail;
	EXPECT_EQ(IAssetTools::Get().CreateAsset("Overwrite", "/AssetToolsTests", nullptr, Factory, {}, true), nullptr);
	EXPECT_EQ(FindResidentPackage(Path), Original->GetPackage());
	Factory->Mode = DAssetToolsFactoryForTest::EMode::Success;
	{
		TStrongObjectPtr<DObject> Protected(Original);
		EXPECT_EQ(IAssetTools::Get().CreateAsset("Overwrite", "/AssetToolsTests", nullptr, Factory, {}, true), nullptr);
		EXPECT_EQ(Protected.Get(), Original);
	}
	auto* Replacement = IAssetTools::Get().CreateAsset("Overwrite", "/AssetToolsTests", nullptr, Factory, {}, true);
	ASSERT_NE(Replacement, nullptr);
	EXPECT_NE(Replacement, Original);
	EXPECT_EQ(FindResidentPackage(Path), Replacement->GetPackage());
	EXPECT_TRUE(Replacement->GetPackage()->IsDirty());
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DFactoryTests, UniqueNamingUsesNumericSuffixesAndExplicitDuplicateRejectsCollision)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Source = IAssetTools::Get().CreateAsset("Numeric007", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	ASSERT_NE(Source, nullptr);
	std::string Package, Name;
	IAssetTools::Get().CreateUniqueAssetName("/AssetToolsTests/Numeric007", {}, Package, Name);
	EXPECT_EQ(Name, "Numeric008");
	auto* Copy = IAssetTools::Get().DuplicateAsset(Name, "/AssetToolsTests", Source);
	ASSERT_NE(Copy, nullptr);
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	EXPECT_EQ(IAssetTools::Get().DuplicateAsset(Name, "/AssetToolsTests", Source), nullptr);
	IAssetTools::Get().CreateUniqueAssetName("/AssetToolsTests/123", {}, Package, Name);
	EXPECT_EQ(Name, "123_2");
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Copy->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Source->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DFactoryTests, SaveFilteringAndPromptDecisionsDoNotConfuseDeclineWithCancellation)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	EXPECT_TRUE(DEditorLoadingAndSavingUtils::SavePackages({}, true));
	auto* Asset = IAssetTools::Get().CreateAsset("PromptSave", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	ASSERT_NE(Asset, nullptr);
	auto* Package = Asset->GetPackage();
	const auto Path = Package->GetPackagePathIdentity();
	FEditorFileUtils::FPromptForCheckoutAndSaveParams Params;
	Params.bPromptToSave = true;
	FEditorFileUtils::SetSavePrompt([](auto, const auto&) { return FEditorFileUtils::PR_Declined; });
	EXPECT_EQ(FEditorFileUtils::PromptForCheckoutAndSave(std::span{&Package, 1}, Params), FEditorFileUtils::PR_Declined);
	EXPECT_TRUE(Package->IsDirty());
	Params.bCanBeDeclined = false;
	EXPECT_EQ(FEditorFileUtils::PromptForCheckoutAndSave(std::span{&Package, 1}, Params), FEditorFileUtils::PR_Cancelled);
	FEditorFileUtils::SetSavePrompt({});
	EXPECT_EQ(FEditorFileUtils::PromptForCheckoutAndSave(std::span{&Package, 1}, Params), FEditorFileUtils::PR_Cancelled);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, true));
	uint32 Saves = 0;
	FScopedPackageSaveObserver Observer([&](const FPackagePath&) { ++Saves; });
	EXPECT_TRUE(DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, true));
	EXPECT_EQ(Saves, 0u);
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path));
}

TEST(DFactoryTests, SaveObserverExcludesNestedBatchesAndIncludesProjectionPendingPersistence)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* First = IAssetTools::Get().CreateAsset("ObservedFirst", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	auto* Second = IAssetTools::Get().CreateAsset("ObservedSecond", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	ASSERT_NE(First, nullptr); ASSERT_NE(Second, nullptr);
	auto* FirstPackage = First->GetPackage(); auto* SecondPackage = Second->GetPackage();
	const auto FirstPath = FirstPackage->GetPackagePathIdentity(), SecondPath = SecondPackage->GetPackagePathIdentity();
	std::vector<FPackagePath> Observed;
	{
		FScopedPackageSaveObserver Observer([&](const FPackagePath& Path) {
			Observed.push_back(Path);
			EXPECT_TRUE(DEditorLoadingAndSavingUtils::SavePackages(std::span{&SecondPackage, 1}, true));
		});
		EXPECT_TRUE(DEditorLoadingAndSavingUtils::SavePackages(std::span{&FirstPackage, 1}, true));
	}
	EXPECT_EQ(Observed, std::vector{FirstPath});
	FirstPackage->MarkDirty();
	{
		Testing::FScopedPackageSaveFault Fault([](auto Phase, size_t) { return Phase == Testing::EPackageSaveFaultPoint::PublishRegistry; });
		FScopedPackageSaveObserver Observer([&](const FPackagePath& Path) { Observed.push_back(Path); });
		EXPECT_FALSE(DEditorLoadingAndSavingUtils::SavePackages(std::span{&FirstPackage, 1}, true));
	}
	EXPECT_EQ(Observed, (std::vector{FirstPath, FirstPath}));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(FirstPath));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(SecondPath));
}

TEST(DFactoryTests, PromptSaveObserverExcludesNestedPromptWritesAndObservesEverySelectedPackage)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* First = IAssetTools::Get().CreateAsset("PromptObservedFirst", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	auto* Second = IAssetTools::Get().CreateAsset("PromptObservedSecond", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	auto* Nested = IAssetTools::Get().CreateAsset("PromptObservedNested", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	ASSERT_NE(First, nullptr); ASSERT_NE(Second, nullptr); ASSERT_NE(Nested, nullptr);
	std::array Packages{First->GetPackage(), Second->GetPackage()};
	auto* NestedPackage = Nested->GetPackage();
	const auto FirstPath = Packages[0]->GetPackagePathIdentity(), SecondPath = Packages[1]->GetPackagePathIdentity();
	const auto NestedPath = NestedPackage->GetPackagePathIdentity();
	std::vector<FPackagePath> Observed;
	FEditorFileUtils::SetSavePrompt([&](auto, const auto&) {
		EXPECT_TRUE(DEditorLoadingAndSavingUtils::SavePackages(std::span{&NestedPackage, 1}, true));
		return FEditorFileUtils::PR_Success;
	});
	{
		FScopedPackageSaveObserver Observer([&](const FPackagePath& Path) { Observed.push_back(Path); });
		FEditorFileUtils::FPromptForCheckoutAndSaveParams Params;
		Params.bPromptToSave = true;
		EXPECT_EQ(FEditorFileUtils::PromptForCheckoutAndSave(Packages, Params), FEditorFileUtils::PR_Success);
	}
	FEditorFileUtils::SetSavePrompt({});
	EXPECT_EQ(Observed, (std::vector{FirstPath, SecondPath}));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(FirstPath));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(SecondPath));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(NestedPath));
}

TEST(DFactoryTests, ReloadModesPreserveDirtyStateUnlessExplicitlyAccepted)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Asset = IAssetTools::Get().CreateAsset("ReloadMode", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Asset, nullptr);
	auto* Package = Asset->GetPackage();
	const auto Path = Package->GetPackagePathIdentity();
	ASSERT_TRUE(SavePackage(Package));
	Package->MarkDirty();
	std::string Error;
	EXPECT_FALSE(DPackageTools::ReloadPackages(std::span{&Package, 1}, Error, EReloadPackagesInteractionMode::AssumeNegative));
	EXPECT_EQ(FindResidentPackage(Path), Package);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(DPackageTools::ReloadPackages(std::span{&Package, 1}, Error));
	EXPECT_TRUE(DPackageTools::ReloadPackages(std::span{&Package, 1}, Error, EReloadPackagesInteractionMode::AssumePositive)) << Error;
	EXPECT_NE(FindResidentPackage(Path), Package);
	EXPECT_FALSE(FindResidentPackage(Path)->IsDirty());
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path));
}

TEST(DFactoryTests, SavedOverwritePublishesMemoryBeforeExplicitPersistenceAndPreservesSiblings)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Original = IAssetTools::Get().CreateAsset("SavedOverwrite", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr);
	ASSERT_NE(Original, nullptr);
	auto* Package = Original->GetPackage();
	ASSERT_NE(NewObject<DObject>(Package, FName("Sibling"), EObjectFlags::Public), nullptr);
	const auto Path = Package->GetPackagePathIdentity();
	ASSERT_TRUE(SavePackage(Package));
	const auto Physical = Testing::GetTestWorkDirectory() / "AssetTools/SavedOverwrite.dasset";
	const auto Before = FFileHelper::LoadFileToArray(Physical);
	ASSERT_TRUE(Before);
	auto* Replacement = IAssetTools::Get().CreateAsset("SavedOverwrite", "/AssetToolsTests", DFactoryAssetForTest::StaticClass(), nullptr, {}, true);
	ASSERT_NE(Replacement, nullptr);
	EXPECT_NE(Replacement, Original);
	EXPECT_FALSE(Replacement->GetPackage()->IsNewlyCreated());
	EXPECT_TRUE(Replacement->GetPackage()->IsDirty());
	ASSERT_NE(Replacement->GetPackage()->FindTopLevelAsset(FName("Sibling")), nullptr);
	const auto Unchanged = FFileHelper::LoadFileToArray(Physical);
	ASSERT_TRUE(Unchanged);
	EXPECT_EQ(*Unchanged, *Before);
	ASSERT_TRUE(SavePackage(Replacement->GetPackage()));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path));
	auto* Reloaded = LoadPackage(Path);
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_NE(Reloaded->FindTopLevelAsset(FName("Sibling")), nullptr);
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path));
}

TEST(DAssetRenameTests, DirtyUnsavedRenamePreservesObjectAndDoesNotLeaveAnUnneededAlias)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameNewSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	TStrongObjectPtr<DObject> Owner(Object);
	const auto OldPackage = Object->GetPackage()->GetPackagePathIdentity();
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameNewTarget"};
	int Events = 0;
	const auto Handle = OnAssetRenamed().AddLambda([&](const FAssetData& Data, const FSoftObjectPath& Old) {
		++Events; EXPECT_EQ(Data.AssetPath.ToString(), "/AssetToolsTests/RenameNewTarget.RenameNewTarget"); EXPECT_EQ(Old.GetPackagePath(), OldPackage);
	});
	EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	OnAssetRenamed().Remove(Handle);
	EXPECT_EQ(Events, 1);
	EXPECT_EQ(Object, Owner.Get());
	EXPECT_EQ(Object->GetObjectPath(), "/AssetToolsTests/RenameNewTarget.RenameNewTarget");
	EXPECT_EQ(FindResidentPackage(OldPackage), nullptr);
	const auto Target = Object->GetPackage()->GetPackagePathIdentity();
	Owner.Reset();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Target, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, SelectedExportMovesWithoutItsSiblingAndSavedAliasesResolveAfterUnload)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameSelected", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	auto* SourcePackage = Object->GetPackage();
	auto* Sibling = NewObject<DMaterial>(SourcePackage, FName("UntouchedSibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Sibling, nullptr);
	ASSERT_TRUE(SavePackage(SourcePackage));
	FSoftObjectPath Old; ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Old));
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameExtracted"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	EXPECT_EQ(Sibling->GetPackage(), SourcePackage);
	EXPECT_EQ(Sibling->GetName(), "UntouchedSibling");
	EXPECT_NE(Object->GetPackage(), SourcePackage);
	const auto Target = Object->GetPackage()->GetPackagePathIdentity();
	EXPECT_TRUE(Cast<DAssetRedirector>(SourcePackage->FindTopLevelAsset(FName("RenameSelected"))));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Old.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Target, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_NE(LoadObject(Old, nullptr), nullptr);
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Old.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Target, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, ExplicitObjectIdentityAndCaseOnlyChangesPreserveLiveOwners)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameExplicit", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	FSoftObjectPath Old, New;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Old));
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/RenameExplicitNonLeaf.IndependentName", New));
	const FAssetRenameData Rename{Old, New};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	EXPECT_EQ(Object->GetObjectPath(), New.ToString());
	FSoftObjectPath Case; ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/renameexplicitnonleaf.independentname", Case));
	const FAssetRenameData CaseRename{New, Case};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&CaseRename, 1}));
	EXPECT_EQ(Object->GetObjectPath(), "/AssetToolsTests/renameexplicitnonleaf.independentname");
	EXPECT_EQ(Object->GetPackage()->GetName(), "renameexplicitnonleaf");
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, DestinationFailureDoesNotPreventLaterItemsAndRetryDoesNotRepeatIdentityEvents)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* First = IAssetTools::Get().CreateAsset("RenameBlockedSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	auto* Blocker = IAssetTools::Get().CreateAsset("RenameOccupied", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	auto* Second = IAssetTools::Get().CreateAsset("RenamePeerSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_TRUE(First && Blocker && Second);
	const std::array Renames{
		FAssetRenameData{TWeakObjectPtr<DObject>(First), "/AssetToolsTests", "RenameOccupied"},
		FAssetRenameData{TWeakObjectPtr<DObject>(Second), "/AssetToolsTests", "RenamePeerTarget"}};
	EXPECT_FALSE(IAssetTools::Get().RenameAssets(Renames));
	EXPECT_EQ(First->GetName(), "RenameBlockedSource");
	EXPECT_EQ(Second->GetName(), "RenamePeerTarget");
	int Events = 0, PostEvents = 0;
	const auto PostHandle = IAssetTools::Get().OnAssetPostRename().AddLambda([&](std::span<const FAssetRenameData>) { ++PostEvents; });
	const auto Handle = OnAssetRenamed().AddLambda([&](const FAssetData&, const FSoftObjectPath&) { ++Events; });
	EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Renames[1], 1}));
	OnAssetRenamed().Remove(Handle);
	IAssetTools::Get().OnAssetPostRename().Remove(PostHandle);
	EXPECT_EQ(Events, 0); EXPECT_EQ(PostEvents, 0);
	for (auto* Object : {First, Blocker, Second}) EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, FailedPersistenceRetainsUnsavedValuesAndIdentityForOrdinaryRetry)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameFaultSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	ASSERT_TRUE(SavePackage(Object->GetPackage()));
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameFaultTarget"};
	int IdentityEvents = 0, PostEvents = 0, Errors = 0;
	const auto IdentityHandle = OnAssetRenamed().AddLambda([&](const auto&, const auto&) { ++IdentityEvents; });
	const auto PostHandle = IAssetTools::Get().OnAssetPostRename().AddLambda([&](auto) { ++PostEvents; });
	FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view) { Errors += Level >= ELogLevel::Error; });
	{
		FScopedFeedbackContext Scope(Feedback);
		Testing::FScopedPackageSaveFault Fault([](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishPackage; });
		EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
		EXPECT_EQ(IdentityEvents, 1); EXPECT_EQ(PostEvents, 1);
		EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	}
	EXPECT_GE(Errors, 2);
	EXPECT_EQ(Object->GetName(), "RenameFaultTarget");
	EXPECT_TRUE(Object->GetPackage()->IsDirty());
	EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	EXPECT_EQ(IdentityEvents, 1); EXPECT_EQ(PostEvents, 1);
	OnAssetRenamed().Remove(IdentityHandle);
	IAssetTools::Get().OnAssetPostRename().Remove(PostHandle);
	FPackagePath Alias; ASSERT_TRUE(FPackagePath::TryCreate("/AssetToolsTests/RenameFaultSource", Alias));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Alias, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, PayloadCleanupFailureDoesNotFailTheCommittedRename)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	const auto Root = Testing::GetTestWorkDirectory() / "RenamePayloadCleanup";
	std::filesystem::create_directories(Root);
	const auto Source = Root / "source.bin", Target = Root / "target.bin";
	{ std::ofstream Stream(Source); Stream << "payload"; }
	auto* Object = IAssetTools::Get().CreateAsset("RenameCleanupSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	const auto Handle = RegisterAssetOwnedPayloadRelocator(DMaterial::StaticClass(),
		[&](DObject*, const FPackagePath&, const FPackagePath&, FAssetOwnedPayloadRelocation& Payload) -> std::expected<void, UnifiedError::FError> {
			Payload.Files.emplace_back(Source, Target);
			Payload.Apply = [&] {
				std::filesystem::remove(Source);
				std::filesystem::create_directory(Source);
				std::ofstream Stream(Source / "blocker"); Stream << "prevent removal";
			};
			return {};
		});
	ASSERT_NE(Handle, 0u);
	int Warnings = 0;
	FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view) { Warnings += Level == ELogLevel::Warn; });
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameCleanupTarget"};
	{
		FScopedFeedbackContext Scope(Feedback);
		EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	}
	UnregisterAssetOwnedPayloadRelocator(Handle);
	EXPECT_GT(Warnings, 0);
	EXPECT_EQ(Object->GetName(), "RenameCleanupTarget");
	EXPECT_FALSE(Object->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(Target));
	EXPECT_TRUE(std::filesystem::is_directory(Source));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	std::filesystem::remove(Source / "blocker"); std::filesystem::remove(Source); std::filesystem::remove(Target);
}

TEST(DAssetRenameTests, DirtyLoadedHardAndSoftReferencersRetainValuesAndSubobjectSuffixes)
{
	using namespace Durin;

	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameReferenced", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	auto* Child = NewObject<DObject>(Object, FName("Child"));
	ASSERT_NE(Child, nullptr);
	TStrongObjectPtr<DObject> ChildOwner(Child);
	auto* Ref = Cast<DAssetRenameReferencesForTest>(IAssetTools::Get().CreateAsset("RenameReferencer", "/AssetToolsTests", DAssetRenameReferencesForTest::StaticClass(), nullptr));
	ASSERT_NE(Ref, nullptr);
	FSoftObjectPath OldChild; ASSERT_TRUE(FSoftObjectPath::TryCreate(Child->GetObjectPath(), OldChild));
	Ref->Hard = Child; Ref->Soft = OldChild; Ref->Value = 123;
	ASSERT_TRUE(SavePackage(Object->GetPackage())); ASSERT_TRUE(SavePackage(Ref->GetPackage()));
	Ref->Value = 456; Ref->MarkPackageDirty();
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenamedReferenced"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	EXPECT_EQ(Ref->Hard.Get(), Child);
	EXPECT_EQ(Ref->Soft.ToSoftObjectPath().ToString(), Child->GetObjectPath());
	EXPECT_EQ(Ref->Value, 456);
	FSoftObjectPath RefPath; ASSERT_TRUE(FSoftObjectPath::TryCreate(Ref->GetObjectPath(), RefPath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(RefPath.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	auto* Reopened = Cast<DAssetRenameReferencesForTest>(LoadObject(RefPath, nullptr));
	ASSERT_NE(Reopened, nullptr); EXPECT_EQ(Reopened->Value, 456); EXPECT_EQ(Reopened->Hard.Get(), Child);
	FPackagePath Alias; ASSERT_TRUE(FPackagePath::TryCreate("/AssetToolsTests/RenameReferenced", Alias));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(RefPath.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Alias, EAssetPackageUnloadPolicy::DiscardUnsaved));
	ChildOwner.Reset();
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, SoftOnlySubobjectRepairDoesNotMoveTheAsset)
{
	using namespace Durin;

	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = Cast<DAssetRenameReferencesForTest>(IAssetTools::Get().CreateAsset("RenameSoftOnly", "/AssetToolsTests", DAssetRenameReferencesForTest::StaticClass(), nullptr));
	ASSERT_NE(Object, nullptr);
	FSoftObjectPath Old, New;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/RenameSoftOnly.RenameSoftOnly:Before", Old));
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/RenameSoftOnly.RenameSoftOnly:After", New));
	Object->Soft = Old;
	const FAssetRenameData Repair{Old, New, true};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Repair, 1}));
	EXPECT_EQ(Object->Soft.ToSoftObjectPath(), New);
	EXPECT_EQ(Object->GetName(), "RenameSoftOnly");
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, SoftOnlyRepairSucceedsWhenItsFollowUpSaveFails)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = Cast<DAssetRenameReferencesForTest>(IAssetTools::Get().CreateAsset("RenameSoftSaveFault", "/AssetToolsTests", DAssetRenameReferencesForTest::StaticClass(), nullptr));
	ASSERT_NE(Object, nullptr);
	FSoftObjectPath Old, New;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/RenameSoftSaveFault.RenameSoftSaveFault:Before", Old));
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/RenameSoftSaveFault.RenameSoftSaveFault:After", New));
	Object->Soft = Old;
	ASSERT_TRUE(SavePackage(Object->GetPackage()));
	int Errors = 0;
	FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view) { Errors += Level >= ELogLevel::Error; });
	const FAssetRenameData Repair{Old, New, true};
	{
		FScopedFeedbackContext Scope(Feedback);
		Testing::FScopedPackageSaveFault Fault([](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishPackage; });
		EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Repair, 1}));
	}
	EXPECT_GT(Errors, 0);
	EXPECT_EQ(Object->Soft.ToSoftObjectPath(), New);
	EXPECT_EQ(Object->GetName(), "RenameSoftSaveFault");
	EXPECT_TRUE(Object->GetPackage()->IsDirty());
	ASSERT_TRUE(SavePackage(Object->GetPackage()));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, MixedPackageFixupKeepsThenDeletesOnlyTheSelectedRedirector)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameFixup", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	auto* OldPackage = Object->GetPackage();
	auto* Sibling = NewObject<DMaterial>(OldPackage, FName("FixupSibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_TRUE(SavePackage(OldPackage));
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameFixupTarget"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	auto* Alias = Cast<DAssetRedirector>(OldPackage->FindTopLevelAsset(FName("RenameFixup")));
	ASSERT_NE(Alias, nullptr);
	const std::array Aliases{Alias, Alias};
	IAssetTools::Get().FixupReferencers(Aliases, false, ERedirectFixupMode::LeaveFixedUpRedirectors);
	EXPECT_EQ(OldPackage->FindTopLevelAsset(FName("RenameFixup")), Alias);
	IAssetTools::Get().FixupReferencers(Aliases);
	EXPECT_EQ(OldPackage->FindTopLevelAsset(FName("RenameFixup")), nullptr);
	EXPECT_EQ(OldPackage->FindTopLevelAsset(FName("FixupSibling")), Sibling);
	const auto OldPath = OldPackage->GetPackagePathIdentity(), NewPath = Object->GetPackage()->GetPackagePathIdentity();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(OldPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(NewPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	auto* Reopened = LoadPackage(OldPath);
	ASSERT_NE(Reopened, nullptr);
	EXPECT_EQ(Reopened->FindTopLevelAsset(FName("RenameFixup")), nullptr);
	EXPECT_NE(Reopened->FindTopLevelAsset(FName("FixupSibling")), nullptr);
	EXPECT_TRUE(Testing::EnsurePackageNotResident(OldPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, WritableMountMoveAndReadOnlyRejectionUseTheSameLiveObject)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	const auto Root = Testing::GetTestWorkDirectory() / "RenameMounts";
	std::filesystem::create_directories(Root / "Writable"); std::filesystem::create_directories(Root / "ReadOnly");
	Testing::RegisterMountPointForTests("/RenameWritable/", (Root / "Writable").generic_string() + "/");
	Testing::RegisterMountPointForTests("/RenameReadOnly/", (Root / "ReadOnly").generic_string() + "/", true, false);
	auto* Object = IAssetTools::Get().CreateAsset("RenameMountSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	const FAssetRenameData ReadOnly{TWeakObjectPtr<DObject>(Object), "/RenameReadOnly", "Blocked"};
	EXPECT_FALSE(IAssetTools::Get().RenameAssets(std::span{&ReadOnly, 1}));
	EXPECT_EQ(Object->GetObjectPath(), "/AssetToolsTests/RenameMountSource.RenameMountSource");
	const FAssetRenameData Writable{TWeakObjectPtr<DObject>(Object), "/RenameWritable", "Moved"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Writable, 1}));
	EXPECT_EQ(Object->GetObjectPath(), "/RenameWritable/Moved.Moved");
	EXPECT_EQ(Object->GetPackage()->GetName(), "Moved");
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, RegistryLagRetryPreservesIdentityAndPublishesItsAlias)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameLagSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	const auto Old = Object->GetPackage()->GetPackagePathIdentity();
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameLagTarget"};
	{
		Testing::FScopedPackageSaveFault Fault([](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishRegistry; });
		EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	}
	const auto New = Object->GetPackage()->GetPackagePathIdentity();
	EXPECT_TRUE(IsAssetRegistryProjectionFenced(New));
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	EXPECT_FALSE(IsAssetRegistryProjectionFenced(New));
	EXPECT_EQ(LoadObject(Testing::MakePackageLeafTopLevelAssetPathForTests(Old), nullptr), Object);
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Old, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(New, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, FixupSavesDirtyHardReferencersBeforeDeletingPureAliases)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameHardSource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	auto* Ref = Cast<DAssetRenameReferencesForTest>(IAssetTools::Get().CreateAsset("RenameHardRef", "/AssetToolsTests", DAssetRenameReferencesForTest::StaticClass(), nullptr));
	ASSERT_TRUE(Object && Ref); Ref->Hard = Object;
	ASSERT_TRUE(SavePackage(Object->GetPackage())); ASSERT_TRUE(SavePackage(Ref->GetPackage()));
	const auto Old = Object->GetPackage()->GetPackagePathIdentity();
	Ref->Value = 789; Ref->MarkPackageDirty();
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameHardTarget"};
	{
		int Publications = 0;
		Testing::FScopedPackageSaveFault Fault([&](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishPackage && ++Publications == 3; });
		EXPECT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	}
	ASSERT_TRUE(Ref->GetPackage()->IsDirty());
	auto* Alias = Cast<DAssetRedirector>(FindResidentPackage(Old)->FindTopLevelAsset(FName("RenameHardSource")));
	ASSERT_NE(Alias, nullptr);
	{
		Testing::FScopedPackageSaveFault Fault([](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishPackage; });
		IAssetTools::Get().FixupReferencers(std::span{&Alias, 1});
	}
	EXPECT_EQ(FindResidentPackage(Old)->FindTopLevelAsset(FName("RenameHardSource")), Alias);
	EXPECT_TRUE(Ref->GetPackage()->IsDirty());
	IAssetTools::Get().FixupReferencers(std::span{&Alias, 1});
	EXPECT_EQ(Ref->Value, 789); EXPECT_EQ(Ref->Hard.Get(), Object);
	EXPECT_FALSE(FindPackageExact(Old));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Ref->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, SavedUnloadedExplicitRenameAndPreparationFailureKeepValidSources)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameUnloaded", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	FSoftObjectPath Old, New; ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Old));
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/AssetToolsTests/RenameUnloadedTarget.OtherName", New));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Old.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	const FAssetRenameData Rename{Old, New};
	const auto Handle = RegisterAssetOwnedPayloadRelocator(DMaterial::StaticClass(), [](DObject*, const FPackagePath&, const FPackagePath&, FAssetOwnedPayloadRelocation&) -> std::expected<void, UnifiedError::FError> {
		return std::unexpected(UnifiedError::FError{AssetWriteErrors::IoError, "Injected owned payload preparation failure."});
	});
	ASSERT_NE(Handle, 0u);
	EXPECT_FALSE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	UnregisterAssetOwnedPayloadRelocator(Handle);
	EXPECT_NE(LoadObject(Old, nullptr), nullptr);
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	EXPECT_EQ(LoadObject(Old, nullptr), LoadObject(New, nullptr));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Old.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(New.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, PreparationRevalidatesSavedBytesWithoutReplacingExternalEdits)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Object = IAssetTools::Get().CreateAsset("RenameStaleFile", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	const auto File = FMountPaths::ResolveAssetPath(Object->GetPackage()->GetPackagePath())->PhysicalPath.generic_string() + ".dasset";
	const auto Before = FFileHelper::LoadFileToArray(File); ASSERT_TRUE(Before);
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameStaleTarget"};
	const auto Handle = RegisterAssetOwnedPayloadRelocator(DMaterial::StaticClass(), [&](DObject*, const FPackagePath&, const FPackagePath&, FAssetOwnedPayloadRelocation&) -> std::expected<void, UnifiedError::FError> {
		std::ofstream Stream(File, std::ios::binary | std::ios::app); Stream.put('x'); return {};
	});
	ASSERT_NE(Handle, 0u);
	EXPECT_FALSE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	UnregisterAssetOwnedPayloadRelocator(Handle);
	EXPECT_EQ(Object->GetName(), "RenameStaleFile");
	const auto After = FFileHelper::LoadFileToArray(File); ASSERT_TRUE(After); EXPECT_EQ(After->size(), Before->size() + 1);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Before, File));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, DiagnosticReentryRejectsWithoutRecursiveFeedbackOrMutation)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Source = IAssetTools::Get().CreateAsset("RenameReentrySource", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	auto* Target = IAssetTools::Get().CreateAsset("RenameReentryTarget", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_TRUE(Source && Target);
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Source), "/AssetToolsTests", "RenameReentryTarget"};
	int Reports = 0;
	FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view) {
		if (Level < ELogLevel::Error) return;
		++Reports; EXPECT_FALSE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	});
	{ FScopedFeedbackContext Scope(Feedback); EXPECT_FALSE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1})); }
	EXPECT_EQ(Reports, 1); EXPECT_EQ(Source->GetName(), "RenameReentrySource");
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Source->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Target->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, AliasOnlyPackagesWithDifferentTargetsFixSelectedExportsIndependently)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* First = IAssetTools::Get().CreateAsset("RenameAliasPair", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(First, nullptr);
	auto* Package = First->GetPackage(); const auto Old = Package->GetPackagePathIdentity();
	auto* Second = NewObject<DMaterial>(Package, FName("Second"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Second, nullptr); ASSERT_TRUE(SavePackage(Package));
	const std::array Renames{FAssetRenameData{TWeakObjectPtr<DObject>(First), "/AssetToolsTests", "AliasPairFirst"},
		FAssetRenameData{TWeakObjectPtr<DObject>(Second), "/AssetToolsTests", "AliasPairSecond"}};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(Renames));
	auto* FirstAlias = Cast<DAssetRedirector>(Package->FindTopLevelAsset(FName("RenameAliasPair")));
	auto* SecondAlias = Cast<DAssetRedirector>(Package->FindTopLevelAsset(FName("Second")));
	ASSERT_TRUE(FirstAlias && SecondAlias);
	IAssetTools::Get().FixupReferencers(std::span{&FirstAlias, 1});
	EXPECT_EQ(Package->FindTopLevelAsset(FName("RenameAliasPair")), nullptr);
	EXPECT_EQ(Package->FindTopLevelAsset(FName("Second")), SecondAlias);
	IAssetTools::Get().FixupReferencers(std::span{&SecondAlias, 1});
	EXPECT_FALSE(FindPackageExact(Old));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(First->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Second->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetRenameTests, MixedPackageAliasSurvivesReadOnlyPersistentSoftReferencers)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	const auto Root = Testing::GetTestWorkDirectory() / "ReadOnlyRenameRef"; std::filesystem::create_directories(Root);
	Testing::RegisterMountPointForTests("/RenameRORef/", Root.generic_string() + "/");
	auto* Object = IAssetTools::Get().CreateAsset("RenameKeptAlias", "/AssetToolsTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr); auto* Package = Object->GetPackage();
	auto* Sibling = NewObject<DMaterial>(Package, FName("StableSibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Sibling, nullptr); ASSERT_TRUE(SavePackage(Package));
	FSoftObjectPath Old; ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Old));
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), "/AssetToolsTests", "RenameKeptTarget"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	auto* Ref = Cast<DAssetRenameReferencesForTest>(IAssetTools::Get().CreateAsset("ReadOnlyRef", "/RenameRORef", DAssetRenameReferencesForTest::StaticClass(), nullptr));
	ASSERT_NE(Ref, nullptr); Ref->Soft = Old; ASSERT_TRUE(SavePackage(Ref->GetPackage()));
	Testing::RegisterMountPointForTests("/RenameRORef/", Root.generic_string() + "/", true, false);
	auto* Alias = Cast<DAssetRedirector>(Package->FindTopLevelAsset(FName("RenameKeptAlias"))); ASSERT_NE(Alias, nullptr);
	IAssetTools::Get().FixupReferencers(std::span{&Alias, 1});
	EXPECT_EQ(Package->FindTopLevelAsset(FName("RenameKeptAlias")), Alias);
	EXPECT_EQ(Package->FindTopLevelAsset(FName("StableSibling")), Sibling);
	Testing::RegisterMountPointForTests("/RenameRORef/", Root.generic_string() + "/");
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Ref->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Old.GetPackagePath(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Object->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetImportTaskTests, DefaultsAndManagedInputsAndOutputsSurviveCollection)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "ManagedImportTask", EObjectFlags::Transient));
	EXPECT_FALSE(Task->bReplaceExisting); EXPECT_FALSE(Task->bReplaceExistingSettings);
	EXPECT_FALSE(Task->bAutomated); EXPECT_FALSE(Task->bSave); EXPECT_FALSE(Task->bAsync);
	EXPECT_TRUE(Task->Filename.empty()); EXPECT_TRUE(Task->DestinationPath.empty()); EXPECT_TRUE(Task->DestinationName.empty());
	EXPECT_FALSE(Task->Factory); EXPECT_FALSE(Task->Options);
	EXPECT_TRUE(Task->GetObjects().empty()); EXPECT_TRUE(Task->ImportedObjectPaths.empty());
	EXPECT_TRUE(Task->IsAsyncImportComplete());
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	Task->Factory = Factory;
	Task->Options = NewObject<DFactoryAssetForTest>(nullptr, "ManagedImportOptions", EObjectFlags::Transient);
	const FObjectKey FactoryKey(Factory), OptionsKey(Task->Options.Get());
	CollectGarbage(EObjectFlags::Standalone);
	ASSERT_NE(ResolveObjectKey(FactoryKey), nullptr); ASSERT_NE(ResolveObjectKey(OptionsKey), nullptr);
	Task->Filename = "source.factorytest"; Task->DestinationPath = "/AssetToolsTests"; Task->DestinationName = "ManagedTask";
	Task->bAsync = true; // Ordinary factories use the synchronous fallback.
	auto* RawTask = Task.Get();
	IAssetTools::Get().ImportAssetTasks(std::array{RawTask});
	ASSERT_TRUE(Task->IsAsyncImportComplete()); ASSERT_EQ(Task->GetObjects().size(), 1u);
	EXPECT_EQ(Factory->LastOptions, Task->Options.Get());
	EXPECT_EQ(Task->ImportedObjectPaths, std::vector<std::string>{"/AssetToolsTests/ManagedTask.ManagedTask"});
	const FObjectKey Output(Task->GetObjects().front());
	CollectGarbage(EObjectFlags::NoFlags);
	ASSERT_NE(ResolveObjectKey(Output), nullptr);
	EXPECT_TRUE(Task->GetObjects().front()->GetPackage()->IsDirty());
	const auto Path = Task->GetObjects().front()->GetPackage()->GetPackagePathIdentity();
	Task.Reset();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetImportTaskTests, SkipsNullContinuesFailuresAndClearsCompletedResultsOnResubmission)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "AssetImportTask", EObjectFlags::Transient));
	TStrongObjectPtr<DAssetImportTask> Failed(NewObject<DAssetImportTask>(nullptr, "AssetImportTask", EObjectFlags::Transient));
	Task->Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	Task->Filename = "source.factorytest"; Task->DestinationPath = "/AssetToolsTests"; Task->DestinationName = "TaskPeer";
	Failed->Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Fail);
	Failed->Filename = "source.factorytest"; Failed->DestinationPath = "/AssetToolsTests"; Failed->DestinationName = "FailedTaskPeer";
	IAssetTools::Get().ImportAssetTasks(std::array<DAssetImportTask*, 3>{nullptr, Failed.Get(), Task.Get()});
	EXPECT_TRUE(Failed->GetObjects().empty()); EXPECT_TRUE(Failed->ImportedObjectPaths.empty());
	ASSERT_EQ(Task->GetObjects().size(), 1u);
	const auto Path = Task->GetObjects().front()->GetPackage()->GetPackagePathIdentity();
	Task->Filename.clear();
	IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
	EXPECT_TRUE(Task->GetObjects().empty()); EXPECT_TRUE(Task->ImportedObjectPaths.empty());
	EXPECT_TRUE(Task->IsAsyncImportComplete());
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetImportTaskTests, RecursiveSubmissionAndSharedFactoryReentryDoNotRepeatPublication)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "AssetImportTask", EObjectFlags::Transient)), Other(NewObject<DAssetImportTask>(nullptr, "AssetImportTask", EObjectFlags::Transient));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	Task->Factory = Factory; Other->Factory = Factory;
	Task->Filename = Other->Filename = "source.factorytest";
	Task->DestinationPath = Other->DestinationPath = "/AssetToolsTests";
	Task->DestinationName = "RecursiveTask"; Other->DestinationName = "SharedFactoryTask";
	Factory->OnCreate = [&] {
		EXPECT_FALSE(Task->IsAsyncImportComplete());
		IAssetTools::Get().ImportAssetTasks(std::array{Task.Get(), Other.Get()});
	};
	IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
	Factory->OnCreate = {};
	EXPECT_EQ(Factory->Calls, 1u); EXPECT_TRUE(Other->GetObjects().empty());
	ASSERT_EQ(Task->GetObjects().size(), 1u);
	const auto Path = Task->GetObjects().front()->GetPackage()->GetPackagePathIdentity();
	Task.Reset(); Other.Reset();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(DAssetImportTaskTests, SaveFailureKeepsPublishedOutputAndOrdinarySaveRetriesIt)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "AssetImportTask", EObjectFlags::Transient));
	Task->Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	Task->Filename = "source.factorytest"; Task->DestinationPath = "/AssetToolsTests"; Task->DestinationName = "TaskSaveRetry";
	Task->bSave = true;
	{
		Testing::FScopedPackageSaveFault Fault([](Testing::EPackageSaveFaultPoint Point, size_t) {
			return Point == Testing::EPackageSaveFaultPoint::StagePackage;
		});
		IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
	}
	ASSERT_EQ(Task->GetObjects().size(), 1u); ASSERT_EQ(Task->ImportedObjectPaths.size(), 1u);
	auto* Package = Task->GetObjects().front()->GetPackage();
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(DEditorLoadingAndSavingUtils::SavePackages(std::array{Package}, true));
	EXPECT_FALSE(Package->IsDirty());
	const auto Path = Package->GetPackagePathIdentity(); Task.Reset();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path));
}

TEST(DAssetImportTaskTests, AutomaticFactoryUsesTransientInvocationAndRejectsUnsupportedOptions)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	const std::array Classes{DAssetToolsFactoryForTest::StaticClass()};
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	DFactory::InvalidateFactoryCache();
	auto* Default = Cast<DAssetToolsFactoryForTest>(Classes.front()->GetDefaultObject());
	ASSERT_NE(Default, nullptr);
	const auto DefaultCalls = Default->Calls;
	TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "AutoImportTask", EObjectFlags::Transient));
	Task->Filename = "AutoTask.factorytest"; Task->DestinationPath = "/AssetToolsTests";
	Task->bSave = true;
	IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
	ASSERT_EQ(Task->GetObjects().size(), 1u);
	EXPECT_EQ(Task->GetObjects().front()->GetClass(), DFactoryAssetForTest::StaticClass());
	EXPECT_EQ(Default->Calls, DefaultCalls);
	EXPECT_EQ(Task->ImportedObjectPaths, std::vector<std::string>{"/AssetToolsTests/AutoTask.AutoTask"});
	EXPECT_FALSE(Task->GetObjects().front()->GetPackage()->IsDirty());
	const auto Path = Task->GetObjects().front()->GetPackage()->GetPackagePathIdentity();
	Task->Options = NewObject<DObject>(nullptr, "UnsupportedTaskOptions", EObjectFlags::Transient);
	Task->DestinationName = "UnsupportedTaskOptions";
	IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
	EXPECT_TRUE(Task->GetObjects().empty()); EXPECT_TRUE(Task->ImportedObjectPaths.empty());
	Task.Reset();
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Path));
}

TEST(DAssetImportTaskTests, FileListHonorsChosenFactoryForSceneFlagAndSynchronizesOnlyRealOutputs)
{
	using namespace Durin;
	InitializeFactoryTestGameThread(); EnsureAssetToolsTestMount();
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	size_t Synced = 0;
	SetAssetImportBrowserSync([&](std::span<DObject* const> Objects) { Synced += Objects.size(); });
	struct FSyncScope { ~FSyncScope() { SetAssetImportBrowserSync({}); } } Scope;
	const std::array<std::string, 2> Files{"ChosenScene.gltf", ""};
	const auto Objects = IAssetTools::Get().ImportAssets(Files, "/AssetToolsTests", Factory, true, nullptr, true, true);
	ASSERT_EQ(Objects.size(), 1u); EXPECT_EQ(Synced, 1u); EXPECT_EQ(Factory->Calls, 1u);
	EXPECT_TRUE(Testing::EnsurePackageNotResident(Objects.front()->GetPackage()->GetPackagePathIdentity(), EAssetPackageUnloadPolicy::DiscardUnsaved));
}

#include "DObject/StrongObjectPtr.h"
#include "Factories/Factory.h"
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
#include "AssetRegistry/Scan.h"

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
			Durin::FFactoryDiagnostics* Diagnostics) const -> Durin::DObject* override
		{
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

	auto MakeImportRequest(std::string_view Path, const Durin::DFactory* Factory)
		-> Durin::FAssetImportRequest
	{
		Durin::FPackagePath PackagePath;
		Durin::FPackagePath::TryCreate(Path, PackagePath);
		return {
			Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(PackagePath),
			DFactoryAssetForTest::StaticClass(), "source.factorytest", Factory};
	}
}

TEST(DFactoryTests, BatchPreflightRejectsEveryDuplicateWithoutCreatingPackages)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	auto First = MakeImportRequest("/AssetToolsTests/BatchDuplicate",
		MakeFactory(DAssetToolsFactoryForTest::EMode::Success));
	auto Second = First;
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
		First.AssetPath.GetPackagePath(), "AnotherAsset", Second.AssetPath));
	const auto Result = Durin::IAssetTools::Get().ImportAssets({.Items = {First, Second}});
	ASSERT_EQ(Result.size(), 2u);
	EXPECT_EQ(Result[0], nullptr);
	EXPECT_EQ(Result[1], nullptr);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(First.AssetPath.GetPackagePath().GetView()), nullptr);
}

TEST(DFactoryTests, BatchContinuesAfterFactoryFailureAndKeepsSuccessfulPeersDirty)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	auto* Success = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const Durin::FAssetImportBatchRequest Batch{.Items = {
		MakeImportRequest("/AssetToolsTests/BatchFirst", Success),
		MakeImportRequest("/AssetToolsTests/BatchFailed", MakeFactory(DAssetToolsFactoryForTest::EMode::Fail)),
		MakeImportRequest("/AssetToolsTests/BatchLast", Success)}};
	const auto Result = Durin::IAssetTools::Get().ImportAssets(Batch);
	ASSERT_EQ(Result.size(), 3u);
	EXPECT_NE(Result[0], nullptr);
	EXPECT_EQ(Result[1], nullptr);
	EXPECT_NE(Result[2], nullptr);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Batch.Items[1].AssetPath.GetPackagePath().GetView()), nullptr);
	for (const size_t Index : {0u, 2u})
	{
		ASSERT_NE(Result[Index], nullptr);
		EXPECT_TRUE(Result[Index]->GetPackage()->IsDirty());
		ASSERT_NE(Result[Index]->GetPackage(), nullptr);
		EXPECT_TRUE(Durin::IAssetTools::Get().DiscardPackage(Result[Index]->GetPackage()));
	}
}

TEST(DFactoryTests, BatchStopAndCancelLeaveUnattemptedPackagesAbsent)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	auto First = MakeImportRequest("/AssetToolsTests/BatchStop", nullptr);
	First.Filename.clear();
	const auto Second = MakeImportRequest("/AssetToolsTests/BatchUnattempted", nullptr);
	const auto Stopped = Durin::IAssetTools::Get().ImportAssets({
		.Items = {First, Second}, .bStopOnFailure = true});
	ASSERT_EQ(Stopped.size(), 2u);
	EXPECT_EQ(Stopped[0], nullptr);
	EXPECT_EQ(Stopped[1], nullptr);
	const auto Canceled = Durin::IAssetTools::Get().ImportAssets({
		.Items = {First, Second}, .ShouldCancel = [] { return true; }});
	ASSERT_EQ(Canceled.size(), 2u);
	for (const auto& Item : Canceled)
		EXPECT_EQ(Item, nullptr);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Second.AssetPath.GetPackagePath().GetView()), nullptr);
	EXPECT_TRUE(Durin::IAssetTools::Get().ImportAssets({}).empty());
}

TEST(DFactoryTests, ImportRejectsUncatalogedDestinationFileAndWrongAssetName)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	const auto File = Durin::Testing::GetTestWorkDirectory() / "AssetTools" / "BatchOccupied.dasset";
	const std::array Bytes{std::byte{42}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, File));
	const auto Occupied = MakeImportRequest("/AssetToolsTests/BatchOccupied",
		MakeFactory(DAssetToolsFactoryForTest::EMode::Success));
	const auto WrongName = MakeImportRequest("/AssetToolsTests/BatchWrongName",
		MakeFactory(DAssetToolsFactoryForTest::EMode::WrongName));
	const auto Result = Durin::IAssetTools::Get().ImportAssets({.Items = {Occupied, WrongName}});
	ASSERT_EQ(Result.size(), 2u);
	for (const auto& Item : Result)
		EXPECT_EQ(Item, nullptr);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(WrongName.AssetPath.GetPackagePath().GetView()), nullptr);
	EXPECT_EQ(std::filesystem::file_size(File), 1u);
	std::filesystem::remove(File);
}

TEST(DFactoryTests, BatchRevalidatesDestinationAfterPreflight)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	const auto Item = MakeImportRequest("/AssetToolsTests/BatchRevalidated",
		MakeFactory(DAssetToolsFactoryForTest::EMode::Success));
	Durin::DPackage* Occupant = nullptr;
	const auto Result = Durin::IAssetTools::Get().ImportAssets({
		.Items = {Item}, .ShouldCancel = [&] {
			Occupant = Durin::CreatePackage(Item.AssetPath.GetPackagePath());
			return false;
		}});
	ASSERT_NE(Occupant, nullptr);
	ASSERT_EQ(Result.size(), 1u);
	EXPECT_EQ(Result[0], nullptr);
	EXPECT_EQ(Durin::FindPackage(Item.AssetPath.GetPackagePath().GetView()), Occupant);
	EXPECT_TRUE(Durin::IAssetTools::Get().DiscardPackage(Occupant));
}

TEST(DFactoryTests, BatchCancellationPreservesAlreadyAcceptedAsset)
{
	InitializeFactoryTestGameThread();
	EnsureAssetToolsTestMount();
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	const auto First = MakeImportRequest("/AssetToolsTests/BatchBeforeCancel", Factory);
	const auto Second = MakeImportRequest("/AssetToolsTests/BatchAfterCancel", Factory);
	size_t Checks = 0;
	const auto Result = Durin::IAssetTools::Get().ImportAssets({
		.Items = {First, Second}, .ShouldCancel = [&] { return ++Checks == 2; }});
	ASSERT_EQ(Result.size(), 2u);
	EXPECT_NE(Result[0], nullptr);
	EXPECT_EQ(Result[1], nullptr);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone); // Owner boundary after factory callbacks have returned.
	EXPECT_EQ(Durin::FindPackage(Second.AssetPath.GetPackagePath().GetView()), nullptr);
	EXPECT_TRUE(Durin::IAssetTools::Get().DiscardPackage(Result[0]->GetPackage()));
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
	auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, Durin::DObject::StaticClass(), nullptr, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
	ASSERT_TRUE(Result) << ResultError;
	ASSERT_NE(Result, nullptr);
	ASSERT_NE(Result->GetPackage(), nullptr);
	EXPECT_EQ(Result->GetOuter(), Result->GetPackage());
	EXPECT_EQ(Result->GetPackage()->FindTopLevelAsset(Result->GetFName()), Result);
	EXPECT_TRUE(Durin::IAssetTools::Get().DiscardPackage(Result->GetPackage()));
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
	auto Result = Durin::IAssetTools::Get().ImportPackageLeafAssetForTesting(
		Path, Durin::DObject::StaticClass(), {}, nullptr, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
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
	const auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
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
	const auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
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
	const auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
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
	const std::array InvalidRootBytes{std::byte{1}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(InvalidRootBytes, InvalidRoot));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/AssetToolsSaveFailure/SaveFailure", Path));
	auto* Factory = MakeFactory(DAssetToolsFactoryForTest::EMode::Success);
	std::string ResultError;
	Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
	});
	const auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
	ASSERT_TRUE(Result);
	EXPECT_FALSE(Durin::SavePackage(Result->GetPackage()));
	EXPECT_EQ(Durin::FindResidentPackage(Path), Result->GetPackage());
	EXPECT_TRUE(Durin::IAssetTools::Get().DiscardPackage(Result->GetPackage()));
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
	const auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
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
		auto LoadedValue = Durin::LoadObjectDetailed<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
		Reloaded = LoadedValue.value_or(nullptr);
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
	auto* Created = IAssetTools::Get().CreatePackageLeafAssetForTesting(SourcePath,
		DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success));
	ASSERT_NE(Created, nullptr);
	std::vector<FPackagePath> Saved;
	ASSERT_TRUE(IAssetTools::Get().SaveAssets({.AssetPaths = {SourcePath},
		.OnSaved = [&](const FPackagePath& Path) { Saved.push_back(Path); }}));
	EXPECT_EQ(Saved, std::vector{SourcePath});
	auto* Copy = IAssetTools::Get().DuplicateAsset({
		.SourcePath = Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath),
		.DestinationDirectory = "/AssetToolsTests/"});
	ASSERT_NE(Copy, nullptr);
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	const auto CopyPath = Copy->GetPackage()->GetPackagePathIdentity();
	ASSERT_TRUE(IAssetTools::Get().SaveAssets({.AssetPaths = {CopyPath}}));
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
		const auto Created = IAssetTools::Get().CreatePackageLeafAssetForTesting(
			Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
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
	const auto Result = IAssetTools::Get().SaveAssets({.AssetPaths = {Paths[0], Paths[1], Paths[2], Paths[0]},
		.OnSaved = [&](const FPackagePath& Path) { Notifications.push_back(Path); }}, &ResultFeedback);
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
	const auto FailedOnly = IAssetTools::Get().SaveAssets({.AssetPaths = {Paths[1]},
		.OnSaved = [&](const FPackagePath& Path) { Notifications.push_back(Path); }}, &FailedOnlyFeedback);
	EXPECT_FALSE(FailedOnly);
	EXPECT_EQ(Notifications, (std::vector{Paths[0], Paths[2]}));
	std::filesystem::remove(Blocker);
	std::string RetryError;
	Durin::FFeedbackContext RetryFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!RetryError.empty()) RetryError += "\n"; RetryError += Message; }
	});
	const auto Retry = IAssetTools::Get().SaveAssets({.AssetPaths = {Paths[1]}}, &RetryFeedback);
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
	const auto Created = IAssetTools::Get().CreatePackageLeafAssetForTesting(
		SourcePath, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
	ASSERT_TRUE(Created);
	EXPECT_FALSE(FindPackageExact(SourcePath));
	std::string CopyError;
	Durin::FFeedbackContext CopyFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CopyError.empty()) CopyError += "\n"; CopyError += Message; }
	});
	const auto Copy = IAssetTools::Get().DuplicateAsset({
		.SourcePath = Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath),
		.DestinationDirectory = "/AssetToolsTests/LiveCopies"}, &CopyFeedback);
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
	const auto Repeated = IAssetTools::Get().DuplicateAsset({
		.SourcePath = Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath),
		.DestinationDirectory = "/AssetToolsTests/LiveCopies"}, &RepeatedFeedback);
	ASSERT_TRUE(Repeated);
	EXPECT_EQ(Repeated->GetPackage()->GetPackagePathIdentity().GetPackageName(), "LiveDuplicateSource_Copy");
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
	const auto Created = IAssetTools::Get().CreatePackageLeafAssetForTesting(
		SourcePath, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
	ASSERT_TRUE(Created);
	TStrongObjectPtr<DObject> SourceOwner(Created);
	ASSERT_TRUE(SavePackage(Created->GetPackage()));
	for (auto Phase : {Testing::EPackageSaveFaultPoint::PublishPackage, Testing::EPackageSaveFaultPoint::PublishRegistry})
	{
		SCOPED_TRACE(static_cast<int>(Phase));
		auto* Copy = IAssetTools::Get().DuplicateAsset({
			.SourcePath = Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath),
			.DestinationDirectory = "/AssetToolsTests/FailedCopies"});
		ASSERT_NE(Copy, nullptr);
		FAssetWriteResult Saved;
		{
			Testing::FScopedPackageSaveFault Fault([Phase](auto Current, size_t) { return Current == Phase; });
			Saved = SavePackage(Copy->GetPackage());
		}
		EXPECT_FALSE(Saved);
		EXPECT_EQ(Saved.Effect, Phase == Testing::EPackageSaveFaultPoint::PublishRegistry
			? EAssetWriteEffect::ContentCommittedProjectionPending : EAssetWriteEffect::None);
		const auto Path = Copy->GetPackage()->GetPackagePathIdentity();
		EXPECT_EQ(FindResidentPackage(Path), Copy->GetPackage());
		EXPECT_EQ(Copy->GetPackage()->IsDirty(), Phase != Testing::EPackageSaveFaultPoint::PublishRegistry);
		EXPECT_EQ(FindResidentPackage(SourcePath), Created->GetPackage());
		const auto Retry = SavePackage(Copy->GetPackage());
		ASSERT_TRUE(Retry) << Retry.Message;
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
	const auto Result = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), Factory, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &ResultFeedback);
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
	EXPECT_EQ(Result.SaveResult->Error, Durin::EAssetWriteError::InvalidPath);
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
	const auto Created = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
	ASSERT_TRUE(Created) << CreatedError;
	Durin::FReimportResult Result;
	Durin::FReimportManager::Reimport(*Created, {.bSave = true},
		[&](Durin::FReimportResult Completed) { Result = std::move(Completed); });
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Message, "100");
	ASSERT_TRUE(Result.SaveResult);
	EXPECT_TRUE(*Result.SaveResult) << Result.SaveResult->Message;
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
	const auto Created = Durin::IAssetTools::Get().CreatePackageLeafAssetForTesting(
		Path, DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Success), nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
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
		auto* Result = IAssetTools::Get().CreatePackageLeafAssetForTesting(Path,
			DFactoryAssetForTest::StaticClass(), MakeFactory(DAssetToolsFactoryForTest::EMode::Fail));
		EXPECT_EQ(Result, nullptr);
		EXPECT_EQ(FindResidentPackage(Path), nullptr);
		EXPECT_TRUE(IsGarbageCollectionRequested());
	}
	EXPECT_TRUE(TryCollectGarbage(Durin::EObjectFlags::Standalone));
	EXPECT_EQ(FindPackage(Path.GetView()), nullptr);
}

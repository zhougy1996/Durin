#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	class DPersistedObject : public DObject
	{
	public:
		explicit DPersistedObject(const FObjectInitializer& I = FObjectInitializer::Get()) : DObject(I) {}
		static auto __DefaultConstructor(const FObjectInitializer& I) -> void { new (I.GetObj()) DPersistedObject(I); }
		static auto StaticClassNoRegister() -> DClass*
		{
			static DClass* Class = [] {
				auto* C = new DClass(EC_StaticConstructor, "DPersistedObject", sizeof(DPersistedObject), alignof(DPersistedObject),
					EObjectFlags::NoFlags, EClassFlags::None, EClassCastFlags::DClass, &InternalConstructor<DPersistedObject>);
				C->SetSuperStructBase(DObject::StaticClass());
				C->Register(DClass::StaticClass, "", "DPersistedObject");
				return C;
			}();
			return Class;
		}
		static auto StaticClass() -> DClass*
		{
			static DClass* Class = [] {
				using namespace DurinCodeGen;
				static const FInt32PropertyParams ValueProperty{"Value", EPropertyFlags::None, 1,
					STRUCT_OFFSET_UINT16(DPersistedObject, Value)};
				static const auto ReferenceProperty = FObjectPropertyParams::ObjectPtr<DObject>("Reference",
					EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DPersistedObject, Reference), &DObject::StaticClass);
				static const FPropertyParamsBase* Properties[]{&ValueProperty, &ReferenceProperty};
				static const FClassParams Params{&StaticClassNoRegister, "DPersistedObject", "DPersistedObject", Properties, std::size(Properties)};
				return ConstructDClass(Params);
			}();
			return Class;
		}
		auto Serialize(FArchive& Ar) -> void override
		{
			DObject::Serialize(Ar);
			if (!Payload.IsEmpty())
			{
				auto* ObjectArchive = RequireObjectArchive(Ar);
				if (!ObjectArchive) return;
				auto Scope = ObjectArchive->EnterField({.DeclaringType = FName("DPersistedObject"), .Name = FName("Payload"),
					.LogicalType = FArchiveLogicalTypeDescriptor::BulkData()});
				FArchiveBulkDataValue Bulk{.PayloadId = {1, 2, 3, 4}, .LogicalSize = Payload.GetSize(),
					.StoredSize = Payload.GetSize(), .ContentHash = FXxHash128::HashBuffer(Payload.GetBytes()), .Buffer = Payload};
				Ar.SerializeBulkData(Bulk, {.ElementSize = 1, .Alignment = 16});
			}
		}
		int32 Value = 7;
		TObjectPtr<DObject> Reference;
		FSharedByteBuffer Payload;
	};
	class FPackagePersistenceTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			ASSERT_TRUE(InitializeTaskScheduler(2));
			Root = Testing::CreateTestFixtureDirectory("PackagePersistence");
			Testing::RegisterMountPointForTests("/Persistence/", Root.generic_string() + "/");
			ASSERT_TRUE(FPackagePath::TryCreate("/Persistence/Package", Path));
			Package = CreatePackage(Path);
			ASSERT_NE(Package, nullptr);
			Asset = NewObject<DPersistedObject>(Package, "Asset", EObjectFlags::Public);
			ASSERT_NE(Asset, nullptr);
			Package->MarkDirty();
			Options.Destination = Root / "Package.dasset";
		}
		auto TearDown() -> void override
		{
			SetPackageDestinationResolver({});
			ShutdownTaskScheduler();
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
		}
		auto Read() -> ObjectPackage::FLinkerTables
		{
			FByteBuffer Main, Bulk;
			EXPECT_TRUE(FFileHelper::LoadFileToArray(Main, Options.Destination));
			auto Companion = Options.Destination; Companion.replace_extension(".dbulk");
			if (std::filesystem::exists(Companion)) EXPECT_TRUE(FFileHelper::LoadFileToArray(Bulk, Companion));
			ObjectPackage::FLinkerTables Linker;
			ObjectPackage::FPackageReaderDiagnostic Diagnostic;
			EXPECT_TRUE(ObjectPackage::ReadPackage(Main, Bulk, Path, Linker, &Diagnostic)) << Diagnostic.Message;
			return Linker;
		}
		std::filesystem::path Root;
		FPackagePath Path;
		DPackage* Package = nullptr;
		DPersistedObject* Asset = nullptr;
		FPackageSaveOptions Options;
	};
}

TEST_F(FPackagePersistenceTests, DeltaCompleteAndInternalReferencesRoundTripWithoutEngine)
{
	Asset->Reference = NewObject<DObject>(Asset, "Child");
	for (auto Mode : {EPackageSaveMode::Delta, EPackageSaveMode::Complete})
	{
		Options.Mode = Mode;
		auto Result = Package->Save(Options);
		ASSERT_TRUE(Result) << Result.Message;
		EXPECT_EQ(Result.CommitState, EPackageCommitState::Committed);
		EXPECT_FALSE(Package->IsDirty());
		auto Linker = Read();
		ASSERT_EQ(Linker.Exports.size(), 2u);
		const auto& Export = Linker.Exports.front();
		EXPECT_EQ(Export.bUseClassDefaults, Mode == EPackageSaveMode::Delta);
		EXPECT_TRUE(std::ranges::any_of(Export.Properties, [](const auto& Tag) { return Tag.Value.Reference.IsExport(); }));
		if (Mode == EPackageSaveMode::Complete)
			EXPECT_TRUE(std::ranges::any_of(Export.Properties, [](const auto& Tag) { return Tag.Value.Signed == 7; }));
	}
}
TEST_F(FPackagePersistenceTests, AsyncCompletionMatchesSynchronousBytes)
{
	Asset->Value = 42;
	ASSERT_TRUE(Package->Save(Options));
	FByteBuffer Before, After;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Before, Options.Destination));
	Package->MarkDirty();
	FPackageSaveResult Admission;
	auto Operation = Package->SaveAsync(Admission, Options);
	ASSERT_TRUE(Operation) << Admission.Message;
	EXPECT_FALSE(Operation->IsCompleted());
	ASSERT_TRUE(Operation->WaitAndComplete());
	EXPECT_TRUE(Operation->IsCompleted());
	EXPECT_TRUE(Operation->Complete());
	ASSERT_TRUE(FFileHelper::LoadFileToArray(After, Options.Destination));
	EXPECT_EQ(Before, After);
	EXPECT_FALSE(Package->IsDirty());
}
TEST_F(FPackagePersistenceTests, ExternalBulkRoundTripsAndObsoleteCompanionIsRemoved)
{
	Options.Mode = EPackageSaveMode::Complete;
	Asset->Payload = FSharedByteBuffer::Take(FByteBuffer(300 * 1024, std::byte{0x5a}));
	auto Result = Package->Save(Options);
	ASSERT_TRUE(Result) << Result.Message;
	auto Linker = Read();
	ASSERT_EQ(Linker.Exports.size(), 1u);
	EXPECT_TRUE(std::ranges::any_of(Linker.Exports.front().Properties, [](const auto& Tag) {
		return Tag.Value.Bytes.size() == 300 * 1024 && Tag.Value.Bytes.front() == std::byte{0x5a};
	}));
	Asset->Payload = {}; Package->MarkDirty();
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_FALSE(std::filesystem::exists(Root / "Package.dbulk"));
}
TEST_F(FPackagePersistenceTests, StaleEditsCancelAndAbandonDoNotPublish)
{
	FPackageSaveResult Admission;
	auto Operation = Package->SaveAsync(Admission, Options);
	ASSERT_TRUE(Operation) << Admission.Message;
	Package->MarkDirty();
	EXPECT_EQ(Operation->WaitAndComplete().Error, EPackageSaveError::StaleData);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	Operation = Package->SaveAsync(Admission, Options);
	ASSERT_TRUE(Operation);
	EXPECT_EQ(Operation->Cancel().Error, EPackageSaveError::Cancelled);
	Operation = Package->SaveAsync(Admission, Options);
	ASSERT_TRUE(Operation);
	Operation.reset();
	EXPECT_TRUE(std::filesystem::is_empty(Root));
}
TEST_F(FPackagePersistenceTests, CompetingWriterAndStagedRollbackPreservePreviousContent)
{
	ASSERT_TRUE(Package->Save(Options));
	FPackageSaveResult Admission;
	auto Older = Package->SaveAsync(Admission, Options);
	ASSERT_TRUE(Older);
	Asset->Value = 33; Package->MarkDirty();
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_EQ(Older->WaitAndComplete().Error, EPackageSaveError::StaleData);
	Asset->Value = 55; Package->MarkDirty();
	auto Staged = FPackageSaveOperation::Begin(Package, Options, Admission, false);
	ASSERT_TRUE(Staged);
	ASSERT_TRUE(Staged->CommitStaged());
	EXPECT_EQ(Staged->RollbackCommit().Error, EPackageSaveError::Cancelled);
	EXPECT_TRUE(Package->IsDirty());
	auto Linker = Read();
	EXPECT_TRUE(std::ranges::any_of(Linker.Exports.front().Properties, [](const auto& Tag) { return Tag.Value.Signed == 33; }));
}
TEST_F(FPackagePersistenceTests, ResolverAndAdmissionFailureAreExplicit)
{
	EXPECT_EQ(Package->Save().Error, EPackageSaveError::InvalidPath);
	SetPackageDestinationResolver([&](const DPackage&) { return Options.Destination; });
	ASSERT_TRUE(Package->Save());
	ShutdownTaskScheduler();
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission));
	EXPECT_EQ(Admission.Error, EPackageSaveError::ShuttingDown);
	EXPECT_TRUE(Package->Save()) << "Synchronous persistence must not require a scheduler.";
}
TEST_F(FPackagePersistenceTests, CorruptedStageAndDestinationConflictDoNotClearDirty)
{
	FPackageSaveResult Admission;
	auto Staged = FPackageSaveOperation::Begin(Package, Options, Admission, false);
	ASSERT_TRUE(Staged) << Admission.Message;
	bool Corrupted = false;
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		if (Entry.path().filename().string().find(".package-save-") != std::string::npos)
		{
			FByteBuffer Bytes;
			ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Entry.path()));
			ASSERT_FALSE(Bytes.empty());
			Bytes.back() ^= std::byte{1};
			ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bytes, Entry.path()));
			Corrupted = true;
		}
	ASSERT_TRUE(Corrupted);
	EXPECT_EQ(Staged->Complete().Error, EPackageSaveError::CorruptFile);
	EXPECT_TRUE(Package->IsDirty());
	Staged = FPackageSaveOperation::Begin(Package, Options, Admission, false);
	ASSERT_TRUE(Staged);
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(FByteBuffer{std::byte{1}}, Options.Destination));
	EXPECT_EQ(Staged->Complete().Error, EPackageSaveError::StaleData);
	FByteBuffer Current;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Options.Destination));
	EXPECT_EQ(Current, FByteBuffer{std::byte{1}});
}
TEST_F(FPackagePersistenceTests, IoFailureAndSchedulerDrainRemainObservable)
{
	const auto Blocker = Root / "Blocked";
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(FByteBuffer{std::byte{1}}, Blocker));
	Options.Destination = Blocker / "Package.dasset";
	EXPECT_FALSE(Package->Save(Options));
	EXPECT_TRUE(Package->IsDirty());
	Options.Destination = Root / "Package.dasset";
	FPackageSaveResult Admission;
	auto Operation = Package->SaveAsync(Admission, Options);
	ASSERT_TRUE(Operation);
	ShutdownTaskScheduler();
	const auto Result = Operation->WaitAndComplete();
	// Shutdown may cancel admitted work; it must expose the outcome and drain safely.
	EXPECT_TRUE(Operation->IsCompleted());
	EXPECT_EQ(std::filesystem::exists(Options.Destination), Result.Succeeded());
	EXPECT_EQ(Package->IsDirty(), !Result.Succeeded());
}
TEST_F(FPackagePersistenceTests, FailedRollbackRetainsBackupAndReportsRecoveryRequired)
{
	ASSERT_TRUE(Package->Save(Options));
	Asset->Value = 19; Package->MarkDirty();
	FPackageSaveResult Admission;
	auto Operation = FPackageSaveOperation::Begin(Package, Options, Admission, false);
	ASSERT_TRUE(Operation);
	ASSERT_TRUE(Operation->CommitStaged());
	ASSERT_TRUE(std::filesystem::remove(Options.Destination));
	ASSERT_TRUE(std::filesystem::create_directory(Options.Destination));
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(FByteBuffer{std::byte{1}}, Options.Destination / "Blocker"));
	const auto Result = Operation->RollbackCommit();
	EXPECT_EQ(Result.CommitState, EPackageCommitState::RecoveryRequired);
	ASSERT_FALSE(Result.RecoveryFiles.empty());
	EXPECT_TRUE(std::filesystem::exists(Result.RecoveryFiles.front()));
	EXPECT_EQ(Result.Error, EPackageSaveError::IoError);
	EXPECT_TRUE(Package->IsDirty());
	bool HasBackup = false;
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		HasBackup |= Entry.path().extension() == ".backup";
	EXPECT_TRUE(HasBackup);
}
TEST_F(FPackagePersistenceTests, FilteredSnapshotsPreserveAuthoredDirtyState)
{
	Options.Capture.PropertyFilter = [](const DObject*, const FProperty*) { return true; };
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_TRUE(Package->IsDirty());
}
TEST_F(FPackagePersistenceTests, ShutdownDuringResolverIsAnAdmissionFailure)
{
	SetPackageDestinationResolver([&](const DPackage&) {
		ShutdownTaskScheduler();
		return Options.Destination;
	});
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission));
	EXPECT_EQ(Admission.Error, EPackageSaveError::ShuttingDown);
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
}

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
	auto FinishSave(Tasks::TTask<FPackageSaveResult>& Task) -> FPackageSaveResult
	{
		EXPECT_TRUE(DPackage::DrainAsyncSaves());
		if (Task.GetState() != ETaskState::Succeeded) return {EPackageSaveError::IoError, "Task execution failed."};
		return Task.GetResult();
	}
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
			if (CaptureHook) CaptureHook(Ar);
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
		std::function<void(FArchive&)> CaptureHook;
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
			ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			PackageSavePrivate::SetAsyncSaveAdmission(true);
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
			PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::None);
			(void)DPackage::DrainAsyncSaves();
			PackageSavePrivate::SetAsyncSaveLimitsForTests(64, 256ull * 1024 * 1024);
			PackageSavePrivate::SetAsyncSaveAdmission(true);
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
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
			ObjectPackage::FPackageReaderResult Diagnostic;
			EXPECT_TRUE((Diagnostic = ObjectPackage::ReadPackage(Main, Bulk, Path, Linker))) << Durin::ObjectPackage::FormatPackageError(Diagnostic);
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
	auto Operation = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Operation.IsValid()) << Admission.Message;
	EXPECT_FALSE(Operation.IsCompleted());
	ASSERT_TRUE(FinishSave(Operation));
	EXPECT_TRUE(Operation.IsCompleted());
	EXPECT_TRUE(Operation.GetResult());
	ASSERT_TRUE(FFileHelper::LoadFileToArray(After, Options.Destination));
	EXPECT_EQ(Before, After);
	EXPECT_FALSE(Package->IsDirty());
}
TEST_F(FPackagePersistenceTests, ProtectedTaskCompletesOnHostPumpAndRejectsGameThreadWait)
{
	FPackageSaveResult Admission;
	auto Task = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Task.IsValid()) << Admission.Message;
	EXPECT_EQ(Task.Wait().WaitStatus, ETaskWaitStatus::UnsupportedThread);
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(Task.IsCompleted());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	PumpGameThreadDeferredWork();
	ASSERT_TRUE(Task.IsCompleted());
	ASSERT_EQ(Task.GetState(), ETaskState::Succeeded);
	EXPECT_TRUE(Task.GetResult());
	EXPECT_FALSE(Package->IsDirty());
}

TEST_F(FPackagePersistenceTests, AsyncCapacityIncludesPublicationAndRejectsDirectFlags)
{
	FPackageSaveResult Admission;
	Options.Flags = SAVE_Async;
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{Options}).IsValid());
	EXPECT_EQ(Admission.Error, EPackageSaveError::InvalidPackageType);
	Options.Flags = SAVE_None;
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 1);
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{Options}).IsValid());
	EXPECT_EQ(Admission.Error, EPackageSaveError::Busy);
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 256ull * 1024 * 1024);
	auto Task = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Task.IsValid());
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{Options}).IsValid());
	EXPECT_EQ(Admission.Error, EPackageSaveError::Busy);
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_TRUE(Task.GetResult());
	auto Next = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Next.IsValid());
	EXPECT_TRUE(FinishSave(Next));
}

TEST_F(FPackagePersistenceTests, MissingDeferredExecutorRejectsBeforeWriting)
{
	ShutdownTaskSystem(ETaskShutdownMode::Drain);
	ASSERT_TRUE(InitializeTaskScheduler(2));
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{Options}).IsValid());
	EXPECT_EQ(Admission.Error, EPackageSaveError::ShuttingDown);
	EXPECT_TRUE(std::filesystem::is_empty(Root));
}

TEST_F(FPackagePersistenceTests, RejectedPublicationCannotStartDiskWorkAndReleasesCapacity)
{
	PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::RejectAdmission);
	bool bWrote = false, bFinished = false;
	auto Result = PackageSavePrivate::SubmitAsyncSave(1, [&] { bWrote = true; }, [&](bool) { bFinished = true; });
	EXPECT_FALSE(Result);
	EXPECT_FALSE(bWrote);
	EXPECT_FALSE(bFinished);
	EXPECT_FALSE(DPackage::HasAsyncFileWrites());
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 256ull * 1024 * 1024);
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{Options}).IsValid());
	EXPECT_TRUE(std::filesystem::is_empty(Root));
	PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::None);
	auto Task = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Task.IsValid());
	EXPECT_TRUE(FinishSave(Task));
}

TEST_F(FPackagePersistenceTests, CanceledPublicationIsReapedOnGameThreadBeforeTaskShutdown)
{
	PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::CancelContinuation);
	FPackageSaveResult Admission;
	auto Task = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Task.IsValid());
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(Task.IsCompleted());
	PackageSavePrivate::SetAsyncSaveAdmission(false);
	PackageSavePrivate::PollAsyncSaves();
	ASSERT_TRUE(Task.IsCompleted());
	ASSERT_EQ(Task.GetState(), ETaskState::Succeeded);
	EXPECT_TRUE(Task.GetResult());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_TRUE(DPackage::DrainAsyncSaves());
	ShutdownTaskSystem(ETaskShutdownMode::Cancel);
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
TEST_F(FPackagePersistenceTests, StaleEditsAndCancellationRejectButDroppedHandlesComplete)
{
	FPackageSaveResult Admission;
	auto Operation = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Operation.IsValid()) << Admission.Message;
	Package->MarkDirty();
	EXPECT_EQ(FinishSave(Operation).Error, EPackageSaveError::StaleData);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	Operation = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Operation.IsValid());
	EXPECT_TRUE(Tasks::Cancel(Operation.GetCompletion()));
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Operation.GetState(), ETaskState::Canceled);
	Operation = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Operation.IsValid());
	Operation = {};
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(Options.Destination));
}
TEST_F(FPackagePersistenceTests, CompetingWriterAndStagedRollbackPreservePreviousContent)
{
	ASSERT_TRUE(Package->Save(Options));
	FPackageSaveResult Admission;
	auto Older = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Older.IsValid());
	Asset->Value = 33; Package->MarkDirty();
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_EQ(FinishSave(Older).Error, EPackageSaveError::StaleData);
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
	(void)DPackage::DrainAsyncSaves();
	ShutdownTaskSystem(ETaskShutdownMode::Drain);
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{}).IsValid());
	EXPECT_EQ(Admission.Error, EPackageSaveError::ShuttingDown);
	EXPECT_TRUE(Package->Save()) << "Synchronous persistence must not require a scheduler.";
}
TEST_F(FPackagePersistenceTests, ChangedStageAndDestinationConflictDoNotClearDirty)
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
			const auto OriginalTime = std::filesystem::last_write_time(Entry.path());
			ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bytes, Entry.path()));
			std::filesystem::last_write_time(Entry.path(), OriginalTime + std::chrono::seconds(10));
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
TEST_F(FPackagePersistenceTests, DirectWriterPublishesWithoutStagingOrBackupAndCannotRollback)
{
	const auto Main = Root / "Direct.dasset";
	const auto Bulk = Root / "Direct.dbulk";
	const FByteBuffer Old{std::byte{1}}, New{std::byte{2}, std::byte{3}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Old, Main));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Old, Bulk));
	FFilePublicationStamp MainStamp, BulkStamp;
	ASSERT_TRUE(FFilePublicationStamp::Inspect(Main, MainStamp));
	ASSERT_TRUE(FFilePublicationStamp::Inspect(Bulk, BulkStamp));
	auto Write = GetDirectFilePackageWriter()->Begin({
		{{Bulk, {}, Bulk.string() + ".backup"}, BulkStamp, {}},
		{{Main, Main.string() + ".stage", Main.string() + ".backup"}, MainStamp, New}});
	FByteBuffer Current;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Main));
	EXPECT_EQ(Current, Old);
	const auto Result = Write->Stage();
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.State, EPackageWriteState::Committed);
	EXPECT_TRUE(Result.RecoveryFiles.empty());
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Main));
	EXPECT_EQ(Current, New);
	EXPECT_FALSE(std::filesystem::exists(Bulk));
	EXPECT_FALSE(std::filesystem::exists(Main.string() + ".stage"));
	EXPECT_FALSE(std::filesystem::exists(Main.string() + ".backup"));
	EXPECT_FALSE(std::filesystem::exists(Bulk.string() + ".backup"));
	EXPECT_FALSE(Write->Rollback());
	EXPECT_TRUE(Write->Finalize());
	Write.reset();
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Main));
	EXPECT_EQ(Current, New);
}

TEST_F(FPackagePersistenceTests, DirectWriterReportsPartialClosureWithoutRestoringOldBytes)
{
	const auto Main = Root / "Partial.dasset";
	const auto Bulk = Root / "Partial.dbulk";
	const FByteBuffer Old{std::byte{1}}, New{std::byte{2}};
	for (const size_t FailIndex : {size_t{0}, size_t{1}})
	{
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Old, Main));
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Old, Bulk));
		FFilePublicationStamp MainStamp, BulkStamp;
		ASSERT_TRUE(FFilePublicationStamp::Inspect(Main, MainStamp));
		ASSERT_TRUE(FFilePublicationStamp::Inspect(Bulk, BulkStamp));
		auto Write = GetDirectFilePackageWriter([=](size_t Index) { return Index == FailIndex; })->Begin({
			{{Bulk, Bulk.string() + ".unused", {}}, BulkStamp, New},
			{{Main, Main.string() + ".unused", {}}, MainStamp, New}});
		const auto Result = Write->Stage();
		EXPECT_FALSE(Result);
		EXPECT_EQ(Result.State, FailIndex == 0 ? EPackageWriteState::NotCommitted : EPackageWriteState::PartiallyWritten);
		EXPECT_TRUE(Result.RecoveryFiles.empty());
		EXPECT_EQ(Result.AffectedFiles.size(), FailIndex);
		if (FailIndex) EXPECT_EQ(Result.AffectedFiles.front(), Bulk);
		EXPECT_EQ(ToPackageSaveResult(Result).CommitState,
			FailIndex == 0 ? EPackageCommitState::NotCommitted : EPackageCommitState::PartiallyWritten);
		Write.reset();
		FByteBuffer Current;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Main));
		EXPECT_EQ(Current, Old);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Bulk));
		EXPECT_EQ(Current, FailIndex == 0 ? Old : New);
	}
}

TEST_F(FPackagePersistenceTests, PhysicalClosureAdmissionExcludesReadersAndProtectedCommits)
{
	const auto Main = Root / "Excluded.dasset";
	const auto Bulk = Root / "Excluded.dbulk";
	const std::array Paths{Main, Bulk};
	auto Read = FPackageFileAccess::TryAcquire(Paths, false);
	ASSERT_TRUE(Read);
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(Paths, false));
	EXPECT_FALSE(FPackageFileAccess::TryAcquire(Paths, true));
	Read.reset();
	auto Protected = GetFilePackageWriter()->Begin({
		{{Main, Main.string() + ".stage", Main.string() + ".backup"}, {}, {std::byte{1}}}});
	ASSERT_TRUE(Protected->Stage());
	auto Direct = GetDirectFilePackageWriter()->Begin({
		{{Main, Main.string() + ".unused", {}}, {}, {std::byte{2}}},
		{{Bulk, {}, {}}, {}, {}}});
	EXPECT_FALSE(FPackageFileAccess::TryAcquire(Paths, false));
	EXPECT_FALSE(Protected->Commit());
	ASSERT_TRUE(Direct->Stage());
	Direct.reset();
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(Paths, false));
	EXPECT_EQ(Protected->Commit().Error, EPackageWriteError::StaleData);
	// The failed operation may remain observable without blocking a fresh save.
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(Paths, true));
}

TEST_F(FPackagePersistenceTests, DirectWriterRejectsStaleDestinationsAndProtectedUse)
{
	const auto Main = Root / "Stale.dasset";
	const FByteBuffer Old{std::byte{7}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Old, Main));
	auto Write = GetDirectFilePackageWriter()->Begin({
		{{Main, Main.string() + ".unused", {}}, {}, {std::byte{2}}}});
	const auto Result = Write->Stage();
	EXPECT_EQ(Result.Error, EPackageWriteError::StaleData);
	EXPECT_EQ(Result.State, EPackageWriteState::NotCommitted);
	EXPECT_TRUE(Result.AffectedFiles.empty());
	FByteBuffer Current;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Main));
	EXPECT_EQ(Current, Old);
	FSavePackageContext Context{Options, GetDirectFilePackageWriter()};
	FPackageSaveResult Admission;
	EXPECT_FALSE(FPackageSaveOperation::Begin(Package, Context, Admission));
	EXPECT_EQ(Admission.Error, EPackageSaveError::InvalidPackageType);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
}

TEST_F(FPackagePersistenceTests, PhysicalClosureAdmissionIsAtomicAndNormalizesAliases)
{
	const auto Main = Root / "Alias.dasset";
	const auto Bulk = Root / "Alias.dbulk";
	const std::array MainPaths{Main};
	const std::array BulkPaths{Bulk};
	const std::array Both{Bulk, Root / "Unused" / ".." / "Alias.dasset"};
	auto MainOwner = FPackageFileAccess::TryAcquire(MainPaths, true);
	ASSERT_TRUE(MainOwner);
	EXPECT_FALSE(FPackageFileAccess::TryAcquire(Both, true));
	// Failed multi-file admission must not retain its non-conflicting prefix.
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(BulkPaths, true));
	MainOwner.reset();
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(Both, true));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, Main));
	std::error_code Ec;
	const auto Alias = Root / "HardLink.dasset";
	std::filesystem::create_hard_link(Main, Alias, Ec);
	if (!Ec)
	{
		EXPECT_FALSE(FPackageFileAccess::TryAcquire(MainPaths, true));
		const std::array AliasPaths{Alias};
		EXPECT_FALSE(FPackageFileAccess::TryAcquire(AliasPaths, true));
	}
}

TEST_F(FPackagePersistenceTests, StagedMetadataChecksDetectChangesWithoutReadingContent)
{
	const auto Destination = Root / "Detached.bin";
	const auto Stage = Root / "Detached.stage";
	const auto Backup = Root / "Detached.backup";
	const FByteBuffer Original{std::byte{7}};
	for (int Change = 0; Change < 5; ++Change)
	{
		SCOPED_TRACE(Change);
		ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Original, Destination));
		FFilePublicationStamp Expected;
		ASSERT_TRUE(FFilePublicationStamp::Inspect(Destination, Expected));
		FByteBuffer Bytes(Change == 4 ? 0 : 128 * 1024 + 3, std::byte{1});
		std::vector<FPackageWriteFile> Files;
		Files.push_back({{Destination, Stage, Backup}, Expected, Bytes});
		auto Write = GetFilePackageWriter()->Begin(std::move(Files));
		ASSERT_TRUE(Write->Stage());
		const auto StagedTime = std::filesystem::last_write_time(Stage);
		if (Change == 0) ASSERT_TRUE(std::filesystem::remove(Stage));
		else if (Change == 1)
		{
			std::filesystem::resize_file(Stage, Bytes.size() - 1);
			std::filesystem::last_write_time(Stage, StagedTime);
		}
		else if (Change == 2)
			std::filesystem::last_write_time(Stage, StagedTime + std::chrono::seconds(10));
		else if (Change == 3)
		{
			// Same-size changes with a restored timestamp are outside this contract.
			Bytes.back() = std::byte{2};
			ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bytes, Stage));
			std::filesystem::last_write_time(Stage, StagedTime);
		}
		const auto Result = Write->Commit();
		FByteBuffer Current;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Current, Destination));
		if (Change >= 3)
		{
			ASSERT_TRUE(Result);
			EXPECT_EQ(Current, Bytes);
			ASSERT_TRUE(Write->Finalize());
		}
		else
		{
			EXPECT_EQ(Result.Error, EPackageWriteError::CorruptFile);
			EXPECT_EQ(Current, Original);
			EXPECT_FALSE(std::filesystem::exists(Backup));
		}
		Write.reset();
		EXPECT_FALSE(std::filesystem::exists(Stage));
	}
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
	auto Operation = Package->SaveAsync(Admission, FSavePackageContext{Options});
	ASSERT_TRUE(Operation.IsValid());
	(void)DPackage::DrainAsyncSaves();
	ShutdownTaskSystem(ETaskShutdownMode::Drain);
	const auto Result = FinishSave(Operation);
	// Shutdown may cancel admitted work; it must expose the outcome and drain safely.
	EXPECT_TRUE(Operation.IsCompleted());
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
		(void)DPackage::DrainAsyncSaves();
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		return Options.Destination;
	});
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission, FSavePackageContext{}).IsValid());
	EXPECT_EQ(Admission.Error, EPackageSaveError::ShuttingDown);
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
}

TEST_F(FPackagePersistenceTests, SharedWriterIsolatesOperationsAndRetainsOccupiedStages)
{
	auto Writer = GetFilePackageWriter();
	const auto Destination = Root / "Detached.bin";
	const auto Stage = Root / "Detached.stage";
	const auto Backup = Root / "Detached.backup";
	FByteBuffer Bytes(4);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, Stage));
	{
		std::vector<FPackageWriteFile> Files;
		Files.push_back({{Destination, Stage, Backup}, {}, Bytes});
		auto Write = Writer->Begin(std::move(Files));
		EXPECT_FALSE(Write->Stage());
	}
	EXPECT_TRUE(std::filesystem::exists(Stage));
	std::filesystem::remove(Stage);
	std::vector<FPackageWriteFile> Files;
	Files.push_back({{Destination, Stage, Backup}, {}, std::move(Bytes)});
	auto Write = Writer->Begin(std::move(Files));
	EXPECT_FALSE(Write->Commit());
	EXPECT_FALSE(Write->Finalize());
	ASSERT_TRUE(Write->Stage());
	ASSERT_TRUE(Write->Commit());
	EXPECT_TRUE(std::filesystem::exists(Destination));
	{
		FSavePackageContext Context{Options, Writer};
		FPackageSaveResult Admission;
		auto Save = FPackageSaveOperation::Begin(Package, Context, Admission);
		ASSERT_TRUE(Admission);
		ASSERT_NE(Save, nullptr);
		Context.Options.Destination = Root / "NotUsed.dasset";
		Context.Writer.reset();
		ASSERT_TRUE(Save->WaitAndComplete());
	}
	EXPECT_TRUE(std::filesystem::exists(Options.Destination));
	ASSERT_TRUE(Write->Rollback());
	EXPECT_FALSE(std::filesystem::exists(Destination));
	EXPECT_TRUE(std::filesystem::exists(Options.Destination));
}

TEST_F(FPackagePersistenceTests, FinalizeFailureRetainsCommittedResultAndRecoveryPath)
{
	ASSERT_TRUE(Package->Save(Options));
	Package->MarkDirty();
	FPackageSaveResult Admission;
	auto Save = FPackageSaveOperation::Begin(Package, Options, Admission, false);
	ASSERT_TRUE(Admission);
	ASSERT_TRUE(Save->CommitStaged());
	std::filesystem::path Backup;
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		if (Entry.path().extension() == ".backup") Backup = Entry.path();
	ASSERT_FALSE(Backup.empty());
	// A nonempty directory deterministically makes backup deletion fail.
	ASSERT_TRUE(std::filesystem::remove(Backup));
	ASSERT_TRUE(std::filesystem::create_directory(Backup));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer(1), Backup / "Blocker"));
	const auto Result = Save->FinalizeCommit();
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.CommitState, EPackageCommitState::Committed);
	ASSERT_EQ(Result.RecoveryFiles.size(), 1);
	EXPECT_EQ(Result.RecoveryFiles.front(), Backup);
	EXPECT_TRUE(std::filesystem::exists(Options.Destination));
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Save->Complete().CommitState, EPackageCommitState::Committed);
}

TEST_F(FPackagePersistenceTests, CaptureValidationPreservesOutputAndOverrideIdentity)
{
	FSavePackageContext Context;
	ObjectPackage::FLinkerTables Linker;
	Linker.Names.push_back("sentinel");
	const auto Missing = Context.Capture(nullptr, Linker);
	EXPECT_EQ(Missing.Error.Reason, EPackageCaptureReason::MissingAssets);
	Context.Options.Capture.bCooking = true;
	const auto Target = Context.Capture(Package, Linker);
	EXPECT_EQ(Target.Error.Reason, EPackageCaptureReason::CookTarget);
	Context.Options.Capture.bCooking = false;
	auto Overrides = std::make_shared<FObjectSaveOverrides>();
	ASSERT_TRUE(Overrides->AddObjectOmission(*Asset));
	Context.Options.Capture.SaveOverrides = Overrides;
	const auto Omitted = Context.Capture(Package, Linker);
	EXPECT_EQ(Omitted.Error.Reason, EPackageCaptureReason::OmittedAsset);
	EXPECT_EQ(Omitted.Error.ObjectPath, Asset->GetObjectPath());
	Overrides.reset();
	Context.Options.Capture.SaveOverrides.reset();
	EXPECT_EQ(Omitted.Error.ObjectPath, Asset->GetObjectPath());
	EXPECT_EQ(Linker.Names, (std::vector<std::string>{"sentinel"}));
}

TEST_F(FPackagePersistenceTests, CaptureRetainsArchiveContextAndSaveAdmissionCause)
{
	Asset->CaptureHook = [](FArchive& Ar) {
		uint32 Value = 12;
		Ar.SerializeRawBytes(std::as_writable_bytes(std::span{&Value, 1}));
	};
	FSavePackageContext Context{Options};
	ObjectPackage::FLinkerTables Linker;
	Linker.Names.push_back("sentinel");
	const auto Result = Context.Capture(Package, Linker);
	EXPECT_EQ(Result.Error.Reason, EPackageCaptureReason::RawOutsideField);
	EXPECT_EQ(Result.Error.ArchiveCode, EArchiveFailureCode::MalformedSerializer);
	EXPECT_EQ(Result.Error.ObjectPath, Asset->GetObjectPath());
	EXPECT_FALSE(Result.Error.ArchivePath.empty());
	EXPECT_EQ(Linker.Names, (std::vector<std::string>{"sentinel"}));
	FPackageSaveResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission, Context).IsValid());
	ASSERT_TRUE(Admission.CaptureCause.has_value());
	EXPECT_EQ(Admission.CaptureCause->Reason, Result.Error.Reason);
	EXPECT_EQ(Admission.CaptureCause->ArchivePath, Result.Error.ArchivePath);
	EXPECT_EQ(Admission.CommitState, EPackageCommitState::NotCommitted);
}

TEST_F(FPackagePersistenceTests, CaptureRetainsNestedPropertyCause)
{
	Asset->CaptureHook = [](FArchive& Ar) {
		auto* ObjectArchive = RequireObjectArchive(Ar);
		ASSERT_NE(ObjectArchive, nullptr);
		ObjectArchive->FailPropertyValue({.Code = EPropertyValueError::UnavailableOperation,
			.Operation = EPropertyValueOperation::CopyConstruct, .PropertyName = "OwnedProperty"});
	};
	ObjectPackage::FLinkerTables Linker;
	const auto Result = FSavePackageContext{Options}.Capture(Package, Linker);
	EXPECT_EQ(Result.Error.Reason, EPackageCaptureReason::ArchiveFailure);
	const auto* Cause = std::get_if<FPropertyValueError>(&Result.Error.Cause);
	ASSERT_NE(Cause, nullptr);
	EXPECT_EQ(Cause->Code, EPropertyValueError::UnavailableOperation);
	EXPECT_EQ(Cause->Operation, EPropertyValueOperation::CopyConstruct);
	EXPECT_EQ(Cause->PropertyName, "OwnedProperty");
}

TEST_F(FPackagePersistenceTests, CaptureRetainsBulkBoundsAndFrozenManifestFailure)
{
	Asset->CaptureHook = [](FArchive& Ar) {
		auto* ObjectArchive = RequireObjectArchive(Ar);
		ASSERT_NE(ObjectArchive, nullptr);
		auto Scope = ObjectArchive->EnterField({.DeclaringType = FName("DPersistedObject"),
			.Name = FName("BrokenBulk"), .LogicalType = FArchiveLogicalTypeDescriptor::BulkData()});
		FArchiveBulkDataValue Bulk{.PayloadId = {1, 2, 3, 4}, .LogicalSize = 9, .StoredSize = 8};
		Ar.SerializeBulkData(Bulk, {.ElementSize = 1, .Alignment = 16});
	};
	ObjectPackage::FLinkerTables Linker;
	const auto Bulk = FSavePackageContext{Options}.Capture(Package, Linker);
	EXPECT_EQ(Bulk.Error.Reason, EPackageCaptureReason::BulkMetadata);
	EXPECT_EQ(Bulk.Error.Actual, 0u);
	EXPECT_EQ(Bulk.Error.Expected, 9u);
	Asset->CaptureHook = [](FArchive& Ar) {
		if (Ar.GetPurpose() != EArchivePurpose::AuthoredPackage) return;
		auto* ObjectArchive = RequireObjectArchive(Ar);
		ASSERT_NE(ObjectArchive, nullptr);
		auto Scope = ObjectArchive->EnterField({.DeclaringType = FName("DPersistedObject"),
			.Name = FName("LateField"), .LogicalType = FArchiveLogicalTypeDescriptor::Scalar(true, 32, false)});
		int32 Value = 2; Ar << Value;
	};
	const auto Manifest = FSavePackageContext{Options}.Capture(Package, Linker);
	EXPECT_EQ(Manifest.Error.Reason, EPackageCaptureReason::EmissionMutation);
}

TEST_F(FPackagePersistenceTests, CaptureRetainsDefaultDeltaDiagnostic)
{
	uint32 Pass = 0;
	Asset->CaptureHook = [&](FArchive& Ar) {
		if (++Pass == 3) Ar.Fail(EArchiveFailureCode::InvalidData, "test-only planner failure");
	};
	FSavePackageContext Context{Options};
	Context.Options.Mode = EPackageSaveMode::Complete;
	ObjectPackage::FLinkerTables Linker;
	Linker.Names.push_back("sentinel");
	const auto Result = Context.Capture(Package, Linker);
	EXPECT_EQ(Result.Error.Reason, EPackageCaptureReason::DefaultDelta);
	const auto* Cause = std::get_if<FDefaultDeltaDiagnostic>(&Result.Error.Cause);
	ASSERT_NE(Cause, nullptr);
	EXPECT_EQ(Cause->Reason, EDefaultDeltaFailureReason::ArchiveFailure);
	EXPECT_EQ(Cause->ArchiveReason, EArchiveFailureCode::InvalidData);
	EXPECT_EQ(Linker.Names, (std::vector<std::string>{"sentinel"}));
}

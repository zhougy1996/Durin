#include "NativePackageNotificationTestSupport.h"
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
#include "Threading/ThreadEvent.h"
#include "Threading/RunnableThread.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	auto FinishSave(Tasks::TTask<FPackageWriteResult>& Task) -> FPackageWriteResult
	{
		EXPECT_TRUE(DPackage::DrainAsyncSaves());
		if (Task.GetState() != ETaskState::Succeeded) return {.Error = Durin::UnifiedError::FError{PackageSaveErrors::IoError, "Task execution failed."}};
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
		auto PreSave(FObjectPreSaveContext& Context) -> void override
		{
			DObject::PreSave(Context);
			if (SaveHook) SaveHook(Context);
		}
		std::function<void(FObjectPreSaveContext&)> SaveHook;
		auto OnCookEvent(ECookEvent Event, FObjectCookContext& Context) const -> void override
		{
			DObject::OnCookEvent(Event, Context);
			if (DependencyHook) DependencyHook(Context);
		}
		std::function<void(FObjectCookContext&)> DependencyHook;
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
			CollectGarbage(Durin::EObjectFlags::Standalone);
		}
		auto Read() -> ObjectPackage::FLinkerTables
		{
			FByteBuffer Main, Bulk;
			auto MainRead = FFileHelper::LoadFileToArray(Options.Destination);
			EXPECT_TRUE(MainRead) << MainRead.error().GetMessage();
			if (MainRead) Main = std::move(*MainRead);
			auto Companion = Options.Destination; Companion.replace_extension(".dbulk");
			if (std::filesystem::exists(Companion))
			{
				auto BulkRead = FFileHelper::LoadFileToArray(Companion);
				EXPECT_TRUE(BulkRead) << BulkRead.error().GetMessage();
				if (BulkRead) Bulk = std::move(*BulkRead);
			}
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
		Tasks::FCancellationToken Cancellation;
	};
}

TEST_F(FPackagePersistenceTests, DeltaCompleteAndInternalReferencesRoundTripWithoutEngine)
{
	Asset->Reference = NewObject<DObject>(Asset, "Child");
	for (auto Mode : {EPackageSaveMode::Delta, EPackageSaveMode::Complete})
	{
		Options.Mode = Mode;
		auto Result = Package->Save(Options);
		ASSERT_TRUE(Result) << (Result.Error ? Result.Error->ToString() : std::string{});
		EXPECT_EQ(Result.State, EPackageWriteState::Committed);
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
TEST_F(FPackagePersistenceTests, AuthoredDependencyEventsPersistBuildEdgesSeparately)
{
	FPackagePath BuildInput;
	ASSERT_TRUE(FPackagePath::TryCreate("/Persistence/BuildOnly", BuildInput));
	uint32 Calls = 0;
	Asset->DependencyHook = [&](FObjectCookContext& Context) {
		EXPECT_FALSE(Context.bCooking);
		EXPECT_EQ(Context.Package, Path.ToString());
		++Calls;
		Context.Dependencies.push_back({ECookBuildDependencyKind::TransitivePackage, BuildInput.ToString()});
		Context.Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue, "cook-only-value", {}, {std::byte{1}}});
	};
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_EQ(Calls, 1u);
	const auto Linker = Read();
	EXPECT_EQ(Linker.Summary.BuildPackageDependencies, std::vector<FPackagePath>{BuildInput});
	EXPECT_TRUE(Linker.Summary.HardPackageDependencies.empty());
	EXPECT_TRUE(Linker.Summary.SoftPackageDependencies.empty());
	EXPECT_TRUE(Linker.Imports.empty());
}

TEST_F(FPackagePersistenceTests, InvalidAuthoredBuildDependencyPreventsPublication)
{
	Asset->DependencyHook = [](FObjectCookContext& Context) {
		Context.Dependencies.push_back({ECookBuildDependencyKind::DirectPackage, "invalid-package"});
	};
	EXPECT_FALSE(Package->Save(Options));
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
}

TEST_F(FPackagePersistenceTests, PreSaveRunsOnceBeforeBothArchivePasses)
{
	uint32 Calls = 0;
	Asset->SaveHook = [&](FObjectPreSaveContext& Context) {
		EXPECT_FALSE(Context.IsCooking());
		++Calls;
	};
	Asset->CaptureHook = [&](FArchive&) { EXPECT_EQ(Calls, 1u); };
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_EQ(Calls, 1u);
}

TEST_F(FPackagePersistenceTests, PreSaveFailureDoesNotPublishAndGraphGrowthIsRejected)
{
	Asset->SaveHook = [](FObjectPreSaveContext& Context) {
		Context.Result = {EObjectCookError::InvalidInput, "save hook rejected"};
	};
	EXPECT_FALSE(Package->Save(Options));
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	Asset->SaveHook = [&](FObjectPreSaveContext&) { NewObject<DObject>(Asset, "UnexpectedSaveChild"); };
	EXPECT_FALSE(Package->Save(Options));
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
}

TEST_F(FPackagePersistenceTests, AsyncCompletionMatchesSynchronousBytes)
{
	Asset->Value = 42;
	ASSERT_TRUE(Package->Save(Options));
	FByteBuffer Before, After;
	auto BeforeRead = FFileHelper::LoadFileToArray(Options.Destination);
	ASSERT_TRUE(BeforeRead) << BeforeRead.error().GetMessage();
	Before = std::move(*BeforeRead);
	Package->MarkDirty();
	auto Operation = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Operation.IsValid());
	EXPECT_FALSE(Operation.IsCompleted());
	ASSERT_TRUE(FinishSave(Operation));
	EXPECT_TRUE(Operation.IsCompleted());
	EXPECT_TRUE(Operation.GetResult());
	auto AfterRead = FFileHelper::LoadFileToArray(Options.Destination);
	ASSERT_TRUE(AfterRead) << AfterRead.error().GetMessage();
	After = std::move(*AfterRead);
	EXPECT_EQ(Before, After);
	EXPECT_FALSE(Package->IsDirty());
}
TEST_F(FPackagePersistenceTests, ProtectedTaskCompletesOnHostPumpAndRejectsGameThreadWait)
{
	auto Task = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Task.IsValid());
	EXPECT_EQ(Task.GetDiagnostics().Target, ETaskTarget::GameThreadDeferred);
	EXPECT_EQ(Task.WaitDetailed().WaitStatus, ETaskWaitStatus::UnsupportedThread);
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(Task.IsCompleted());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	// File I/O retirement can precede worker terminal publication. The owner
	// drain pumps until the protected save's publication is complete.
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	ASSERT_TRUE(Task.IsCompleted());
	ASSERT_EQ(Task.GetState(), ETaskState::Succeeded);
	EXPECT_TRUE(Task.GetResult());
	EXPECT_FALSE(Package->IsDirty());
}

TEST_F(FPackagePersistenceTests, AsyncCapacityIncludesPublication)
{
	FPackageWriteResult Admission;
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 1);
	{
		auto Rejected = FSavePackageContext{Options}.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	{
		const auto Failure = Admission;
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::Busy));
	}
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 256ull * 1024 * 1024);
	auto Task = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Task.IsValid());
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	{
		auto Rejected = FSavePackageContext{Options}.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	{
		const auto Failure = Admission;
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::Busy));
	}
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_TRUE(Task.GetResult());
	auto Next = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Next.IsValid());
	EXPECT_TRUE(FinishSave(Next));
}

TEST_F(FPackagePersistenceTests, MissingDeferredExecutorRejectsBeforeWriting)
{
	ShutdownTaskSystem(ETaskShutdownMode::Drain);
	ASSERT_TRUE(InitializeTaskScheduler(2));
	FPackageWriteResult Admission;
	{
		auto Rejected = FSavePackageContext{Options}.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	{
		const auto Failure = Admission;
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::ShuttingDown));
	}
	EXPECT_TRUE(std::filesystem::is_empty(Root));
}

TEST_F(FPackagePersistenceTests, RejectedPublicationDoesNotWaitForOccupiedIoWorkers)
{
	const auto WorkerCount = GetTaskSchedulerDiagnostics().BlockingIOWorkerCount;
	ASSERT_GT(WorkerCount, 0u);
	FThreadEvent Started, Release, Returned;
	std::atomic<uint32> Entered = 0;
	std::vector<Tasks::TTask<void>> Blockers;
	struct FRelease {
		FThreadEvent& Event;
		std::vector<Tasks::TTask<void>>& Tasks;
		~FRelease() { Event.Trigger(); for (const auto& Task : Tasks) Task.Wait(); }
	} Resume{Release, Blockers};
	for (uint32 Index = 0; Index < WorkerCount; ++Index)
		Blockers.push_back(Tasks::LaunchBlockingIO(DURIN_SOURCE_LOCATION, [&] {
			if (Entered.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
			Release.Wait();
		}));
	ASSERT_TRUE(Started.WaitFor(5));
	PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::RejectAdmission);
	bool ReturnedWithoutIoProgress = false;
	std::jthread Unblock([&] { ReturnedWithoutIoProgress = Returned.WaitFor(5); Release.Trigger(); });
	bool Wrote = false, Finished = false;
	const auto Rejected = PackageSavePrivate::SubmitTypedAsyncSave<void>(1, [&] { Wrote = true; }, [&](bool) { Finished = true; },
		[&] { ADD_FAILURE() << "Rejected work must not invoke abandonment."; });
	Returned.Trigger();
	Unblock.join();
	EXPECT_TRUE(ReturnedWithoutIoProgress);
	EXPECT_FALSE(Rejected);
	EXPECT_FALSE(Wrote);
	EXPECT_FALSE(Finished);
	EXPECT_FALSE(DPackage::HasAsyncFileWrites());
}

TEST_F(FPackagePersistenceTests, RejectedPublicationCannotStartDiskWorkAndReleasesCapacity)
{
	PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::RejectAdmission);
	bool bWrote = false, bFinished = false;
	const auto Rejected = PackageSavePrivate::SubmitTypedAsyncSave<void>(1, [&] { bWrote = true; }, [&](bool) { bFinished = true; },
		[&] { ADD_FAILURE() << "Rejected work must not invoke abandonment."; });
	EXPECT_FALSE(Rejected);
	EXPECT_FALSE(bWrote);
	EXPECT_FALSE(bFinished);
	EXPECT_FALSE(DPackage::HasAsyncFileWrites());
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 256ull * 1024 * 1024);
	FPackageWriteResult Admission;
	{
		auto Rejected = FSavePackageContext{Options}.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	EXPECT_TRUE(std::filesystem::is_empty(Root));
	PackageSavePrivate::SetAsyncSavePublicationFaultForTests(PackageSavePrivate::EPublicationFault::None);
	auto Task = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Task.IsValid());
	EXPECT_TRUE(FinishSave(Task));
}

TEST_F(FPackagePersistenceTests, CanceledPublicationReturnsDomainCancellationBeforeTaskShutdown)
{
	Options.IsCancellationRequested = [this] { return Cancellation.IsCanceled(); };
	auto Task = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Task.IsValid());
	Cancellation.Cancel();
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(Task.IsCompleted());
	PackageSavePrivate::SetAsyncSaveAdmission(false);
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_FALSE(Task.IsCompleted());
	{
		const auto Failure = FinishSave(Task);
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::Cancelled));
	}
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	ShutdownTaskSystem(ETaskShutdownMode::Drain);
}

TEST_F(FPackagePersistenceTests, ExternalBulkRoundTripsAndObsoleteCompanionIsRemoved)
{
	Options.Mode = EPackageSaveMode::Complete;
	Asset->Payload = FSharedByteBuffer::Take(FByteBuffer(300 * 1024, std::byte{0x5a}));
	auto Result = Package->Save(Options);
	ASSERT_TRUE(Result) << (Result.Error ? Result.Error->ToString() : std::string{});
	auto Linker = Read();
	ASSERT_EQ(Linker.Exports.size(), 1u);
	EXPECT_TRUE(std::ranges::any_of(Linker.Exports.front().Properties, [](const auto& Tag) {
		return Tag.Value.Bytes.size() == 300 * 1024 && Tag.Value.Bytes.front() == std::byte{0x5a};
	}));
	Asset->Payload = {}; Package->MarkDirty();
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_FALSE(std::filesystem::exists(Root / "Package.dbulk"));
}
TEST_F(FPackagePersistenceTests, AsyncSavePublishesCapturedBytesAndCancellationPreservesDirtyState)
{
	Testing::FScopedPackageNotificationCount Saved(OnPackageSaved(), Package);
	Asset->Value = 33;
	auto Operation = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Operation.IsValid());
	Asset->Value = 55;
	Package->MarkDirty();
	ASSERT_TRUE(FinishSave(Operation));
	ASSERT_FALSE(Package->IsDirty());
	EXPECT_EQ(Saved.Count, 1);
	auto Linker = Read();
	ASSERT_FALSE(Linker.Exports.empty());
	EXPECT_TRUE(std::ranges::any_of(Linker.Exports.front().Properties, [](const auto& Tag) { return Tag.Value.Signed == 33; }));
	EXPECT_EQ(Asset->Value, 55);
	Package->MarkDirty();
	Options.IsCancellationRequested = [this] { return Cancellation.IsCanceled(); };
	Operation = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Operation.IsValid());
	Cancellation.Cancel();
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Operation.GetState(), ETaskState::Succeeded);
	{
		const auto Failure = Operation.GetResult();
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::Cancelled));
	}
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Saved.Count, 1);
	// A new request gets a new token; cancellation cannot be reset or copied.
	Tasks::FCancellationToken RetryCancellation;
	Options.IsCancellationRequested = [&RetryCancellation] { return RetryCancellation.IsCanceled(); };
	Operation = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Operation.IsValid());
	Operation = {};
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Saved.Count, 2);
	EXPECT_TRUE(std::filesystem::exists(Options.Destination));
}
TEST_F(FPackagePersistenceTests, CompetingWriterAndStagedRollbackPreservePreviousContent)
{
	ASSERT_TRUE(Package->Save(Options));
	std::optional<UnifiedError::FError> Admission;
	auto Older = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Older.IsValid());
	Asset->Value = 33; Package->MarkDirty();
	ASSERT_TRUE(Package->Save(Options));
	{
		const auto Failure = FinishSave(Older);
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageWriteErrors::StaleData));
	}
	Asset->Value = 55; Package->MarkDirty();
	auto Staged = FPackageSaveOperation::Begin(Package, Options, &Admission, false);
	ASSERT_TRUE(Staged);
	ASSERT_TRUE(Staged->CommitStaged());
	{
		const auto Failure = Staged->RollbackCommit();
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::Cancelled));
	}
	EXPECT_TRUE(Package->IsDirty());
	auto Linker = Read();
	EXPECT_TRUE(std::ranges::any_of(Linker.Exports.front().Properties, [](const auto& Tag) { return Tag.Value.Signed == 33; }));
}
TEST_F(FPackagePersistenceTests, ResolverAndAdmissionFailureAreExplicit)
{
	{
		const auto Failure = Package->Save();
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::InvalidPath));
	}
	SetPackageDestinationResolver([&](const DPackage&) { return Options.Destination; });
	ASSERT_TRUE(Package->Save());
	(void)DPackage::DrainAsyncSaves();
	ShutdownTaskSystem(ETaskShutdownMode::Drain);
	FPackageWriteResult Admission;
	{
		auto Rejected = FSavePackageContext{}.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	{
		const auto Failure = Admission;
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::ShuttingDown));
	}
	EXPECT_TRUE(Package->Save()) << "Synchronous persistence must not require a scheduler.";
}
TEST_F(FPackagePersistenceTests, ChangedStageAndDestinationConflictDoNotClearDirty)
{
	std::optional<UnifiedError::FError> Admission;
	auto Staged = FPackageSaveOperation::Begin(Package, Options, &Admission, false);
	ASSERT_TRUE(Staged) << (Admission ? Admission->ToString() : std::string{});
	bool Corrupted = false;
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		if (Entry.path().filename().string().find(".package-save-") != std::string::npos)
		{
			EXPECT_TRUE(Entry.path().filename().string().ends_with(".stage.tmp"));
			FByteBuffer Bytes;
			auto BytesRead = FFileHelper::LoadFileToArray(Entry.path());
			ASSERT_TRUE(BytesRead) << BytesRead.error().GetMessage();
			Bytes = std::move(*BytesRead);
			ASSERT_FALSE(Bytes.empty());
			Bytes.back() ^= std::byte{1};
			const auto OriginalTime = std::filesystem::last_write_time(Entry.path());
			ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bytes, Entry.path()));
			std::filesystem::last_write_time(Entry.path(), OriginalTime + std::chrono::seconds(10));
			Corrupted = true;
		}
	ASSERT_TRUE(Corrupted);
	{
		const auto Failure = Staged->Complete();
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageWriteErrors::CorruptFile));
	}
	EXPECT_TRUE(Package->IsDirty());
	Staged = FPackageSaveOperation::Begin(Package, Options, &Admission, false);
	ASSERT_TRUE(Staged);
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(FByteBuffer{std::byte{1}}, Options.Destination));
	{
		const auto Failure = Staged->Complete();
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageWriteErrors::StaleData));
	}
	FByteBuffer Current;
	auto CurrentRead = FFileHelper::LoadFileToArray(Options.Destination);
	ASSERT_TRUE(CurrentRead) << CurrentRead.error().GetMessage();
	Current = std::move(*CurrentRead);
	EXPECT_EQ(Current, FByteBuffer{std::byte{1}});
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
	auto Writer = FPackageFileAccess::TryAcquire(Paths, true);
	ASSERT_TRUE(Writer);
	EXPECT_FALSE(FPackageFileAccess::TryAcquire(Paths, false));
	EXPECT_FALSE(Protected->Commit());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{2}}, Main));
	Writer.reset();
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(Paths, false));
	{
		const auto Failure = Protected->Commit();
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageWriteErrors::StaleData));
	}
	// The failed operation may remain observable without blocking a fresh save.
	EXPECT_TRUE(FPackageFileAccess::TryAcquire(Paths, true));
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
		auto CurrentRead = FFileHelper::LoadFileToArray(Destination);
		ASSERT_TRUE(CurrentRead) << CurrentRead.error().GetMessage();
		Current = std::move(*CurrentRead);
		if (Change >= 3)
		{
			ASSERT_TRUE(Result);
			EXPECT_EQ(Current, Bytes);
			ASSERT_TRUE(Write->Finalize());
		}
		else
		{
			{
				const auto Failure = Result;
				EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageWriteErrors::CorruptFile));
			}
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
	auto Operation = FSavePackageContext{Options}.SaveAsync(Package);
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
	std::optional<UnifiedError::FError> Admission;
	auto Operation = FPackageSaveOperation::Begin(Package, Options, &Admission, false);
	ASSERT_TRUE(Operation);
	ASSERT_TRUE(Operation->CommitStaged());
	ASSERT_TRUE(std::filesystem::remove(Options.Destination));
	ASSERT_TRUE(std::filesystem::create_directory(Options.Destination));
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(FByteBuffer{std::byte{1}}, Options.Destination / "Blocker"));
	const auto Result = Operation->RollbackCommit();
	EXPECT_EQ(Result.State, EPackageWriteState::RecoveryRequired);
	ASSERT_FALSE(Result.RecoveryFiles.empty());
	EXPECT_TRUE(std::filesystem::exists(Result.RecoveryFiles.front()));
	EXPECT_EQ(Result.RecoveryFiles.front().extension(), ".tmp");
	{
		const auto Failure = Result;
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(UnifiedError::FErrorId{"File", "IoError"}));
	}
	EXPECT_TRUE(Package->IsDirty());
	bool HasBackup = false;
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		HasBackup |= Entry.path().filename().string().ends_with(".backup.tmp");
	EXPECT_TRUE(HasBackup);
}
TEST_F(FPackagePersistenceTests, FilteredSnapshotsPreserveAuthoredDirtyState)
{
	Testing::FScopedPackageNotificationCount Saved(OnPackageSaved(), Package);
	Options.Capture.PropertyFilter = [](const DObject*, const FProperty*) { return true; };
	ASSERT_TRUE(Package->Save(Options));
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Saved.Count, 0);
}
TEST_F(FPackagePersistenceTests, ShutdownDuringResolverIsAnAdmissionFailure)
{
	SetPackageDestinationResolver([&](const DPackage&) {
		(void)DPackage::DrainAsyncSaves();
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		return Options.Destination;
	});
	FPackageWriteResult Admission;
	{
		auto Rejected = FSavePackageContext{}.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	{
		const auto Failure = Admission;
		EXPECT_TRUE(Failure.Error && Failure.Error->Is(PackageSaveErrors::ShuttingDown));
	}
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
		std::optional<UnifiedError::FError> Error;
		EXPECT_FALSE(Write->Stage(&Error));
		ASSERT_TRUE(Error);
		EXPECT_TRUE(Error->Is({"File", "IoError"}));
		EXPECT_NE(Error->GetMessage().find(Stage.generic_string()), std::string::npos);
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
		std::optional<UnifiedError::FError> Admission;
		auto Save = FPackageSaveOperation::Begin(Package, Context, &Admission);
		ASSERT_FALSE(Admission);
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
	std::optional<UnifiedError::FError> Admission;
	auto Save = FPackageSaveOperation::Begin(Package, Options, &Admission, false);
	ASSERT_FALSE(Admission);
	ASSERT_TRUE(Save->CommitStaged());
	std::filesystem::path Backup;
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		if (Entry.path().filename().string().ends_with(".backup.tmp")) Backup = Entry.path();
	ASSERT_FALSE(Backup.empty());
	// A nonempty directory deterministically makes backup deletion fail.
	ASSERT_TRUE(std::filesystem::remove(Backup));
	ASSERT_TRUE(std::filesystem::create_directory(Backup));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer(1), Backup / "Blocker"));
	const auto Result = Save->FinalizeCommit();
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.State, EPackageWriteState::Committed);
	ASSERT_EQ(Result.RecoveryFiles.size(), 1);
	EXPECT_EQ(Result.RecoveryFiles.front(), Backup);
	EXPECT_TRUE(std::filesystem::exists(Options.Destination));
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Save->Complete().State, EPackageWriteState::Committed);
}

TEST_F(FPackagePersistenceTests, CaptureValidationPreservesOutputAndOverrideIdentity)
{
	FSavePackageContext Context;
	ObjectPackage::FLinkerTables Linker;
	Linker.Names.push_back("sentinel");
	const auto Missing = Context.Capture(nullptr, Linker);
	EXPECT_EQ(Missing.error().Reason, EPackageCaptureReason::MissingAssets);
	Context.Options.Capture.bCooking = true;
	const auto Target = Context.Capture(Package, Linker);
	EXPECT_EQ(Target.error().Reason, EPackageCaptureReason::CookTarget);
	Context.Options.Capture.bCooking = false;
	auto Overrides = std::make_shared<FObjectSaveOverrides>();
	ASSERT_TRUE(Overrides->AddObjectOmission(*Asset));
	Context.Options.Capture.SaveOverrides = Overrides;
	const auto Omitted = Context.Capture(Package, Linker);
	EXPECT_EQ(Omitted.error().Reason, EPackageCaptureReason::OmittedAsset);
	EXPECT_EQ(Omitted.error().ObjectPath, Asset->GetObjectPath());
	Overrides.reset();
	Context.Options.Capture.SaveOverrides.reset();
	EXPECT_EQ(Omitted.error().ObjectPath, Asset->GetObjectPath());
	EXPECT_EQ(Linker.Names, (std::vector<std::string>{"sentinel"}));
}

TEST_F(FPackagePersistenceTests, CaptureRejectionTaskRetainsArchiveContext)
{
	Asset->CaptureHook = [](FArchive& Ar) {
		uint32 Value = 12;
		Ar.SerializeRawBytes(std::as_writable_bytes(std::span{&Value, 1}));
	};
	FSavePackageContext Context{Options};
	ObjectPackage::FLinkerTables Linker;
	Linker.Names.push_back("sentinel");
	const auto Result = Context.Capture(Package, Linker);
	EXPECT_EQ(Result.error().Reason, EPackageCaptureReason::RawOutsideField);
	EXPECT_EQ(Result.error().ArchiveCode, EArchiveFailureCode::MalformedSerializer);
	EXPECT_EQ(Result.error().ObjectPath, Asset->GetObjectPath());
	EXPECT_FALSE(Result.error().ArchivePath.empty());
	EXPECT_EQ(Linker.Names, (std::vector<std::string>{"sentinel"}));
	FPackageWriteResult Admission;
	{
		auto Rejected = Context.SaveAsync(Package);
		ASSERT_TRUE(Rejected.IsValid());
		ASSERT_TRUE(Rejected.IsCompleted());
		Admission = Rejected.GetResult();
	}
	ASSERT_TRUE(Admission.Error);
	EXPECT_TRUE(Admission.Error->Is({"PackageCapture", "UnsupportedProperty"}));
	EXPECT_TRUE(std::ranges::any_of(Admission.Error->GetContexts(), [&](const auto& Entry) {
		return Entry.Key == "object" && Entry.Value == Asset->GetObjectPath();
	}));
	EXPECT_EQ(Admission.State, EPackageWriteState::NotCommitted);
}

TEST_F(FPackagePersistenceTests, CaptureRejectsPropertyFailure)
{
	Asset->CaptureHook = [](FArchive& Ar) {
		auto* ObjectArchive = RequireObjectArchive(Ar);
		ASSERT_NE(ObjectArchive, nullptr);
		ObjectArchive->FailPropertyValue({.Code = EPropertyValueError::UnavailableOperation,
			.Operation = EPropertyValueOperation::CopyConstruct, .PropertyName = "OwnedProperty"});
	};
	ObjectPackage::FLinkerTables Linker;
	const auto Result = FSavePackageContext{Options}.Capture(Package, Linker);
	EXPECT_EQ(Result.error().Reason, EPackageCaptureReason::ArchiveFailure);
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
	EXPECT_EQ(Bulk.error().Reason, EPackageCaptureReason::BulkMetadata);
	EXPECT_EQ(Bulk.error().Actual, 0u);
	EXPECT_EQ(Bulk.error().Expected, 9u);
	Asset->CaptureHook = [](FArchive& Ar) {
		if (Ar.GetPurpose() != EArchivePurpose::AuthoredPackage) return;
		auto* ObjectArchive = RequireObjectArchive(Ar);
		ASSERT_NE(ObjectArchive, nullptr);
		auto Scope = ObjectArchive->EnterField({.DeclaringType = FName("DPersistedObject"),
			.Name = FName("LateField"), .LogicalType = FArchiveLogicalTypeDescriptor::Scalar(true, 32, false)});
		int32 Value = 2; Ar << Value;
	};
	const auto Manifest = FSavePackageContext{Options}.Capture(Package, Linker);
	EXPECT_EQ(Manifest.error().Reason, EPackageCaptureReason::EmissionMutation);
}

TEST_F(FPackagePersistenceTests, CapturePreservesOutputOnDefaultDeltaFailure)
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
	EXPECT_EQ(Result.error().Reason, EPackageCaptureReason::DefaultDelta);
	EXPECT_EQ(Linker.Names, (std::vector<std::string>{"sentinel"}));
}

TEST_F(FPackagePersistenceTests, DrainPumpsTypedPublicationAndRejectsReentrantSaves)
{
	bool UnrelatedRan = false;
	auto Unrelated = Tasks::LaunchGameThreadDeferred(DURIN_SOURCE_LOCATION, [&] {
		UnrelatedRan = true;
		{
			std::optional<UnifiedError::FError> Error;
			EXPECT_FALSE(DPackage::DrainAsyncSaves(&Error));
			EXPECT_TRUE(Error && Error->Is(PackageSaveErrors::Busy));
		}
		{
			const auto Failure = PackageSavePrivate::CheckAsyncAdmission();
			EXPECT_TRUE(!Failure && Failure.error().Is(PackageSaveErrors::Busy));
		}
	});
	uint32 Destroyed = 0, Calls = 0;
	std::weak_ptr<int> Weak, IoWeak;
	std::atomic<bool> IoDestroyed = false;
	FThreadEvent ReleaseIo;
	Tasks::TTask<int> Task;
	{
		auto Owner = std::shared_ptr<int>(new int(1), [&](int* Value) {
			EXPECT_TRUE(IsInGameThread());
			EXPECT_TRUE(Task.IsValid());
			EXPECT_FALSE(Task.IsCompleted());
			++Destroyed; delete Value;
		});
		Weak = Owner;
		auto IoPayload = std::shared_ptr<int>(new int(2), [&](int* Value) {
			EXPECT_FALSE(IsInGameThread()); IoDestroyed = true; delete Value;
		});
		IoWeak = IoPayload;
		auto Submitted = PackageSavePrivate::SubmitTypedAsyncSave<int>(1, [Owner, IoPayload, &ReleaseIo] { ReleaseIo.Wait(); },
			[&, Owner](bool Success) {
				EXPECT_TRUE(Success); EXPECT_TRUE(IsInGameThread()); ++Calls;
				{
					std::optional<UnifiedError::FError> Error;
					EXPECT_FALSE(DPackage::DrainAsyncSaves(&Error));
					EXPECT_TRUE(Error && Error->Is(PackageSaveErrors::Busy));
				}
				{
					const auto Failure = PackageSavePrivate::CheckAsyncAdmission();
					EXPECT_TRUE(!Failure && Failure.error().Is(PackageSaveErrors::Busy));
				}
				return 37;
			}, [Owner] { ADD_FAILURE() << "Successful publication must not invoke abandonment."; });
		ASSERT_TRUE(Submitted);
		Task = std::move(*Submitted);
	}
	ReleaseIo.Trigger();
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_TRUE(IoDestroyed.load());
	EXPECT_TRUE(IoWeak.expired());
	EXPECT_FALSE(Weak.expired());
	EXPECT_FALSE(Task.IsCompleted());
	EXPECT_EQ(Task.GetDiagnostics().Target, ETaskTarget::GameThreadDeferred);
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_TRUE(UnrelatedRan);
	EXPECT_TRUE(Unrelated.IsCompleted());
	EXPECT_EQ(Task.GetResult(), 37);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Destroyed, 1u);
	EXPECT_TRUE(PackageSavePrivate::CheckAsyncAdmission());
}

TEST_F(FPackagePersistenceTests, PublicationExceptionsRetireCapturesAndReleaseCapacity)
{
	PackageSavePrivate::SetAsyncSaveLimitsForTests(1, 1024);
	uint32 Calls = 0, Abandoned = 0, Destroyed = 0;
	Tasks::TTask<int> Task;
	{
		auto Owner = std::shared_ptr<int>(new int(1), [&](int* Value) {
			EXPECT_TRUE(IsInGameThread());
			EXPECT_TRUE(Task.IsValid());
			EXPECT_FALSE(Task.IsCompleted());
			++Destroyed; delete Value;
		});
		auto Submitted = PackageSavePrivate::SubmitTypedAsyncSave<int>(1, [Owner] {},
			[&, Owner](bool) -> int { ++Calls; throw std::runtime_error("publication failure"); },
			[&, Owner] {
				EXPECT_TRUE(IsInGameThread()); ++Abandoned;
				throw std::runtime_error("abandonment failure");
			});
		ASSERT_TRUE(Submitted);
		Task = std::move(*Submitted);
	}
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Task.GetState(), ETaskState::Failed);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Abandoned, 1u);
	EXPECT_EQ(Destroyed, 1u);
	EXPECT_NE(Task.GetDiagnostic().find("publication failure"), std::string::npos);
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_EQ(Abandoned, 1u);
	EXPECT_TRUE(PackageSavePrivate::CheckAsyncAdmission(1));
}

TEST_F(FPackagePersistenceTests, FailedIoStillRunsTypedPublication)
{
	uint32 Calls = 0;
	auto Submitted = PackageSavePrivate::SubmitTypedAsyncSave<int>(1, [] { throw std::runtime_error("I/O failure"); },
		[&](bool Success) { EXPECT_FALSE(Success); ++Calls; return -1; },
		[] { ADD_FAILURE() << "A handled I/O failure is still a completed publication."; });
	ASSERT_TRUE(Submitted);
	auto Task = std::move(*Submitted);
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Task.GetState(), ETaskState::Succeeded);
	EXPECT_EQ(Task.GetResult(), -1);
	EXPECT_EQ(Calls, 1u);
}

TEST_F(FPackagePersistenceTests, FailedPublicationRetiresOwnerOnGameThread)
{
	Tasks::FTask Publication;
	uint32 Invoked = 0, Cleaned = 0, Destroyed = 0;
	{
		auto Owner = std::shared_ptr<int>(new int(1), [&](int* Value) {
			EXPECT_TRUE(IsInGameThread()); ++Destroyed; delete Value;
		});
		auto Admission = PackageSavePrivate::SubmitAsyncSaveTask(1, [Owner] {},
			[&](const Tasks::FTask& Disk, const std::shared_ptr<PackageSavePrivate::FAsyncSaveState>&) {
				Publication = Tasks::LaunchGameThreadDeferred(DURIN_SOURCE_LOCATION, [&] {
					EXPECT_TRUE(IsInGameThread());
					++Invoked;
					throw std::runtime_error("save publication failure");
				}, Disk);
				return Publication;
			}, [&, Owner] { EXPECT_TRUE(IsInGameThread()); ++Cleaned; });
		ASSERT_TRUE(Admission);
	}
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Publication.GetState(), ETaskState::Failed);
	EXPECT_EQ(Publication.GetDiagnostics().TerminalReason, ETaskTerminalReason::CallbackFailure);
	EXPECT_NE(Publication.GetDiagnostic().find("save publication failure"), std::string::npos);
	EXPECT_EQ(Invoked, 1u);
	EXPECT_EQ(Cleaned, 1u);
	EXPECT_EQ(Destroyed, 1u);
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_EQ(Cleaned, 1u);
	EXPECT_EQ(Destroyed, 1u);
}

TEST_F(FPackagePersistenceTests, TerminalPublicationDoesNotRetireRunningProducer)
{
	FThreadEvent Started, Release;
	// Even an already-terminal publication is not proof that its producer exited.
	Tasks::FTaskEvent Publication("Save.TestPublication");
	Publication.Trigger();
	uint32 Cleaned = 0;
	auto Admission = PackageSavePrivate::SubmitAsyncSaveTask(1,
		[&] { Started.Trigger(); Release.Wait(); },
		[&](const Tasks::FTask&, const std::shared_ptr<PackageSavePrivate::FAsyncSaveState>&) { return Tasks::FTask(Publication); },
		[&] { EXPECT_TRUE(IsInGameThread()); ++Cleaned; });
	ASSERT_TRUE(Admission);
	const bool bStarted = Started.WaitFor(5);
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_EQ(Cleaned, 0u);
	Release.Trigger();
	EXPECT_TRUE(bStarted);
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Cleaned, 1u);
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_EQ(Cleaned, 1u);
}

TEST_F(FPackagePersistenceTests, DrainFromDeferredCallbackReturnsBusyWithoutStealingPublication)
{
	std::optional<UnifiedError::FError> NestedError;
	bool Nested = true;
	auto Callback = Tasks::LaunchGameThreadDeferred(DURIN_SOURCE_LOCATION, [&] { Nested = DPackage::DrainAsyncSaves(&NestedError); });
	FPackageWriteResult Admission;
	auto Save = FSavePackageContext{Options}.SaveAsync(Package);
	ASSERT_TRUE(Admission);
	PumpGameThreadDeferredWork({.MaxCallbacks = 1});
	EXPECT_TRUE(Callback.IsCompleted());
	{
		EXPECT_FALSE(Nested);
		EXPECT_TRUE(NestedError && NestedError->Is(PackageSaveErrors::Busy));
	}
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_TRUE(Save.GetResult());
}

TEST_F(FPackagePersistenceTests, BusinessSkippedPublicationCleanupMayPumpOtherAcceptedPublications)
{
	Tasks::FTask Publication;
	Tasks::TTask<int> Other;
	uint64 CurrentSerial = 1;
	const uint64 SubmittedSerial = CurrentSerial;
	uint32 Applied = 0, Skipped = 0, Cleaned = 0;
	auto Admission = PackageSavePrivate::SubmitAsyncSaveTask(1, [] {},
		[&](const Tasks::FTask& Disk, const std::shared_ptr<PackageSavePrivate::FAsyncSaveState>&) {
			Publication = Tasks::LaunchGameThreadDeferred(DURIN_SOURCE_LOCATION, [&, SubmittedSerial] {
				EXPECT_TRUE(IsInGameThread());
				if (SubmittedSerial != CurrentSerial) { ++Skipped; return; }
				++Applied;
			}, Disk);
			return Publication;
		}, [&] {
			EXPECT_TRUE(IsInGameThread());
			while (!Other.IsCompleted()) PumpGameThreadDeferredWork({.MaxCallbacks = 1});
			{
				const auto Failure = PackageSavePrivate::CheckAsyncAdmission();
				EXPECT_TRUE(!Failure && Failure.error().Is(PackageSaveErrors::Busy));
			}
			++Cleaned;
		});
	ASSERT_TRUE(Admission);
	++CurrentSerial;
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!Publication.IsCompleted() && std::chrono::steady_clock::now() < Deadline)
		PumpGameThreadDeferredWork({.MaxCallbacks = 1});
	ASSERT_TRUE(Publication.IsCompleted());
	EXPECT_EQ(Publication.GetState(), ETaskState::Succeeded);
	EXPECT_EQ(Applied, 0u);
	EXPECT_EQ(Skipped, 1u);
	auto OtherSubmitted = PackageSavePrivate::SubmitTypedAsyncSave<int>(1, [] {},
		[](bool Success) { EXPECT_TRUE(Success); return 41; },
		[] { ADD_FAILURE(); });
	ASSERT_TRUE(OtherSubmitted);
	Other = std::move(*OtherSubmitted);
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_EQ(Cleaned, 1u);
	EXPECT_EQ(Other.GetResult(), 41);
	EXPECT_TRUE(PackageSavePrivate::CheckAsyncAdmission());
	PackageSavePrivate::PollAsyncSaves();
	EXPECT_EQ(Cleaned, 1u);
}

TEST_F(FPackagePersistenceTests, CustomWriterFailureRetainsIdentityThroughAsyncCompletion)
{
	struct FRejectedWriter final : IPackageWriter
	{
		auto Begin(std::vector<FPackageWriteFile>) -> std::unique_ptr<IPackageWriteOperation> override
		{
			struct FRejectedOperation final : IPackageWriteOperation
			{
				auto Stage(std::optional<UnifiedError::FError>* OutError) -> bool override
				{ if (OutError) OutError->emplace(UnifiedError::FErrorId{"TestWriter", "Rejected"}, "Backend refused staging."); return false; }
				auto Commit() -> FPackageWriteResult override { return {.Error = UnifiedError::FError{{"TestWriter", "UnexpectedCommit"}}}; }
				auto Rollback() -> FPackageWriteResult override { return {}; }
				auto Finalize() -> FPackageWriteResult override { return {}; }
			};
			return std::make_unique<FRejectedOperation>();
		}
	};
	auto Save = FSavePackageContext{Options, std::make_shared<FRejectedWriter>()}.SaveAsync(Package);
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	ASSERT_TRUE(Save.IsCompleted());
	const auto& Result = Save.GetResult();
	ASSERT_TRUE(Result.Error);
	EXPECT_TRUE(Result.Error->Is({"TestWriter", "Rejected"}));
	EXPECT_EQ(Result.State, EPackageWriteState::NotCommitted);
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
	EXPECT_TRUE(Package->IsDirty());
}

TEST_F(FPackagePersistenceTests, RejectedStageWithoutDiagnosticNeverPublishes)
{
	struct FRejectedWriter final : IPackageWriter
	{
		bool bCommitted = false;
		auto Begin(std::vector<FPackageWriteFile>) -> std::unique_ptr<IPackageWriteOperation> override
		{
			struct FOperation final : IPackageWriteOperation
			{
				bool& bCommitted;
				explicit FOperation(bool& Committed) : bCommitted(Committed) {}
				auto Stage(std::optional<UnifiedError::FError>*) -> bool override { return false; }
				auto Commit() -> FPackageWriteResult override { bCommitted = true; return {}; }
				auto Rollback() -> FPackageWriteResult override { return {}; }
				auto Finalize() -> FPackageWriteResult override { return {}; }
			};
			return std::make_unique<FOperation>(bCommitted);
		}
	};
	auto Writer = std::make_shared<FRejectedWriter>();
	auto Save = FSavePackageContext{Options, Writer}.SaveAsync(Package);
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	ASSERT_TRUE(Save.IsCompleted());
	const auto& Result = Save.GetResult();
	EXPECT_FALSE(Result);
	ASSERT_TRUE(Result.Error);
	EXPECT_TRUE(Result.Error->Is(PackageSaveErrors::IoError));
	EXPECT_FALSE(Writer->bCommitted);
	EXPECT_EQ(Result.State, EPackageWriteState::NotCommitted);
	EXPECT_FALSE(std::filesystem::exists(Options.Destination));
}

TEST(FPackageWriteErrorTests, FileErrorConversionOwnsContextAndNativeCause)
{
	const auto Error = [] {
		FFileSystemError Native{"Cannot read input.dasset (input.dbulk), offset 17, size 29.",
			std::make_error_code(std::errc::permission_denied)};
		return ToError(Native);
	}();
	EXPECT_TRUE(Error.Is({"File", "IoError"}));
	for (const char* Text : {"input.dasset", "input.dbulk", "offset 17", "size 29"})
		EXPECT_NE(Error.GetMessage().find(Text), std::string::npos);
	EXPECT_NE(Error.GetMessage().find(std::make_error_code(std::errc::permission_denied).message()), std::string::npos);
}

TEST_F(FPackagePersistenceTests, FailedReplacementLeavesRollbackToClosureOwner)
{
	const auto Destination = Root / "original.bin";
	const auto MissingStage = Root / "missing.stage";
	const auto Backup = Root / "original.backup";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{7}}, Destination));
	FFileReplacement Replacement{Destination, MissingStage, Backup};
	std::optional<FFileSystemError> Error;
	ASSERT_FALSE(Replacement.Publish(&Error));
	ASSERT_TRUE(Error);
	EXPECT_NE(Error->GetMessage().find("rename"), std::string::npos);
	EXPECT_NE(Error->GetMessage().find(MissingStage.generic_string()), std::string::npos);
	EXPECT_NE(Error->GetMessage().find(Destination.generic_string()), std::string::npos);
	EXPECT_TRUE(Replacement.bBackedUp);
	EXPECT_FALSE(std::filesystem::exists(Destination));
	EXPECT_TRUE(std::filesystem::exists(Backup));
	ASSERT_TRUE(Replacement.Rollback(&Error));
	EXPECT_FALSE(Error);
	EXPECT_FALSE(Replacement.bBackedUp);
	EXPECT_TRUE(std::filesystem::exists(Destination));
	EXPECT_FALSE(std::filesystem::exists(Backup));
	const auto Restored = FFileHelper::LoadFileToArray(Destination);
	ASSERT_TRUE(Restored);
	EXPECT_EQ(*Restored, (FByteBuffer{std::byte{7}}));
}

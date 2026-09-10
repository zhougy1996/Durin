#include "Threading/TaskComposition.h"
#include <gtest/gtest.h>

#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "Asset/PackageBulkData.h"
#include "Asset/PackageResource.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/ThreadEvent.h"

namespace
{
	using namespace Durin;
	using namespace Durin;

	auto MakeBytes(std::initializer_list<uint8> Values) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Bytes;
		Bytes.reserve(Values.size());
		for (const uint8 Value : Values) Bytes.push_back(static_cast<std::byte>(Value));
		return Bytes;
	}

	class FFailingPackageResource final : public FPackageResource
	{
	public:
		FFailingPackageResource()
			: FPackageResource(4)
		{
		}
		std::atomic_bool bFail = true;

	private:
		auto ReadRangeImpl(uint64, uint64 Size, const std::atomic_bool&) -> FPackageResourceReadResult override
		{
			if (bFail.load()) return {.Status = EPackageResourceReadStatus::SegmentDigestMismatch, .Message = "Injected digest failure."};
			return {.Status = EPackageResourceReadStatus::Success, .Buffer = FSharedByteBuffer::Take(FByteBuffer(Size, std::byte{7}))};
		}
	};

	class FSlowPackageResource final : public FPackageResource
	{
	public:
		FSlowPackageResource() : FPackageResource(4) {}

	private:
		auto ReadRangeImpl(uint64, uint64 Size, const std::atomic_bool& bCancelled)
			-> FPackageResourceReadResult override
		{
			for (uint32 Index = 0; Index < 100; ++Index)
			{
				if (bCancelled.load(std::memory_order_acquire))
					return {.Status = EPackageResourceReadStatus::Cancelled};
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return {.Status = EPackageResourceReadStatus::Success,
				.Buffer = FSharedByteBuffer::Take(Durin::FByteBuffer(Size))};
		}
	};

	class FPackageTaskEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			bOwnsScheduler = !IsTaskSchedulerRunning();
			if (bOwnsScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
		}

		auto TearDown() -> void override
		{
			if (bOwnsScheduler) ShutdownTaskScheduler(true);
		}

	private:
		bool bOwnsScheduler = false;
	};

	// Holds a read open across an unsupported wait; the timeout keeps failure
	// cleanup bounded even if an assertion exits before explicit release.
	class FBlockedPackageResource final : public FPackageResource
	{
	public:
		FBlockedPackageResource() : FPackageResource(4) {}
		FThreadEvent Started;
		FThreadEvent Release;

	private:
		auto ReadRangeImpl(uint64, uint64 Size, const std::atomic_bool&)
			-> FPackageResourceReadResult override
		{
			Started.Trigger();
			if (!Release.WaitFor(2.0)) return {.Status = EPackageResourceReadStatus::IoError};
			return {.Status = EPackageResourceReadStatus::Success,
				.Buffer = FSharedByteBuffer::Take(FByteBuffer(Size, std::byte{0x31}))};
		}
	};

	class FPackageWaitRunnable final : public FRunnable
	{
	public:
		explicit FPackageWaitRunnable(FPackageResourceRequest InRequest)
			: Request(std::move(InRequest)) {}
		auto Run() -> uint32 override
		{
			Result = Request.Wait();
			return 0;
		}
		FPackageResourceRequest Request;
		FPackageResourceReadResult Result;
	};

	[[maybe_unused]] testing::Environment* GPackageTaskEnvironment =
		testing::AddGlobalTestEnvironment(new FPackageTaskEnvironment());
}

TEST(FBulkDataTests, DefaultValueIsEmpty)
{
	FBulkData Value;
	EXPECT_EQ(Value.GetState(), EBulkDataState::Empty);
	EXPECT_FALSE(Value.HasData());
	EXPECT_EQ(Value.GetMetadata().LogicalSize, 0u);
	EXPECT_EQ(Value.AcquireRead().Status, EBulkReadStatus::Empty);
}

TEST(FBulkDataTests, DetachedLocksResizeAndCopyOnWrite)
{
	const Durin::FByteBuffer Bytes = MakeBytes({1, 2, 3, 4});
	FBulkData First;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreateDetached(Bytes, First, &Error)) << Error;
	FBulkData Second = First;
	Durin::FByteView Read;
	Durin::FBulkDataReadResult ReadLease;
	Durin::FMutableByteView Write;
	ReadLease = First.AcquireRead();
	ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
	Read = ReadLease.Lock.GetBytes();
	EXPECT_TRUE(std::ranges::equal(Read, Bytes));
	EXPECT_EQ(First.TryUnload(), EBulkUnloadResult::Busy);
	ReadLease.Lock.Reset();

	auto WriteLease = Second.AcquireWrite();
	ASSERT_TRUE(WriteLease.TryResize(2));
	Write = WriteLease.GetBytes();
	Write[0] = std::byte{9};
	WriteLease.Reset();
	ReadLease = First.AcquireRead();
	ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
	Read = ReadLease.Lock.GetBytes();
	EXPECT_TRUE(std::ranges::equal(Read, Bytes));
	ReadLease.Lock.Reset();
	ReadLease = Second.AcquireRead();
	ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
	Read = ReadLease.Lock.GetBytes();
	EXPECT_EQ(Read.size(), 2u);
	EXPECT_EQ(Read[0], std::byte{9});
	ReadLease.Lock.Reset();
	EXPECT_EQ(Second.TryUnload(), EBulkUnloadResult::NotResident);
}

TEST(FPackageResourceTests, LoadsUnloadsAndRetiresAttachedBulkData)
{
	const uint64 Size = EditorBulkDataExternalThreshold + 1;
	Durin::FByteBuffer Segment(static_cast<size_t>(Size), std::byte{0x6a});
	const FPackageBulkDataEntry Entry{
		.FieldIndex = 1,
		.Placement = EPackageBulkDataPlacement::External,
		.LogicalSize = Size,
		.StoredSize = Size,
		.Alignment = EditorBulkDataExternalAlignment,
		.ContentId = FXxHash128::HashBuffer(Segment)};
	const FPackageBulkSegmentSummary Summary{
		.Extent = Size, .Digest = FXxHash128::HashBuffer(Segment)};
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "PackageResource";
	std::filesystem::create_directories(Root);
	const std::filesystem::path PackagePath = Root / "Range.dasset";
	std::filesystem::path SegmentPath = PackagePath;
	SegmentPath.replace_extension(".dbulk");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Segment, SegmentPath));

	FPackageResourceManager Manager;
	FPackageResourceHandle Handle;
	std::string Error;
	auto Registration1 = Manager.RegisterLoosePackage(
		"/Tests/Range", PackagePath, Summary, std::span{&Entry, 1}
	);
	ASSERT_TRUE(Registration1) << Registration1.Message;
	Handle = std::move(Registration1.Resource);
	FBulkData Value;
	ASSERT_TRUE(FBulkData::TryAttach({
		.LogicalSize = Size,
		.Range = {
			.Resource = Handle,
			.StoredSize = Size,
			.Alignment = EditorBulkDataExternalAlignment}}, Value, &Error)) << Error;
	EXPECT_EQ(Value.GetState(), EBulkDataState::Attached);
	ASSERT_TRUE(Value.ReloadAsync().Wait()) << Error;
	EXPECT_EQ(Value.GetState(), EBulkDataState::Resident);
	Durin::FByteView Read;
	Durin::FBulkDataReadResult ReadLease;
	ReadLease = Value.AcquireRead();
	ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
	Read = ReadLease.Lock.GetBytes();
	EXPECT_TRUE(std::ranges::equal(Read, Segment));
	ReadLease.Lock.Reset();
	ASSERT_EQ(Value.TryUnload(), EBulkUnloadResult::Unloaded);
	Manager.RetirePackage("/Tests/Range");
	ReadLease = Value.AcquireRead();
	EXPECT_FALSE(ReadLease) << ReadLease.Error.Message;
	EXPECT_EQ(ReadLease.Error.Status, EPackageResourceReadStatus::Retired);
	EXPECT_EQ(Value.GetState(), EBulkDataState::Retired);
}

TEST(FPackageResourceTests, OwnedCaptureDetachesLazyReadsFromCallerStorage)
{
	const uint64 Size = EditorBulkDataExternalThreshold + 1;
	FByteBuffer Segment(static_cast<size_t>(Size), std::byte{0x6a});
	const FPackageBulkDataEntry Entry{
		.FieldIndex = 1,
		.Placement = EPackageBulkDataPlacement::External,
		.LogicalSize = Size,
		.StoredSize = Size,
		.Alignment = EditorBulkDataExternalAlignment,
		.ContentId = FXxHash128::HashBuffer(Segment)};
	const FPackageBulkSegmentSummary Summary{.Extent = Size, .Digest = Entry.ContentId};
	FPackageResourceHandle Handle;
	std::string Error;
	ASSERT_TRUE(CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment, Handle, &Error)) << Error;
	FBulkData Value;
	ASSERT_TRUE(FBulkData::TryAttach({
		.LogicalSize = Size,
		.Range = {.Resource = Handle, .StoredSize = Size,
			.Alignment = EditorBulkDataExternalAlignment}}, Value, &Error)) << Error;
	std::ranges::fill(Segment, std::byte{0x17});
	Segment.clear();
	Segment.shrink_to_fit();
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		ASSERT_TRUE(Value.ReloadAsync().Wait());
		FByteView Read;
		Durin::FBulkDataReadResult ReadLease;
		ReadLease = Value.AcquireRead();
		ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
		Read = ReadLease.Lock.GetBytes();
		EXPECT_EQ(FXxHash128::HashBuffer(Read), Entry.ContentId);
		ReadLease.Lock.Reset();
		ASSERT_EQ(Value.TryUnload(), EBulkUnloadResult::Unloaded);
	}
	const auto Slice = Handle->ReadRange(1, 7);
	ASSERT_TRUE(Slice);
	EXPECT_EQ(Slice.Buffer.GetSize(), 7u);
	EXPECT_EQ(Handle->ReadRange(Size, 1).Status, EPackageResourceReadStatus::InvalidRange);
	Handle->Retire();
	EXPECT_EQ(Handle->ReadRange(0, 1).Status, EPackageResourceReadStatus::Retired);
	Handle.reset();
	EXPECT_TRUE(std::ranges::all_of(Slice.Buffer.GetBytes(),
		[](std::byte Byte) { return Byte == std::byte{0x6a}; }));
}

TEST(FPackageResourceTests, OwnedCaptureRejectsMismatchedGenerationAndClearsOutput)
{
	const uint64 Size = EditorBulkDataExternalThreshold + 1;
	FByteBuffer Segment(static_cast<size_t>(Size), std::byte{0x6a});
	FPackageBulkDataEntry Entry{
		.FieldIndex = 1,
		.Placement = EPackageBulkDataPlacement::External,
		.LogicalSize = Size,
		.StoredSize = Size,
		.Alignment = EditorBulkDataExternalAlignment,
		.ContentId = FXxHash128::HashBuffer(Segment)};
	FPackageBulkSegmentSummary Summary{.Extent = Size, .Digest = Entry.ContentId};
	FPackageResourceHandle Handle;
	std::string Error;
	ASSERT_TRUE(CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment, Handle, &Error)) << Error;
	Segment[0] = std::byte{0x17};
	EXPECT_FALSE(CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment, Handle, &Error));
	EXPECT_FALSE(Handle);
	EXPECT_FALSE(Error.empty());
	Summary.Digest = FXxHash128::HashBuffer(Segment);
	EXPECT_FALSE(CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment, Handle, &Error));
	EXPECT_FALSE(Handle);
	Entry.ContentId = Summary.Digest;
	ASSERT_TRUE(CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment, Handle, &Error)) << Error;
	Segment.pop_back();
	EXPECT_FALSE(CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment, Handle, &Error));
	EXPECT_FALSE(Handle);
}

TEST(FPackageResourceTests, AdmissionValidatesEachRangeAndPaddingInOnePass)
{
	const uint64 FirstSize = EditorBulkDataExternalThreshold + 1;
	const uint64 SecondOffset = (FirstSize + EditorBulkDataExternalAlignment - 1)
		& ~uint64(EditorBulkDataExternalAlignment - 1);
	const uint64 SecondSize = EditorBulkDataExternalThreshold + 3;
	Durin::FByteBuffer Segment(static_cast<size_t>(SecondOffset + SecondSize));
	std::ranges::fill(std::span(Segment).first(static_cast<size_t>(FirstSize)),
		std::byte{0x31});
	std::ranges::fill(std::span(Segment).subspan(
		static_cast<size_t>(SecondOffset), static_cast<size_t>(SecondSize)),
		std::byte{0x72});
	const std::array Entries{
		FPackageBulkDataEntry{
			.FieldIndex = 1,
			.Placement = EPackageBulkDataPlacement::External,
			.LogicalSize = FirstSize,
			.StoredSize = FirstSize,
			.Alignment = EditorBulkDataExternalAlignment,
			.ContentId = FXxHash128::HashBuffer(
				std::span(Segment).first(static_cast<size_t>(FirstSize)))},
		FPackageBulkDataEntry{
			.FieldIndex = 2,
			.Placement = EPackageBulkDataPlacement::External,
			.LogicalSize = SecondSize,
			.StoredSize = SecondSize,
			.SegmentOffset = SecondOffset,
			.Alignment = EditorBulkDataExternalAlignment,
			.ContentId = FXxHash128::HashBuffer(std::span(Segment).subspan(
				static_cast<size_t>(SecondOffset), static_cast<size_t>(SecondSize)))}};
	FPackageBulkSegmentSummary Summary{
		.Extent = Segment.size(), .Digest = FXxHash128::HashBuffer(Segment)};
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "PackageResourceValidation";
	std::filesystem::create_directories(Root);
	const std::filesystem::path PackagePath = Root / "Ranges.dasset";
	std::filesystem::path SegmentPath = PackagePath;
	SegmentPath.replace_extension(".dbulk");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Segment, SegmentPath));

	FPackageResourceManager Manager;
	FPackageResourceHandle Handle;
	std::string Error;
	auto Registration2 = Manager.RegisterLoosePackage(
		"/Tests/Ranges", PackagePath, Summary, Entries
	);
	ASSERT_TRUE(Registration2) << Registration2.Message;
	Handle = std::move(Registration2.Resource);
	const FPackageResourceReadStats Stats = Handle->GetReadStats();
	EXPECT_EQ(Stats.ValidationBytesRead, Segment.size());
	EXPECT_LE(Stats.PeakValidationScratchBytes, 64u * 1024u);
	EXPECT_EQ(Stats.RequestCount, 0u);
	Manager.RetirePackage("/Tests/Ranges");

	Segment[7] ^= std::byte{0x01};
	Summary.Digest = FXxHash128::HashBuffer(Segment);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Segment, SegmentPath));
	auto Registration3 = Manager.RegisterLoosePackage(
		"/Tests/BadRange", PackagePath, Summary, Entries
	);
	EXPECT_FALSE(Registration3) << Registration3.Message;

	EXPECT_NE(Registration3.Message.find("field digest"), std::string::npos);

	Segment[7] ^= std::byte{0x01};
	ASSERT_GT(SecondOffset, FirstSize);
	Segment[static_cast<size_t>(FirstSize)] = std::byte{0x01};
	Summary.Digest = FXxHash128::HashBuffer(Segment);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Segment, SegmentPath));
	auto Registration4 = Manager.RegisterLoosePackage(
		"/Tests/BadPadding", PackagePath, Summary, Entries
	);
	EXPECT_FALSE(Registration4) << Registration4.Message;

	EXPECT_NE(Registration4.Message.find("padding"), std::string::npos);
}

namespace
{
	class FPreparedPackageResourceTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			const auto Root = Durin::Testing::GetTestWorkDirectory()
				/ testing::UnitTest::GetInstance()->current_test_info()->name();
			std::filesystem::create_directories(Root);
			MainPath = Root / "Prepared.dasset";
			BulkPath = Root / "Prepared.dbulk";
			Main = MakeBytes({1, 2, 3, 4}); // Storage tests: main schema is the codec's responsibility.
			Bulk.resize(EditorBulkDataExternalThreshold + 1, std::byte{0x39});
			Summary = {Bulk.size(), FXxHash128::HashBuffer(Bulk)};
			Entry = {.FieldIndex = 1, .Placement = EPackageBulkDataPlacement::External,
				.LogicalSize = Bulk.size(), .StoredSize = Bulk.size(),
				.Alignment = EditorBulkDataExternalAlignment, .ContentId = Summary.Digest};
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Main, MainPath));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, BulkPath));
		}
		auto Prepare(FPreparedPackageResource& Out, uint64 Budget = 1024 * 1024,
			const std::function<bool()>& IsCancelled = {}) -> FPreparedPackageResourceResult
		{
			return FPreparedPackageResource::Prepare(MainPath, FSharedByteBuffer::Copy(Main),
				Summary, std::span{&Entry, 1}, Budget, Out, IsCancelled);
		}
		std::filesystem::path MainPath, BulkPath;
		FByteBuffer Main, Bulk;
		FPackageBulkSegmentSummary Summary;
		FPackageBulkDataEntry Entry;
	};
}

TEST_F(FPreparedPackageResourceTests, SnapshotSurvivesDiskReplacementAndOwnerRelease)
{
	FPackageResourceManager Manager;
	FPackageResourceHandle Live;
	auto Registration5 = Manager.RegisterLoosePackage("/Tests/Prepared", MainPath, Summary, std::span{&Entry, 1});
	ASSERT_TRUE(Registration5) << Registration5.Message;
	Live = std::move(Registration5.Resource);
	FPreparedPackageResource Prepared;
	ASSERT_TRUE(Prepare(Prepared));
	EXPECT_EQ(Prepared.GetRetainedBytes(), Main.size() + Bulk.size());
	EXPECT_EQ(Manager.FindPackage("/Tests/Prepared"), Live);
	EXPECT_FALSE(Live->IsRetired());
	ASSERT_TRUE(Prepared.Revalidate());
	FBulkData Payload;
	ASSERT_TRUE(FBulkData::TryAttach({.LogicalSize = Bulk.size(),
		.Range = {.Resource = Prepared.GetBulkResource(), .StoredSize = Bulk.size(),
			.Alignment = EditorBulkDataExternalAlignment}}, Payload));
	Bulk[0] ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bulk, BulkPath));
	EXPECT_EQ(Prepared.Revalidate().Status, EPreparedPackageResourceStatus::Stale);
	Prepared = {};
	ASSERT_TRUE(Payload.ReloadAsync().Wait());
	FByteView Read;
	Durin::FBulkDataReadResult ReadLease;
	ReadLease = Payload.AcquireRead();
	ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
	Read = ReadLease.Lock.GetBytes();
	EXPECT_EQ(Read[0], std::byte{0x39});
	ReadLease.Lock.Reset();
	ASSERT_EQ(Payload.TryUnload(), EBulkUnloadResult::Unloaded);
	std::filesystem::remove(BulkPath);
	ASSERT_TRUE(Payload.ReloadAsync().Wait());
	ReadLease = Payload.AcquireRead();
	ASSERT_TRUE(ReadLease) << ReadLease.Error.Message;
	Read = ReadLease.Lock.GetBytes();
	EXPECT_EQ(Read[0], std::byte{0x39});
	ReadLease.Lock.Reset();
}

TEST_F(FPreparedPackageResourceTests, RechecksMainContentInsteadOfSizeOrTimestamp)
{
	FPreparedPackageResource Prepared;
	ASSERT_TRUE(Prepare(Prepared));
	const auto Timestamp = std::filesystem::last_write_time(MainPath);
	Main[0] ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Main, MainPath));
	std::filesystem::last_write_time(MainPath, Timestamp);
	EXPECT_EQ(Prepared.Revalidate().Status, EPreparedPackageResourceStatus::Stale);
	EXPECT_EQ(Prepared.GetMainBytes()[0], std::byte{1});
}

TEST_F(FPreparedPackageResourceTests, FailurePreservesOutputAndDoesNotRecoverBackup)
{
	FPreparedPackageResource Prepared;
	ASSERT_TRUE(Prepare(Prepared));
	const auto Original = Prepared.GetBulkResource();
	auto Backup = BulkPath;
	Backup += ".durin-backup";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, Backup));
	Bulk[0] ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, BulkPath));
	EXPECT_EQ(Prepare(Prepared).Status, EPreparedPackageResourceStatus::InvalidClosure);
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
	FByteBuffer Actual;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Actual, BulkPath));
	EXPECT_EQ(Actual, Bulk);
	EXPECT_TRUE(std::filesystem::exists(Backup));
	std::filesystem::remove(BulkPath);
	EXPECT_EQ(Prepare(Prepared).Status, EPreparedPackageResourceStatus::IoError);
	EXPECT_FALSE(std::filesystem::exists(BulkPath));
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
}

TEST_F(FPreparedPackageResourceTests, RejectsBudgetAndCancellationWithoutPublishing)
{
	FPreparedPackageResource Prepared;
	ASSERT_TRUE(Prepare(Prepared, Main.size() + Bulk.size()));
	const auto Original = Prepared.GetBulkResource();
	EXPECT_EQ(Prepare(Prepared, Main.size() + Bulk.size() - 1).Status,
		EPreparedPackageResourceStatus::BudgetExceeded);
	EXPECT_EQ(Prepare(Prepared, 0).Status, EPreparedPackageResourceStatus::BudgetExceeded);
	EXPECT_EQ(Prepare(Prepared, 1024 * 1024, [] { return true; }).Status,
		EPreparedPackageResourceStatus::Cancelled);
	uint32 Checks = 0;
	EXPECT_EQ(Prepare(Prepared, 1024 * 1024, [&] { return ++Checks == 4; }).Status,
		EPreparedPackageResourceStatus::Cancelled);
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
	EXPECT_TRUE(Prepared.Revalidate());
	Summary.Extent = std::numeric_limits<uint64>::max();
	EXPECT_EQ(Prepare(Prepared, std::numeric_limits<uint64>::max()).Status,
		EPreparedPackageResourceStatus::BudgetExceeded);
}

TEST_F(FPreparedPackageResourceTests, ValidatesNoBulkClosureAndRejectsUnexpectedCompanion)
{
	FPreparedPackageResource Prepared;
	const auto MainBytes = FSharedByteBuffer::Copy(Main);
	EXPECT_EQ(FPreparedPackageResource::Prepare(MainPath, MainBytes, {}, {}, 4, Prepared).Status,
		EPreparedPackageResourceStatus::InvalidClosure);
	std::filesystem::remove(BulkPath);
	ASSERT_TRUE(FPreparedPackageResource::Prepare(MainPath, MainBytes, {}, {}, 4, Prepared));
	EXPECT_TRUE(Prepared.GetMainBytes().SharesStorageWith(MainBytes));
	EXPECT_FALSE(Prepared.GetBulkResource());
	ASSERT_TRUE(Prepared.Revalidate());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, BulkPath));
	EXPECT_EQ(Prepared.Revalidate().Status, EPreparedPackageResourceStatus::InvalidClosure);
}

TEST_F(FPreparedPackageResourceTests, RejectsMainReplacementDuringBulkCapture)
{
	FPreparedPackageResource Prepared;
	ASSERT_TRUE(Prepare(Prepared));
	const auto Original = Prepared.GetBulkResource();
	uint32 Checks = 0;
	bool bReplaced = false;
	const auto Result = Prepare(Prepared, 1024 * 1024, [&] {
		if (++Checks == 3)
		{
			auto Replacement = Main;
			Replacement[0] ^= std::byte{1};
			bReplaced = FFileHelper::SaveArrayToFileAtomically(Replacement, MainPath);
		}
		return false;
	});
	ASSERT_TRUE(bReplaced);
	EXPECT_EQ(Result.Status, EPreparedPackageResourceStatus::Stale);
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
}

TEST(FPackageResourceTests, AsyncCancellationAndRetirementConserveTerminalResults)
{
	auto Resource = std::make_shared<FSlowPackageResource>();
	FPackageResourceRequest Cancelled = Resource->ReadRangeAsync(0, 4);
	Cancelled.Cancel();
	EXPECT_EQ(Cancelled.Wait().Status, EPackageResourceReadStatus::Cancelled);

	FPackageResourceRequest Retiring = Resource->ReadRangeAsync(0, 4);
	Resource->Retire();
	const EPackageResourceReadStatus Status = Retiring.Wait().Status;
	EXPECT_TRUE(Status == EPackageResourceReadStatus::Cancelled
		|| Status == EPackageResourceReadStatus::Success);
	EXPECT_TRUE(Resource->IsRetired());
	EXPECT_EQ(Resource->ReadRangeAsync(0, 1).Wait().Status,
		EPackageResourceReadStatus::Retired);
}

TEST(FPackageResourceTests, BlockingReadsLeaveCpuAvailableAndTransformsShareTerminalOutcomes)
{
	auto FirstResource = std::make_shared<FBlockedPackageResource>();
	auto SecondResource = std::make_shared<FBlockedPackageResource>();
	auto First = FirstResource->ReadRangeAsync(0, 4);
	auto Second = SecondResource->ReadRangeAsync(0, 4);
	ASSERT_TRUE(FirstResource->Started.WaitFor(1.0));
	ASSERT_TRUE(SecondResource->Started.WaitFor(1.0));
	FThreadEvent CpuRan;
	auto Cpu = Tasks::LaunchTask("PackageIoIsolation", [&] { CpuRan.Trigger(); }).GetCompletion().GetTaskHandle();
	EXPECT_TRUE(CpuRan.WaitFor(0.5));
	auto Copy = First;
	auto Transform = FPackageResourceRequest::Transform(First, [](FPackageResourceReadResult Value) { return Value; });
	FirstResource->Release.Trigger();
	SecondResource->Release.Trigger();
	EXPECT_TRUE(First.Wait());
	EXPECT_TRUE(Copy.Wait());
	EXPECT_TRUE(Transform.Wait());
	EXPECT_EQ(First.Wait().Buffer.GetBytes().data(), Copy.Wait().Buffer.GetBytes().data());
	EXPECT_TRUE(Second.Wait());
	WaitTask(Cpu);

	auto Slow = std::make_shared<FSlowPackageResource>();
	auto Canceled = Slow->ReadRangeAsync(0, 4);
	Canceled.Cancel();
	auto Recovery = FPackageResourceRequest::Transform(Canceled, [](FPackageResourceReadResult Value) {
		EXPECT_EQ(EPackageResourceReadStatus::Cancelled, Value.Status);
		return FPackageResourceReadResult{.Status = EPackageResourceReadStatus::Success};
	});
	EXPECT_TRUE(Recovery.Wait());
}

TEST(FPackageResourceTests, SubmissionAfterSchedulerClosureIsALifecycleViolation)
{
	ShutdownTaskScheduler(true);
	EXPECT_DEATH({
		auto Resource = std::make_shared<FSlowPackageResource>();
		(void)Resource->ReadRangeAsync(0, 4);
	}, "");
	EXPECT_DEATH({
		(void)FPackageResourceRequest::Transform(
			FPackageResourceRequest::Completed({.Status = EPackageResourceReadStatus::Success}),
			[](FPackageResourceReadResult Result) { return Result; });
	}, "");
	EXPECT_TRUE(InitializeTaskScheduler(2));
}

TEST(FPackageResourceTests, RejectedRenderingWaitDoesNotPublishRequestCompletion)
{
	auto Resource = std::make_shared<FBlockedPackageResource>();
	FPackageResourceRequest Request = Resource->ReadRangeAsync(0, 4);
	ASSERT_TRUE(Resource->Started.WaitFor(1.0));
	FPackageWaitRunnable Runnable(Request);
	std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable,
		"PackageRejectedWait", 0, EThreadPriority::Normal, EThreadRole::RenderingThread));
	ASSERT_NE(Thread, nullptr);
	Thread->WaitForCompletion();
	EXPECT_EQ(Runnable.Result.Status, EPackageResourceReadStatus::IoError);
	EXPECT_NE(Runnable.Result.Message.find("wait was rejected"), std::string::npos);
	EXPECT_FALSE(Request.IsReady());
	Resource->Release.Trigger();
	const auto Result = Request.Wait();
	ASSERT_TRUE(Result);
	EXPECT_EQ(Result.Buffer.GetSize(), 4u);
	EXPECT_EQ(Result.Buffer.GetBytes().front(), std::byte{0x31});
}

TEST(FEditorBulkDataTests, SeparatesInstanceAndContentIdentityWithoutForcedLoad)
{
	const std::array Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
	FEditorBulkData First(FGuid{1, 2, 3, 4});
	ASSERT_TRUE(First.UpdatePayload(Bytes));
	const FGuid InstanceId = First.GetInstanceId();
	const FXxHash128 ContentId = First.GetPayloadId();
	EXPECT_EQ(ContentId, FXxHash128::HashBuffer(Bytes));
	FEditorBulkData Snapshot = First;
	EXPECT_TRUE(First.Identical(Snapshot));

	const std::array Replacement{std::byte{9}};
	ASSERT_TRUE(First.UpdatePayload(Replacement));
	EXPECT_EQ(First.GetInstanceId(), InstanceId);
	EXPECT_NE(First.GetPayloadId(), ContentId);
	EXPECT_EQ(Snapshot.GetPayloadId(), ContentId);
	EXPECT_TRUE(std::ranges::equal(Snapshot.GetPayload().Wait().Buffer.GetBytes(), Bytes));

	auto Resource = std::make_shared<FSlowPackageResource>();
	FEditorBulkData PackageBacked;
	std::string Error;
	ASSERT_TRUE(FEditorBulkData::TryCreatePackageBacked(
		FGuid{5, 6, 7, 8}, FXxHash128::HashBuffer(
			Durin::FByteBuffer(4, std::byte{0})), 4,
		{.Resource = Resource, .StoredSize = 4}, PackageBacked, &Error)) << Error;
	EXPECT_FALSE(PackageBacked.IsMemoryResident());
	EXPECT_EQ(PackageBacked.GetPayloadSize(), 4u);
	EXPECT_FALSE(PackageBacked.GetPayloadId().IsZero());
	EXPECT_TRUE(PackageBacked.GetPayload().Wait());
	Resource->Retire();
	EXPECT_EQ(PackageBacked.GetPayload().Wait().Status, EPackageResourceReadStatus::Retired);
}

TEST(FEditorBulkDataTests, ConcurrentCopiesObserveOneCoherentSnapshot)
{
	const std::array Initial{std::byte{1}, std::byte{2}, std::byte{3}};
	const std::array Replacement{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
	FEditorBulkData Value(FGuid{11, 12, 13, 14});
	ASSERT_TRUE(Value.UpdatePayload(Initial));
	std::atomic_bool Done = false;
	std::atomic_bool Coherent = true;
	std::thread Writer([&] {
		for (uint32 Index = 0; Index < 2000; ++Index)
		{
			if (!Value.UpdatePayload(Index % 2 == 0
				? Durin::FByteView(Initial)
				: Durin::FByteView(Replacement)))
				Coherent.store(false, std::memory_order_release);
		}
		Done.store(true, std::memory_order_release);
	});
	while (!Done.load(std::memory_order_acquire))
	{
		const FEditorBulkData Snapshot = Value;
		const FPackageResourceReadResult Payload = Snapshot.GetPayload().Wait();
		if (!Payload || Payload.Buffer.GetSize() != Snapshot.GetPayloadSize()
			|| FXxHash128::HashBuffer(Payload.Buffer.GetBytes()) != Snapshot.GetPayloadId()
			|| Snapshot.GetInstanceId() != FGuid{11, 12, 13, 14})
			Coherent.store(false, std::memory_order_release);
	}
	Writer.join();
	EXPECT_TRUE(Coherent.load(std::memory_order_acquire));
}

TEST(FEditorBulkDataTests, RequestsAndFailedReplacementConserveCapturedState)
{
	auto Resource = std::make_shared<FSlowPackageResource>();
	FEditorBulkData Value;
	std::string Error;
	ASSERT_TRUE(FEditorBulkData::TryCreatePackageBacked(
		FGuid{21, 22, 23, 24}, FXxHash128::HashBuffer(
			Durin::FByteBuffer(4, std::byte{0})), 4,
		{.Resource = Resource, .StoredSize = 4}, Value, &Error)) << Error;
	FPackageResourceRequest Captured = Value.GetPayload();
	const std::array Replacement{std::byte{4}, std::byte{3}};
	ASSERT_TRUE(Value.UpdatePayload(Replacement));
	const FPackageResourceReadResult Original = Captured.Wait();
	ASSERT_TRUE(Original);
	EXPECT_EQ(Original.Buffer.GetSize(), 4u);
	EXPECT_TRUE(std::ranges::all_of(
		Original.Buffer.GetBytes(), [](std::byte Byte) { return Byte == std::byte{0}; }));

	const FGuid InstanceId = Value.GetInstanceId();
	const FXxHash128 ContentId = Value.GetPayloadId();
	EXPECT_EQ(Value.GetInstanceId(), InstanceId);
	EXPECT_EQ(Value.GetPayloadId(), ContentId);
	EXPECT_TRUE(std::ranges::equal(Value.GetPayload().Wait().Buffer.GetBytes(), Replacement));
}

TEST(FPackageResourceRangeTests, SharesBoundedStorageFactsAcrossEditorAndRuntimeBulk)
{
	auto Resource = std::make_shared<FSlowPackageResource>();
	const FPackageResourceRange Range{.Resource = Resource, .StoredSize = 4};
	std::string Error;
	EXPECT_TRUE(ValidatePackageResourceRange(Range, 4, &Error)) << Error;
	FPackageResourceRange Invalid = Range;
	Invalid.StorageFlags = 1;
	EXPECT_FALSE(ValidatePackageResourceRange(Invalid, 4, &Error));

	FEditorBulkData Editor;
	ASSERT_TRUE(FEditorBulkData::TryCreatePackageBacked(
		FGuid{31, 32, 33, 34}, FXxHash128::HashBuffer(
			Durin::FByteBuffer(4, std::byte{0})), 4, Range, Editor, &Error)) << Error;
	FBulkData Runtime;
	ASSERT_TRUE(FBulkData::TryAttach(
		{.LogicalSize = 4, .Range = Range}, Runtime, &Error)) << Error;
	EXPECT_EQ(Editor.GetPayloadSize(), Runtime.GetMetadata().LogicalSize);
	EXPECT_EQ(Runtime.GetMetadata().Range.Resource, Resource);
	EXPECT_FALSE(Editor.IsMemoryResident());
}

TEST(FBulkDataTests, ScopedLocksReleaseOnReturnAndRetainReplacedStorage)
{
	FBulkData Value;
	ASSERT_TRUE(FBulkData::TryCreateDetached(MakeBytes({1, 2}), Value));
	const auto Visit = [&] {
		auto Read = Value.AcquireRead();
		EXPECT_TRUE(Read);
		EXPECT_EQ(Value.GetState(), EBulkDataState::ReadLocked);
	};
	Visit();
	EXPECT_EQ(Value.GetState(), EBulkDataState::Detached);
	{
		auto Write = Value.AcquireWrite();
		EXPECT_FALSE(Write.TryResize(MaximumBulkDataBytes + 1));
		EXPECT_EQ(Write.GetBytes().size(), 2u);
		auto Moved = std::move(Write);
		Write.Reset();
		EXPECT_EQ(Value.GetState(), EBulkDataState::WriteLocked);
		EXPECT_EQ(Value.AcquireRead().Status, EBulkReadStatus::Busy);
		Moved.GetBytes()[0] = std::byte{9};
	}
	EXPECT_EQ(Value.GetState(), EBulkDataState::Detached);
	auto Read = Value.AcquireRead();
	ASSERT_TRUE(Read);
	auto Moved = std::move(Read.Lock);
	Read.Lock.Reset();
	Value = FBulkData{};
	EXPECT_EQ(Moved.GetBytes()[0], std::byte{9});
	Moved.Reset();
	Moved.Reset();
}

TEST(FPackageResourceTests, RegistrationRejectsEmptySegmentsWithOwnedDiagnostics)
{
	FPackageResourceManager Manager;
	const auto Empty = Manager.RegisterLoosePackage("/Tests/Empty", "Empty.dasset", {}, {});
	EXPECT_EQ(Empty.Status, EPackageResourceRegistrationStatus::InvalidMetadata);
	EXPECT_FALSE(Empty.Resource);
	EXPECT_FALSE(Empty.Message.empty());
}

TEST(FBulkDataTests, ReadFailurePreservesPackageCauseAndSupportsExplicitRetry)
{
	auto Resource = std::make_shared<FFailingPackageResource>();
	FBulkData Value;
	ASSERT_TRUE(FBulkData::TryAttach({.LogicalSize = 4, .Range = {.Resource = Resource, .StoredSize = 4}}, Value));
	auto Failed = Value.AcquireRead();
	EXPECT_EQ(Failed.Status, EBulkReadStatus::ReadFailed);
	EXPECT_FALSE(Failed.Lock);
	EXPECT_EQ(Failed.Error.Status, EPackageResourceReadStatus::SegmentDigestMismatch);
	EXPECT_EQ(Failed.Error.Message, "Injected digest failure.");
	EXPECT_EQ(Value.GetState(), EBulkDataState::Failed);
	Resource->bFail.store(false);
	auto Retry = Value.AcquireRead();
	ASSERT_TRUE(Retry);
	EXPECT_EQ(Retry.Status, EBulkReadStatus::Acquired);
	EXPECT_EQ(Retry.Lock.GetBytes()[0], std::byte{7});
}

#include "Threading/TaskComposition.h"
#include <gtest/gtest.h>

#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "Asset/EditorBulkDataStorage.h"
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
			if (bFail.load()) return std::unexpected(FPackageResourceReadError{.Status = EPackageResourceReadStatus::SegmentDigestMismatch, .Reason = EPackageResourceReadReason::ContentMismatch, .ActualDigest = {7, 8}, .ExpectedDigest = {1, 2}});
			return FSharedByteBuffer::Take(FByteBuffer(Size, std::byte{7}));
		}
	};

	// Gates reads explicitly when a test needs an in-flight request. The timeout
	// bounds cleanup after an assertion exits before releasing the read.
	class FTestPackageResource final : public FPackageResource
	{
	public:
		explicit FTestPackageResource(bool bBlockRead = false) : FPackageResource(4)
		{
			if (!bBlockRead) Release.Trigger();
		}
		FThreadEvent Started;
		FThreadEvent Release;

	private:
		auto ReadRangeImpl(uint64, uint64 Size, const std::atomic_bool& bCancelled)
			-> FPackageResourceReadResult override
		{
			Started.Trigger();
			if (!Release.WaitFor(2.0)) return std::unexpected(FPackageResourceReadError{.Status = EPackageResourceReadStatus::IoError});
			if (bCancelled.load(std::memory_order_acquire))
				return std::unexpected(FPackageResourceReadError{.Status = EPackageResourceReadStatus::Cancelled});
			return FSharedByteBuffer::Take(Durin::FByteBuffer(Size));
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
			if (!Release.WaitFor(2.0)) return std::unexpected(FPackageResourceReadError{.Status = EPackageResourceReadStatus::IoError});
			return FSharedByteBuffer::Take(FByteBuffer(Size, std::byte{0x31}));
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
	{
		auto ValueResult = FBulkData::TryCreateDetached(Bytes);
		ASSERT_TRUE(ValueResult);
		First = std::move(*ValueResult);
	}
	FBulkData Second = First;
	Durin::FByteView Read;
	Durin::FBulkDataReadResult ReadLease;
	Durin::FMutableByteView Write;
	ReadLease = First.AcquireRead();
	ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
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
	ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
	Read = ReadLease.Lock.GetBytes();
	EXPECT_TRUE(std::ranges::equal(Read, Bytes));
	ReadLease.Lock.Reset();
	ReadLease = Second.AcquireRead();
	ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
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
	ASSERT_TRUE(Registration1) << FormatPackageResourceRegistrationError(Registration1.error());
	Handle = std::move((*Registration1));
	FBulkData Value;
	{
		auto ValueResult = FBulkData::TryAttach({
		.LogicalSize = Size,
		.Range = {
			.Resource = Handle,
			.StoredSize = Size,
			.Alignment = EditorBulkDataExternalAlignment}});
		ASSERT_TRUE(ValueResult);
		Value = std::move(*ValueResult);
	}
	EXPECT_EQ(Value.GetState(), EBulkDataState::Attached);
	ASSERT_TRUE(Value.ReloadAsync().Wait()) << Error;
	EXPECT_EQ(Value.GetState(), EBulkDataState::Resident);
	Durin::FByteView Read;
	Durin::FBulkDataReadResult ReadLease;
	ReadLease = Value.AcquireRead();
	ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
	Read = ReadLease.Lock.GetBytes();
	EXPECT_TRUE(std::ranges::equal(Read, Segment));
	ReadLease.Lock.Reset();
	ASSERT_EQ(Value.TryUnload(), EBulkUnloadResult::Unloaded);
	Manager.RetirePackage("/Tests/Range");
	ReadLease = Value.AcquireRead();
	EXPECT_FALSE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
	EXPECT_EQ(ReadLease.Error.error().Status, EPackageResourceReadStatus::Retired);
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
	{
		auto ValueResult = CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment);
		ASSERT_TRUE(ValueResult);
		Handle = std::move(*ValueResult);
	}
	FBulkData Value;
	{
		auto ValueResult = FBulkData::TryAttach({
		.LogicalSize = Size,
		.Range = {.Resource = Handle, .StoredSize = Size,
			.Alignment = EditorBulkDataExternalAlignment}});
		ASSERT_TRUE(ValueResult);
		Value = std::move(*ValueResult);
	}
	std::ranges::fill(Segment, std::byte{0x17});
	Segment.clear();
	Segment.shrink_to_fit();
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		ASSERT_TRUE(Value.ReloadAsync().Wait());
		FByteView Read;
		Durin::FBulkDataReadResult ReadLease;
		ReadLease = Value.AcquireRead();
		ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
		Read = ReadLease.Lock.GetBytes();
		EXPECT_EQ(FXxHash128::HashBuffer(Read), Entry.ContentId);
		ReadLease.Lock.Reset();
		ASSERT_EQ(Value.TryUnload(), EBulkUnloadResult::Unloaded);
	}
	const auto Slice = Handle->ReadRange(1, 7);
	ASSERT_TRUE(Slice);
	EXPECT_EQ(Slice->GetSize(), 7u);
	EXPECT_EQ(Handle->ReadRange(Size, 1).error().Status, EPackageResourceReadStatus::InvalidRange);
	Handle->Retire();
	EXPECT_EQ(Handle->ReadRange(0, 1).error().Status, EPackageResourceReadStatus::Retired);
	Handle.reset();
	EXPECT_TRUE(std::ranges::all_of(Slice->GetBytes(),
		[](std::byte Byte) { return Byte == std::byte{0x6a}; }));
}

TEST(FPackageResourceTests, OwnedCaptureRejectsMismatchedGenerationWithoutAValue)
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
	{
		auto ValueResult = CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment);
		ASSERT_TRUE(ValueResult);
		Handle = std::move(*ValueResult);
	}
	Segment[0] = std::byte{0x17};
	auto SegmentFailure = CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment);
	if (SegmentFailure) { Handle = std::move(*SegmentFailure); }
	EXPECT_FALSE(SegmentFailure);
	EXPECT_EQ(SegmentFailure.error().Code, EPackageBulkDataError::SegmentDigestMismatch);
	EXPECT_EQ(SegmentFailure.error().Summary.Digest, Summary.Digest);
	EXPECT_EQ(SegmentFailure.error().ActualDigest, FXxHash128::HashBuffer(Segment));
	EXPECT_TRUE(Handle);
	Summary.Digest = FXxHash128::HashBuffer(Segment);
	auto FieldFailure = CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment);
	if (FieldFailure) { Handle = std::move(*FieldFailure); }
	EXPECT_FALSE(FieldFailure);
	EXPECT_EQ(FieldFailure.error().Code, EPackageBulkDataError::FieldDigestMismatch);
	ASSERT_TRUE(FieldFailure.error().Entry);
	EXPECT_EQ(FieldFailure.error().Entry->ContentId, Entry.ContentId);
	EXPECT_TRUE(Handle);
	Entry.ContentId = Summary.Digest;
	{
		auto ValueResult = CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment);
		ASSERT_TRUE(ValueResult);
		Handle = std::move(*ValueResult);
	}
	Segment.pop_back();
	auto ExtentFailure = CreateOwnedPackageResource(Summary, std::span{&Entry, 1}, Segment);
	if (ExtentFailure) { Handle = std::move(*ExtentFailure); }
	EXPECT_FALSE(ExtentFailure);
	EXPECT_EQ(ExtentFailure.error().Code, EPackageBulkDataError::ExtentMismatch);
	EXPECT_EQ(ExtentFailure.error().Actual, Segment.size());
	EXPECT_EQ(ExtentFailure.error().Expected, Size);
	EXPECT_TRUE(Handle);
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
	ASSERT_TRUE(Registration2) << FormatPackageResourceRegistrationError(Registration2.error());
	Handle = std::move((*Registration2));
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
	EXPECT_FALSE(Registration3) << FormatPackageResourceRegistrationError(Registration3.error());

	ASSERT_TRUE(Registration3.error().PrimaryCause);
	const auto* RangeFailure = std::get_if<FPackageBulkValidationFailure>(&*Registration3.error().PrimaryCause);
	ASSERT_TRUE(RangeFailure);
	EXPECT_EQ(RangeFailure->Error.Code, EPackageBulkDataError::FieldDigestMismatch);
	EXPECT_EQ(RangeFailure->Error.Index, 0u);

	Segment[7] ^= std::byte{0x01};
	ASSERT_GT(SecondOffset, FirstSize);
	Segment[static_cast<size_t>(FirstSize)] = std::byte{0x01};
	Summary.Digest = FXxHash128::HashBuffer(Segment);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Segment, SegmentPath));
	auto Registration4 = Manager.RegisterLoosePackage(
		"/Tests/BadPadding", PackagePath, Summary, Entries
	);
	EXPECT_FALSE(Registration4) << FormatPackageResourceRegistrationError(Registration4.error());

	ASSERT_TRUE(Registration4.error().PrimaryCause);
	const auto* PaddingFailure = std::get_if<FPackageBulkValidationFailure>(&*Registration4.error().PrimaryCause);
	ASSERT_TRUE(PaddingFailure);
	EXPECT_EQ(PaddingFailure->Error.Code, EPackageBulkDataError::NonzeroPadding);
	EXPECT_EQ(PaddingFailure->Error.Offset, FirstSize);
	EXPECT_EQ(PaddingFailure->Error.Actual, 1u);
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
		auto Prepare(uint64 Budget = 1024 * 1024,
			const std::function<bool()>& IsCancelled = {}) -> std::expected<FPreparedPackageResource, FPreparedPackageResourceError>
		{
			return FPreparedPackageResource::Prepare(MainPath, FSharedByteBuffer::Copy(Main),
				Summary, std::span{&Entry, 1}, Budget, IsCancelled);
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
	ASSERT_TRUE(Registration5) << FormatPackageResourceRegistrationError(Registration5.error());
	Live = std::move((*Registration5));
	FPreparedPackageResource Prepared;
	{
		auto Result = Prepare();
		ASSERT_TRUE(Result);
		Prepared = std::move(*Result);
	}
	EXPECT_EQ(Prepared.GetRetainedBytes(), Main.size() + Bulk.size());
	EXPECT_EQ(Manager.FindPackage("/Tests/Prepared"), Live);
	EXPECT_FALSE(Live->IsRetired());
	ASSERT_TRUE(Prepared.Revalidate());
	FBulkData Payload;
	{
		auto ValueResult = FBulkData::TryAttach({.LogicalSize = Bulk.size(),
		.Range = {.Resource = Prepared.GetBulkResource(), .StoredSize = Bulk.size(),
			.Alignment = EditorBulkDataExternalAlignment}});
		ASSERT_TRUE(ValueResult);
		Payload = std::move(*ValueResult);
	}
	Bulk[0] ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bulk, BulkPath));
	EXPECT_EQ(Prepared.Revalidate().error().Code, EPreparedPackageResourceError::Stale);
	Prepared = {};
	ASSERT_TRUE(Payload.ReloadAsync().Wait());
	FByteView Read;
	Durin::FBulkDataReadResult ReadLease;
	ReadLease = Payload.AcquireRead();
	ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
	Read = ReadLease.Lock.GetBytes();
	EXPECT_EQ(Read[0], std::byte{0x39});
	ReadLease.Lock.Reset();
	ASSERT_EQ(Payload.TryUnload(), EBulkUnloadResult::Unloaded);
	std::filesystem::remove(BulkPath);
	ASSERT_TRUE(Payload.ReloadAsync().Wait());
	ReadLease = Payload.AcquireRead();
	ASSERT_TRUE(ReadLease) << FormatPackageResourceReadError(ReadLease.Error);
	Read = ReadLease.Lock.GetBytes();
	EXPECT_EQ(Read[0], std::byte{0x39});
	ReadLease.Lock.Reset();
}

TEST_F(FPreparedPackageResourceTests, RechecksMainContentInsteadOfSizeOrTimestamp)
{
	FPreparedPackageResource Prepared;
	{
		auto Result = Prepare();
		ASSERT_TRUE(Result);
		Prepared = std::move(*Result);
	}
	const auto Timestamp = std::filesystem::last_write_time(MainPath);
	Main[0] ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Main, MainPath));
	std::filesystem::last_write_time(MainPath, Timestamp);
	const auto Changed = Prepared.Revalidate();
	EXPECT_EQ(Changed.error().Code, EPreparedPackageResourceError::Stale);
	EXPECT_EQ(Changed.error().Reason, EPreparedPackageResourceReason::DigestChanged);
	EXPECT_EQ(Changed.error().Path, MainPath);
	EXPECT_EQ(Changed.error().ActualDigest, FXxHash128::HashBuffer(Main));
	EXPECT_EQ(Changed.error().ExpectedDigest, FXxHash128::HashBuffer(Prepared.GetMainBytes()));
	EXPECT_EQ(Prepared.GetMainBytes()[0], std::byte{1});
}

TEST_F(FPreparedPackageResourceTests, FailurePreservesOutputAndDoesNotRecoverBackup)
{
	FPreparedPackageResource Prepared;
	{
		auto Result = Prepare();
		ASSERT_TRUE(Result);
		Prepared = std::move(*Result);
	}
	const auto Original = Prepared.GetBulkResource();
	auto Backup = BulkPath;
	Backup += ".durin-backup";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, Backup));
	Bulk[0] ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, BulkPath));
	EXPECT_EQ(Prepare().error().Code, EPreparedPackageResourceError::InvalidClosure);
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
	FByteBuffer Actual;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Actual, BulkPath));
	EXPECT_EQ(Actual, Bulk);
	EXPECT_TRUE(std::filesystem::exists(Backup));
	std::filesystem::remove(BulkPath);
	const auto Missing = Prepare();
	EXPECT_EQ(Missing.error().Code, EPreparedPackageResourceError::IoError);
	EXPECT_EQ(Missing.error().Reason, EPreparedPackageResourceReason::FileIo);
	const auto* FileFailure = std::get_if<FFileIO::FFileError>(&Missing.error().Cause);
	ASSERT_TRUE(FileFailure);
	EXPECT_EQ(FileFailure->Operation, FFileIO::EFileOperation::OpenRead);
	EXPECT_EQ(FileFailure->Path, BulkPath);
	EXPECT_FALSE(std::filesystem::exists(BulkPath));
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
}

TEST_F(FPreparedPackageResourceTests, RejectsBudgetAndCancellationWithoutPublishing)
{
	FPreparedPackageResource Prepared;
	{
		auto Result = Prepare(Main.size() + Bulk.size());
		ASSERT_TRUE(Result);
		Prepared = std::move(*Result);
	}
	const auto Original = Prepared.GetBulkResource();
	const auto Budget = Prepare(Main.size() + Bulk.size() - 1);
	EXPECT_EQ(Budget.error().Code, EPreparedPackageResourceError::BudgetExceeded);
	EXPECT_EQ(Budget.error().Reason, EPreparedPackageResourceReason::ClosureBudget);
	EXPECT_EQ(Budget.error().MainBytes, Main.size());
	EXPECT_EQ(Budget.error().BulkBytes, Bulk.size());
	EXPECT_EQ(Budget.error().MaximumBytes, Main.size() + Bulk.size() - 1);
	EXPECT_EQ(Prepare(0).error().Code, EPreparedPackageResourceError::BudgetExceeded);
	EXPECT_EQ(Prepare(1024 * 1024, [] { return true; }).error().Code,
		EPreparedPackageResourceError::Cancelled);
	uint32 Checks = 0;
	EXPECT_EQ(Prepare(1024 * 1024, [&] { return ++Checks == 4; }).error().Code,
		EPreparedPackageResourceError::Cancelled);
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
	EXPECT_TRUE(Prepared.Revalidate());
	Summary.Extent = std::numeric_limits<uint64>::max();
	EXPECT_EQ(Prepare(std::numeric_limits<uint64>::max()).error().Code,
		EPreparedPackageResourceError::BudgetExceeded);
}

TEST_F(FPreparedPackageResourceTests, ValidatesNoBulkClosureAndRejectsUnexpectedCompanion)
{
	FPreparedPackageResource Prepared;
	const auto MainBytes = FSharedByteBuffer::Copy(Main);
	EXPECT_EQ(FPreparedPackageResource::Prepare(MainPath, MainBytes, {}, {}, 4).error().Code,
		EPreparedPackageResourceError::InvalidClosure);
	std::filesystem::remove(BulkPath);
	{
		auto ValueResult = FPreparedPackageResource::Prepare(MainPath, MainBytes, {}, {}, 4);
		ASSERT_TRUE(ValueResult);
		Prepared = std::move(*ValueResult);
	}
	EXPECT_TRUE(Prepared.GetMainBytes().SharesStorageWith(MainBytes));
	EXPECT_FALSE(Prepared.GetBulkResource());
	ASSERT_TRUE(Prepared.Revalidate());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bulk, BulkPath));
	EXPECT_EQ(Prepared.Revalidate().error().Code, EPreparedPackageResourceError::InvalidClosure);
}

TEST_F(FPreparedPackageResourceTests, RejectsMainReplacementDuringBulkCapture)
{
	FPreparedPackageResource Prepared;
	{
		auto Result = Prepare();
		ASSERT_TRUE(Result);
		Prepared = std::move(*Result);
	}
	const auto Original = Prepared.GetBulkResource();
	uint32 Checks = 0;
	bool bReplaced = false;
	const auto Result = Prepare(1024 * 1024, [&] {
		if (++Checks == 3)
		{
			auto Replacement = Main;
			Replacement[0] ^= std::byte{1};
			bReplaced = FFileHelper::SaveArrayToFileAtomically(Replacement, MainPath);
		}
		return false;
	});
	ASSERT_TRUE(bReplaced);
	EXPECT_EQ(Result.error().Code, EPreparedPackageResourceError::Stale);
	EXPECT_EQ(Prepared.GetBulkResource(), Original);
}

TEST(FPackageResourceTests, AsyncCancellationAndRetirementConserveTerminalResults)
{
	auto Resource = std::make_shared<FTestPackageResource>(true);
	FPackageResourceRequest Cancelled = Resource->ReadRangeAsync(0, 4);
	ASSERT_TRUE(Resource->Started.WaitFor(1.0));
	Cancelled.Cancel();
	Resource->Release.Trigger();
	EXPECT_EQ(Cancelled.Wait().error().Status, EPackageResourceReadStatus::Cancelled);

	FPackageResourceRequest Retiring = Resource->ReadRangeAsync(0, 4);
	Resource->Retire();
	const EPackageResourceReadStatus Status = GetPackageResourceReadStatus(Retiring.Wait());
	EXPECT_TRUE(Status == EPackageResourceReadStatus::Cancelled
		|| Status == EPackageResourceReadStatus::Success);
	EXPECT_TRUE(Resource->IsRetired());
	EXPECT_EQ(Resource->ReadRangeAsync(0, 1).Wait().error().Status,
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
	EXPECT_EQ(First.Wait()->GetBytes().data(), Copy.Wait()->GetBytes().data());
	EXPECT_TRUE(Second.Wait());
	WaitTask(Cpu);

	auto CancelResource = std::make_shared<FTestPackageResource>(true);
	auto Canceled = CancelResource->ReadRangeAsync(0, 4);
	ASSERT_TRUE(CancelResource->Started.WaitFor(1.0));
	Canceled.Cancel();
	CancelResource->Release.Trigger();
	auto Recovery = FPackageResourceRequest::Transform(Canceled, [](FPackageResourceReadResult Value) {
		EXPECT_EQ(EPackageResourceReadStatus::Cancelled, GetPackageResourceReadStatus(Value));
		return FPackageResourceReadResult{FSharedByteBuffer{}};
	});
	EXPECT_TRUE(Recovery.Wait());
}

TEST(FPackageResourceTests, SubmissionAfterSchedulerClosureIsALifecycleViolation)
{
	ShutdownTaskScheduler(true);
	EXPECT_DEATH({
		auto Resource = std::make_shared<FTestPackageResource>();
		(void)Resource->ReadRangeAsync(0, 4);
	}, "");
	EXPECT_DEATH({
		(void)FPackageResourceRequest::Transform(
			FPackageResourceRequest::Completed(FSharedByteBuffer{}),
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
	EXPECT_EQ(Runnable.Result.error().Status, EPackageResourceReadStatus::IoError);
	EXPECT_EQ(Runnable.Result.error().Reason, EPackageResourceReadReason::WaitRejected);
	EXPECT_TRUE(Runnable.Result.error().WaitStatus);
	EXPECT_FALSE(Request.IsReady());
	Resource->Release.Trigger();
	const auto Result = Request.Wait();
	ASSERT_TRUE(Result);
	EXPECT_EQ(Result->GetSize(), 4u);
	EXPECT_EQ(Result->GetBytes().front(), std::byte{0x31});
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
	EXPECT_TRUE(std::ranges::equal(Snapshot.GetPayload().Wait()->GetBytes(), Bytes));

	auto Resource = std::make_shared<FTestPackageResource>();
	FEditorBulkData PackageBacked;
	std::string Error;
	{
		auto ValueResult = FEditorBulkData::TryCreatePackageBacked(FGuid{5, 6, 7, 8}, FXxHash128::HashBuffer(
			Durin::FByteBuffer(4, std::byte{0})), 4, {.Resource = Resource, .StoredSize = 4});
		ASSERT_TRUE(ValueResult);
		PackageBacked = std::move(*ValueResult);
	}
	EXPECT_FALSE(PackageBacked.IsMemoryResident());
	EXPECT_EQ(PackageBacked.GetPayloadSize(), 4u);
	EXPECT_FALSE(PackageBacked.GetPayloadId().IsZero());
	EXPECT_TRUE(PackageBacked.GetPayload().Wait());
	Resource->Retire();
	EXPECT_EQ(PackageBacked.GetPayload().Wait().error().Status, EPackageResourceReadStatus::Retired);
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
		if (!Payload || Payload->GetSize() != Snapshot.GetPayloadSize()
			|| FXxHash128::HashBuffer(Payload->GetBytes()) != Snapshot.GetPayloadId()
			|| Snapshot.GetInstanceId() != FGuid{11, 12, 13, 14})
			Coherent.store(false, std::memory_order_release);
	}
	Writer.join();
	EXPECT_TRUE(Coherent.load(std::memory_order_acquire));
}

TEST(FEditorBulkDataTests, RejectedPackageSourceRetainsIdentityCauseAndOriginalPayload)
{
	FEditorBulkData Value(FGuid{1, 2, 3, 4});
	const auto Bytes = MakeBytes({9, 8});
	ASSERT_TRUE(Value.UpdatePayload(Bytes));
	const FGuid OriginalInstance = Value.GetInstanceId();
	const FXxHash128 OriginalContent = Value.GetPayloadId();
	const FGuid RejectedInstance{5, 6, 7, 8};
	const FXxHash128 RejectedContent = FXxHash128::HashBuffer(MakeBytes({1, 2, 3, 4}));
	FEditorBulkDataSource Source{.Resource = std::make_shared<FTestPackageResource>(), .StoredSize = 4};
	auto Identity = FEditorBulkData::TryCreatePackageBacked({}, RejectedContent, 4, Source);
	if (Identity) { Value = std::move(*Identity); }
	EXPECT_EQ(Identity.error().Code, EEditorBulkDataError::InvalidInstanceIdentity);
	EXPECT_EQ(Identity.error().ContentId, RejectedContent);
	auto Content = FEditorBulkData::TryCreatePackageBacked(RejectedInstance, {}, 4, Source);
	if (Content) { Value = std::move(*Content); }
	EXPECT_EQ(Content.error().Code, EEditorBulkDataError::MissingContentIdentity);
	EXPECT_EQ(Content.error().InstanceId, RejectedInstance);
	auto Size = FEditorBulkData::TryCreatePackageBacked(RejectedInstance, RejectedContent, 3, Source);
	if (Size) { Value = std::move(*Size); }
	EXPECT_EQ(Size.error().Code, EEditorBulkDataError::LogicalSizeMismatch);
	EXPECT_EQ(Size.error().Actual, 3u);
	EXPECT_EQ(Size.error().Expected, 4u);
	Source.SegmentOffset = 4;
	auto Range = FEditorBulkData::TryCreatePackageBacked(RejectedInstance, RejectedContent, 4, Source);
	if (Range) { Value = std::move(*Range); }
	Source = {};
	EXPECT_EQ(Range.error().Code, EEditorBulkDataError::InvalidRange);
	EXPECT_EQ(Range.error().InstanceId, RejectedInstance);
	EXPECT_EQ(Range.error().ContentId, RejectedContent);
	ASSERT_TRUE(Range.error().RangeCause);
	EXPECT_EQ(Range.error().RangeCause->Code, EPackageResourceRangeError::OutsideSegment);
	EXPECT_EQ(Range.error().RangeCause->SegmentOffset, 4u);
	EXPECT_EQ(Range.error().RangeCause->SegmentExtent, 4u);
	EXPECT_EQ(Value.GetInstanceId(), OriginalInstance);
	EXPECT_EQ(Value.GetPayloadId(), OriginalContent);
	EXPECT_EQ(Value.GetPayloadSize(), 2u);
	EXPECT_TRUE(Value.IsMemoryResident());
	EXPECT_TRUE(std::ranges::equal(Value.GetPayload().Wait()->GetBytes(), Bytes));
}

TEST(FEditorBulkDataTests, RequestsAndFailedReplacementConserveCapturedState)
{
	auto Resource = std::make_shared<FTestPackageResource>(true);
	FEditorBulkData Value;
	std::string Error;
	{
		auto ValueResult = FEditorBulkData::TryCreatePackageBacked(FGuid{21, 22, 23, 24}, FXxHash128::HashBuffer(
			Durin::FByteBuffer(4, std::byte{0})), 4, {.Resource = Resource, .StoredSize = 4});
		ASSERT_TRUE(ValueResult);
		Value = std::move(*ValueResult);
	}
	FPackageResourceRequest Captured = Value.GetPayload();
	ASSERT_TRUE(Resource->Started.WaitFor(1.0));
	const std::array Replacement{std::byte{4}, std::byte{3}};
	ASSERT_TRUE(Value.UpdatePayload(Replacement));
	EXPECT_FALSE(Captured.IsReady());
	Resource->Release.Trigger();
	const FPackageResourceReadResult Original = Captured.Wait();
	ASSERT_TRUE(Original);
	EXPECT_EQ(Original->GetSize(), 4u);
	EXPECT_TRUE(std::ranges::all_of(
		Original->GetBytes(), [](std::byte Byte) { return Byte == std::byte{0}; }));

	const FGuid InstanceId = Value.GetInstanceId();
	const FXxHash128 ContentId = Value.GetPayloadId();
	EXPECT_EQ(Value.GetInstanceId(), InstanceId);
	EXPECT_EQ(Value.GetPayloadId(), ContentId);
	EXPECT_TRUE(std::ranges::equal(Value.GetPayload().Wait()->GetBytes(), Replacement));
}

TEST(FPackageResourceRangeTests, SharesBoundedStorageFactsAcrossEditorAndRuntimeBulk)
{
	auto Resource = std::make_shared<FTestPackageResource>();
	const FPackageResourceRange Range{.Resource = Resource, .StoredSize = 4};
	std::string Error;
	EXPECT_TRUE(ValidatePackageResourceRange(Range, 4));
	FPackageResourceRange Invalid = Range;
	Invalid.StorageFlags = 1;
	EXPECT_FALSE(ValidatePackageResourceRange(Invalid, 4));

	FEditorBulkData Editor;
	{
		auto ValueResult = FEditorBulkData::TryCreatePackageBacked(FGuid{31, 32, 33, 34}, FXxHash128::HashBuffer(
			Durin::FByteBuffer(4, std::byte{0})), 4, Range);
		ASSERT_TRUE(ValueResult);
		Editor = std::move(*ValueResult);
	}
	FBulkData Runtime;
	{
		auto ValueResult = FBulkData::TryAttach({.LogicalSize = 4, .Range = Range});
		ASSERT_TRUE(ValueResult);
		Runtime = std::move(*ValueResult);
	}
	EXPECT_EQ(Editor.GetPayloadSize(), Runtime.GetMetadata().LogicalSize);
	EXPECT_EQ(Runtime.GetMetadata().Range.Resource, Resource);
	EXPECT_FALSE(Editor.IsMemoryResident());
}

TEST(FPackageResourceRangeTests, FailuresOwnRangeFactsAndDistinguishBounds)
{
	auto Resource = std::make_shared<FTestPackageResource>();
	FPackageResourceRange Range{.Resource = Resource, .StoredSize = 4};
	EXPECT_EQ(ValidatePackageResourceRange({}, 4).error().Code,
		EPackageResourceRangeError::MissingResource);
	Range.StorageFlags = 7;
	auto Flags = ValidatePackageResourceRange(Range, 4);
	Range.StorageFlags = 0;
	EXPECT_EQ(Flags.error().Code, EPackageResourceRangeError::UnsupportedFlags);
	EXPECT_EQ(Flags.error().StorageFlags, 7u);
	EXPECT_EQ(Flags.error().SegmentExtent, 4u);
	auto Limit = ValidatePackageResourceRange(Range, 3);
	EXPECT_EQ(Limit.error().Code, EPackageResourceRangeError::SizeLimit);
	EXPECT_EQ(Limit.error().StoredSize, 4u);
	EXPECT_EQ(Limit.error().MaximumStoredSize, 3u);
	Range.Alignment = 3;
	EXPECT_EQ(ValidatePackageResourceRange(Range, 4).error().Code,
		EPackageResourceRangeError::InvalidAlignment);
	Range.Alignment = 2;
	Range.SegmentOffset = 1;
	EXPECT_EQ(ValidatePackageResourceRange(Range, 4).error().Code,
		EPackageResourceRangeError::MisalignedOffset);
	Range.SegmentOffset = 2;
	auto Bounds = ValidatePackageResourceRange(Range, 4);
	Range = {};
	Resource.reset();
	EXPECT_EQ(Bounds.error().Code, EPackageResourceRangeError::OutsideSegment);
	EXPECT_EQ(Bounds.error().SegmentOffset, 2u);
	EXPECT_EQ(Bounds.error().StoredSize, 4u);
	EXPECT_EQ(Bounds.error().SegmentExtent, 4u);
}

TEST(FBulkDataTests, FailedAttachmentRetainsCauseAndExistingDetachedPayload)
{
	FBulkData Value;
	const auto Bytes = MakeBytes({5, 6});
	{
		auto ValueResult = FBulkData::TryCreateDetached(Bytes);
		ASSERT_TRUE(ValueResult);
		Value = std::move(*ValueResult);
	}
	FBulkDataMetadata Metadata{.LogicalSize = 4,
		.Range = {.Resource = std::make_shared<FTestPackageResource>(),
			.SegmentOffset = 4, .StoredSize = 4}};
	auto Bounds = FBulkData::TryAttach(Metadata);
	if (Bounds) { Value = std::move(*Bounds); }
	EXPECT_EQ(Bounds.error().Code, EBulkDataError::InvalidRange);
	ASSERT_TRUE(Bounds.error().RangeCause);
	EXPECT_EQ(Bounds.error().RangeCause->Code, EPackageResourceRangeError::OutsideSegment);
	EXPECT_EQ(Bounds.error().RangeCause->SegmentOffset, 4u);
	Metadata.LogicalSize = 3;
	auto Mismatch = FBulkData::TryAttach(Metadata);
	if (Mismatch) { Value = std::move(*Mismatch); }
	EXPECT_EQ(Mismatch.error().Code, EBulkDataError::LogicalSizeMismatch);
	EXPECT_EQ(Mismatch.error().Actual, 3u);
	EXPECT_EQ(Mismatch.error().Expected, 4u);
	Metadata.LogicalSize = Metadata.Range.StoredSize = MaximumBulkDataBytes + 1;
	auto Limit = FBulkData::TryAttach(Metadata);
	if (Limit) { Value = std::move(*Limit); }
	EXPECT_EQ(Limit.error().Code, EBulkDataError::LogicalSizeLimit);
	EXPECT_EQ(Limit.error().Actual, MaximumBulkDataBytes + 1);
	EXPECT_EQ(Limit.error().Expected, MaximumBulkDataBytes);
	EXPECT_EQ(Value.GetState(), EBulkDataState::Detached);
	EXPECT_EQ(Value.GetMetadata().LogicalSize, 2u);
	auto Read = Value.AcquireRead();
	ASSERT_TRUE(Read);
	EXPECT_TRUE(std::ranges::equal(Read.Lock.GetBytes(), Bytes));
}

TEST(FBulkDataTests, ScopedLocksReleaseOnReturnAndRetainReplacedStorage)
{
	FBulkData Value;
	{
		auto ValueResult = FBulkData::TryCreateDetached(MakeBytes({1, 2}));
		ASSERT_TRUE(ValueResult);
		Value = std::move(*ValueResult);
	}
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
	ASSERT_FALSE(Empty);
	EXPECT_EQ(Empty.error().Code, EPackageResourceRegistrationError::EmptySegment);
	EXPECT_EQ(Empty.error().Path, std::filesystem::path("Empty.dasset"));
}

TEST(FBulkDataTests, ReadFailurePreservesPackageCauseAndSupportsExplicitRetry)
{
	auto Resource = std::make_shared<FFailingPackageResource>();
	FBulkData Value;
	{
		auto ValueResult = FBulkData::TryAttach({.LogicalSize = 4, .Range = {.Resource = Resource, .StoredSize = 4}});
		ASSERT_TRUE(ValueResult);
		Value = std::move(*ValueResult);
	}
	auto Failed = Value.AcquireRead();
	EXPECT_EQ(Failed.Status, EBulkReadStatus::ReadFailed);
	EXPECT_FALSE(Failed.Lock);
	EXPECT_EQ(Failed.Error.error().Status, EPackageResourceReadStatus::SegmentDigestMismatch);
	EXPECT_EQ(Failed.Error.error().Reason, EPackageResourceReadReason::ContentMismatch);
	EXPECT_EQ(Failed.Error.error().ActualDigest, (FXxHash128{7, 8}));
	EXPECT_EQ(Value.GetState(), EBulkDataState::Failed);
	Resource->bFail.store(false);
	auto Retry = Value.AcquireRead();
	ASSERT_TRUE(Retry);
	EXPECT_EQ(Retry.Status, EBulkReadStatus::Acquired);
	EXPECT_EQ(Retry.Lock.GetBytes()[0], std::byte{7});
}

TEST(FPackageResourceTests, BulkMetadataFailuresRetainOwnedFieldContext)
{
	FPackageBulkDataEntry Entry{.FieldIndex = 3, .ContentId = {1, 2}};
	auto Invalid = ValidatePackageBulkDataMetadata({}, std::span{&Entry, 1});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.error().Code, EPackageBulkDataError::NonCanonicalIndex);
	EXPECT_EQ(Invalid.error().Index, 0u);
	EXPECT_EQ(Invalid.error().Actual, 3u);
	EXPECT_EQ(Invalid.error().Expected, 1u);
	Entry.FieldIndex = 1;
	ASSERT_TRUE(ValidatePackageBulkDataMetadata({}, std::span{&Entry, 1}));
	ASSERT_TRUE(Invalid.error().Entry);
	EXPECT_EQ(Invalid.error().Entry->FieldIndex, 3u);
	auto Limit = ValidatePackageBulkDataMetadata({.Extent = PackageBulkDataMaximumSegmentBytes + 1}, {});
	EXPECT_EQ(Limit.error().Code, EPackageBulkDataError::SegmentLimit);
	EXPECT_EQ(Limit.error().Actual, PackageBulkDataMaximumSegmentBytes + 1);
	EXPECT_EQ(Limit.error().Expected, PackageBulkDataMaximumSegmentBytes);
	FPackageResourceManager Manager;
	const auto Registered = Manager.RegisterLoosePackage("/TypedBulk", "TypedBulk.dasset",
		{.Extent = 1, .Flags = 1}, {});
	EXPECT_FALSE(Registered);
	ASSERT_TRUE(Registered.error().BulkCause);
	EXPECT_EQ(Registered.error().BulkCause->Code, EPackageBulkDataError::UnsupportedFlags);
	EXPECT_EQ(Registered.error().BulkCause->Summary.Flags, 1u);
}

TEST(FPackageResourceTests, BulkPaddingFailureRetainsExactOffsetAfterSegmentDigestValidation)
{
	const uint64 Size = EditorBulkDataExternalThreshold + 1;
	const uint64 Offset = (Size + EditorBulkDataExternalAlignment - 1) & ~uint64(EditorBulkDataExternalAlignment - 1);
	FByteBuffer Bytes(static_cast<size_t>(Offset + Size));
	const auto Content = FXxHash128::HashBuffer(std::span(Bytes).first(static_cast<size_t>(Size)));
	std::array Entries{
		FPackageBulkDataEntry{.FieldIndex = 1, .Placement = EPackageBulkDataPlacement::External,
			.LogicalSize = Size, .StoredSize = Size, .Alignment = EditorBulkDataExternalAlignment, .ContentId = Content},
		FPackageBulkDataEntry{.FieldIndex = 2, .Placement = EPackageBulkDataPlacement::External,
			.LogicalSize = Size, .StoredSize = Size, .SegmentOffset = Offset,
			.Alignment = EditorBulkDataExternalAlignment, .ContentId = Content}};
	Bytes[static_cast<size_t>(Size)] = std::byte{17};
	const FPackageBulkSegmentSummary Summary{.Extent = Bytes.size(), .Digest = FXxHash128::HashBuffer(Bytes)};
	auto Invalid = ValidatePackageBulkDataSegment(Summary, Entries, Bytes);
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.error().Code, EPackageBulkDataError::NonzeroPadding);
	EXPECT_EQ(Invalid.error().Offset, Size);
	EXPECT_EQ(Invalid.error().Actual, 17u);
	EXPECT_EQ(Invalid.error().Index, 1u);
	ASSERT_TRUE(Invalid.error().Entry);
	EXPECT_EQ(Invalid.error().Entry->FieldIndex, 2u);
	Bytes[static_cast<size_t>(Size)] = std::byte{0};
	EXPECT_TRUE(ValidatePackageBulkDataSegment({.Extent = Bytes.size(), .Digest = FXxHash128::HashBuffer(Bytes)}, Entries, Bytes));
}

TEST(FEditorBulkStorageTests, NestedFailureOwnsObjectAndFieldRoute)
{
	FByteBuffer Payload;
	FCanonicalMemoryWriter Writer(Payload, EArchivePurpose::BulkData);
	std::string StructName = "FContainer", DeclaringType = "FContainer", FieldName = "Payload", Signature;
	uint64 FieldCount = 1, PayloadSize = 0;
	uint8 Kind = static_cast<uint8>(DurinCodeGen::EPropertyGenFlags::BulkData);
	Writer << StructName << FieldCount << DeclaringType << FieldName << Kind << Signature << PayloadSize;
	ASSERT_FALSE(Writer.IsError());
	FAssetPackageInspection Inspection;
	Inspection.Objects.push_back({.Id = 17, .ObjectPath = "/Tests/Container.Root",
		.Fields = {{.Name = "Source", .Kind = DurinCodeGen::EPropertyGenFlags::Struct,
			.Payload = Payload, .SourceFormatVersion = ObjectPackage::DastV10FormatVersion}}});
	auto Result = InspectEditorBulkDataStorageDescriptors(Inspection);
	ASSERT_FALSE(Result);
	Inspection = {};
	Payload.clear();
	EXPECT_EQ(Result.error().Code, EEditorBulkDataStorageError::InvalidDescriptor);
	EXPECT_EQ(Result.error().ObjectId, 17u);
	EXPECT_EQ(Result.error().ObjectPath, "/Tests/Container.Root");
	EXPECT_EQ(Result.error().FieldRoute, (std::vector<std::string>{"Source", "Payload"}));
	EXPECT_EQ(Result.error().Depth, 1u);
}

TEST(FEditorBulkStorageTests, RejectsVersionAndPreservesArchiveHeaderCause)
{
	FAssetPackageInspection Inspection;
	Inspection.Objects.push_back({.Id = 19, .Fields = {{.Name = "Payload",
		.Kind = DurinCodeGen::EPropertyGenFlags::BulkData, .SourceFormatVersion = 1}}});
	auto Version = InspectEditorBulkDataCompanionPaths("/Tests/Bad.dasset", Inspection);
	ASSERT_FALSE(Version);
	EXPECT_EQ(Version.error().Code, EEditorBulkDataStorageError::UnsupportedVersion);
	EXPECT_EQ(Version.error().SourceFormatVersion, 1u);
	EXPECT_EQ(Version.error().Path, std::filesystem::path("/Tests/Bad.dasset"));
	Inspection.Objects.front().Fields.front().Kind = DurinCodeGen::EPropertyGenFlags::Struct;
	auto Header = InspectEditorBulkDataStorageDescriptors(Inspection);
	ASSERT_FALSE(Header);
	EXPECT_EQ(Header.error().Code, EEditorBulkDataStorageError::InvalidStructHeader);
	ASSERT_TRUE(Header.error().ArchiveCode);
	EXPECT_EQ(*Header.error().ArchiveCode, EArchiveFailureCode::TruncatedPayload);
}

TEST(FEditorBulkStorageTests, OrphanInspectionRetainsFilesystemCauseAndCandidate)
{
	const auto PackagePath = std::filesystem::path(__FILE__) / "child.dasset";
	auto Expected = PackagePath;
	Expected.replace_extension(".dbulk");
	auto Result = InspectOrphanedEditorBulkDataCompanionPaths(PackagePath, {});
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, EEditorBulkDataStorageError::FileSystem);
	EXPECT_TRUE(Result.error().SystemError);
	EXPECT_EQ(Result.error().Path, Expected);
}

TEST(FPackageResourceTests, GenerationFailurePreservesUnownedBackupAndOwnsCause)
{
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "TypedResourceRecovery";
	std::filesystem::create_directories(Root);
	const auto Package = Root / "Generation.dasset";
	auto SegmentPath = Package;
	SegmentPath.replace_extension(".dbulk");
	auto BackupPath = SegmentPath;
	BackupPath += ".durin-backup";
	const FByteBuffer Bytes(EditorBulkDataExternalThreshold + 1, std::byte{0x23});
	const FPackageBulkDataEntry Entry{.FieldIndex = 1,
		.Placement = EPackageBulkDataPlacement::External, .LogicalSize = Bytes.size(),
		.StoredSize = Bytes.size(), .Alignment = EditorBulkDataExternalAlignment,
		.ContentId = FXxHash128::HashBuffer(Bytes)};
	const FPackageBulkSegmentSummary Summary{.Extent = Bytes.size(), .Digest = Entry.ContentId};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(MakeBytes({1, 2}), SegmentPath));
	FPackageResourceManager Manager;
	const auto Failed = Manager.RegisterLoosePackage("/Tests/TypedRecovery", Package, Summary, std::span{&Entry, 1});
	EXPECT_EQ(Failed.error().Code, EPackageResourceRegistrationError::InvalidGeneration);
	ASSERT_FALSE(Failed);
	ASSERT_TRUE(Failed.error().PrimaryCause);
	const auto* BulkFailure = std::get_if<FPackageBulkValidationFailure>(&*Failed.error().PrimaryCause);
	ASSERT_TRUE(BulkFailure);
	EXPECT_EQ(BulkFailure->Error.Code, EPackageBulkDataError::ExtentMismatch);
	EXPECT_EQ(BulkFailure->Error.Actual, 2u);
	EXPECT_EQ(BulkFailure->Error.Expected, Bytes.size());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, BackupPath));
	const auto StillInvalid = Manager.RegisterLoosePackage("/Tests/TypedRecovery", Package, Summary, std::span{&Entry, 1});
	EXPECT_EQ(StillInvalid.error().Code, EPackageResourceRegistrationError::InvalidGeneration);
	EXPECT_TRUE(std::filesystem::exists(BackupPath));
	FByteBuffer Unchanged;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Unchanged, SegmentPath));
	EXPECT_EQ(Unchanged, MakeBytes({1, 2}));
	// Repair is explicit; registration only validates and publishes the resource.
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, SegmentPath));
	const auto Recovered = Manager.RegisterLoosePackage("/Tests/TypedRecovery", Package, Summary, std::span{&Entry, 1});
	ASSERT_TRUE(Recovered) << FormatPackageResourceRegistrationError(Recovered.error());
	EXPECT_TRUE(std::filesystem::exists(BackupPath));
	const auto Read = (*Recovered)->ReadRange(0, Bytes.size());
	ASSERT_TRUE(Read);
	EXPECT_TRUE(std::ranges::equal(Read->GetBytes(), Bytes));
	EXPECT_EQ(BulkFailure->Error.Actual, 2u);
	EXPECT_EQ(BulkFailure->Path, SegmentPath);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(MakeBytes({1}), SegmentPath));
	const auto Changed = (*Recovered)->ReadRange(0, 4);
	EXPECT_EQ(Changed.error().Status, EPackageResourceReadStatus::TruncatedSegment);
	EXPECT_EQ(Changed.error().Reason, EPackageResourceReadReason::ChangedBeforeRead);
	EXPECT_EQ(Changed.error().Path, SegmentPath);
	EXPECT_EQ(Changed.error().Actual, 1u);
	EXPECT_EQ(Changed.error().Expected, Bytes.size());
	EXPECT_EQ(Changed.error().Size, 4u);
}

TEST(FPackageResourceTests, RejectedReadOwnsRequestedBoundsWithoutAdmittingIo)
{
	auto Resource = std::make_shared<FTestPackageResource>();
	const auto Read = Resource->ReadRange(3, 2);
	EXPECT_EQ(Read.error().Status, EPackageResourceReadStatus::InvalidRange);
	EXPECT_EQ(Read.error().Reason, EPackageResourceReadReason::InvalidRange);
	EXPECT_EQ(Read.error().Offset, 3u);
	EXPECT_EQ(Read.error().Size, 2u);
	EXPECT_EQ(Read.error().Extent, 4u);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	Resource.reset();
	EXPECT_EQ(Read.error().Extent, 4u);
	const auto Invalid = FPackageResourceRequest{}.Wait();
	EXPECT_EQ(Invalid.error().Reason, EPackageResourceReadReason::InvalidRequest);
}

#include <gtest/gtest.h>

#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "Asset/PackageBulkData.h"
#include "Asset/PackageResource.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "Asset/PackageBulkDataWire.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	auto MakeBytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Bytes;
		Bytes.reserve(Values.size());
		for (const uint8 Value : Values) Bytes.push_back(static_cast<std::byte>(Value));
		return Bytes;
	}

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
				.Buffer = FSharedByteBuffer::Take(std::vector<std::byte>(Size))};
		}
	};
}

TEST(FPackageBulkDataWireTests, DirectoryV2MatchesGoldenBytes)
{
	const FPackageBulkDataEntry Entry{
		.FieldIndex = 1,
		.Placement = EPackageBulkDataPlacement::Inline,
		.LogicalSize = 3,
		.StoredSize = 3,
		.Alignment = 1,
		.ContentId = {0x0102030405060708ull, 0x1112131415161718ull}};
	std::vector<std::byte> Bytes;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Private::EncodePackageBulkDataDirectory(
		std::span{&Entry, 1}, Bytes, &Error)) << Error;
	const auto ToHex = [](std::span<const std::byte> Value) {
		constexpr char Digits[] = "0123456789abcdef";
		std::string Result;
		Result.reserve(Value.size() * 2);
		for (const std::byte Byte : Value)
		{
			const uint8 Number = std::to_integer<uint8>(Byte);
			Result.push_back(Digits[Number >> 4]);
			Result.push_back(Digits[Number & 0xf]);
		}
		return Result;
	};
	EXPECT_EQ(ToHex(Bytes),
		"02000000480000000100000000000000"
		"01000000000000000000000000000000"
		"03000000000000000300000000000000"
		"00000000000000000100000000000000"
		"08070605040302011817161514131211"
		"0000000000000000");

	std::vector<FPackageBulkDataEntry> Decoded;
	ASSERT_TRUE(Durin::Asset::Private::DecodePackageBulkDataDirectory(Bytes, Decoded, &Error)) << Error;
	ASSERT_EQ(Decoded.size(), 1u);
	EXPECT_EQ(Decoded.front(), Entry);
}

TEST(FPackageBulkDataWireTests, RejectsInvalidMetadataAndRawSegments)
{
	const uint64 Size = EditorBulkDataExternalThreshold + 1;
	std::vector<std::byte> Segment(static_cast<size_t>(Size), std::byte{0x5a});
	FPackageBulkDataEntry Entry{
		.FieldIndex = 1,
		.Placement = EPackageBulkDataPlacement::External,
		.LogicalSize = Size,
		.StoredSize = Size,
		.Alignment = EditorBulkDataExternalAlignment,
		.ContentId = FXxHash128::HashBuffer(Segment)};
	FPackageBulkSegmentSummary Summary{
		.Extent = Size,
		.Digest = FXxHash128::HashBuffer(Segment)};
	std::string Error;
	ASSERT_TRUE(ValidatePackageBulkDataSegment(
		Summary, std::span{&Entry, 1}, Segment, &Error)) << Error;

	const auto RejectMetadata = [&](auto Mutate) {
		FPackageBulkDataEntry Candidate = Entry;
		FPackageBulkSegmentSummary CandidateSummary = Summary;
		Mutate(Candidate, CandidateSummary);
		EXPECT_FALSE(ValidatePackageBulkDataMetadata(
			CandidateSummary, std::span{&Candidate, 1}, &Error));
	};
	RejectMetadata([](auto& Value, auto&) { Value.FieldIndex = 2; });
	RejectMetadata([](auto& Value, auto&) { Value.StorageFlags = 1; });
	RejectMetadata([](auto& Value, auto&) { ++Value.StoredSize; });
	RejectMetadata([](auto& Value, auto&) { Value.Alignment = 8; });
	RejectMetadata([](auto& Value, auto&) { Value.SegmentOffset = 16; });
	RejectMetadata([](auto&, auto& Value) { ++Value.Extent; });
	RejectMetadata([](auto&, auto& Value) { Value.Digest = {}; });

	auto Corrupt = Segment;
	Corrupt.back() ^= std::byte{1};
	EXPECT_FALSE(ValidatePackageBulkDataSegment(
		Summary, std::span{&Entry, 1}, Corrupt, &Error));
	Corrupt = Segment;
	Corrupt.pop_back();
	EXPECT_FALSE(ValidatePackageBulkDataSegment(
		Summary, std::span{&Entry, 1}, Corrupt, &Error));

	std::vector<std::byte> TwoSegment(
		static_cast<size_t>(Size * 2 + 15), std::byte{0});
	std::ranges::fill(std::span(TwoSegment).first(static_cast<size_t>(Size)), std::byte{0x11});
	std::ranges::fill(std::span(TwoSegment).subspan(
		static_cast<size_t>(Size + 15), static_cast<size_t>(Size)), std::byte{0x22});
	std::array Entries{
		Entry,
		FPackageBulkDataEntry{
			.FieldIndex = 2,
			.Placement = EPackageBulkDataPlacement::External,
			.LogicalSize = Size,
			.StoredSize = Size,
			.SegmentOffset = Size + 15,
			.Alignment = EditorBulkDataExternalAlignment,
			.ContentId = {9, 10}}};
	Summary = {.Extent = TwoSegment.size(), .Digest = FXxHash128::HashBuffer(TwoSegment)};
	ASSERT_TRUE(ValidatePackageBulkDataSegment(Summary, Entries, TwoSegment, &Error)) << Error;
	TwoSegment[static_cast<size_t>(Size)] = std::byte{'D'};
	Summary.Digest = FXxHash128::HashBuffer(TwoSegment);
	EXPECT_FALSE(ValidatePackageBulkDataSegment(Summary, Entries, TwoSegment, &Error));

	std::vector<std::byte> Encoded;
	ASSERT_TRUE(Durin::Asset::Private::EncodePackageBulkDataDirectory(
		std::span{&Entry, 1}, Encoded, &Error)) << Error;
	Encoded[0] = std::byte{3};
	std::vector<FPackageBulkDataEntry> Decoded;
	EXPECT_FALSE(Durin::Asset::Private::DecodePackageBulkDataDirectory(Encoded, Decoded, &Error));
	Encoded[0] = std::byte{2};
	Encoded.back() = std::byte{1};
	EXPECT_FALSE(Durin::Asset::Private::DecodePackageBulkDataDirectory(Encoded, Decoded, &Error));
}

TEST(FPackageBulkDataWireTests, BuildsCanonicalHeaderlessSegmentAtPlacementBoundary)
{
	const std::vector<std::byte> Empty;
	const std::vector<std::byte> Inline(
		static_cast<size_t>(EditorBulkDataExternalThreshold), std::byte{0x11});
	const std::vector<std::byte> ExternalA(
		static_cast<size_t>(EditorBulkDataExternalThreshold + 1), std::byte{0x22});
	const std::vector<std::byte> ExternalB(
		static_cast<size_t>(EditorBulkDataExternalThreshold + 3), std::byte{0x33});
	const uint64 OffsetB = (ExternalA.size() + EditorBulkDataExternalAlignment - 1)
		& ~uint64(EditorBulkDataExternalAlignment - 1);
	const auto Payload = [](FGuid Id, const std::vector<std::byte>& Bytes,
		EEditorBulkDataStorageKind Kind, uint64 Offset, uint32 Alignment) {
		return FEditorBulkDataStoragePayload{
			.Descriptor = {.PayloadId = Id, .LogicalByteCount = Bytes.size(),
				.StoredByteCount = Bytes.size(), .ContentHash = FXxHash128::HashBuffer(Bytes),
				.StorageKind = Kind, .SegmentOffset = Offset, .Alignment = Alignment},
			.Buffer = FSharedByteBuffer::Copy(Bytes)};
	};
	const std::array Payloads{
		Payload({1, 1, 1, 1}, Empty, EEditorBulkDataStorageKind::Inline, 0, 1),
		Payload({2, 2, 2, 2}, Inline, EEditorBulkDataStorageKind::Inline, 0, 1),
		Payload({3, 3, 3, 3}, ExternalA, EEditorBulkDataStorageKind::External, 0,
			EditorBulkDataExternalAlignment),
		Payload({4, 4, 4, 4}, ExternalB, EEditorBulkDataStorageKind::External, OffsetB,
			EditorBulkDataExternalAlignment)};
	std::vector<std::byte> Segment;
	FPackageBulkSegmentSummary Summary;
	std::vector<FPackageBulkDataEntry> Entries;
	std::string Error;
	ASSERT_TRUE(BuildPackageBulkDataSegment(
		Payloads, Segment, Summary, Entries, &Error)) << Error;
	ASSERT_EQ(Entries.size(), Payloads.size());
	EXPECT_EQ(Entries[0].Placement, EPackageBulkDataPlacement::Inline);
	EXPECT_EQ(Entries[1].Placement, EPackageBulkDataPlacement::Inline);
	EXPECT_EQ(Entries[2].SegmentOffset, 0u);
	EXPECT_EQ(Entries[3].SegmentOffset, OffsetB);
	EXPECT_EQ(Summary.Extent, OffsetB + ExternalB.size());
	EXPECT_TRUE(std::ranges::all_of(
		std::span(Segment).subspan(ExternalA.size(), OffsetB - ExternalA.size()),
		[](std::byte Byte) { return Byte == std::byte{0}; }));
	EXPECT_TRUE(std::ranges::equal(
		std::span(Segment).first(ExternalA.size()), ExternalA));
	EXPECT_TRUE(std::ranges::equal(
		std::span(Segment).subspan(OffsetB), ExternalB));
}

TEST(FBulkDataTests, DefaultValueIsEmpty)
{
	FBulkData Value;
	EXPECT_EQ(Value.GetState(), EBulkDataState::Empty);
	EXPECT_FALSE(Value.HasData());
	EXPECT_EQ(Value.GetMetadata().LogicalSize, 0u);
}

TEST(FBulkDataTests, DetachedLocksResizeAndCopyOnWrite)
{
	const std::vector<std::byte> Bytes = MakeBytes({1, 2, 3, 4});
	FBulkData First;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreateDetached(Bytes, First, &Error)) << Error;
	FBulkData Second = First;
	std::span<const std::byte> Read;
	std::span<std::byte> Write;
	ASSERT_TRUE(First.LockReadOnly(Read, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(Read, Bytes));
	EXPECT_FALSE(First.LockReadWrite(Write, &Error));
	ASSERT_TRUE(First.UnlockReadOnly(&Error)) << Error;

	ASSERT_TRUE(Second.LockReadWrite(Write, &Error)) << Error;
	ASSERT_TRUE(Second.Resize(2, Write, &Error)) << Error;
	Write[0] = std::byte{9};
	ASSERT_TRUE(Second.UnlockWrite(&Error)) << Error;
	ASSERT_TRUE(First.LockReadOnly(Read, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(Read, Bytes));
	ASSERT_TRUE(First.UnlockReadOnly(&Error)) << Error;
	ASSERT_TRUE(Second.LockReadOnly(Read, &Error)) << Error;
	EXPECT_EQ(Read.size(), 2u);
	EXPECT_EQ(Read[0], std::byte{9});
	ASSERT_TRUE(Second.UnlockReadOnly(&Error)) << Error;
	EXPECT_FALSE(Second.Unload(&Error));
}

TEST(FPackageResourceTests, LoadsUnloadsAndRetiresAttachedBulkData)
{
	const uint64 Size = EditorBulkDataExternalThreshold + 1;
	std::vector<std::byte> Segment(static_cast<size_t>(Size), std::byte{0x6a});
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
	ASSERT_TRUE(Manager.RegisterLoosePackage(
		"/Tests/Range", PackagePath, Summary, std::span{&Entry, 1}, Handle, &Error)) << Error;
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
	std::span<const std::byte> Read;
	ASSERT_TRUE(Value.LockReadOnly(Read, &Error)) << Error;
	EXPECT_TRUE(std::ranges::equal(Read, Segment));
	ASSERT_TRUE(Value.UnlockReadOnly(&Error)) << Error;
	ASSERT_TRUE(Value.Unload(&Error)) << Error;
	Manager.RetirePackage("/Tests/Range");
	EXPECT_FALSE(Value.LockReadOnly(Read, &Error));
	EXPECT_EQ(Value.GetState(), EBulkDataState::Retired);
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
			std::vector<std::byte>(4, std::byte{0})), 4,
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
				? std::span<const std::byte>(Initial)
				: std::span<const std::byte>(Replacement)))
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
			std::vector<std::byte>(4, std::byte{0})), 4,
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
	EXPECT_FALSE(Value.ReplaceBytes(FGuid{}, Replacement));
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
			std::vector<std::byte>(4, std::byte{0})), 4, Range, Editor, &Error)) << Error;
	FBulkData Runtime;
	ASSERT_TRUE(FBulkData::TryAttach(
		{.LogicalSize = 4, .Range = Range}, Runtime, &Error)) << Error;
	EXPECT_EQ(Editor.GetPayloadSize(), Runtime.GetMetadata().LogicalSize);
	EXPECT_EQ(Runtime.GetMetadata().Range.Resource, Resource);
	EXPECT_FALSE(Editor.IsMemoryResident());
}

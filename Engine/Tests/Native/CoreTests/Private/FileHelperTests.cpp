#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto TestRoot(std::string_view Name) -> std::filesystem::path
	{
		static std::atomic_uint64_t Counter = 0;
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
			/ "AtomicFilePublication"
			/ std::format("{}-{:016x}", Name, Counter.fetch_add(1, std::memory_order_relaxed));
		std::filesystem::create_directories(Root);
		return Root;
	}

	auto TryReadBytes(const std::filesystem::path& Path, Durin::FByteBuffer& OutBytes) -> bool
	{
		std::ifstream File(Path, std::ios::binary);
		if (!File.is_open()) return false;
		File.seekg(0, std::ios::end);
		const std::streamsize Size = File.tellg();
		if (Size < 0) return false;
		File.seekg(0, std::ios::beg);
		OutBytes.resize(static_cast<size_t>(Size));
		File.read(reinterpret_cast<char*>(OutBytes.data()), Size);
		return !File.fail();
	}

	auto ReadBytes(const std::filesystem::path& Path) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Bytes;
		EXPECT_TRUE(TryReadBytes(Path, Bytes));
		return Bytes;
	}

	auto IsCompleteWriterPayload(
		const Durin::FByteBuffer& Bytes,
		std::span<const size_t> ExpectedSizes) -> bool
	{
		return std::ranges::find(ExpectedSizes, Bytes.size()) != ExpectedSizes.end()
			&& !Bytes.empty()
			&& std::ranges::all_of(Bytes, [Value = Bytes.front()](std::byte Byte) { return Byte == Value; });
	}

	auto PathLongerThan(
		const std::filesystem::path& Root,
		std::string_view FileName,
		size_t MinimumLength
	) -> std::filesystem::path
	{
		std::filesystem::path Parent = std::filesystem::absolute(Root).lexically_normal();
		const std::filesystem::path FileNamePath(FileName);
		for (size_t Index = 0; (Parent / FileNamePath).native().size() <= MinimumLength; ++Index)
		{
			Parent /= std::format("segment-{:04}-abcdefghijklmnop", Index);
		}
		return Parent / FileNamePath;
	}
}

TEST(FFileHelperTests, PublishesAndReplacesCompleteBytes)
{
	const std::filesystem::path Destination = TestRoot("Replace") / "Value.bin";
	const std::array First{std::byte{0x11}, std::byte{0x22}};
	const std::array Second{std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};

	Durin::FFileHelper::FAtomicFileError Error;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(First, Destination, &Error)) << Error.ToString();
	EXPECT_EQ(ReadBytes(Destination), std::vector(First.begin(), First.end()));

	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Second, Destination, &Error)) << Error.ToString();
	EXPECT_EQ(ReadBytes(Destination), std::vector(Second.begin(), Second.end()));
	Durin::FByteBuffer Loaded;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Loaded, Destination));
	EXPECT_EQ(Loaded, std::vector(Second.begin(), Second.end()));
}

TEST(FFileHelperTests, CopiesFilesThroughAtomicReplacement)
{
	const std::filesystem::path Root = TestRoot("CopyReplace");
	const std::filesystem::path Source = Root / "Source.bin";
	const std::filesystem::path Destination = Root / "Destination.bin";
	Durin::FByteBuffer Expected(3 * 64 * 1024 + 17);
	for (size_t Index = 0; Index < Expected.size(); ++Index)
		Expected[Index] = static_cast<std::byte>((Index * 53 + 7) & 0xff);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Expected, Source));
	const std::array Previous{std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Previous, Destination));

	Durin::FFileHelper::FAtomicFileError Error;
	ASSERT_TRUE(Durin::FFileHelper::CopyFileAtomically(Source, Destination, &Error))
		<< Error.ToString();
	EXPECT_EQ(ReadBytes(Source), Expected);
	EXPECT_EQ(ReadBytes(Destination), Expected);

	EXPECT_FALSE(Durin::FFileHelper::CopyFileAtomically(
		Root / "Missing.bin", Destination, &Error));
	EXPECT_EQ(ReadBytes(Destination), Expected);
}

TEST(FFileHelperTests, HashesFilesIncrementallyAcrossBufferBoundaries)
{
	const std::filesystem::path FilePath = TestRoot("StreamingHash") / "Value.bin";
	Durin::FByteBuffer Bytes(3 * 64 * 1024 + 17);
	for (size_t Index = 0; Index < Bytes.size(); ++Index)
		Bytes[Index] = static_cast<std::byte>((Index * 37) & 0xff);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, FilePath));
	Durin::FXxHash128 Actual;
	std::error_code Error;
	ASSERT_TRUE(Durin::FFileHelper::HashFileXx128(FilePath, Actual, Error));
	EXPECT_FALSE(Error);
	EXPECT_EQ(Actual, Durin::FXxHash128::HashBuffer(Bytes));

	EXPECT_FALSE(Durin::FFileHelper::HashFileXx128(
		FilePath.parent_path() / "Missing.bin", Actual, Error));
	EXPECT_TRUE(Error);
}

TEST(FFileHelperTests, RandomReadHandleSupportsExactOutOfOrderRanges)
{
	const std::filesystem::path FilePath = TestRoot("RandomRead") / "Value.bin";
	Durin::FByteBuffer Bytes(257);
	for (size_t Index = 0; Index < Bytes.size(); ++Index)
		Bytes[Index] = static_cast<std::byte>((Index * 29) & 0xff);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, FilePath));

	Durin::FFileHelper::FFileIoError Error;
	auto Handle = Durin::FFileHelper::OpenRead(FilePath, &Error);
	ASSERT_NE(Handle, nullptr) << Error.ToString();
	EXPECT_EQ(Handle->GetSize(), Bytes.size());
	std::array<std::byte, 13> Tail{};
	ASSERT_TRUE(Handle->ReadAt(201, Tail, &Error)) << Error.ToString();
	EXPECT_TRUE(std::ranges::equal(Tail, std::span(Bytes).subspan(201, Tail.size())));
	std::array<std::byte, 17> Head{};
	ASSERT_TRUE(Handle->ReadAt(3, Head, &Error)) << Error.ToString();
	EXPECT_TRUE(std::ranges::equal(Head, std::span(Bytes).subspan(3, Head.size())));
	EXPECT_TRUE(Handle->ReadAt(Handle->GetSize(), {}, &Error));
	EXPECT_FALSE(Error.NativeError);
}

TEST(FFileHelperTests, RandomReadHandleRejectsInvalidRangesDeterministically)
{
	const std::filesystem::path FilePath = TestRoot("RandomReadBounds") / "Value.bin";
	const std::array Bytes{std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, FilePath));
	auto Handle = Durin::FFileHelper::OpenRead(FilePath);
	ASSERT_NE(Handle, nullptr);
	std::array<std::byte, 1> Output{};
	Durin::FFileHelper::FFileIoError Error;
	EXPECT_FALSE(Handle->ReadAt(3, Output, &Error));
	EXPECT_EQ(Error.Operation, Durin::FFileHelper::EFileIoOperation::Read);
	EXPECT_EQ(Error.Offset, 3);
	EXPECT_EQ(Error.Size, 1);
	EXPECT_EQ(Error.NativeError, std::make_error_code(std::errc::result_out_of_range));
	EXPECT_FALSE(Handle->ReadAt(std::numeric_limits<uint64>::max(), Output, &Error));

	Error = {};
	EXPECT_EQ(Durin::FFileHelper::OpenRead(FilePath.parent_path() / "Missing.bin", &Error), nullptr);
	EXPECT_EQ(Error.Operation, Durin::FFileHelper::EFileIoOperation::OpenRead);
	EXPECT_TRUE(Error.NativeError);
}

TEST(FFileHelperTests, RandomReadHandleSupportsSparseFilesBeyondFourGiB)
{
	const std::filesystem::path FilePath = TestRoot("SparseRandomRead") / "Value.bin";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Durin::FByteView{}, FilePath));
	constexpr uint64 SparseSize = (uint64{4} << 30) + 37;
	std::error_code Error;
	std::filesystem::resize_file(FilePath, SparseSize, Error);
	if (Error) GTEST_SKIP() << "Sparse/large fixture is unavailable: " << Error.message();
	auto Handle = Durin::FFileHelper::OpenRead(FilePath);
	ASSERT_NE(Handle, nullptr);
	EXPECT_EQ(Handle->GetSize(), SparseSize);
	std::array<std::byte, 16> Tail;
	Tail.fill(std::byte{0xff});
	ASSERT_TRUE(Handle->ReadAt(SparseSize - Tail.size(), Tail));
	EXPECT_TRUE(std::ranges::all_of(Tail, [](std::byte Value) { return Value == std::byte{0}; }));
}

TEST(FFileHelperTests, EmptyFilesClearSuccessfulLoadResults)
{
	const std::filesystem::path FilePath = TestRoot("EmptyRead") / "Empty.bin";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Durin::FByteView{}, FilePath));

	Durin::FByteBuffer Bytes{std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, FilePath));
	EXPECT_TRUE(Bytes.empty());

	Durin::FByteBuffer RawBytes{std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(RawBytes, FilePath));
	EXPECT_TRUE(RawBytes.empty());

	std::vector<uint32> Words{0x11223344};
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Words, FilePath));
	EXPECT_TRUE(Words.empty());

	std::string Text = "stale text";
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToString(Text, FilePath.generic_string()));
	EXPECT_TRUE(Text.empty());
}

TEST(FFileHelperTests, LoadsExactTextBytesAndPreservesResultsOnFailure)
{
	const std::filesystem::path Root = TestRoot("TransactionalRead");
	const std::filesystem::path FilePath = Root / "Value.txt";
	const std::string Expected = "first\r\nsecond\n";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Expected)), FilePath));

	std::string Text = "stale text";
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToString(Text, FilePath.generic_string()));
	EXPECT_EQ(Text, Expected);

	const std::filesystem::path MissingPath = Root / "Missing.txt";
	Text = "preserved text";
	EXPECT_FALSE(Durin::FFileHelper::LoadFileToString(Text, MissingPath.generic_string()));
	EXPECT_EQ(Text, "preserved text");

	Durin::FByteBuffer Bytes{std::byte{0x11}, std::byte{0x22}};
	EXPECT_FALSE(Durin::FFileHelper::LoadFileToArray(Bytes, MissingPath));
	EXPECT_EQ(Bytes, (Durin::FByteBuffer{std::byte{0x11}, std::byte{0x22}}));
}

TEST(FFileHelperTests, ConcurrentWritersNeverExposePartialBytes)
{
	const std::filesystem::path Destination = TestRoot("Concurrent") / "Value.bin";
	constexpr size_t WriterCount = 6;
	constexpr size_t PublicationsPerWriter = 16;
	std::array<size_t, WriterCount + 1> PayloadSizes{};
	for (size_t Index = 0; Index < PayloadSizes.size(); ++Index)
		PayloadSizes[Index] = 64 * 1024 + Index * 4093;
	const std::vector Initial(PayloadSizes.front(), std::byte{0x01});
	Durin::FFileHelper::FAtomicFileError InitialError;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Initial, Destination, &InitialError))
		<< InitialError.ToString();

	std::atomic_bool bStart = false;
	std::atomic_bool bStopReader = false;
	std::atomic_bool bObservedPartial = false;
	std::atomic_bool bPublicationFailed = false;
	std::atomic_int PublicationErrorCode = 0;
	std::atomic_int PublicationOperation = 0;
	std::vector<std::thread> Writers;
	for (size_t WriterIndex = 0; WriterIndex < WriterCount; ++WriterIndex)
	{
		Writers.emplace_back([&, WriterIndex] {
			const std::vector Payload(
				PayloadSizes[WriterIndex + 1],
				static_cast<std::byte>(WriterIndex + 2));
			while (!bStart.load(std::memory_order_acquire)) std::this_thread::yield();
			for (size_t Publication = 0; Publication < PublicationsPerWriter; ++Publication)
			{
				Durin::FFileHelper::FAtomicFileError Error;
				if (!Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination, &Error))
				{
					PublicationErrorCode.store(Error.NativeError.value(), std::memory_order_release);
					PublicationOperation.store(static_cast<int>(Error.Operation), std::memory_order_release);
					bPublicationFailed.store(true, std::memory_order_release);
					return;
				}
			}
		});
	}

	std::thread Reader([&] {
		while (!bStart.load(std::memory_order_acquire)) std::this_thread::yield();
		while (!bStopReader.load(std::memory_order_acquire))
		{
			Durin::FByteBuffer Bytes;
			if (!Durin::FFileHelper::LoadFileToArray(Bytes, Destination)
				|| !IsCompleteWriterPayload(Bytes, PayloadSizes))
			{
				bObservedPartial.store(true, std::memory_order_release);
				return;
			}
		}
	});

	bStart.store(true, std::memory_order_release);
	for (std::thread& Writer : Writers) Writer.join();
	bStopReader.store(true, std::memory_order_release);
	Reader.join();

	EXPECT_FALSE(bPublicationFailed.load())
		<< "operation " << PublicationOperation.load() << ", native error " << PublicationErrorCode.load();
	EXPECT_FALSE(bObservedPartial.load());
	EXPECT_TRUE(IsCompleteWriterPayload(ReadBytes(Destination), PayloadSizes));
}

TEST(FFileHelperTests, FailedReplacementPreservesDestinationAndCleansTemporaryFile)
{
	const std::filesystem::path Root = TestRoot("Failure");
	const std::filesystem::path Destination = Root / "ExistingDestination";
	std::filesystem::create_directories(Destination);
	const std::array Payload{std::byte{0x44}};

	Durin::FFileHelper::FAtomicFileError Error;
	EXPECT_FALSE(Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination, &Error));
	EXPECT_EQ(Error.Operation, Durin::FFileHelper::EAtomicFileOperation::ReplaceDestination);
	EXPECT_NE(Error.NativeError.value(), 0);
	EXPECT_EQ(Error.Path, std::filesystem::absolute(Destination).lexically_normal());
	EXPECT_EQ(Error.PathLength, Error.Path.native().size());
	EXPECT_GT(Error.LongestComponentLength, 0);
	EXPECT_TRUE(std::filesystem::is_directory(Destination));

	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Root))
	{
		EXPECT_FALSE(Entry.path().filename().generic_string().starts_with(".durin-tmp-"));
	}
}

TEST(FFileHelperTests, SupportsStandardAndAtomicIoBeyondMaxPath)
{
	const std::filesystem::path Root = TestRoot("LongPath");
	const std::filesystem::path Destination = PathLongerThan(Root, "Value.bin", 300);
	ASSERT_TRUE(Destination.is_absolute());
	ASSERT_GT(Destination.native().size(), 260);

	const std::array First{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(First, Destination));
	EXPECT_TRUE(std::filesystem::is_regular_file(Destination));
	EXPECT_EQ(std::filesystem::file_size(Destination), First.size());

	Durin::FByteBuffer Loaded;
	ASSERT_TRUE(TryReadBytes(Destination, Loaded));
	EXPECT_EQ(Loaded, std::vector(First.begin(), First.end()));

	bool bFoundByTraversal = false;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Destination.parent_path()))
	{
		bFoundByTraversal |= Entry.path().filename() == Destination.filename();
	}
	EXPECT_TRUE(bFoundByTraversal);

	const std::array Second{std::byte{0x44}, std::byte{0x55}};
	Durin::FFileHelper::FAtomicFileError Error;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Second, Destination, &Error)) << Error.ToString();
	EXPECT_EQ(ReadBytes(Destination), std::vector(Second.begin(), Second.end()));

	std::error_code CleanupError;
	Durin::Testing::RemoveTestWorkDirectory(Root, CleanupError);
	EXPECT_FALSE(CleanupError);
}

TEST(FFileHelperTests, FixedTemporaryNameAvoidsHistoricalMaxPathInflation)
{
	const std::filesystem::path Root = std::filesystem::absolute(TestRoot("HistoricalTemporary")).lexically_normal();
	const std::string FileName = "Artifact.bin";
	const size_t ComponentLength = 259 - Root.native().size() - FileName.size() - 2;
	ASSERT_GT(ComponentLength, 0);
	ASSERT_LE(ComponentLength, 255);
	const std::filesystem::path Destination = Root / std::string(ComponentLength, 'd') / FileName;
	ASSERT_EQ(Destination.native().size(), 259);
	ASSERT_GT(Destination.native().size() + 4, 260);

	const std::array Payload{std::byte{0x71}, std::byte{0x72}};
	Durin::FFileHelper::FAtomicFileError Error;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination, &Error)) << Error.ToString();
	EXPECT_EQ(ReadBytes(Destination), std::vector(Payload.begin(), Payload.end()));
}

TEST(FFileHelperTests, OverlongComponentFailsWithPathMetricsAndNoOrphan)
{
	const std::filesystem::path Root = TestRoot("OverlongComponent");
	const std::filesystem::path Destination = Root / std::string(256, 'c');
	const std::array Payload{std::byte{0x61}};

	Durin::FFileHelper::FAtomicFileError Error;
	EXPECT_FALSE(Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination, &Error));
	EXPECT_EQ(Error.Operation, Durin::FFileHelper::EAtomicFileOperation::ReplaceDestination);
	EXPECT_NE(Error.NativeError.value(), 0);
	EXPECT_EQ(Error.LongestComponentLength, 256);
	EXPECT_NE(Error.ToString().find("longest component: 256"), std::string::npos);

	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Root))
	{
		EXPECT_FALSE(Entry.path().filename().generic_string().starts_with(".durin-tmp-"));
	}
}

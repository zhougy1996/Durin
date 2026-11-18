#include "HAL/PlatformFile.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FileFingerprintCache.h"
#include "Image/ImageDecoder.h"
#include "Json/Json.h"
#include "Yaml/Yaml.h"
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#endif
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

namespace
{
	struct FTestDirectoryVisitor final : Durin::IPlatformFile::FDirectoryVisitor
	{
		std::function<bool(std::string_view)> Filter;
		std::function<bool(const Durin::FFilePath&, bool)> Callback;
		std::set<std::string> Files;
		std::set<std::string> Directories;
		auto ShouldVisitLeafPathname(std::string_view Name) -> bool override
		{
			return !Filter || Filter(Name);
		}
		auto Visit(const Durin::FFilePath& Path, bool bIsDirectory) -> bool override
		{
			(bIsDirectory ? Directories : Files).insert(Path.filename().string());
			return !Callback || Callback(Path, bIsDirectory);
		}
	};
}

TEST(FFileManagerTests, DirectorySingleLayerAndRecursiveVisits)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryDepth");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "top"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "nested" / "deep" / "leaf"));
	FTestDirectoryVisitor Visitor;
	auto Result = IFileManager::Get().IterateDirectory(Root, Visitor);
	ASSERT_TRUE(Result);
	EXPECT_EQ(Visitor.Files, (std::set<std::string>{"top"}));
	EXPECT_EQ(Visitor.Directories, (std::set<std::string>{"nested"}));
	Visitor = {};
	Result = IFileManager::Get().IterateDirectoryRecursively(Root, Visitor);
	ASSERT_TRUE(Result);
	EXPECT_EQ(Visitor.Files, (std::set<std::string>{"top", "leaf"}));
	EXPECT_EQ(Visitor.Directories, (std::set<std::string>{"nested", "deep"}));
}

TEST(FFileManagerTests, QueriesDistinguishFilesDirectoriesAndFailures)
{
	using namespace Durin;
	auto& Files = IFileManager::Get();
	const auto Root = TestRoot("ManagerQueries");
	const auto Path = Root / "data";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}, std::byte{2}}, Path));
	EXPECT_TRUE(Files.FileExists(Path));
	EXPECT_FALSE(Files.DirectoryExists(Path));
	EXPECT_FALSE(Files.FileExists(Root));
	EXPECT_TRUE(Files.DirectoryExists(Root));
	EXPECT_EQ(Files.FileSize(Path), 2);
	EXPECT_EQ(Files.FileSize(Root), -1);
	EXPECT_EQ(Files.FileSize(Root / "missing"), -1);
	EXPECT_FALSE(Files.FileExists(Root / "missing"));
	EXPECT_FALSE(Files.DirectoryExists(Root / "missing"));
	EXPECT_FALSE(Files.MakeDirectory(Root / "parent" / "child"));
	EXPECT_TRUE(Files.MakeDirectory(Root / "parent" / "child", true));
	EXPECT_TRUE(Files.MakeDirectory(Root / "parent" / "child"));
	EXPECT_FALSE(Files.MakeDirectory(Path));
	EXPECT_FALSE(Files.MakeDirectory(Path / "child", true));
}

TEST(FFileManagerTests, CallableVisitorsPreserveDepthAndStop)
{
	using namespace Durin;
	const auto Root = TestRoot("ManagerCallable");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "nested" / "leaf"));
	for (bool bRecursive : {false, true})
	{
		size_t Count = 0;
		auto Visitor = [&](const FFilePath&, bool) { ++Count; return true; };
		auto& Files = IFileManager::Get();
		EXPECT_TRUE(bRecursive ? Files.IterateDirectoryRecursively(Root, Visitor) : Files.IterateDirectory(Root, Visitor));
		EXPECT_EQ(Count, bRecursive ? 2u : 1u);
		Count = 0;
		auto Stop = [&](const FFilePath&, bool) { ++Count; return false; };
		EXPECT_FALSE(bRecursive ? Files.IterateDirectoryRecursively(Root, Stop) : Files.IterateDirectory(Root, Stop));
		EXPECT_EQ(Count, 1u);
	}
}

TEST(FFileManagerTests, DirectoryFilterPrunesBeforeMetadata)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryFilter");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / ".hidden" / "excluded"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "kept" / "leaf"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "removed"));
	FTestDirectoryVisitor Visitor;
	Visitor.Filter = [&](std::string_view Name)
	{
		if (Name == "removed")
		{
			EXPECT_TRUE(std::filesystem::remove(Root / Name));
			return false;
		}
		return !Name.starts_with('.');
	};
	const auto Result = IFileManager::Get().IterateDirectoryRecursively(Root, Visitor);
	ASSERT_TRUE(Result);
	EXPECT_EQ(Visitor.Files, (std::set<std::string>{"leaf"}));
	EXPECT_EQ(Visitor.Directories, (std::set<std::string>{"kept"}));
}

TEST(FFileManagerTests, DirectoryVisitorStopsGlobally)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryStop");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "one" / "child"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "two" / "child"));
	for (bool bRecursive : {false, true})
	{
		FTestDirectoryVisitor Visitor;
		Visitor.Callback = [](const FFilePath&, bool) { return false; };
		const auto Result = bRecursive ? IFileManager::Get().IterateDirectoryRecursively(Root, Visitor)
			: IFileManager::Get().IterateDirectory(Root, Visitor);
		EXPECT_FALSE(Result);
		EXPECT_EQ(Visitor.Files.size() + Visitor.Directories.size(), 1u);
	}
}

TEST(FFileManagerTests, DirectoryMissingRootIsExplicitAndNotDirectoryIsAnError)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryMissing");
	FTestDirectoryVisitor Visitor;
	for (bool bRecursive : {false, true})
	{
		const auto Missing = Root / "missing";
		const auto Result = bRecursive ? IFileManager::Get().IterateDirectoryRecursively(Missing, Visitor)
			: IFileManager::Get().IterateDirectory(Missing, Visitor);
		ASSERT_FALSE(Result);
	}
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "file"));
	const auto Result = IFileManager::Get().IterateDirectory(Root / "file", Visitor);
	ASSERT_FALSE(Result);
	EXPECT_TRUE(Visitor.Files.empty());
}

TEST(FFileManagerTests, DirectoryDisappearingChildrenAreSkipped)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryErrors");
	for (const char* Name : {"removed1", "removed2", "kept"})
		ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / Name / "leaf"));
	FTestDirectoryVisitor Visitor;
	Visitor.Callback = [](const FFilePath& Path, bool bIsDirectory)
	{
		if (bIsDirectory && Path.filename().string().starts_with("removed"))
		{
			std::error_code Error;
			Durin::Testing::RemoveTestWorkDirectory(Path, Error);
			EXPECT_FALSE(Error);
		}
		return true;
	};
	const auto Result = IFileManager::Get().IterateDirectoryRecursively(Root, Visitor);
	EXPECT_TRUE(Result);
	EXPECT_EQ(Visitor.Files, (std::set<std::string>{"leaf"}));
}

TEST(FFileManagerTests, DirectoryMissingEntryDoesNotPreventVisitorStop)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryMetadataError");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "one"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "two"));
	FTestDirectoryVisitor Visitor;
	FFilePath Removed;
	Visitor.Filter = [&](std::string_view Name)
	{
		if (Removed.empty())
		{
			Removed = Root / Name;
			EXPECT_TRUE(std::filesystem::remove(Removed));
		}
		return true;
	};
	Visitor.Callback = [](const FFilePath&, bool) { return false; };
	const auto Result = IFileManager::Get().IterateDirectory(Root, Visitor);
	ASSERT_FALSE(Result);
	EXPECT_EQ(Visitor.Files.size(), 1u);
}

TEST(FFileManagerTests, DirectoryDoesNotFollowLinksOrCycles)
{
	using namespace Durin;
	const auto Root = TestRoot("DirectoryLinks");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Root / "real" / "leaf"));
	std::error_code Error;
	std::filesystem::create_directory_symlink(Root, Root / "real" / "cycle", Error);
	if (Error) GTEST_SKIP() << "Directory symlink fixture unavailable: " << Error.message();
	std::filesystem::create_symlink(Root / "absent", Root / "dangling", Error);
	ASSERT_FALSE(Error);
	FTestDirectoryVisitor Visitor;
	const auto Result = IFileManager::Get().IterateDirectoryRecursively(Root, Visitor);
	ASSERT_TRUE(Result);
	EXPECT_EQ(Visitor.Files, (std::set<std::string>{"leaf"}));
	EXPECT_EQ(Visitor.Directories, (std::set<std::string>{"real"}));
	Visitor = {};
	const auto LinkedRoot = IFileManager::Get().IterateDirectoryRecursively(Root / "real" / "cycle", Visitor);
	ASSERT_TRUE(LinkedRoot);
	EXPECT_TRUE(Visitor.Files.empty());
	EXPECT_TRUE(Visitor.Directories.empty());
}

TEST(FFileHelperTests, ReturnsValuesAndExactTextWithoutOutputParameters)
{
	using namespace Durin;
	static_assert(std::is_same_v<FFilePath, std::filesystem::path>);
	const FFilePath Path = TestRoot("ResultValues") / "Nested" / "Text.bin";
	const std::string Text("a\0b\r\n", 5);
	const auto Bytes = std::as_bytes(std::span(Text));
	auto Saved = FFileHelper::SaveArrayToFile(Bytes, Path);
	ASSERT_TRUE(Saved) << Saved.error().GetMessage();
	auto Loaded = FFileHelper::LoadFileToString(Path);
	ASSERT_TRUE(Loaded) << Loaded.error().GetMessage();
	EXPECT_EQ(*Loaded, Text);
	auto Hash = FFileHelper::HashFileXx128(Path);
	ASSERT_TRUE(Hash) << Hash.error().GetMessage();
	EXPECT_EQ(*Hash, FXxHash128::HashBuffer(Bytes));
	auto File = IPlatformFile::GetPlatformPhysical().OpenRead(Path, Durin::IPlatformFile::EOpenReadFlags::AllowWrite | Durin::IPlatformFile::EOpenReadFlags::AllowDelete);
	ASSERT_TRUE(File) << File.error().GetMessage();
	std::array<std::byte, 2> Part{};
	ASSERT_TRUE((*File)->ReadAt(reinterpret_cast<uint8*>((Part).data()), static_cast<int64>(std::span(Part).size_bytes()), static_cast<int64>(1)));
	EXPECT_EQ(Part[0], std::byte{0});
	EXPECT_EQ(Part[1], std::byte{'b'});
	auto Invalid = (*File)->ReadAt(reinterpret_cast<uint8*>((Part).data()), static_cast<int64>(std::span(Part).size_bytes()), static_cast<int64>(5));
	ASSERT_FALSE(Invalid);
	File->reset();
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Path));
	auto Empty = FFileHelper::LoadFileToArray(Path);
	ASSERT_TRUE(Empty);
	EXPECT_TRUE(Empty->empty());
}

#if defined(_WIN32)
TEST(FFileHelperTests, ReadWrappersPreserveSharingFailure)
{
	using namespace Durin;
	const FFilePath Path = TestRoot("WrapperDiagnostics") / "Locked.bin";
	const FByteBuffer Bytes{std::byte{1}, std::byte{2}, std::byte{3}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, Path));
	const HANDLE Native = CreateFileW(Path.c_str(), GENERIC_READ, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(Native, INVALID_HANDLE_VALUE);
	std::unique_ptr<void, decltype(&CloseHandle)> Lock(Native, &CloseHandle);
	FJsonDocument Json;

	const auto JsonResult = Json.LoadFromFile(Path);
	ASSERT_FALSE(JsonResult);
	FYamlDocument Yaml;

	const auto YamlResult = Yaml.LoadFromFile(Path);
	ASSERT_FALSE(YamlResult);
	FFileFingerprintCache Cache;

	const auto Fingerprint = Cache.Get(Path);
	ASSERT_FALSE(Fingerprint);
	EXPECT_EQ(Cache.GetContentReadCount(), 0u);
	auto Decoded = Image::DecodeImageFromFile(Path.string());
	ASSERT_FALSE(Decoded);
	EXPECT_EQ(Decoded.error().Code, Image::EImageDecodeError::FileRead);
	ASSERT_TRUE(Decoded.error().FileError);
	EXPECT_EQ(Decoded.error().FileError->NativeError.value(), ERROR_SHARING_VIOLATION);
	for (const auto& Diagnostic : {JsonResult.error().ToString(), YamlResult.error().ToString(), Fingerprint.error().GetMessage(),
		Image::ToString(Decoded.error())})
	{
		EXPECT_NE(Diagnostic.find("for reading"), std::string::npos);
		EXPECT_NE(Diagnostic.find("Locked.bin"), std::string::npos);
	}
	Lock.reset();
	const auto Unlocked = Cache.Get(Path);
	ASSERT_TRUE(Unlocked);
	EXPECT_EQ(Unlocked->ContentHash, FXxHash64::HashBuffer(Bytes));
}
#endif

TEST(FFileHelperTests, FingerprintReturnsValuesAndPreservesCacheHits)
{
	using namespace Durin;
	const auto Path = Durin::Testing::GetTestWorkDirectory() / "FingerprintExpected.bin";
	const FByteBuffer Bytes{std::byte{1}, std::byte{2}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, Path));
	FFileFingerprintCache Cache;
	const auto First = Cache.Get(Path);
	ASSERT_TRUE(First);
	const auto Cached = Cache.Get(Path);
	ASSERT_TRUE(Cached);
	EXPECT_EQ(Cached->ContentHash, First->ContentHash);
	EXPECT_EQ(Cache.GetContentReadCount(), 1u);
	const FByteBuffer Changed{std::byte{3}, std::byte{4}, std::byte{5}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Changed, Path));
	const auto Updated = Cache.Get(Path);
	ASSERT_TRUE(Updated);
	EXPECT_EQ(Updated->ContentHash, FXxHash64::HashBuffer(Changed));
	EXPECT_EQ(Cache.GetContentReadCount(), 2u);
	std::filesystem::remove(Path);
	const auto Missing = Cache.Get(Path);
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().NativeError, std::errc::no_such_file_or_directory);
	EXPECT_EQ(Cache.GetContentReadCount(), 2u);
}

TEST(FFileHelperTests, DistinguishesMissingFilesFromQueryFailure)
{
	using namespace Durin;
	const FFilePath Root = TestRoot("ResultErrors");
	const FFilePath Missing = Root / "Missing.bin";
	EXPECT_FALSE(IFileManager::Get().FileExists(Missing));
	auto Loaded = FFileHelper::LoadFileToArray(Missing);
	ASSERT_FALSE(Loaded);
	EXPECT_EQ(Loaded.error().NativeError, std::errc::no_such_file_or_directory);
	EXPECT_NE(Loaded.error().GetMessage().find((std::filesystem::absolute(Missing).lexically_normal()).generic_string()), std::string::npos);
	EXPECT_NE(Loaded.error().GetMessage().find("Missing.bin"), std::string::npos);
	auto Invalid = IFileManager::Get().FileExists(Root / std::string(300, 'x'));
	EXPECT_FALSE(Invalid);
	const FFilePath Blocker = Root / "Blocker";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Blocker));
	auto Saved = FFileHelper::SaveArrayToFile({}, Blocker / "Child");
	ASSERT_FALSE(Saved);
	EXPECT_TRUE(Saved.error().NativeError);
}

TEST(FFileHelperTests, FingerprintReuseDistinguishesCurrentChangedAndMissing)
{
	using namespace Durin;
	const auto Root = TestRoot("ReuseResult");
	const auto Path = Root / "Source.bin";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, Path));
	FFileFingerprintCache Cache;
	const auto Stored = Cache.Get(Path);
	ASSERT_TRUE(Stored);
	const auto Current = Cache.TryReuse(*Stored);
	ASSERT_TRUE(Current);
	EXPECT_EQ(*Current, EFileFingerprintReuseStatus::Current);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer(2, std::byte{2}), Path));
	const auto Changed = Cache.TryReuse(*Stored);
	ASSERT_TRUE(Changed);
	EXPECT_EQ(*Changed, EFileFingerprintReuseStatus::Stale);
	std::filesystem::remove(Path);
	const auto Missing = Cache.TryReuse(*Stored);
	ASSERT_TRUE(Missing);
	EXPECT_EQ(*Missing, EFileFingerprintReuseStatus::Stale);
	EXPECT_EQ(Cache.GetContentReadCount(), 1u);
}

TEST(FFileHelperTests, PublicationResultsPreserveExistingBytes)
{
	using namespace Durin;
	const FFilePath Root = TestRoot("ResultPublication");
	const FFilePath Path = Root / "Value.bin";
	const FByteBuffer Bytes(64 * 1024 + 3, std::byte{0x53});
	ASSERT_TRUE(FFileHelper::SaveArrayToNewFile(Bytes, Path));
	auto Existing = FFileHelper::SaveArrayToNewFile({}, Path);
	ASSERT_FALSE(Existing);
	auto Loaded = FFileHelper::LoadFileToArray(Path);
	ASSERT_TRUE(Loaded);
	EXPECT_EQ(*Loaded, Bytes);
	ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(Bytes, Root / "Atomic.bin"));
	ASSERT_TRUE(FFileHelper::CopyFileAtomically(Path, Root / "Copy.bin"));
	Loaded = FFileHelper::LoadFileToArray(Root / "Copy.bin");
	ASSERT_TRUE(Loaded);
	EXPECT_EQ(*Loaded, Bytes);
	auto Failed = FFileHelper::SaveArrayToFileAtomically(Bytes, Root);
	ASSERT_FALSE(Failed);
	EXPECT_TRUE(std::filesystem::is_directory(Root));
}

TEST(FFileHelperTests, ExclusivelyCreatesCompleteBytes)
{
	const auto Root = TestRoot("Exclusive");
	const auto Destination = Root / "Nested" / "Stage.bin";
	const Durin::FByteBuffer Payload(128 * 1024 + 7, std::byte{0x53});
	auto Created = Durin::FFileHelper::SaveArrayToNewFile(Payload, Destination);
	ASSERT_TRUE(Created) << Created.error().GetMessage();
	EXPECT_EQ(ReadBytes(Destination), Payload);
	auto Existing = Durin::FFileHelper::SaveArrayToNewFile({}, Destination);
	ASSERT_FALSE(Existing);
	EXPECT_EQ(ReadBytes(Destination), Payload);
	EXPECT_FALSE(Durin::FFileHelper::SaveArrayToNewFile({}, Destination));
	EXPECT_EQ(ReadBytes(Destination), Payload);
	auto Directory = Durin::FFileHelper::SaveArrayToNewFile({}, Root);
	EXPECT_FALSE(Directory);
	EXPECT_TRUE(std::filesystem::is_directory(Root));
	auto Empty = Durin::FFileHelper::SaveArrayToNewFile({}, Root / "Empty.bin");
	ASSERT_TRUE(Empty) << Empty.error().GetMessage();
	EXPECT_TRUE(ReadBytes(Root / "Empty.bin").empty());
}

TEST(FFileHelperTests, ConcurrentExclusiveCreatorsPreserveWinner)
{
	const auto Destination = TestRoot("ExclusiveRace") / "Stage.bin";
	std::atomic_bool Start = false;
	std::atomic_uint32_t Successes = 0;
	std::atomic_int Winner = -1;
	std::vector<std::thread> Writers;
	for (int Index = 0; Index < 8; ++Index)
	{
		Writers.emplace_back([&, Index]
		{
			const Durin::FByteBuffer Payload(64 * 1024 + Index, static_cast<std::byte>(Index));
			while (!Start.load()) std::this_thread::yield();
			if (Durin::FFileHelper::SaveArrayToNewFile(Payload, Destination))
			{
				++Successes;
				Winner = Index;
			}
		});
	}
	Start = true;
	for (auto& Writer : Writers) Writer.join();
	ASSERT_EQ(Successes.load(), 1u);
	EXPECT_EQ(ReadBytes(Destination), Durin::FByteBuffer(64 * 1024 + Winner.load(),
		static_cast<std::byte>(Winner.load())));
}

TEST(FFileHelperTests, PublishesAndReplacesCompleteBytes)
{
	const std::filesystem::path Destination = TestRoot("Replace") / "Value.bin";
	const std::array First{std::byte{0x11}, std::byte{0x22}};
	const std::array Second{std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};

	auto FirstWrite = Durin::FFileHelper::SaveArrayToFileAtomically(First, Destination);
	ASSERT_TRUE(FirstWrite) << FirstWrite.error().GetMessage();
	EXPECT_EQ(ReadBytes(Destination), std::vector(First.begin(), First.end()));

	auto SecondWrite = Durin::FFileHelper::SaveArrayToFileAtomically(Second, Destination);
	ASSERT_TRUE(SecondWrite) << SecondWrite.error().GetMessage();
	EXPECT_EQ(ReadBytes(Destination), std::vector(Second.begin(), Second.end()));
	auto Loaded = Durin::FFileHelper::LoadFileToArray(Destination);
	ASSERT_TRUE(Loaded);
	EXPECT_EQ(*Loaded, std::vector(Second.begin(), Second.end()));
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

	auto Copied = Durin::FFileHelper::CopyFileAtomically(Source, Destination);
	ASSERT_TRUE(Copied)
		<< Copied.error().GetMessage();
	EXPECT_EQ(ReadBytes(Source), Expected);
	EXPECT_EQ(ReadBytes(Destination), Expected);
	for (const auto& Entry : std::filesystem::directory_iterator(Root))
		EXPECT_TRUE(Entry.path() == Source || Entry.path() == Destination) << Entry.path();

	auto Missing = Durin::FFileHelper::CopyFileAtomically(Root / "Missing.bin", Destination);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(ReadBytes(Destination), Expected);
}

TEST(FFileHelperTests, HashesFilesIncrementallyAcrossBufferBoundaries)
{
	const std::filesystem::path FilePath = TestRoot("StreamingHash") / "Value.bin";
	Durin::FByteBuffer Bytes(3 * 64 * 1024 + 17);
	for (size_t Index = 0; Index < Bytes.size(); ++Index)
		Bytes[Index] = static_cast<std::byte>((Index * 37) & 0xff);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, FilePath));
	auto Actual = Durin::FFileHelper::HashFileXx128(FilePath);
	ASSERT_TRUE(Actual) << Actual.error().GetMessage();
	EXPECT_EQ(*Actual, Durin::FXxHash128::HashBuffer(Bytes));
	auto Missing = Durin::FFileHelper::HashFileXx128(FilePath.parent_path() / "Missing.bin");
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().NativeError, std::errc::no_such_file_or_directory);
}

TEST(FFileHelperTests, RandomReadHandleSupportsExactOutOfOrderRanges)
{
	const std::filesystem::path FilePath = TestRoot("RandomRead") / "Value.bin";
	Durin::FByteBuffer Bytes(257);
	for (size_t Index = 0; Index < Bytes.size(); ++Index)
		Bytes[Index] = static_cast<std::byte>((Index * 29) & 0xff);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, FilePath));

	auto Handle = Durin::IPlatformFile::GetPlatformPhysical().OpenRead(FilePath, Durin::IPlatformFile::EOpenReadFlags::AllowWrite | Durin::IPlatformFile::EOpenReadFlags::AllowDelete);
	ASSERT_TRUE(Handle) << Handle.error().GetMessage();
	EXPECT_EQ((*Handle)->Size(), Bytes.size());
	std::array<std::byte, 13> Tail{};
	ASSERT_TRUE((*Handle)->ReadAt(reinterpret_cast<uint8*>((Tail).data()), static_cast<int64>(std::span(Tail).size_bytes()), static_cast<int64>(201)));
	EXPECT_TRUE(std::ranges::equal(Tail, std::span(Bytes).subspan(201, Tail.size())));
	std::array<std::byte, 17> Head{};
	ASSERT_TRUE((*Handle)->ReadAt(reinterpret_cast<uint8*>((Head).data()), static_cast<int64>(std::span(Head).size_bytes()), static_cast<int64>(3)));
	EXPECT_TRUE(std::ranges::equal(Head, std::span(Bytes).subspan(3, Head.size())));
	EXPECT_TRUE((*Handle)->ReadAt(nullptr, 0, static_cast<int64>((*Handle)->Size())));
}

TEST(FFileHelperTests, RandomReadHandleRejectsInvalidRangesDeterministically)
{
	const std::filesystem::path FilePath = TestRoot("RandomReadBounds") / "Value.bin";
	const std::array Bytes{std::byte{0x11}, std::byte{0x22}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, FilePath));
	auto Handle = Durin::IPlatformFile::GetPlatformPhysical().OpenRead(FilePath, Durin::IPlatformFile::EOpenReadFlags::AllowWrite | Durin::IPlatformFile::EOpenReadFlags::AllowDelete);
	ASSERT_TRUE(Handle);
	std::array<std::byte, 1> Output{};
	auto Invalid = (*Handle)->ReadAt(reinterpret_cast<uint8*>((Output).data()), static_cast<int64>(std::span(Output).size_bytes()), static_cast<int64>(3));
	ASSERT_FALSE(Invalid);
	EXPECT_FALSE((*Handle)->ReadAt(reinterpret_cast<uint8*>((Output).data()), static_cast<int64>(std::span(Output).size_bytes()), static_cast<int64>(std::numeric_limits<uint64>::max())));

	auto Missing = Durin::IPlatformFile::GetPlatformPhysical().OpenRead(FilePath.parent_path() / "Missing.bin", Durin::IPlatformFile::EOpenReadFlags::AllowWrite | Durin::IPlatformFile::EOpenReadFlags::AllowDelete);
	ASSERT_FALSE(Missing);
	EXPECT_TRUE(Missing.error().NativeError);
}

TEST(FFileHelperTests, RandomReadHandleSupportsSparseFilesBeyondFourGiB)
{
	const std::filesystem::path FilePath = TestRoot("SparseRandomRead") / "Value.bin";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Durin::FByteView{}, FilePath));
	constexpr uint64 SparseSize = (uint64{4} << 30) + 37;
	std::error_code Error;
	std::filesystem::resize_file(FilePath, SparseSize, Error);
	if (Error) GTEST_SKIP() << "Sparse/large fixture is unavailable: " << Error.message();
	auto Handle = Durin::IPlatformFile::GetPlatformPhysical().OpenRead(FilePath, Durin::IPlatformFile::EOpenReadFlags::AllowWrite | Durin::IPlatformFile::EOpenReadFlags::AllowDelete);
	ASSERT_TRUE(Handle);
	EXPECT_EQ((*Handle)->Size(), SparseSize);
	std::array<std::byte, 16> Tail;
	Tail.fill(std::byte{0xff});
	ASSERT_TRUE((*Handle)->ReadAt(reinterpret_cast<uint8*>((Tail).data()), static_cast<int64>(std::span(Tail).size_bytes()), static_cast<int64>(SparseSize - Tail.size())));
	EXPECT_TRUE(std::ranges::all_of(Tail, [](std::byte Value) { return Value == std::byte{0}; }));
}

TEST(FFileHelperTests, EmptyFilesReturnEmptyValues)
{
	const auto Path = TestRoot("EmptyRead") / "Empty.bin";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile({}, Path));
	auto Bytes = Durin::FFileHelper::LoadFileToArray(Path);
	ASSERT_TRUE(Bytes) << Bytes.error().GetMessage();
	EXPECT_TRUE(Bytes->empty());
	auto Text = Durin::FFileHelper::LoadFileToString(Path);
	ASSERT_TRUE(Text) << Text.error().GetMessage();
	EXPECT_TRUE(Text->empty());
}

TEST(FPlatformFileTests, ReadWriteSeekAppendAndTruncate)
{
	using namespace Durin;
	const auto Path = TestRoot("PlatformReadWrite") / "data.bin";
	auto& Platform = IPlatformFile::GetPlatformPhysical();
	auto File = Platform.OpenWrite(Path, IPlatformFile::EOpenWriteFlags::AllowRead);
	ASSERT_TRUE(File) << File.error().GetMessage();
	const std::array<uint8, 4> Bytes{1, 2, 3, 4};
	ASSERT_TRUE((*File)->Write(Bytes.data(), Bytes.size()));
	EXPECT_EQ((*File)->Tell(), 4);
	EXPECT_EQ((*File)->Size(), 4);
	ASSERT_TRUE((*File)->Seek(1));
	std::array<uint8, 2> Read{};
	ASSERT_TRUE((*File)->ReadAt(Read.data(), Read.size(), 2));
	EXPECT_EQ(Read, (std::array<uint8, 2>{3, 4}));
	EXPECT_EQ((*File)->Tell(), 1);
	ASSERT_TRUE((*File)->Read(Read.data(), Read.size()));
	EXPECT_EQ(Read, (std::array<uint8, 2>{2, 3}));
	EXPECT_FALSE((*File)->Seek(-1));
	EXPECT_FALSE((*File)->SeekFromEnd(1));
	ASSERT_TRUE((*File)->Truncate(2));
	EXPECT_EQ((*File)->Size(), 2);
	ASSERT_TRUE((*File)->Flush(true));
	File->reset();
	File = Platform.OpenWrite(Path, IPlatformFile::EOpenWriteFlags::Append);
	ASSERT_TRUE(File);
	EXPECT_EQ((*File)->Tell(), 2);
	ASSERT_TRUE((*File)->Write(Bytes.data() + 2, 2));
	File->reset();
	File = Platform.OpenRead(Path, IPlatformFile::EOpenReadFlags::None);
	ASSERT_TRUE(File);
	EXPECT_EQ((*File)->Size(), 4);
	ASSERT_TRUE((*File)->SeekFromEnd(-2));
	ASSERT_TRUE((*File)->Read(Read.data(), Read.size()));
	EXPECT_EQ(Read, (std::array<uint8, 2>{3, 4}));
	EXPECT_FALSE((*File)->Read(Read.data(), 1));
	EXPECT_FALSE((*File)->Write(Bytes.data(), 1));
	EXPECT_FALSE((*File)->Truncate(0));
	EXPECT_FALSE((*File)->ReadAt(Read.data(), -1, 0));
	EXPECT_FALSE((*File)->ReadAt(Read.data(), 1, -1));
}

#if defined(_WIN32)
TEST(FPlatformFileTests, ReadSharingFlagsAreExplicit)
{
	using namespace Durin;
	const auto Path = TestRoot("PlatformSharing") / "data.bin";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Path));
	auto& Platform = IPlatformFile::GetPlatformPhysical();
	auto Reader = Platform.OpenRead(Path, IPlatformFile::EOpenReadFlags::None);
	ASSERT_TRUE(Reader);
	auto Writer = Platform.OpenWrite(Path, IPlatformFile::EOpenWriteFlags::AllowRead);
	ASSERT_FALSE(Writer);
	EXPECT_EQ(Writer.error().NativeError.value(), ERROR_SHARING_VIOLATION);
	Reader->reset();
	Reader = Platform.OpenRead(Path, IPlatformFile::EOpenReadFlags::AllowWrite | IPlatformFile::EOpenReadFlags::AllowDelete);
	ASSERT_TRUE(Reader);
	Writer = Platform.OpenWrite(Path, IPlatformFile::EOpenWriteFlags::AllowRead);
	ASSERT_TRUE(Writer) << Writer.error().GetMessage();
	Writer->reset();
	std::error_code Error;
	std::filesystem::rename(Path, Path.parent_path() / "renamed.bin", Error);
	EXPECT_FALSE(Error);
	EXPECT_EQ((*Reader)->Size(), 0);
}
#endif

TEST(FFileHelperTests, ConcurrentWritersNeverExposePartialBytes)
{
	const std::filesystem::path Destination = TestRoot("Concurrent") / "Value.bin";
	constexpr size_t WriterCount = 6;
	constexpr size_t PublicationsPerWriter = 16;
	std::array<size_t, WriterCount + 1> PayloadSizes{};
	for (size_t Index = 0; Index < PayloadSizes.size(); ++Index)
		PayloadSizes[Index] = 64 * 1024 + Index * 4093;
	const std::vector Initial(PayloadSizes.front(), std::byte{0x01});
	auto InitialWrite = Durin::FFileHelper::SaveArrayToFileAtomically(Initial, Destination);
	ASSERT_TRUE(InitialWrite)
		<< InitialWrite.error().GetMessage();

	std::atomic_bool bStart = false;
	std::atomic_bool bStopReader = false;
	std::atomic_bool bObservedPartial = false;
	std::optional<Durin::FFileSystemError> ReadError;
	size_t SuccessfulReads = 0; // Reader-owned until join.
	std::atomic_bool bPublicationFailed = false;
	std::atomic_int PublicationErrorCode = 0;
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
				if (auto Saved = Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination); !Saved)
				{
					PublicationErrorCode.store(Saved.error().NativeError.value(), std::memory_order_release);
					bPublicationFailed.store(true, std::memory_order_release);
					return;
				}
			}
		});
	}

	std::thread Reader([&] {
		while (!bStart.load(std::memory_order_acquire)) std::this_thread::yield();
		do
		{
			auto Bytes = Durin::FFileHelper::LoadFileToArray(Destination);
#if defined(_WIN32)
			// Publication can transiently deny opening or hide the destination. Keep this
			// policy local: ordinary file reads intentionally do not retry.
			constexpr size_t MaxReadAttempts = 64;
			for (size_t Attempt = 1; !Bytes && Attempt < MaxReadAttempts; ++Attempt)
			{
				const auto& Error = Bytes.error();
				if (Error.NativeError.category() != std::system_category()
					|| (Error.NativeError.value() != ERROR_ACCESS_DENIED
						&& Error.NativeError.value() != ERROR_SHARING_VIOLATION
						&& Error.NativeError.value() != ERROR_FILE_NOT_FOUND))
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				Bytes = Durin::FFileHelper::LoadFileToArray(Destination);
			}
#endif
			if (!Bytes)
			{
				ReadError = std::move(Bytes.error());
				return;
			}
			++SuccessfulReads;
			if (!IsCompleteWriterPayload(*Bytes, PayloadSizes))
			{
				bObservedPartial.store(true, std::memory_order_release);
				return;
			}
		} while (!bStopReader.load(std::memory_order_acquire));
	});

	bStart.store(true, std::memory_order_release);
	for (std::thread& Writer : Writers) Writer.join();
	bStopReader.store(true, std::memory_order_release);
	Reader.join();

	EXPECT_FALSE(bPublicationFailed.load())
		<< "native error " << PublicationErrorCode.load();
	EXPECT_FALSE(ReadError.has_value()) << (ReadError ? ReadError->GetMessage() : "");
	EXPECT_GT(SuccessfulReads, 0u);
	EXPECT_FALSE(bObservedPartial.load());
	EXPECT_TRUE(IsCompleteWriterPayload(ReadBytes(Destination), PayloadSizes));
}

TEST(FFileHelperTests, FailedReplacementPreservesDestinationAndCleansTemporaryFile)
{
	const std::filesystem::path Root = TestRoot("Failure");
	const std::filesystem::path Destination = Root / "ExistingDestination";
	std::filesystem::create_directories(Destination);
	const std::array Payload{std::byte{0x44}};

	auto Failed = Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination);
	ASSERT_FALSE(Failed);
	EXPECT_NE(Failed.error().NativeError.value(), 0);
	EXPECT_NE(Failed.error().GetMessage().find((std::filesystem::absolute(Destination).lexically_normal()).generic_string()), std::string::npos);
	EXPECT_NE(Failed.error().GetMessage().find(Destination.filename().generic_string()), std::string::npos);
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
	auto Saved = Durin::FFileHelper::SaveArrayToFileAtomically(Second, Destination);
	ASSERT_TRUE(Saved) << Saved.error().GetMessage();
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
	auto Saved = Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination);
	ASSERT_TRUE(Saved) << Saved.error().GetMessage();
	EXPECT_EQ(ReadBytes(Destination), std::vector(Payload.begin(), Payload.end()));
}

TEST(FFileHelperTests, OverlongComponentFailsWithPathMetricsAndNoOrphan)
{
	const std::filesystem::path Root = TestRoot("OverlongComponent");
	const std::filesystem::path Destination = Root / std::string(256, 'c');
	const std::array Payload{std::byte{0x61}};

	auto Failed = Durin::FFileHelper::SaveArrayToFileAtomically(Payload, Destination);
	ASSERT_FALSE(Failed);
	EXPECT_NE(Failed.error().NativeError.value(), 0);
	EXPECT_NE(Failed.error().GetMessage().find(Destination.filename().generic_string()), std::string::npos);

	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Root))
	{
		EXPECT_FALSE(Entry.path().filename().generic_string().starts_with(".durin-tmp-"));
	}
}

TEST(FFileHelperTests, BoundedReadRejectsSizeChangesAndLimitsBeforeReading)
{
	using namespace Durin;
	const auto Path = TestRoot("BoundedRead") / "Source.bin";
	const FByteBuffer Payload{std::byte{1}, std::byte{0}, std::byte{3}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Payload, Path));
	auto Read = FFileHelper::LoadFileToArray(Path, {.MaximumBytes = 3, .ExpectedBytes = 3});
	ASSERT_TRUE(Read); EXPECT_EQ(*Read, Payload);
	Read = FFileHelper::LoadFileToArray(Path, {.MaximumBytes = 2});
	ASSERT_FALSE(Read);
	EXPECT_EQ(Read.error().NativeError, std::errc::file_too_large);
	Read = FFileHelper::LoadFileToArray(Path, {.MaximumBytes = 100, .ExpectedBytes = 2});
	ASSERT_FALSE(Read); EXPECT_EQ(Read.error().NativeError, std::errc::message_size);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile({}, Path));
	Read = FFileHelper::LoadFileToArray(Path, {.MaximumBytes = 0, .ExpectedBytes = 0});
	ASSERT_TRUE(Read); EXPECT_TRUE(Read->empty());
	Read = FFileHelper::LoadFileToArray(Path.parent_path() / "Missing", {.MaximumBytes = 1});
	ASSERT_FALSE(Read);
	Durin::Testing::RemoveTestWorkDirectory(Path.parent_path());
}

#include "HAL/PlatformLibrary.h"
#include "HAL/PlatformProcess.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		class FPlatformLibraryTests : public testing::Test
		{
		protected:
			std::filesystem::path Root;
			FModuleHandle Handle = nullptr;

			void SetUp() override
			{
				Root = Testing::CreateTestFixtureDirectory("PlatformLibrary");
			}

			void TearDown() override
			{
				if (Handle) EXPECT_TRUE(FPlatformLibrary::Unload(Handle));
				Testing::RemoveTestWorkDirectory(Root);
			}

			auto CopyFixture(const std::filesystem::path& Directory) -> std::filesystem::path
			{
				std::filesystem::create_directories(Directory);
				const auto Source = std::filesystem::path(u8"" DURIN_LIBRARY_FIXTURE_PATH);
				const auto Dependency = std::filesystem::path(u8"" DURIN_LIBRARY_DEPENDENCY_PATH);
				std::filesystem::copy_file(Source, Directory / Source.filename());
				std::filesystem::copy_file(Dependency, Directory / Dependency.filename());
				return Directory / Source.filename();
			}

			void CheckExports(const std::filesystem::path& Library)
			{
				const auto Loaded = FPlatformLibrary::Load(Library);
				ASSERT_TRUE(Loaded) << Loaded.error().ToString();
				Handle = *Loaded;
				const auto Function = FPlatformLibrary::FindSymbol(Handle, "DurinLibraryFixtureValue");
				ASSERT_TRUE(Function) << Function.error().ToString();
				EXPECT_EQ(reinterpret_cast<int(*)()>(*Function)(), 42);
				const auto Data = FPlatformLibrary::FindSymbol(Handle, "DurinLibraryFixtureData");
				ASSERT_TRUE(Data) << Data.error().ToString();
				EXPECT_EQ(*static_cast<int*>(*Data), 73);
			}
		};
	}

	TEST_F(FPlatformLibraryTests, LoadsUnicodePathAndAdjacentDependency)
	{
		CheckExports(CopyFixture(Root / std::filesystem::path(u8"\u52a8\u6001\u5e93 \U0001f30d")));
	}

	TEST_F(FPlatformLibraryTests, LoadsPathLongerThanMaxPath)
	{
		auto Directory = Root;
		while (Directory.native().size() < 320) Directory /= "long-library-directory-0123456789";
		CheckExports(CopyFixture(Directory));
	}

	TEST_F(FPlatformLibraryTests, ReportsMissingAdjacentDependency)
	{
		const auto Directory = Root / "missing-dependency";
		std::filesystem::create_directories(Directory);
		const auto Source = std::filesystem::path(u8"" DURIN_LIBRARY_FIXTURE_PATH);
		const auto Library = Directory / Source.filename();
		std::filesystem::copy_file(Source, Library);
		const auto Loaded = FPlatformLibrary::Load(Library);
		ASSERT_FALSE(Loaded);
		EXPECT_EQ(Loaded.error().Operation, EPlatformLibraryOperation::Load);
		EXPECT_EQ(Loaded.error().Path, Library);
		EXPECT_FALSE(Loaded.error().Message.empty());
	}

#if defined(_WIN32)
	TEST_F(FPlatformLibraryTests, AcceptsExistingExtendedPathPrefix)
	{
		const auto Library = CopyFixture(Root / "extended-prefix");
		CheckExports(std::filesystem::path(L"\\\\?\\" + Library.native()));
	}
#elif defined(__APPLE__)
	TEST_F(FPlatformLibraryTests, PreservesSymlinkParentTraversal)
	{
		const auto Library = CopyFixture(Root / "physical");
		std::filesystem::create_directory(Root / "physical" / "child");
		std::filesystem::create_directory_symlink(Root / "physical" / "child", Root / "link");
		CheckExports(Root / "link" / ".." / Library.filename());
	}
#endif

	TEST_F(FPlatformLibraryTests, RejectsInvalidPathsAndHandles)
	{
		for (const auto& Path : {std::filesystem::path{}, std::filesystem::path("relative-library.dll"),
			Root / std::filesystem::path(std::string("nul\0suffix", 10))})
		{
			const auto Loaded = FPlatformLibrary::Load(Path);
			ASSERT_FALSE(Loaded);
			EXPECT_EQ(Loaded.error().Operation, EPlatformLibraryOperation::Load);
			EXPECT_EQ(Loaded.error().NativeError, std::errc::invalid_argument);
		}
		const auto Symbol = FPlatformLibrary::FindSymbol(nullptr, "export");
		ASSERT_FALSE(Symbol);
		EXPECT_EQ(Symbol.error().Operation, EPlatformLibraryOperation::FindSymbol);
		const auto Released = FPlatformLibrary::Unload(nullptr);
		ASSERT_FALSE(Released);
		EXPECT_EQ(Released.error().Operation, EPlatformLibraryOperation::Unload);
	}

	TEST_F(FPlatformLibraryTests, CapturesErrorsWithoutAmbientLoaderState)
	{
		const auto MissingPath = Root / "missing-library";
		const auto Missing = FPlatformLibrary::Load(MissingPath);
		ASSERT_FALSE(Missing);
		const auto OriginalMessage = Missing.error().Message;
		CheckExports(CopyFixture(Root / "valid"));
		ASSERT_NE(Handle, nullptr);
		const auto Symbol = FPlatformLibrary::FindSymbol(Handle, "DurinMissingExport");
		ASSERT_FALSE(Symbol);
		EXPECT_EQ(Symbol.error().Operation, EPlatformLibraryOperation::FindSymbol);
		EXPECT_EQ(Symbol.error().Symbol, "DurinMissingExport");
		EXPECT_FALSE(Symbol.error().Message.empty());
		EXPECT_EQ(Missing.error().Path, MissingPath);
		EXPECT_EQ(Missing.error().Message, OriginalMessage);
		for (const auto& Name : {std::string{}, std::string("bad\0suffix", 10), std::string("\xff")})
		{
			const auto Invalid = FPlatformLibrary::FindSymbol(Handle, Name);
			ASSERT_FALSE(Invalid);
			EXPECT_EQ(Invalid.error().NativeError, std::errc::invalid_argument);
		}
	}

	TEST_F(FPlatformLibraryTests, EachLoadOwnsOneReference)
	{
		const auto Library = CopyFixture(Root / "references");
		CheckExports(Library);
		ASSERT_NE(Handle, nullptr);
		const auto Other = FPlatformLibrary::Load(Library);
		ASSERT_TRUE(Other) << Other.error().ToString();
		ASSERT_TRUE(FPlatformLibrary::Unload(*Other));
		const auto Symbol = FPlatformLibrary::FindSymbol(Handle, "DurinLibraryFixtureValue");
		ASSERT_TRUE(Symbol);
		EXPECT_EQ(reinterpret_cast<int(*)()>(*Symbol)(), 42);
	}

	TEST_F(FPlatformLibraryTests, ExecutablePathIsAbsoluteUtf8)
	{
		const std::string_view Executable = FPlatformProcess::ExecutablePath();
		const auto Path = std::filesystem::path(std::u8string(Executable.begin(), Executable.end()));
		EXPECT_TRUE(Path.is_absolute());
		EXPECT_TRUE(std::filesystem::is_regular_file(Path));
	}
}

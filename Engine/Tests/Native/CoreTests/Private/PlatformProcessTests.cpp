#include "HAL/PlatformProcess.h"
#include "NativeTestSupport.h"
#include "ProcessFixtureProtocol.h"

#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		auto Utf8Path(const std::filesystem::path& Path) -> std::string
		{
			const auto Text = Path.generic_u8string();
			return {reinterpret_cast<const char*>(Text.data()), Text.size()};
		}

		class FPlatformProcessTests : public testing::Test
		{
		protected:
			std::filesystem::path Root;
			std::string Executable;
			std::filesystem::path ReportPath;
			std::filesystem::path ReleasePath;
			std::optional<uint32> Child;

			void SetUp() override
			{
				Root = Testing::CreateTestFixtureDirectory("PlatformProcess");
				Executable = DURIN_PROCESS_FIXTURE_PATH;
				ReportPath = Root / "report.txt";
				ReleasePath = Root / "release";
			}

			void TearDown() override
			{
				std::ofstream(ReleasePath).close();
				if (Child) EXPECT_TRUE(FPlatformProcess::WaitForProcessExit(*Child));
				Testing::RemoveTestWorkDirectory(Root);
			}

			auto Options() const -> FProcessLaunchOptions
			{
				return {.Arguments = {Utf8Path(ReportPath)}};
			}

			auto WaitForReport() -> std::optional<Tests::ProcessFixture::FReport>
			{
				const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < Deadline)
				{
					if (const auto Report = Tests::ProcessFixture::ReadReport(ReportPath))
					{
						Child = Report->ProcessId;
						return Report;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				return {};
			}
		};
	}

	TEST_F(FPlatformProcessTests, PreservesArgumentsWithoutShellParsing)
	{
		auto Launch = Options();
		const std::vector<std::string> Values = {"", "plain", "two words", "\t\n", "\"quoted\"", "'single'",
			"trailing\\", "space and trailing\\", "slash\\\"quote", "many\\\\\"quotes\\\\", "$HOME; & | > < * ? %PATH%",
			std::string(reinterpret_cast<const char*>(u8"\u53c2\u6570 \U0001f30d"))};
		Launch.Arguments.insert(Launch.Arguments.end(), Values.begin(), Values.end());
		const auto Result = FPlatformProcess::ExecuteProcess(Executable, Launch);
		ASSERT_TRUE(Result) << Result.error().ToString();
		EXPECT_EQ(*Result, 0);
		const auto Report = Tests::ProcessFixture::ReadReport(ReportPath);
		ASSERT_TRUE(Report);
		EXPECT_EQ(Report->Arguments, Launch.Arguments);
		EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(std::u8string(Report->WorkingDirectory.begin(), Report->WorkingDirectory.end())), std::filesystem::current_path()));
	}

	TEST_F(FPlatformProcessTests, UsesChildWorkingDirectoryAndUnicodeExecutable)
	{
		const auto Directory = Root / std::filesystem::path(u8"\u5b50\u8fdb\u7a0b \U0001f30d");
		std::filesystem::create_directory(Directory);
		const auto Source = std::filesystem::path(std::u8string(Executable.begin(), Executable.end()));
		const auto Copy = Directory / (std::filesystem::path(u8"\u7a0b\u5e8f with spaces") += Source.extension());
		std::filesystem::copy_file(Source, Copy);
		const auto ParentDirectory = std::filesystem::current_path();
		auto Launch = Options();
		Launch.WorkingDirectory = Directory;
		const auto Result = FPlatformProcess::ExecuteProcess(Utf8Path(Copy), Launch);
		ASSERT_TRUE(Result) << Result.error().ToString();
		EXPECT_EQ(*Result, 0);
		const auto Report = Tests::ProcessFixture::ReadReport(ReportPath);
		ASSERT_TRUE(Report);
		EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(std::u8string(Report->WorkingDirectory.begin(), Report->WorkingDirectory.end())), Directory));
		EXPECT_EQ(std::filesystem::current_path(), ParentDirectory);
	}

	TEST_F(FPlatformProcessTests, AppliesEnvironmentOnlyToChild)
	{
		// Reserved test names avoid interference with ordinary runtime configuration.
		struct FEnvironmentScope
		{
			std::map<std::string, std::optional<std::string>> Saved;
			void Set(const std::string& Name, const char* Value)
			{
				const char* Previous = std::getenv(Name.c_str());
				Saved[Name] = Previous ? std::optional<std::string>(Previous) : std::nullopt;
				ASSERT_EQ(_putenv_s(Name.c_str(), Value), 0);
			}
			~FEnvironmentScope()
			{
				for (const auto& [Name, Value] : Saved) _putenv_s(Name.c_str(), Value ? Value->c_str() : "");
			}
		} Parent;
		Parent.Set("DURIN_PROCESS_INHERITED", "inherited");
		Parent.Set("DURIN_PROCESS_OVERRIDE", "parent");
		Parent.Set("DURIN_PROCESS_REMOVE", "remove-me");
		Parent.Set("DURIN_PROCESS_CASE", "parent-case");
#if defined(__APPLE__)
		Parent.Set("durin_process_case", "lower-parent");
#endif
		auto Launch = Options();
		const std::string Unicode(reinterpret_cast<const char*>(u8"\u503c \U0001f30d=quoted\"\\"));
		Launch.EnvironmentOverrides = {{"DURIN_PROCESS_OVERRIDE", "child=value"}, {"DURIN_PROCESS_REMOVE", std::nullopt},
			{"DURIN_PROCESS_EMPTY", ""}, {"DURIN_PROCESS_UNICODE", Unicode}, {"DURIN_PROCESS_CASE", "upper"},
			{"DURIN_PROCESS_PROBE_EXIT", "7"}};
		Launch.EnvironmentOverrides[Tests::ProcessFixture::EnvironmentNames[7]] = Unicode;
		const auto Result = FPlatformProcess::ExecuteProcess(Executable, Launch);
		ASSERT_TRUE(Result) << Result.error().ToString();
		EXPECT_EQ(*Result, 7);
		const auto Report = Tests::ProcessFixture::ReadReport(ReportPath);
		ASSERT_TRUE(Report);
		EXPECT_EQ(Report->Environment[0], "inherited");
		EXPECT_EQ(Report->Environment[1], "child=value");
		EXPECT_FALSE(Report->Environment[2]);
		EXPECT_EQ(Report->Environment[3], "");
		EXPECT_EQ(Report->Environment[4], Unicode);
		EXPECT_EQ(Report->Environment[5], "upper");
		EXPECT_EQ(Report->Environment[7], Unicode);
#if defined(_WIN32)
		EXPECT_EQ(Report->Environment[6], "upper");
#else
		EXPECT_EQ(Report->Environment[6], "lower-parent");
#endif
		EXPECT_STREQ(std::getenv("DURIN_PROCESS_OVERRIDE"), "parent");
		EXPECT_STREQ(std::getenv("DURIN_PROCESS_REMOVE"), "remove-me");
		EXPECT_STREQ(std::getenv("DURIN_PROCESS_CASE"), "parent-case");
	}

	TEST_F(FPlatformProcessTests, LaunchUsesOptionsWithoutWaitingForExit)
	{
		auto Launch = Options();
		Launch.WorkingDirectory = Root;
		Launch.EnvironmentOverrides["DURIN_PROCESS_PROBE_RELEASE"] = Utf8Path(ReleasePath);
		const auto Result = FPlatformProcess::LaunchProcess(Executable, Launch);
		ASSERT_TRUE(Result) << Result.error().ToString();
		const auto Report = WaitForReport();
		ASSERT_TRUE(Report);
		EXPECT_EQ(Report->Arguments, Launch.Arguments);
		EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(std::u8string(Report->WorkingDirectory.begin(), Report->WorkingDirectory.end())), Root));
		EXPECT_FALSE(std::filesystem::exists(ReleasePath));
		EXPECT_FALSE(std::filesystem::exists(Root / "report.txt.finished"));
	}

	TEST_F(FPlatformProcessTests, RejectsInvalidOptionsBeforeCreatingChild)
	{
		std::vector<FProcessLaunchOptions> Invalid;
		auto Launch = Options();
		Launch.Arguments.push_back(std::string("bad\0arg", 7)); Invalid.push_back(Launch);
		Launch = Options(); Launch.Arguments.push_back("\xc0\x80"); Invalid.push_back(Launch);
		Launch = Options(); Launch.WorkingDirectory = "relative"; Invalid.push_back(Launch);
		for (const auto& Name : {std::string{}, std::string("bad=name"), std::string("bad\0name", 8)})
		{
			Launch = Options(); Launch.EnvironmentOverrides[Name] = "value"; Invalid.push_back(Launch);
		}
		Launch = Options(); Launch.EnvironmentOverrides["NAME"] = std::string("bad\0value", 9); Invalid.push_back(Launch);
#if defined(_WIN32)
		Launch = Options(); Launch.EnvironmentOverrides = {{"NAME", "one"}, {"name", "two"}}; Invalid.push_back(Launch);
		Launch = Options(); Launch.Arguments.emplace_back(32767, 'x'); Invalid.push_back(Launch);
#endif
		for (const auto& Bad : Invalid)
		{
			const auto Result = FPlatformProcess::ExecuteProcess(Executable, Bad);
			ASSERT_FALSE(Result);
			EXPECT_EQ(Result.error().Code, EPlatformProcessError::InvalidArguments);
			EXPECT_FALSE(std::filesystem::exists(ReportPath));
		}
		EXPECT_FALSE(FPlatformProcess::ExecuteProcess("relative-executable", Options()));
	}

	TEST_F(FPlatformProcessTests, ReportsMissingWorkingDirectoryWithoutChangingParent)
	{
		const auto ParentDirectory = std::filesystem::current_path();
		auto Launch = Options();
		Launch.WorkingDirectory = Root / "missing-directory";
		const auto Result = FPlatformProcess::ExecuteProcess(Executable, Launch);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, EPlatformProcessError::Launch);
		EXPECT_TRUE(Result.error().NativeError);
		EXPECT_EQ(std::filesystem::current_path(), ParentDirectory);
		EXPECT_FALSE(std::filesystem::exists(ReportPath));
	}
}

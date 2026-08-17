#include "NativeTestApplicationProtocol.h"
#include "NativeTestSupport.h"

#include <fstream>
#include <gtest/gtest.h>

namespace Durin::Testing::ApplicationHost
{
	namespace
	{
		class FProtocolDirectory
		{
		public:
			explicit FProtocolDirectory(std::string_view Name)
				: Path(CreateTestFixtureDirectory(Name))
			{
			}

			~FProtocolDirectory()
			{
				RemoveTestWorkDirectory(Path);
			}

			std::filesystem::path Path;
		};
	}

	TEST(NativeTestApplicationProtocol, StringVectorRoundTripPreservesExactValues)
	{
		FProtocolDirectory Directory("ApplicationHostProtocolRoundTrip");
		const std::filesystem::path Record = Directory.Path / "record.bin";
		const std::vector<std::string> Expected{
			"nonce", "argument with spaces", "line one\nline two", "NAME=value=with=equals"};
		std::string Error;
		ASSERT_TRUE(WriteStringVectorAtomic(Record, Expected, Error)) << Error;
		std::vector<std::string> Actual;
		ASSERT_TRUE(ReadStringVector(Record, Actual, Error)) << Error;
		EXPECT_EQ(Actual, Expected);
	}

	TEST(NativeTestApplicationProtocol, ResultRoundTripPreservesFailureClassification)
	{
		FProtocolDirectory Directory("ApplicationHostResultRoundTrip");
		const std::filesystem::path Record = Directory.Path / "result.bin";
		const FResult Expected{
			"nonce", EResultStage::Test, EResultStatus::Crashed, 139, 11,
			"signal evidence"};
		std::string Error;
		ASSERT_TRUE(WriteResultAtomic(Record, Expected, Error)) << Error;
		FResult Actual;
		ASSERT_TRUE(ReadResult(Record, Actual, Error)) << Error;
		EXPECT_EQ(Actual.Nonce, Expected.Nonce);
		EXPECT_EQ(Actual.Stage, Expected.Stage);
		EXPECT_EQ(Actual.Status, Expected.Status);
		EXPECT_EQ(Actual.ExitCode, Expected.ExitCode);
		EXPECT_EQ(Actual.Signal, Expected.Signal);
		EXPECT_EQ(Actual.Message, Expected.Message);
	}

	TEST(NativeTestApplicationProtocol, RequestRoundTripContainsWireOrdering)
	{
		FProtocolDirectory Directory("ApplicationHostRequestRoundTrip");
		const std::filesystem::path Record = Directory.Path / "request.bin";
		const FRequest Expected{
			"nonce", "/tmp/artifacts/test", "/tmp/artifacts/work", 42,
			"/tmp/artifacts", {"--gtest_filter=Suite.Case", "argument with spaces"}};
		std::string Error;
		ASSERT_TRUE(WriteRequestAtomic(Record, Expected, Error)) << Error;
		std::vector<std::string> Wire;
		ASSERT_TRUE(ReadStringVector(Record, Wire, Error)) << Error;
		EXPECT_EQ(Wire, (std::vector<std::string>{
			"nonce", "/tmp/artifacts/test", "/tmp/artifacts/work", "42",
			"/tmp/artifacts", "--gtest_filter=Suite.Case", "argument with spaces"}));
		FRequest Actual;
		ASSERT_TRUE(ReadRequest(Record, Actual, Error)) << Error;
		EXPECT_EQ(Actual.Nonce, Expected.Nonce);
		EXPECT_EQ(Actual.Executable, Expected.Executable);
		EXPECT_EQ(Actual.WorkingDirectory, Expected.WorkingDirectory);
		EXPECT_EQ(Actual.ControllerPid, Expected.ControllerPid);
		EXPECT_EQ(Actual.ArtifactRoot, Expected.ArtifactRoot);
		EXPECT_EQ(Actual.Arguments, Expected.Arguments);
		EXPECT_TRUE(ValidateRequest(Actual, "nonce", Error)) << Error;
	}

	TEST(NativeTestApplicationProtocol, ResultValidationRejectsInconsistentStates)
	{
		std::string Error;
		EXPECT_TRUE(ValidateResult({"nonce", EResultStage::Test,
			EResultStatus::Passed, 0, 0, ""}, "nonce", Error));
		EXPECT_TRUE(ValidateResult({"nonce", EResultStage::Test,
			EResultStatus::Failed, 7, 0, ""}, "nonce", Error));
		EXPECT_TRUE(ValidateResult({"nonce", EResultStage::Test,
			EResultStatus::Crashed, 134, 6, ""}, "nonce", Error));
		EXPECT_FALSE(ValidateResult({"nonce", EResultStage::Test,
			EResultStatus::Passed, 7, 0, ""}, "nonce", Error));
		EXPECT_NE(Error.find("exitCode=0"), std::string::npos);
		EXPECT_FALSE(ValidateResult({"nonce", EResultStage::Test,
			EResultStatus::Crashed, 7, 6, ""}, "nonce", Error));
		EXPECT_FALSE(ValidateResult({"stale", EResultStage::Test,
			EResultStatus::Passed, 0, 0, ""}, "nonce", Error));
		EXPECT_NE(Error.find("nonce mismatch"), std::string::npos);
	}

	TEST(NativeTestApplicationProtocol, RequestValidationNamesInvalidFields)
	{
		const FRequest Valid{
			"nonce", "/tmp/artifacts/test", "/tmp/artifacts/work", 42,
			"/tmp/artifacts", {"argument"}};
		std::string Error;
		FRequest Invalid = Valid;
		Invalid.ControllerPid = 1;
		EXPECT_FALSE(ValidateRequest(Invalid, "nonce", Error));
		EXPECT_NE(Error.find("controllerPid"), std::string::npos);
		Invalid = Valid;
		Invalid.Arguments[0] = std::string("bad\0argument", 12);
		EXPECT_FALSE(ValidateRequest(Invalid, "nonce", Error));
		EXPECT_NE(Error.find("argument[0]"), std::string::npos);
		EXPECT_FALSE(ValidateRequest(Valid, "stale", Error));
		EXPECT_NE(Error.find("nonce mismatch"), std::string::npos);
	}

	TEST(NativeTestApplicationProtocol, RejectsUnknownResultStageAndStatus)
	{
		FProtocolDirectory Directory("ApplicationHostUnknownResultValues");
		std::string Error;
		FResult Result;
		const std::filesystem::path UnknownStage = Directory.Path / "stage.bin";
		ASSERT_TRUE(WriteStringVectorAtomic(UnknownStage,
			{"nonce", "unknown-stage", "passed", "0", "0", ""}, Error)) << Error;
		EXPECT_FALSE(ReadResult(UnknownStage, Result, Error));
		EXPECT_NE(Error.find("stage"), std::string::npos);
		const std::filesystem::path UnknownStatus = Directory.Path / "status.bin";
		ASSERT_TRUE(WriteStringVectorAtomic(UnknownStatus,
			{"nonce", "test", "unknown-status", "0", "0", ""}, Error)) << Error;
		EXPECT_FALSE(ReadResult(UnknownStatus, Result, Error));
		EXPECT_NE(Error.find("status"), std::string::npos);
	}

	TEST(NativeTestApplicationProtocol, RejectsRequestNumericAndProtocolLimits)
	{
		FProtocolDirectory Directory("ApplicationHostRequestLimits");
		std::string Error;
		FRequest Request;
		const std::filesystem::path Numeric = Directory.Path / "numeric.bin";
		ASSERT_TRUE(WriteStringVectorAtomic(Numeric,
			{"nonce", "/tmp/test", "/tmp/work", "not-a-pid", "/tmp"}, Error)) << Error;
		EXPECT_FALSE(ReadRequest(Numeric, Request, Error));
		EXPECT_NE(Error.find("controllerPid"), std::string::npos);
		const std::filesystem::path Oversized = Directory.Path / "oversized.bin";
		EXPECT_FALSE(WriteStringVectorAtomic(Oversized,
			{std::string(16 * 1024 * 1024 + 1, 'x')}, Error));
		EXPECT_NE(Error.find("field size"), std::string::npos);
		std::vector<std::string> TooManyFields(65537);
		EXPECT_FALSE(WriteStringVectorAtomic(
			Directory.Path / "too-many.bin", TooManyFields, Error));
		EXPECT_NE(Error.find("field count"), std::string::npos);
	}

	TEST(NativeTestApplicationProtocol, RejectsTruncatedAndTrailingRecords)
	{
		FProtocolDirectory Directory("ApplicationHostMalformedRecords");
		std::string Error;
		const std::filesystem::path Truncated = Directory.Path / "truncated.bin";
		{
			std::ofstream Output(Truncated, std::ios::binary);
			Output << "invalid";
		}
		std::vector<std::string> Values;
		EXPECT_FALSE(ReadStringVector(Truncated, Values, Error));

		const std::filesystem::path Trailing = Directory.Path / "trailing.bin";
		Error.clear();
		ASSERT_TRUE(WriteStringVectorAtomic(Trailing, {"valid"}, Error)) << Error;
		{
			std::ofstream Output(Trailing, std::ios::binary | std::ios::app);
			Output << 'x';
		}
		Error.clear();
		EXPECT_FALSE(ReadStringVector(Trailing, Values, Error));
		EXPECT_NE(Error.find("trailing"), std::string::npos);
	}

	TEST(NativeTestApplicationProtocol, ContainmentRejectsRootAndSiblingPrefixes)
	{
		const std::filesystem::path Root = "/tmp/durin-host";
		EXPECT_TRUE(IsContainedPath(Root, Root / "run-p1-nonce"));
		EXPECT_FALSE(IsContainedPath(Root, Root));
		EXPECT_FALSE(IsContainedPath(Root, "/tmp/durin-host-escape/run"));
	}
}

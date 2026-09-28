#include "DerivedDataBuildSession.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Identity() -> FBuildInputReference
	{ return {"Source", FXxHash128::HashBuffer("captured"), "Fixture", 1, "Bytes", 1}; }
	auto Definition() -> FBuildDefinition
	{ return FBuildDefinition::TryCreate("Execution.Fixture", {}, {{"Source", "Captured"}}).value(); }
	struct FResolver final : IBuildInputResolver
	{
		mutable uint32 Describes = 0, Resolves = 0;
		bool FailResolve = false, ThrowDescribe = false;
		FSharedByteBuffer Bytes = FSharedByteBuffer::Take(FByteBuffer(32, std::byte{3}));
		auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
		{
			++Describes;
			if (ThrowDescribe) throw std::runtime_error("describe exception");
			if (Sources.size() != 1) return std::unexpected(FBuildFailure{.Description = "Invalid source table"});
			return std::vector{Identity()};
		}
		auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInput>, FBuildFailure> override
		{
			++Resolves;
			if (FailResolve || Inputs.size() != 1 || Inputs[0] != Identity())
				return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InputUnavailable,
					.Description = "Source unavailable"});
			return std::vector<FBuildInput>{{.Identity = Identity(), .Values = {{"Data", Bytes}}}};
		}
	};
	struct FFunction final : IBuildFunction
	{
		mutable uint32 Builds = 0, Validates = 0;
		bool FailBuild = false, RejectOutput = false, ThrowBuild = false, FloodMetrics = false;
		auto GetDescriptor() const -> FBuildFunctionDescriptor override
		{ return {"Execution.Fixture", 1, 1, "Fixture.Output", 1, FCacheBucket::FromString("Execution")}; }
		auto Build(FBuildContext& Context) const -> FBuildFunctionResult override
		{
			++Builds; Context.ReportMetric("Fixture.Builds", 1);
			if (FloodMetrics)
				for (uint32 Index = 0; Index < 300; ++Index) Context.ReportMetric(std::string(256, 'm'), Index);
			if (ThrowBuild) throw std::runtime_error("producer exception");
			if (FailBuild) return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::ProducerFailure,
				.Description = "Producer failed"});
			return FBuildOutput::TryCreate({.Schema = "Fixture.Output", .SchemaVersion = 1,
				.Values = Context.GetInputs()[0].Values}).value();
		}
		auto Validate(const FBuildAction&, const FBuildOutput& Output, const FBuildCancellation&) const
			-> FBuildValidationResult override
		{
			++Validates;
			if (RejectOutput || !Output.FindValue("Data"))
				return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InvalidOutput,
					.Description = "Invalid fixture output"});
			return {};
		}
	};
	struct FHarness
	{
		std::shared_ptr<FResolver> Resolver = std::make_shared<FResolver>();
		std::shared_ptr<FFunction> Function = std::make_shared<FFunction>();
		FSharedByteBuffer Stored;
		bool GetFailure = false, PutFailure = false, ThrowGet = false, ThrowPut = false;
		uint32 GetCalls = 0, PutCalls = 0, SinkCalls = 0;
		FBuildServiceOptions ServiceOptions;
		std::shared_ptr<IBuild> Service;
		std::shared_ptr<FBuildSession> Session;
		FHarness()
		{
			ServiceOptions.Cache.Get = [&](const FCacheGetRequest&) -> FCacheGetResult {
				++GetCalls;
				if (ThrowGet) throw std::runtime_error("read exception");
				if (GetFailure) return std::unexpected(FCacheError{ECacheError::StorageFailure, "read failed"});
				if (Stored.IsEmpty()) return std::optional<FSharedByteBuffer>{};
				return std::optional<FSharedByteBuffer>{Stored};
			};
			ServiceOptions.Cache.Put = [&](const FCachePutRequest& Request) -> FCachePutResult {
				++PutCalls;
				if (ThrowPut) throw std::runtime_error("write exception");
				if (PutFailure) return std::unexpected(FCacheError{ECacheError::StorageFailure, "write failed"});
				Stored = FSharedByteBuffer::Copy(Request.Value); return {};
			};
			ServiceOptions.Diagnostics = [&](const FBuildAction&, const FBuildDiagnostic&) { ++SinkCalls; };
			Service = CreateBuild(ServiceOptions); Service->Register(Function).value();
			Session = Service->CreateSession().value();
		}
		auto Run(FBuildPolicy Policy = {}, FBuildCancellation Cancel = {}) -> FBuildCompleteParams
		{
			auto Inputs = FBuildInputs::TryCreate(Definition().GetSources(), Resolver).value();
			std::optional<FBuildCompleteParams> Completion;
			auto Request = Session->Build(Definition(), [&](auto Value) { Completion = std::move(Value); },
				std::move(Inputs), {.Policy = Policy, .Cancellation = std::move(Cancel)});
			EXPECT_TRUE(Request.has_value()); EXPECT_TRUE(Completion.has_value());
			return std::move(*Completion);
		}
	};
}

TEST(FBuildExecutionTests, NormalMissBuildsStoresAndWarmHitAvoidsPayloadResolution)
{
	FHarness H;
	auto Cold = H.Run();
	ASSERT_EQ(Cold.GetStatus(), EStatus::Ok);
	EXPECT_TRUE(HasBuildStatus(Cold.GetBuildStatus(), EBuildStatus::CacheQuery));
	EXPECT_TRUE(HasBuildStatus(Cold.GetBuildStatus(), EBuildStatus::BuildLocal));
	EXPECT_TRUE(HasBuildStatus(Cold.GetBuildStatus(), EBuildStatus::CacheStore));
	EXPECT_EQ(Cold.GetReport().Diagnostics.size(), 0u);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u);
	auto Warm = H.Run();
	ASSERT_EQ(Warm.GetStatus(), EStatus::Ok);
	EXPECT_TRUE(HasBuildStatus(Warm.GetBuildStatus(), EBuildStatus::CacheQueryHit));
	EXPECT_FALSE(HasBuildStatus(Warm.GetBuildStatus(), EBuildStatus::BuildLocal));
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u);
	EXPECT_NE(Warm.GetCacheKey(), nullptr); EXPECT_NE(Warm.GetOutput(), nullptr);
}

TEST(FBuildExecutionTests, ForceBuildAndStorePolicyControlOnlyTheirDeclaredOperations)
{
	FHarness H;
	ASSERT_EQ(H.Run().GetStatus(), EStatus::Ok);
	EXPECT_EQ(H.GetCalls, 1u); EXPECT_EQ(H.PutCalls, 1u);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u);
	auto Forced = H.Run({.ForceBuild = true});
	ASSERT_EQ(Forced.GetStatus(), EStatus::Ok);
	EXPECT_FALSE(HasBuildStatus(Forced.GetBuildStatus(), EBuildStatus::CacheQuery));
	EXPECT_TRUE(HasBuildStatus(Forced.GetBuildStatus(), EBuildStatus::BuildLocal));
	EXPECT_EQ(H.GetCalls, 1u); EXPECT_EQ(H.PutCalls, 2u);
	EXPECT_EQ(H.Function->Builds, 2u); EXPECT_EQ(H.Resolver->Resolves, 2u);
	auto NoCache = H.Run({.QueryCache = false, .StoreOnBuild = false});
	ASSERT_EQ(NoCache.GetStatus(), EStatus::Ok);
	EXPECT_FALSE(HasBuildStatus(NoCache.GetBuildStatus(), EBuildStatus::CacheQuery));
	EXPECT_FALSE(HasBuildStatus(NoCache.GetBuildStatus(), EBuildStatus::CacheStore));
	EXPECT_EQ(H.GetCalls, 1u); EXPECT_EQ(H.PutCalls, 2u);
}

TEST(FBuildExecutionTests, PublicTerminalFactoriesNormalizeInvalidCacheFacts)
{
	const auto Facts = EBuildStatus::CacheKey | EBuildStatus::CacheQuery
		| EBuildStatus::CacheQueryHit | EBuildStatus::BuildLocal | EBuildStatus::CacheStore;
	auto Failed = FBuildCompleteParams::Error({.Reason = EBuildFailureReason::InternalFailure},
		FCacheKey{}, Facts, {});
	EXPECT_EQ(Failed.GetStatus(), EStatus::Error);
	EXPECT_EQ(Failed.GetCacheKey(), nullptr);
	EXPECT_EQ(Failed.GetOutput(), nullptr);
	EXPECT_NE(Failed.GetFailure(), nullptr);
	EXPECT_EQ(Failed.GetBuildStatus(), EBuildStatus::None);
	auto Canceled = FBuildCompleteParams::Canceled(std::nullopt, Facts, {});
	EXPECT_EQ(Canceled.GetStatus(), EStatus::Canceled);
	EXPECT_EQ(Canceled.GetCacheKey(), nullptr);
	EXPECT_EQ(Canceled.GetOutput(), nullptr);
	EXPECT_EQ(Canceled.GetFailure(), nullptr);
	EXPECT_EQ(Canceled.GetBuildStatus(), EBuildStatus::None);
}

TEST(FBuildExecutionTests, CorruptCacheIsDiagnosedAndRebuilt)
{
	FHarness H;
	H.Stored = FSharedByteBuffer::Take(FByteBuffer(8, std::byte{9}));
	auto Result = H.Run();
	ASSERT_EQ(Result.GetStatus(), EStatus::Ok);
	ASSERT_EQ(Result.GetReport().Diagnostics.size(), 1u);
	EXPECT_EQ(Result.GetReport().Diagnostics[0].Error.Code, ECacheError::Corrupt);
	EXPECT_EQ(H.SinkCalls, 1u); EXPECT_EQ(H.Function->Builds, 1u);
}

TEST(FBuildExecutionTests, CacheInfrastructureAndStoreFailuresPreserveUsableOutput)
{
	FHarness H;
	H.GetFailure = true; H.PutFailure = true;
	auto Result = H.Run();
	ASSERT_EQ(Result.GetStatus(), EStatus::Ok);
	EXPECT_NE(Result.GetOutput(), nullptr);
	ASSERT_EQ(Result.GetReport().Diagnostics.size(), 2u);
	EXPECT_EQ(Result.GetReport().Diagnostics[0].Error.Code, ECacheError::StorageFailure);
	EXPECT_EQ(Result.GetReport().Diagnostics[1].Operation, EBuildOperation::CacheStore);
}

TEST(FBuildExecutionTests, ThrowingCacheOperationsFallBackAndPreserveUsableOutput)
{
	FHarness H;
	H.ThrowGet = true; H.ThrowPut = true;
	auto Result = H.Run();
	ASSERT_EQ(Result.GetStatus(), EStatus::Ok);
	EXPECT_NE(Result.GetOutput(), nullptr);
	ASSERT_EQ(Result.GetReport().Diagnostics.size(), 2u);
	EXPECT_EQ(Result.GetReport().Diagnostics[0].Operation, EBuildOperation::CacheQuery);
	EXPECT_EQ(Result.GetReport().Diagnostics[0].Error.Code, ECacheError::StorageFailure);
	EXPECT_EQ(Result.GetReport().Diagnostics[1].Operation, EBuildOperation::CacheStore);
	EXPECT_EQ(Result.GetReport().Diagnostics[1].Error.Code, ECacheError::StorageFailure);
}

TEST(FBuildExecutionTests, ProducerFailureCancellationAndNoLocalPolicyAreDistinct)
{
	FHarness H;
	H.Function->FailBuild = true;
	auto Failed = H.Run({.QueryCache = false, .StoreOnBuild = false});
	ASSERT_EQ(Failed.GetStatus(), EStatus::Error);
	EXPECT_EQ(Failed.GetFailure()->Reason, EBuildFailureReason::ProducerFailure);
	EXPECT_EQ(Failed.GetFailure()->Operation, EBuildOperation::Build);
	EXPECT_EQ(Failed.GetOutput(), nullptr);
	H.Function->FailBuild = false;
	auto Canceled = H.Run({.QueryCache = false, .StoreOnBuild = false}, FBuildCancellation([] { return true; }));
	EXPECT_EQ(Canceled.GetStatus(), EStatus::Canceled);
	EXPECT_EQ(Canceled.GetFailure(), nullptr); EXPECT_EQ(Canceled.GetOutput(), nullptr);
	auto Disabled = H.Run({.QueryCache = true, .BuildLocal = false, .StoreOnBuild = false});
	ASSERT_EQ(Disabled.GetStatus(), EStatus::Error);
	EXPECT_EQ(Disabled.GetFailure()->Reason, EBuildFailureReason::InputUnavailable);
}

TEST(FBuildExecutionTests, OutputFailureAndThrowingSinksCannotContradictCompletion)
{
	FHarness H;
	H.Function->RejectOutput = true;
	auto Invalid = H.Run({.QueryCache = false, .StoreOnBuild = false});
	ASSERT_EQ(Invalid.GetStatus(), EStatus::Error);
	EXPECT_EQ(Invalid.GetFailure()->Reason, EBuildFailureReason::InvalidOutput);
	EXPECT_EQ(Invalid.GetFailure()->Operation, EBuildOperation::Validate);
	EXPECT_EQ(Invalid.GetOutput(), nullptr);
	FBuildServiceOptions Options;
	Options.Cache.Get = [](const auto&) -> FCacheGetResult {
		return std::unexpected(FCacheError{ECacheError::StorageFailure, "failure"});
	};
	Options.Diagnostics = [](const auto&, const auto&) { throw 7; };
	Options.Metrics = [](auto, auto) { throw 9; };
	auto Service = CreateBuild(std::move(Options)); auto Function = std::make_shared<FFunction>();
	Service->Register(Function).value(); auto Session = Service->CreateSession().value();
	auto Resolver = std::make_shared<FResolver>(); auto Inputs = FBuildInputs::TryCreate(Definition().GetSources(), Resolver).value();
	std::optional<FBuildCompleteParams> Completion;
	ASSERT_TRUE(Session->Build(Definition(), [&](auto Value) { Completion = std::move(Value); }, std::move(Inputs),
		{.Policy = {.StoreOnBuild = false}}));
	ASSERT_TRUE(Completion); EXPECT_EQ(Completion->GetStatus(), EStatus::Ok);
}

TEST(FBuildExecutionTests, ProducerExceptionsBecomeStableInternalFailures)
{
	FHarness H;
	H.Function->ThrowBuild = true;
	auto Result = H.Run({.QueryCache = false, .StoreOnBuild = false});
	ASSERT_EQ(Result.GetStatus(), EStatus::Error);
	ASSERT_NE(Result.GetFailure(), nullptr);
	EXPECT_EQ(Result.GetFailure()->Reason, EBuildFailureReason::InternalFailure);
	EXPECT_EQ(Result.GetFailure()->Operation, EBuildOperation::Build);
	EXPECT_EQ(Result.GetOutput(), nullptr);
}

TEST(FBuildExecutionTests, InputCaptureExceptionsBecomeBoundedStableFailures)
{
	auto Resolver = std::make_shared<FResolver>(); Resolver->ThrowDescribe = true;
	auto Inputs = FBuildInputs::TryCreate(Definition().GetSources(), Resolver);
	ASSERT_FALSE(Inputs);
	EXPECT_EQ(Inputs.error().Reason, EBuildFailureReason::InternalFailure);
	EXPECT_EQ(Inputs.error().Operation, EBuildOperation::Describe);
	EXPECT_EQ(Inputs.error().Description, "describe exception");
}

TEST(FBuildExecutionTests, ExecutionReportMetricsAreBounded)
{
	FHarness H; H.Function->FloodMetrics = true;
	auto Result = H.Run({.QueryCache = false, .StoreOnBuild = false});
	ASSERT_EQ(Result.GetStatus(), EStatus::Ok);
	EXPECT_EQ(Result.GetReport().Metrics.size(), FBuildExecutionReport::MaximumMetrics);
	for (const auto& Metric : Result.GetReport().Metrics)
		EXPECT_LE(Metric.Name.size(), FBuildExecutionReport::MaximumMetricNameBytes);
}

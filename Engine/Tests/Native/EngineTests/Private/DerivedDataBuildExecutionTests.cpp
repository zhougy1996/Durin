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
		bool FailResolve = false;
		FSharedByteBuffer Bytes = FSharedByteBuffer::Take(FByteBuffer(32, std::byte{3}));
		auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
		{
			++Describes;
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
		bool FailBuild = false, RejectOutput = false;
		auto GetDescriptor() const -> FBuildFunctionDescriptor override
		{ return {"Execution.Fixture", 1, 1, "Fixture.Output", 1, FCacheBucket::FromString("Execution")}; }
		auto Build(FBuildContext& Context) const -> FBuildFunctionResult override
		{
			++Builds; Context.ReportMetric("Fixture.Builds", 1);
			if (FailBuild) return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::ProducerFailure,
				.Description = "Producer failed"});
			return FBuildOutput::TryCreate({.Schema = "Fixture.Output", .SchemaVersion = 1,
				.Values = Context.GetInputs()[0].Values}).value();
		}
		auto Validate(const FBuildAction&, const FBuildOutput& Output, const FBuildCancellation&) const
			-> std::expected<void, FBuildFailure> override
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
		bool GetFailure = false, PutFailure = false;
		uint32 SinkCalls = 0;
		FBuildServiceOptions ServiceOptions;
		std::shared_ptr<IBuild> Service;
		std::shared_ptr<FBuildSession> Session;
		FHarness()
		{
			ServiceOptions.Cache.Get = [&](const FCacheGetRequest&) -> FCacheGetResult {
				if (GetFailure) return std::unexpected(FCacheError{ECacheError::StorageFailure, "read failed"});
				if (Stored.IsEmpty()) return std::optional<FSharedByteBuffer>{};
				return std::optional<FSharedByteBuffer>{Stored};
			};
			ServiceOptions.Cache.Put = [&](const FCachePutRequest& Request) -> FCachePutResult {
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

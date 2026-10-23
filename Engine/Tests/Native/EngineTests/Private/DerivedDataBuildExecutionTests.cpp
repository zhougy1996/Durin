#include "DerivedDataBuildSession.h"
#include "Serialization/CompactBinary.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Ref() -> FBuildInputReference { return {"Source", FXxHash128::HashBuffer("captured"), "Fixture", 1, "Bytes", 1}; }
	auto Definition() -> FBuildDefinition { FBuildDefinitionBuilder B("Execution.Fixture"); B.AddInput("Source", "Captured"); return std::move(B).Build().value(); }
	struct FResolver final : IBuildInputResolver
	{
		mutable uint32 Describes = 0, Resolves = 0;
		bool FailDescribe = false;
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override { ++Describes; if (FailDescribe) return std::unexpected(FBuildInputError{"description failed"}); return std::vector{Ref()}; }
		auto Resolve(std::span<const FBuildInputReference>, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInput>, FBuildInputError> override { ++Resolves; return std::vector<FBuildInput>{{.Identity = Ref(), .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(32, std::byte{3}))}}}}; }
	};
	struct FFunction final : IBuildFunction
	{
		mutable uint32 Builds = 0; bool Fail = false, Throw = false, Log = false, ExecutionFail = false, ObserveFailure = false;
		auto GetName() const -> std::string_view override { return "Execution.Fixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& C) const -> void override { C.SetConstantsSchema(1); C.SetOutput("Fixture.Output", 1); C.SetCacheBucket(FCacheBucket::FromString("Execution")); }
		auto Build(FBuildContext& C) const -> void override { ++Builds; if (ExecutionFail) { if (ObserveFailure) { FCbWriter W; W.AddUInt64(17, "Code"); C.AddObservation(FValueId::FromName("Failure"), W.SaveObject().value()); C.AddValue(FValueId::FromName("Partial"), FSharedByteBuffer::Take(FByteBuffer(8))); } return C.FailExecution("temporary resource failure"); } if (Throw) throw std::runtime_error("fixture exception"); if (Fail) return C.AddError("deterministic failure"), void(); const auto* I = C.FindInput("Source"); if (!I) return C.AddError("missing input"), void(); C.AddValue(FValueId::FromName("Data"), I->Values[0].Data); if (Log) C.AddLog("Fixture", EBuildLogSeverity::Warning, "transient warning"); }
	};
	struct FMemoryCache final : ICache
	{
		mutable uint32 Gets = 0, Puts = 0;
		mutable bool Corrupt = false;
		bool FailPut = false;
		mutable std::optional<FCacheRecord> Stored;
		auto Get(const FCacheGetRequest&) const -> FCacheGetResult override
		{
			++Gets;
			if (Corrupt)
			{
				Corrupt = false;
				return std::unexpected(FCacheError{ECacheError::Corrupt, "fixture corruption"});
			}
			return Stored;
		}
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
		{
			++Puts;
			if (FailPut) return std::unexpected(FCacheError{ECacheError::StorageFailure, "fixture write failure"});
			Stored = Request.Record; return {};
		}
	};
	struct FHarness
	{
		std::shared_ptr<FResolver> Resolver = std::make_shared<FResolver>(); std::shared_ptr<FFunction> Function = std::make_shared<FFunction>(); std::shared_ptr<FMemoryCache> Cache = std::make_shared<FMemoryCache>();
		std::shared_ptr<IBuild> Service; std::shared_ptr<FBuildSession> Session;
		FHarness()
		{
			FBuildServiceOptions O; O.Cache = Cache;
			Service = CreateBuild(std::move(O)); Service->Register(Function).value(); Session = Service->CreateSession().value();
		}
		auto Run(FBuildRequestOptions O = {}) -> FBuildCompleteParams
		{
			O.InputResolver = Resolver; FRequestOwner Owner; std::optional<FBuildCompleteParams> Result;
			Session->Build(Definition(), Owner, [&](auto V) { Result = std::move(V); }, {}, std::move(O)).value();
			require(Owner.Wait() == ERequestWaitResult::Completed); return std::move(*Result);
		}
	};
}

TEST(FBuildExecutionTests, ColdBuildPersistsAndWarmHitAvoidsResolve)
{
	FHarness H; auto Cold = H.Run(); ASSERT_EQ(Cold.GetStatus(), EStatus::Ok); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u); EXPECT_EQ(H.Cache->Puts, 1u); EXPECT_EQ(H.Resolver->Describes, 1u);
	auto Warm = H.Run(); ASSERT_EQ(Warm.GetStatus(), EStatus::Ok); EXPECT_TRUE(HasBuildExecutionFlag(Warm.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u); EXPECT_EQ(H.Resolver->Describes, 2u);
}

TEST(FBuildExecutionTests, CorruptRecordFallsBackOnce)
{
	FHarness H; H.Cache->Corrupt = true; auto Result = H.Run(); EXPECT_EQ(Result.GetStatus(), EStatus::Ok); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Cache->Gets, 1u); EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, DescriptorOnlyRecordFallsBackAndIsReplacedByCompleteOutput)
{
	FHarness H;
	ASSERT_EQ(H.Run().GetStatus(), EStatus::Ok);
	ASSERT_TRUE(H.Cache->Stored);
	H.Cache->Stored = H.Cache->Stored->RemoveValueData();
	auto Result = H.Run();
	ASSERT_EQ(Result.GetStatus(), EStatus::Ok);
	EXPECT_FALSE(HasBuildExecutionFlag(Result.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit));
	EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(H.Resolver->Resolves, 2u);
	EXPECT_EQ(H.Cache->Puts, 2u);
	ASSERT_TRUE(H.Cache->Stored);
	ASSERT_NE(Result.GetCacheKey(), nullptr);
	EXPECT_TRUE(H.Cache->Stored->ToOutput(*Result.GetCacheKey()));
	EXPECT_TRUE(HasBuildExecutionFlag(H.Run().GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit));
	EXPECT_EQ(H.Function->Builds, 2u);
}

TEST(FBuildExecutionTests, PersistenceFailurePreservesSuccessfulLocalOutput)
{
	FHarness H; H.Cache->FailPut = true;
	auto Result = H.Run(); ASSERT_EQ(Result.GetStatus(), EStatus::Ok);
	ASSERT_NE(Result.GetOutput(), nullptr);
	EXPECT_TRUE(Result.GetOutput()->CheckLimits());
	EXPECT_EQ(Result.GetOutput()->GetValues().size(), 1u);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Cache->Puts, 1u);
	EXPECT_FALSE(H.Cache->Stored);
}

TEST(FBuildExecutionTests, TransientLogsPreventCacheStorage)
{
	FHarness H; H.Function->Log = true; auto Result = H.Run(); ASSERT_EQ(Result.GetStatus(), EStatus::Ok); ASSERT_NE(Result.GetOutput(), nullptr); EXPECT_TRUE(Result.GetOutput()->HasLogs()); EXPECT_EQ(H.Cache->Puts, 0u);
}

TEST(FBuildExecutionTests, ErrorOutputIsNotPersistedAndNextRequestRebuilds)
{
	FHarness H; H.Function->Fail = true; auto Cold = H.Run(); ASSERT_EQ(Cold.GetStatus(), EStatus::Error); ASSERT_NE(Cold.GetOutput(), nullptr); EXPECT_TRUE(Cold.GetOutput()->HasError()); EXPECT_EQ(H.Cache->Puts, 0u);
	H.Function->Fail = false; auto Retry = H.Run(); EXPECT_EQ(Retry.GetStatus(), EStatus::Ok); EXPECT_FALSE(HasBuildExecutionFlag(Retry.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)); EXPECT_EQ(H.Function->Builds, 2u); EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, CancellationRemainsDistinct)
{
	FHarness H; FBuildRequestOptions O; O.Cancellation = FBuildCancellation([] { return true; }); auto Result = H.Run(std::move(O)); EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); EXPECT_EQ(Result.GetOutput(), nullptr);
}

TEST(FBuildExecutionTests, ProducerExceptionIsInfrastructureFailureAndIsNotCached)
{
	FHarness H; H.Function->Throw = true; auto Result = H.Run(); EXPECT_EQ(Result.GetStatus(), EStatus::Error); EXPECT_EQ(Result.GetOutput(), nullptr); EXPECT_EQ(H.Cache->Puts, 0u);
}

TEST(FBuildExecutionTests, ExecutionFailureCanRetryWithSameIdentityAndLargerCacheBudget)
{
	FHarness H;
	H.Function->ExecutionFail = true;
	FBuildRequestOptions Limited;
	Limited.Policy.MaximumWorkingSetBytes = 1024;
	auto Failed = H.Run(Limited);
	EXPECT_EQ(Failed.GetStatus(), EStatus::Error);
	EXPECT_EQ(Failed.GetOutput(), nullptr);
	EXPECT_FALSE(Failed.GetDiagnostic().empty());
	ASSERT_NE(Failed.GetCacheKey(), nullptr);
	EXPECT_EQ(H.Cache->Puts, 0u);
	H.Function->ExecutionFail = false;
	auto Recovered = H.Run();
	EXPECT_EQ(Recovered.GetStatus(), EStatus::Ok);
	EXPECT_TRUE(Recovered.GetDiagnostic().empty());
	ASSERT_NE(Recovered.GetCacheKey(), nullptr);
	EXPECT_EQ(*Failed.GetCacheKey(), *Recovered.GetCacheKey());
	EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(H.Cache->Puts, 1u);
	EXPECT_TRUE(HasBuildExecutionFlag(H.Run().GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit));
}

TEST(FBuildExecutionTests, RequestDescriptionFailureCompletesWithoutProducerOrCacheQuery)
{
	FHarness H; H.Resolver->FailDescribe = true;
	auto Result = H.Run();
	EXPECT_EQ(Result.GetStatus(), EStatus::Error);
	EXPECT_EQ(Result.GetOutput(), nullptr);
	EXPECT_EQ(H.Resolver->Describes, 1u);
	EXPECT_EQ(H.Resolver->Resolves, 0u);
	EXPECT_EQ(H.Function->Builds, 0u);
	EXPECT_EQ(H.Cache->Gets, 0u);
}

TEST(FBuildExecutionTests, RequestResolverOverridesSessionResolverAndWarmHitSkipsPayload)
{
	FHarness H;
	auto Shared = std::make_shared<FResolver>(); Shared->FailDescribe = true;
	H.Session = H.Service->CreateSession(Shared).value();
	EXPECT_EQ(H.Run().GetStatus(), EStatus::Ok);
	EXPECT_EQ(H.Run().GetStatus(), EStatus::Ok);
	EXPECT_EQ(Shared->Describes, 0u);
	EXPECT_EQ(H.Resolver->Describes, 2u);
	EXPECT_EQ(H.Resolver->Resolves, 1u);
}

TEST(FBuildExecutionTests, AdmissionRejectsAmbiguousInputsWithoutCompletion)
{
	FHarness H;
	auto D = Definition();
	auto Inputs = std::move(FBuildInputsBuilder(D.GetSources(), H.Resolver)).Build().value();
	FRequestOwner Owner;
	FBuildRequestOptions Options; Options.InputResolver = H.Resolver;
	uint32 Calls = 0;
	auto Request = H.Session->Build(std::move(D), Owner, [&](auto) { ++Calls; },
		std::move(Inputs), std::move(Options));
	ASSERT_FALSE(Request);
	EXPECT_EQ(Request.error().Reason, EBuildAdmissionReason::InvalidRequest);
	EXPECT_EQ(Calls, 0u);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildExecutionTests, CompletionAndRetainedHandleDoNotPinCapturedResolver)
{
	FHarness H;
	std::weak_ptr<const IBuildInputResolver> Captured = H.Resolver;
	FBuildRequestOptions Options; Options.InputResolver = std::move(H.Resolver);
	FRequestOwner Owner;
	auto Request = H.Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		EXPECT_TRUE(Captured.expired());
	}, {}, std::move(Options));
	ASSERT_TRUE(Request);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_TRUE(Captured.expired());
}

TEST(FBuildExecutionTests, QueuedProducerUsesOnePreparationAndSkipsProductionOnWarmHit)
{
	class FQueuedScheduler final : public IBuildScheduler
	{
	public:
		std::deque<std::function<void()>> Queue;
		uint32 Submissions = 0;
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{ ++Submissions; Queue.push_back(std::move(Work)); return std::shared_ptr<IBuildScheduledWork>{}; }
		auto Run() -> void
		{ while (!Queue.empty()) { auto Work = std::move(Queue.front()); Queue.pop_front(); Work(); } }
	};
	FHarness H;
	auto Producer = std::make_shared<FQueuedScheduler>();
	auto Session = H.Service->CreateSession({}, Producer).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	for (uint32 Round = 0; Round < 2; ++Round)
	{
		auto Request = Session->Build(Definition(), Owner, [&](auto Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
			EXPECT_EQ(HasBuildExecutionFlag(Result.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit), Round == 1);
			++Calls;
		}, {}, {.InputResolver = H.Resolver});
		ASSERT_TRUE(Request);
		EXPECT_EQ(Calls, Round);
		Producer->Run();
		EXPECT_EQ(Request->Wait(), ERequestWaitResult::Completed);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	}
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(H.Resolver->Describes, 2u);
	EXPECT_EQ(H.Resolver->Resolves, 1u);
	EXPECT_EQ(H.Function->Builds, 1u);
	EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, EarlyInlineSuccessHitsCacheAndErrorsRetry)
{
	for (bool Negative : {false, true})
	{
		FHarness H; H.Function->Fail = Negative;
		FBuildRequestOptions Options; Options.Policy.ResultDelivery = EBuildResultDelivery::BeforePersistence;
		auto Cold = H.Run(Options), Warm = H.Run(Options);
		EXPECT_EQ(Cold.GetStatus(), Negative ? EStatus::Error : EStatus::Ok);
		EXPECT_EQ(Warm.GetStatus(), Cold.GetStatus());
		ASSERT_TRUE(Cold.GetOutput()); ASSERT_TRUE(Warm.GetOutput());
		EXPECT_EQ(H.Function->Builds, Negative ? 2u : 1u); EXPECT_EQ(H.Cache->Puts, Negative ? 0u : 1u); EXPECT_EQ(H.Resolver->Resolves, Negative ? 2u : 1u);
		EXPECT_EQ(HasBuildExecutionFlag(Warm.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit), !Negative);
		EXPECT_EQ(Cold.GetOutput()->GetValues().size(), Warm.GetOutput()->GetValues().size());
		if (!Negative) EXPECT_EQ(Cold.GetOutput()->GetValues()[0].Value.GetRawHash(), Warm.GetOutput()->GetValues()[0].Value.GetRawHash());
	}
}

TEST(FBuildExecutionTests, ExecutionFailureReturnsOnlyTransientObservationsAndNeverCachesThem)
{
	FHarness H; H.Function->ExecutionFail = true; H.Function->ObserveFailure = true;
	for (uint32 Attempt = 0; Attempt < 2; ++Attempt) {
		auto Failed = H.Run();
		ASSERT_EQ(Failed.GetStatus(), EStatus::Error); ASSERT_NE(Failed.GetOutput(), nullptr);
		EXPECT_TRUE(Failed.GetOutput()->GetValues().empty()); EXPECT_TRUE(Failed.GetOutput()->GetMetadata().empty());
		ASSERT_EQ(Failed.GetOutput()->GetObservations().size(), 1u);
		EXPECT_EQ(Failed.GetOutput()->GetObservations()[0].Object.GetView().Find("Code").AsUInt64(), 17u);
		EXPECT_EQ(H.Cache->Puts, 0u);
	}
	H.Function->ExecutionFail = false;
	EXPECT_EQ(H.Run().GetStatus(), EStatus::Ok); EXPECT_EQ(H.Function->Builds, 3u); EXPECT_EQ(H.Cache->Puts, 1u);
}

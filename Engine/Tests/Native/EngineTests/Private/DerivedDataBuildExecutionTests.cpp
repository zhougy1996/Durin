#include "RequestOwnerAccess.h"
#include "BuildServiceBackendPrivate.h"
#include "BuildCacheRecordPrivate.h"
#include "CacheDispatchPrivate.h"
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
		auto ResolveInputMeta(const FBuildDefinition& Definition, IRequestOwner& Owner, FOnBuildInputMetaResolved&& OnResolved) const -> void override
			{
				if (Owner.IsCanceled()) return OnResolved({{}, EStatus::Canceled}); ++Describes; if (FailDescribe) return OnResolved({{}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Error}); return OnResolved({std::vector{Ref()}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Ok}); }
		auto ResolveInputData(const FBuildAction& Action, IRequestOwner& Owner, FOnBuildInputDataResolved&& OnResolved) const -> void override
			{
				if (Owner.IsCanceled()) return OnResolved({{}, EStatus::Canceled}); ++Resolves; return OnResolved({std::vector<FBuildInput>{{.Identity = Ref(), .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(32, std::byte{3}))}}}}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Ok}); }
	};
	struct FFunction final : IBuildFunction
	{
		mutable uint32 Builds = 0; bool Fail = false, Throw = false, Log = false, ExecutionFail = false, ObserveFailure = false;
		auto GetName() const -> std::string_view override { return "Execution.Fixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& C) const -> void override { C.SetConstantsSchema(1); C.SetOutput("Fixture.Output", 1); C.SetCacheBucket(FCacheBucket::FromString("Execution")); }
		auto Build(FBuildContext& C) const -> void override { ++Builds; if (ExecutionFail) { if (ObserveFailure) { FCbWriter W; W.AddUInt64(17, "Code"); C.AddObservation(FValueId::FromName("Failure"), W.SaveObject().value()); C.AddValue(FValueId::FromName("Partial"), FSharedByteBuffer::Take(FByteBuffer(8))); } return C.FailExecution("temporary resource failure"); } if (Throw) throw std::runtime_error("fixture exception"); if (Fail) return C.AddError("deterministic failure"), void(); const auto* I = C.FindInput("Source"); if (!I) return C.AddError("missing input"), void(); C.AddValue(FValueId::FromName("Data"), I->Values[0].Data); if (Log) C.AddLog("Fixture", EBuildLogSeverity::Warning, "transient warning"); }
	};
	struct FMemoryCache final : ICacheBackend
	{
		mutable uint32 Gets = 0, Puts = 0;
		mutable bool Corrupt = false;
		bool FailPut = false;
		mutable std::optional<FCacheRecord> Stored;
		mutable FCacheResourceLimits LastGetLimits, LastPutLimits;
		auto Get(const FCacheGetBackendRequest& Request) const -> FCacheGetResult override
		{
			++Gets;
			LastGetLimits = {Request.RecordLimits, Request.MaximumEncodedBytes, Request.MaximumWorkingSetBytes};
			if (Corrupt)
			{
				Corrupt = false;
				return std::unexpected(FCacheError{ECacheError::Corrupt, "fixture corruption"});
			}
			return Stored;
		}
		auto Put(const FCachePutBackendRequest& Request) const -> FCachePutResult override
		{
			++Puts;
			LastPutLimits = {Request.RecordLimits, Request.MaximumEncodedBytes, Request.MaximumWorkingSetBytes};
			if (FailPut) return std::unexpected(FCacheError{ECacheError::StorageFailure, "fixture write failure"});
			Stored = Request.Record; return Request.Record;
		}
	};
	struct FHarness
	{
		std::shared_ptr<FResolver> Resolver = std::make_shared<FResolver>(); std::shared_ptr<FFunction> Function = std::make_shared<FFunction>(); std::shared_ptr<FMemoryCache> Cache = std::make_shared<FMemoryCache>();
		std::shared_ptr<IBuild> Service; std::shared_ptr<FBuildSession> Session;
		FCacheResourceLimits CacheLimits;
		FHarness(FCacheResourceLimits Limits = {}) : CacheLimits(Limits)
		{
			Durin::DerivedData::Private::FBuildBackendOptions O;
			O.Cache = Cache;
			O.CacheLimits = Limits;
			Service = Durin::DerivedData::Private::CreateBuildForBackend(std::move(O));
			Service->Register(Function).value();
			Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
		}
		auto Run(FBuildRequestLimits Limits = {}, bool Canceled = false, FBuildPolicy Policy = {}) -> FBuildCompleteParams
		{
			auto Configured = Durin::DerivedData::Private::CreateBuildForBackend({.BuildLimits = Limits, .Cache = Cache, .CacheLimits = CacheLimits});
			Configured->Register(Function).value();
			auto Session = Configured->CreateSession("Fixture", Resolver);
			FRequestOwner Owner; if (Canceled) Owner.Cancel(); std::optional<FBuildCompleteParams> Result;
			Session.Build(Definition(), {}, Policy, Owner, [&](auto Value) { Result = std::move(Value); });
			Owner.Wait(); return std::move(*Result);
		}

	};
}

TEST(FBuildExecutionTests, ColdBuildPersistsAndWarmHitAvoidsResolve)
{
	FHarness H; auto Cold = H.Run(); ASSERT_EQ(Cold.Status, EStatus::Ok); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u); EXPECT_EQ(H.Cache->Puts, 1u); EXPECT_EQ(H.Resolver->Describes, 1u);
	auto Warm = H.Run(); ASSERT_EQ(Warm.Status, EStatus::Ok); EXPECT_TRUE(HasBuildExecutionFlag(Warm.BuildStatus, EBuildExecutionFlags::CacheQueryHit)); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u); EXPECT_EQ(H.Resolver->Describes, 2u);
}

TEST(FBuildExecutionTests, CorruptRecordFallsBackOnce)
{
	FHarness H; H.Cache->Corrupt = true; auto Result = H.Run(); EXPECT_EQ(Result.Status, EStatus::Ok); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Cache->Gets, 1u); EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, DescriptorOnlyRecordFallsBackAndIsReplacedByCompleteOutput)
{
	FHarness H;
	ASSERT_EQ(H.Run().Status, EStatus::Ok);
	ASSERT_TRUE(H.Cache->Stored);
	H.Cache->Stored = H.Cache->Stored->RemoveValueData();
	auto Result = H.Run();
	ASSERT_EQ(Result.Status, EStatus::Ok);
	EXPECT_FALSE(HasBuildExecutionFlag(Result.BuildStatus, EBuildExecutionFlags::CacheQueryHit));
	EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(H.Resolver->Resolves, 2u);
	EXPECT_EQ(H.Cache->Puts, 2u);
	ASSERT_TRUE(H.Cache->Stored);
	ASSERT_TRUE(Result.CacheKey.IsValid());
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Result.CacheKey, *H.Cache->Stored));
	EXPECT_TRUE(HasBuildExecutionFlag(H.Run().BuildStatus, EBuildExecutionFlags::CacheQueryHit));
	EXPECT_EQ(H.Function->Builds, 2u);
}

TEST(FBuildExecutionTests, PersistenceFailurePreservesSuccessfulLocalOutput)
{
	FHarness H; H.Cache->FailPut = true;
	auto Result = H.Run(); ASSERT_EQ(Result.Status, EStatus::Ok);
	ASSERT_TRUE(Result.Output.IsValid());
	EXPECT_TRUE(Result.Output.CheckLimits());
	EXPECT_EQ(Result.Output.GetValues().size(), 1u);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Cache->Puts, 1u);
	EXPECT_FALSE(H.Cache->Stored);
}

TEST(FBuildExecutionTests, TransientLogsPreventCacheStorage)
{
	FHarness H; H.Function->Log = true; auto Result = H.Run(); ASSERT_EQ(Result.Status, EStatus::Ok); ASSERT_TRUE(Result.Output.IsValid()); EXPECT_TRUE(Result.Output.HasLogs()); EXPECT_EQ(H.Cache->Puts, 0u);
}

TEST(FBuildExecutionTests, ErrorOutputIsNotPersistedAndNextRequestRebuilds)
{
	FHarness H; H.Function->Fail = true; auto Cold = H.Run(); ASSERT_EQ(Cold.Status, EStatus::Error); ASSERT_TRUE(Cold.Output.IsValid()); EXPECT_TRUE(Cold.Output.HasError()); EXPECT_EQ(H.Cache->Puts, 0u);
	H.Function->Fail = false; auto Retry = H.Run(); EXPECT_EQ(Retry.Status, EStatus::Ok); EXPECT_FALSE(HasBuildExecutionFlag(Retry.BuildStatus, EBuildExecutionFlags::CacheQueryHit)); EXPECT_EQ(H.Function->Builds, 2u); EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, CancellationRemainsDistinct)
{
	FHarness H; auto Result = H.Run({}, true); EXPECT_EQ(Result.Status, EStatus::Canceled); EXPECT_TRUE(Result.Output.IsValid());
}

TEST(FBuildExecutionTests, UniformPoliciesControlQueryBuildAndStoreIndependently)
{
	FHarness H;
	auto Run = [&](EBuildPolicy Policy) { return H.Run({}, false, Policy); };
	EXPECT_EQ(Run(EBuildPolicy::None).Status, EStatus::Error);
	EXPECT_EQ(Run(EBuildPolicy::Cache).Status, EStatus::Error);
	EXPECT_EQ(H.Function->Builds, 0u);
	EXPECT_EQ(H.Cache->Gets, 1u);
	EXPECT_EQ(Run(EBuildPolicy::Build).Status, EStatus::Ok);
	EXPECT_EQ(H.Cache->Gets, 1u);
	EXPECT_EQ(H.Cache->Puts, 0u);
	EXPECT_EQ(Run(EBuildPolicy::Build | EBuildPolicy::CacheStore).Status, EStatus::Ok);
	EXPECT_EQ(H.Cache->Gets, 1u);
	EXPECT_EQ(H.Cache->Puts, 1u);
	auto Hit = Run(EBuildPolicy::CacheQuery);
	EXPECT_EQ(Hit.Status, EStatus::Ok);
	EXPECT_TRUE(HasBuildExecutionFlag(Hit.BuildStatus, EBuildExecutionFlags::CacheQueryHit));
	EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(Run(EBuildPolicy::Build | EBuildPolicy::CacheStore).Status, EStatus::Ok);
	EXPECT_EQ(H.Function->Builds, 3u);
	EXPECT_EQ(H.Cache->Gets, 2u);
	EXPECT_EQ(H.Cache->Puts, 2u);
}

TEST(FBuildExecutionTests, ProductLimitRejectsBothLocalOutputAndWarmCacheOutput)
{
	FHarness H;
	ASSERT_EQ(H.Run().Status, EStatus::Ok);
	FBuildRequestLimits Limited; Durin::DerivedData::FBuildPolicy LimitedPolicy = EBuildPolicy::CacheQuery;
	Limited.Output.MaximumTotalBytes = 1;
	EXPECT_EQ(H.Run(Limited, false, LimitedPolicy).Status, EStatus::Error);
	EXPECT_EQ(H.Function->Builds, 1u);
	LimitedPolicy = EBuildPolicy::Build;
	EXPECT_EQ(H.Run(Limited, false, LimitedPolicy).Status, EStatus::Error);
	EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(H.Cache->Puts, 1u);
	EXPECT_EQ(H.Run().Status, EStatus::Ok);
	EXPECT_EQ(H.Function->Builds, 2u);
}

TEST(FBuildExecutionTests, ServiceResourceCeilingsReachCacheIndependentlyOfProductLimits)
{
	FCacheResourceLimits Limits{.Record = {.MaximumTotalBytes = 1234, .MaximumMetadataBytes = 512, .MaximumValues = 7},
		.MaximumEncodedBytes = 2345, .MaximumWorkingSetBytes = 3456};
	FHarness H(Limits);
	ASSERT_EQ(H.Run().Status, EStatus::Ok);
	const auto Check = [&](const FCacheResourceLimits& Actual) {
		EXPECT_EQ(Actual.Record.MaximumTotalBytes, Limits.Record.MaximumTotalBytes);
		EXPECT_EQ(Actual.Record.MaximumMetadataBytes, Limits.Record.MaximumMetadataBytes);
		EXPECT_EQ(Actual.Record.MaximumValues, Limits.Record.MaximumValues);
		EXPECT_EQ(Actual.MaximumEncodedBytes, Limits.MaximumEncodedBytes);
		EXPECT_EQ(Actual.MaximumWorkingSetBytes, Limits.MaximumWorkingSetBytes);
	};
	Check(H.Cache->LastGetLimits);
	Check(H.Cache->LastPutLimits);
	FBuildRequestLimits Options; Options.Output.MaximumTotalBytes = 8192;
	ASSERT_EQ(H.Run(Options).Status, EStatus::Ok);
	Check(H.Cache->LastGetLimits);
	EXPECT_EQ(H.Function->Builds, 1u);
}

TEST(FBuildExecutionTests, ProducerExceptionIsInfrastructureFailureAndIsNotCached)
{
	FHarness H; H.Function->Throw = true; auto Result = H.Run(); EXPECT_EQ(Result.Status, EStatus::Error); EXPECT_TRUE(Result.Output.IsValid()); EXPECT_EQ(H.Cache->Puts, 0u);
}

TEST(FBuildExecutionTests, ExecutionFailureCanRetryWithSameIdentityAndDifferentProductLimit)
{
	FHarness H;
	H.Function->ExecutionFail = true;
	FBuildRequestLimits Limited;
	Limited.Output.MaximumTotalBytes = 1024;
	auto Failed = H.Run(Limited);
	EXPECT_EQ(Failed.Status, EStatus::Error);
	EXPECT_TRUE(Failed.Output.IsValid());
	EXPECT_FALSE((Failed.Output.GetLogs().empty() ? std::string_view{} : std::string_view(Failed.Output.GetLogs().front().Text)).empty());
	ASSERT_TRUE(Failed.CacheKey.IsValid());
	EXPECT_EQ(H.Cache->Puts, 0u);
	H.Function->ExecutionFail = false;
	auto Recovered = H.Run();
	EXPECT_EQ(Recovered.Status, EStatus::Ok);
	EXPECT_TRUE((Recovered.Output.GetLogs().empty() ? std::string_view{} : std::string_view(Recovered.Output.GetLogs().front().Text)).empty());
	ASSERT_TRUE(Recovered.CacheKey.IsValid());
	EXPECT_EQ(Failed.CacheKey, Recovered.CacheKey);
	EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(H.Cache->Puts, 1u);
	EXPECT_TRUE(HasBuildExecutionFlag(H.Run().BuildStatus, EBuildExecutionFlags::CacheQueryHit));
}

TEST(FBuildExecutionTests, RequestDescriptionFailureCompletesWithoutProducerOrCacheQuery)
{
	FHarness H; H.Resolver->FailDescribe = true;
	auto Result = H.Run();
	EXPECT_EQ(Result.Status, EStatus::Error);
	EXPECT_TRUE(Result.Output.IsValid());
	EXPECT_EQ(H.Resolver->Describes, 1u);
	EXPECT_EQ(H.Resolver->Resolves, 0u);
	EXPECT_EQ(H.Function->Builds, 0u);
	EXPECT_EQ(H.Cache->Gets, 0u);
}

TEST(FBuildExecutionTests, CapturedInputsCanUseSessionResolver)
{
	FHarness H;
	auto D = Definition();
	FBuildInputsBuilder Builder; Builder.AddInput({.Identity = Ref(), .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(32, std::byte{3}))}}}); auto Inputs = std::move(Builder).Build();
	FRequestOwner Owner;

	uint32 Calls = 0;
	H.Service->CreateSession("Fixture", H.Resolver).Build(std::move(D), std::move(Inputs), {}, Owner, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });


	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildExecutionTests, ConfiguredResolverIsReleasedAfterOwnerCompletes)
{
	FHarness H;
	std::weak_ptr<const IBuildInputResolver> Captured = H.Resolver;

	FRequestOwner Owner;
	H.Service->CreateSession("Fixture", std::move(H.Resolver)).Build(Definition(), {}, {}, Owner, [&](FBuildCompleteParams Result) {
		EXPECT_EQ(Result.Status, EStatus::Ok);
		EXPECT_FALSE(Captured.expired()); });

	Owner.Wait();
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
			-> std::shared_ptr<IBuildScheduledWork> override
		{ ++Submissions; Queue.push_back(std::move(Work)); return std::shared_ptr<IBuildScheduledWork>{}; }
		auto Run() -> void
		{ while (!Queue.empty()) { auto Work = std::move(Queue.front()); Queue.pop_front(); Work(); } }
	};
	FHarness H;
	auto Producer = std::make_shared<FQueuedScheduler>();
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(H.Service->CreateSession("Fixture", H.Resolver, Producer));
	FRequestOwner Owner;
	uint32 Calls = 0;
	for (uint32 Round = 0; Round < 2; ++Round)
	{
		Session->Build(Definition(), {}, {}, Owner, [&](auto Result) {
			EXPECT_EQ(Result.Status, EStatus::Ok);
			EXPECT_EQ(HasBuildExecutionFlag(Result.BuildStatus, EBuildExecutionFlags::CacheQueryHit), Round == 1);
			++Calls;
		});

		EXPECT_EQ(Calls, Round);
		Producer->Run();
		Owner.Wait();
		Owner.Wait();
	}
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(H.Resolver->Describes, 2u);
	EXPECT_EQ(H.Resolver->Resolves, 1u);
	EXPECT_EQ(H.Function->Builds, 1u);
	EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, InlineSuccessHitsCacheAndErrorsRetry)
{
	for (bool Negative : {false, true})
	{
		FHarness H; H.Function->Fail = Negative;
		auto Cold = H.Run(), Warm = H.Run();
		EXPECT_EQ(Cold.Status, Negative ? EStatus::Error : EStatus::Ok);
		EXPECT_EQ(Warm.Status, Cold.Status);
		ASSERT_TRUE(Cold.Output.IsValid()); ASSERT_TRUE(Warm.Output.IsValid());
		EXPECT_EQ(H.Function->Builds, Negative ? 2u : 1u); EXPECT_EQ(H.Cache->Puts, Negative ? 0u : 1u); EXPECT_EQ(H.Resolver->Resolves, Negative ? 2u : 1u);
		EXPECT_EQ(HasBuildExecutionFlag(Warm.BuildStatus, EBuildExecutionFlags::CacheQueryHit), !Negative);
		EXPECT_EQ(Cold.Output.GetValues().size(), Warm.Output.GetValues().size());
		if (!Negative) EXPECT_EQ(Cold.Output.GetValues()[0].Value.GetRawHash(), Warm.Output.GetValues()[0].Value.GetRawHash());
	}
}

TEST(FBuildExecutionTests, ExecutionFailureReturnsOnlyTransientObservationsAndNeverCachesThem)
{
	FHarness H; H.Function->ExecutionFail = true; H.Function->ObserveFailure = true;
	for (uint32 Attempt = 0; Attempt < 2; ++Attempt) {
		auto Failed = H.Run();
		ASSERT_EQ(Failed.Status, EStatus::Error); ASSERT_TRUE(Failed.Output.IsValid());
		EXPECT_TRUE(Failed.Output.GetValues().empty()); EXPECT_TRUE(Failed.Output.GetMetadata().empty());
		ASSERT_EQ(Failed.Output.GetObservations().size(), 1u);
		EXPECT_EQ(Failed.Output.GetObservations()[0].Object.GetView().Find("Code").AsUInt64(), 17u);
		EXPECT_EQ(H.Cache->Puts, 0u);
	}
	H.Function->ExecutionFail = false;
	EXPECT_EQ(H.Run().Status, EStatus::Ok); EXPECT_EQ(H.Function->Builds, 3u); EXPECT_EQ(H.Cache->Puts, 1u);
}

#include <gtest/gtest.h>

#include "AssetBuild/BuildCache.h"
#include "AssetBuild/BuildHost.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset::Build;

	auto GetBuildHostTestGate() -> FModuleOwnedCallbackGate
	{
		static FModuleTestOwner Context("AssetBuildCoreTests.Host");
		static auto Registration = Context.CreateOwnedCallbackRegistration(
			"AssetBuildCoreTests.Host");
		return Registration.GetGate();
	}

	class FScopedDerivedDataCacheDirectory
	{
	public:
		FScopedDerivedDataCacheDirectory()
			: Previous(FPaths::DerivedDataCacheDir()),
			  Root(Testing::GetTestWorkDirectory() / "AssetBuildCoreTests")
		{
			Testing::RemoveTestWorkDirectory(Root);
			FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedDerivedDataCacheDirectory()
		{
			FPaths::SetDerivedDataCacheDirForTests(Previous);
			Testing::RemoveTestWorkDirectory(Root);
		}

	private:
		std::string Previous;
		std::filesystem::path Root;
	};
}

TEST(FAssetBuildCoreTests, CacheClientHonorsExplicitQueryAndStorePolicies)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	Asset::FDerivedDataObjectStore Store("AssetBuildCoreTests/Objects", 1024);
	FBuildCacheClient Client(Store);
	const std::string Key(32, 'a');
	FBuildCachePolicy Policy;
	EXPECT_EQ(Client.Query(Key, "Value", Policy).Status, EBuildCacheQueryStatus::Missing);
	const FBuildValue Value = FBuildValue::FromOwned("Value", {7, 8, 9});
	std::string Error;
	ASSERT_TRUE(Client.Store(Key, Value, Policy, &Error)) << Error;
	const FBuildCacheQueryResult Hit = Client.Query(Key, "Value", Policy);
	ASSERT_EQ(Hit.Status, EBuildCacheQueryStatus::Hit) << Hit.Diagnostic;
	EXPECT_TRUE(std::ranges::equal(Hit.Value.GetBytes(), Value.GetBytes()));
	Policy.bQueryCache = false;
	EXPECT_EQ(Client.Query(Key, "Value", Policy).Status, EBuildCacheQueryStatus::Skipped);
	Policy.bStoreBuildResult = false;
	EXPECT_TRUE(Client.Store(std::string(32, 'b'), Value, Policy, &Error));
}

TEST(FAssetBuildCoreTests, HostAcceptsMultipleServicesAndDrainsInDeclaredOrder)
{
	ShutdownBuildHost();
	std::vector<std::string> Events;
	uint32 PumpedA = 0;
	uint32 PumpedB = 0;
	auto MakeService = [&Events](std::string Identity, int32 DrainOrder, uint32& Pumped) {
		const std::string CallbackIdentity = Identity;
		return FBuildServiceContribution{
			.Identity = std::move(Identity),
			.DrainOrder = DrainOrder,
			.OwnerGate = GetBuildHostTestGate(),
			.Start = [&Events, Name = CallbackIdentity] { Events.push_back("start:" + Name); return true; },
			.StopAdmission = [&Events, Name = CallbackIdentity] { Events.push_back("stop:" + Name); },
			.PumpCompletions = [&Pumped](uint32 Maximum) { const uint32 Count = std::min(Maximum, 1u); Pumped += Count; return Count; },
			.Wait = [](double) { return true; },
			.Drain = [&Events, Name = CallbackIdentity] { Events.push_back("drain:" + Name); },
			.Snapshot = [] { return std::tuple{1u, 2u, uint64{3}}; }};
	};
	std::string Error;
	auto ServiceA = RegisterBuildServiceContribution(MakeService("Durin.Tests.A", 1, PumpedA), &Error);
	auto ServiceB = RegisterBuildServiceContribution(MakeService("Durin.Tests.B", 2, PumpedB), &Error);
	ASSERT_TRUE(ServiceA.IsValid() && ServiceB.IsValid()) << Error;
	EXPECT_FALSE(RegisterBuildServiceContribution(MakeService("Durin.Tests.A", 0, PumpedA), &Error).IsValid());
	ASSERT_TRUE(InitializeBuildHost(&Error)) << Error;
	EXPECT_TRUE(InitializeBuildHost(&Error));
	const FBuildHostSnapshot Snapshot = GetBuildHostSnapshot();
	EXPECT_EQ(Snapshot.ServiceCount, 2u);
	EXPECT_EQ(Snapshot.QueuedRequestCount, 2u);
	EXPECT_EQ(Snapshot.RunningRequestCount, 4u);
	EXPECT_EQ(Snapshot.InFlightEstimatedBytes, 6u);
	EXPECT_EQ(PumpBuildHostCompletions(2), 2u);
	EXPECT_EQ(PumpedA + PumpedB, 2u);
	EXPECT_TRUE(WaitForBuildHost(1.0));
	ShutdownBuildHost();
	ShutdownBuildHost();
	const auto DrainB = std::ranges::find(Events, "drain:Durin.Tests.B");
	const auto DrainA = std::ranges::find(Events, "drain:Durin.Tests.A");
	ASSERT_NE(DrainA, Events.end());
	ASSERT_NE(DrainB, Events.end());
	EXPECT_LT(DrainB, DrainA);
}

TEST(FAssetBuildCoreTests, HostRollsBackPartialStartupAndAllowsRetry)
{
	ShutdownBuildHost();
	uint32 Starts = 0;
	uint32 Stops = 0;
	uint32 Drains = 0;
	bool bAllowSecond = false;
	std::string Error;
	auto First = RegisterBuildServiceContribution({
		.Identity = "Durin.Tests.RollbackA",
		.DrainOrder = 1,
		.OwnerGate = GetBuildHostTestGate(),
		.Start = [&] { ++Starts; return true; },
		.StopAdmission = [&] { ++Stops; },
		.Wait = [](double) { return true; },
		.Drain = [&] { ++Drains; }}, &Error);
	auto Second = RegisterBuildServiceContribution({
		.Identity = "Durin.Tests.RollbackB",
		.DrainOrder = 2,
		.OwnerGate = GetBuildHostTestGate(),
		.Start = [&] { ++Starts; return bAllowSecond; }}, &Error);
	ASSERT_TRUE(First.IsValid() && Second.IsValid()) << Error;
	EXPECT_FALSE(InitializeBuildHost(&Error));
	EXPECT_FALSE(GetBuildHostSnapshot().bAcceptingRequests);
	EXPECT_EQ(Stops, 1u);
	EXPECT_EQ(Drains, 1u);
	bAllowSecond = true;
	EXPECT_TRUE(InitializeBuildHost(&Error)) << Error;
	ShutdownBuildHost();
}

TEST(FAssetBuildCoreTests, HostOwnerRetirementRejectsLaterCallbacksAndDestroysCaptures)
{
	ShutdownBuildHost();
	FModuleTestOwner Context("AssetBuildCoreTests.HostRetirement");
	auto OwnerRegistration = Context.CreateOwnedCallbackRegistration(
		"AssetBuildCore.BuildHost");
	auto Capture = std::make_shared<int>(7);
	const std::weak_ptr<int> WeakCapture = Capture;
	uint32 PumpCount = 0;
	std::string Error;
	auto Service = RegisterBuildServiceContribution({
		.Identity = "Durin.Tests.Retirement",
		.OwnerGate = OwnerRegistration.GetGate(),
		.Start = [] { return true; },
		.PumpCompletions = [Capture, &PumpCount](uint32) {
			++PumpCount;
			return static_cast<uint32>(*Capture);
		}}, &Error);
	Capture.reset();
	ASSERT_TRUE(Service.IsValid()) << Error;
	ASSERT_TRUE(InitializeBuildHost(&Error)) << Error;
	EXPECT_EQ(PumpBuildHostCompletions(), 7u);
	EXPECT_EQ(PumpCount, 1u);
	const auto Retiring = OwnerRegistration.Retire();
	EXPECT_EQ(0u, Retiring.InFlightInvocationCount);
	EXPECT_EQ(PumpBuildHostCompletions(), 0u);
	EXPECT_EQ(PumpCount, 1u);
	Service.Reset();
	EXPECT_TRUE(WeakCapture.expired());
	EXPECT_TRUE(OwnerRegistration.Reset().Succeeded());
	ShutdownBuildHost();
}

TEST(FAssetBuildCoreTests, CacheRequiredAndBestEffortWritePoliciesDiffer)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	Asset::FDerivedDataObjectStore Store("AssetBuildCoreTests/Policy", 1024);
	FBuildCacheClient Client(Store);
	const FBuildValue Invalid;
	std::string Error;
	EXPECT_TRUE(Client.Store(std::string(32, 'c'), Invalid,
		{.bRequireStoreSuccess = false}, &Error));
	EXPECT_FALSE(Client.Store(std::string(32, 'd'), Invalid,
		{.bRequireStoreSuccess = true}, &Error));
}

#include <gtest/gtest.h>

#include "AssetBuild/BuildCache.h"
#include "AssetBuild/BuildHost.h"
#include "AssetBuild/BuildRegistry.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::AssetBuild;

	auto MakeDefinition(std::string Name = "Echo") -> FBuildDefinition
	{
		return {
			.Function = {"Durin.Tests", std::move(Name)},
			.ImplementationIdentity = "Durin.Tests.Implementation.V1",
			.RecipeIdentity = "Durin.Tests.Recipe.V1",
			.TargetPlatform = "Win64",
			.TargetProfile = "Test",
			.Inputs = {FBuildValue::FromOwned("Input", {1, 2, 3})}};
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

TEST(FAssetBuildCoreTests, PortableDefinitionsOwnNamedContentIdentifiedValues)
{
	FBuildDefinition Definition = MakeDefinition();
	std::string Error;
	ASSERT_TRUE(ValidateBuildDefinition(Definition, &Error)) << Error;
	EXPECT_EQ(BuildFunctionIdentityString(Definition.Function), "Durin.Tests::Echo");
	ASSERT_EQ(Definition.Inputs.size(), 1u);
	EXPECT_EQ(Definition.Inputs[0].GetSize(), 3u);
	EXPECT_EQ(Definition.Inputs[0].GetContentIdentity(),
		FXxHash128::HashBuffer(std::array<uint8, 3>{1, 2, 3}));

	Definition.Inputs.push_back(FBuildValue::FromOwned("Input", {4}));
	EXPECT_FALSE(ValidateBuildDefinition(Definition, &Error));
	Definition = MakeDefinition();
	Definition.Function.Owner = "bad owner";
	EXPECT_FALSE(ValidateBuildDefinition(Definition, &Error));
}

TEST(FAssetBuildCoreTests, RegistryRejectsDuplicatesAndRunsIndependentFunctions)
{
	std::string Error;
	auto Echo = RegisterLocalBuildFunction(
		{"Durin.Tests", "Echo"},
		[](const FBuildDefinition& Definition, const FBuildPolicy&,
			const FBuildRequestOwner&) {
			return FBuildFunctionResult{
				.bSucceeded = true,
				.Values = {FBuildValue::FromOwned(
					"Output", std::vector<uint8>(Definition.Inputs[0].GetBytes().begin(),
						Definition.Inputs[0].GetBytes().end()))}};
		}, {}, &Error);
	ASSERT_TRUE(Echo.IsValid()) << Error;
	auto Sum = RegisterLocalBuildFunction(
		{"Durin.Tests", "Sum"},
		[](const FBuildDefinition&, const FBuildPolicy&, const FBuildRequestOwner&) {
			return FBuildFunctionResult{
				.bSucceeded = true,
				.Values = {FBuildValue::FromOwned("Output", {6})}};
		}, {}, &Error);
	ASSERT_TRUE(Sum.IsValid()) << Error;
	auto Duplicate = RegisterLocalBuildFunction(
		{"Durin.Tests", "Echo"}, [](const auto&, const auto&, const auto&) {
			return FBuildFunctionResult{};
		}, {}, &Error);
	EXPECT_FALSE(Duplicate.IsValid());
	EXPECT_EQ(GetRegisteredLocalBuildFunctionCount(), 2u);

	FBuildFunctionResult Result;
	bool bCallbackCalled = false;
	ASSERT_TRUE(ExecuteLocalBuildFunction(MakeDefinition(), {}, Result,
		[&bCallbackCalled](const FBuildFunctionResult& Completed) {
			bCallbackCalled = Completed.bSucceeded;
		}, &Error)) << Error;
	EXPECT_TRUE(Result.bSucceeded);
	EXPECT_TRUE(bCallbackCalled);
	EXPECT_TRUE(std::ranges::equal(Result.Values[0].GetBytes(),
		std::array<uint8, 3>{1, 2, 3}));

	FBuildDefinition Missing = MakeDefinition("Missing");
	EXPECT_FALSE(ExecuteLocalBuildFunction(Missing, {}, Result, {}, &Error));
	Sum.Reset();
	Echo.Reset();
	EXPECT_EQ(GetRegisteredLocalBuildFunctionCount(), 0u);
}

TEST(FAssetBuildCoreTests, RequestOwnersCancelAndBoundWaits)
{
	auto Owner = std::make_shared<FBuildRequestOwner>();
	std::atomic_bool Entered = false;
	std::atomic_bool Release = false;
	std::string Error;
	auto Registration = RegisterLocalBuildFunction(
		{"Durin.Tests", "Blocking"},
		[&Entered, &Release](const FBuildDefinition&, const FBuildPolicy&,
			const FBuildRequestOwner& RequestOwner) {
			Entered = true;
			while (!Release && !RequestOwner.IsCanceled()) std::this_thread::yield();
			return FBuildFunctionResult{
				.bSucceeded = !RequestOwner.IsCanceled(),
				.bCanceled = RequestOwner.IsCanceled()};
		}, Owner, &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	FBuildFunctionResult Result;
	std::thread Worker([&] {
		ExecuteLocalBuildFunction(MakeDefinition("Blocking"), {}, Result, {}, &Error);
	});
	while (!Entered) std::this_thread::yield();
	EXPECT_FALSE(Owner->Wait(0.001));
	Owner->Cancel();
	EXPECT_TRUE(Owner->Wait(5.0));
	Worker.join();
	EXPECT_TRUE(Result.bCanceled);
	const FBuildRequestOwnerSnapshot Snapshot = Owner->GetSnapshot();
	EXPECT_EQ(Snapshot.AcceptedRequestCount, 1u);
	EXPECT_EQ(Snapshot.CompletedRequestCount, 1u);
	EXPECT_EQ(Snapshot.ActiveRequestCount, 0u);
	Release = true;
}

TEST(FAssetBuildCoreTests, CacheClientHonorsExplicitQueryAndStorePolicies)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	Asset::FDerivedDataObjectStore Store("AssetBuildCoreTests/Objects", 1024);
	FBuildCacheClient Client(Store);
	const std::string Key(32, 'a');
	FBuildPolicy Policy;
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
		.Start = [&] { ++Starts; return true; },
		.StopAdmission = [&] { ++Stops; },
		.Wait = [](double) { return true; },
		.Drain = [&] { ++Drains; }}, &Error);
	auto Second = RegisterBuildServiceContribution({
		.Identity = "Durin.Tests.RollbackB",
		.DrainOrder = 2,
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

TEST(FAssetBuildCoreTests, FunctionCallbackMayReenterAndUnloadRegistration)
{
	std::string Error;
	auto Registration = RegisterLocalBuildFunction(
		{"Durin.Tests", "Reentrant"},
		[](const FBuildDefinition&, const FBuildPolicy&, const FBuildRequestOwner&) {
			return FBuildFunctionResult{.bSucceeded = true};
		}, {}, &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	FBuildFunctionResult Result;
	ASSERT_TRUE(ExecuteLocalBuildFunction(MakeDefinition("Reentrant"), {}, Result,
		[&](const FBuildFunctionResult&) {
			EXPECT_TRUE(IsLocalBuildFunctionRegistered({"Durin.Tests", "Reentrant"}));
			Registration.Reset();
		}, &Error)) << Error;
	EXPECT_FALSE(IsLocalBuildFunctionRegistered({"Durin.Tests", "Reentrant"}));
}

TEST(FAssetBuildCoreTests, UnloadingFunctionCancelsAndDrainsActiveRequest)
{
	auto Owner = std::make_shared<FBuildRequestOwner>();
	std::atomic_bool Entered = false;
	std::string WorkerError;
	auto Registration = RegisterLocalBuildFunction(
		{"Durin.Tests", "UnloadActive"},
		[&](const FBuildDefinition&, const FBuildPolicy&,
			const FBuildRequestOwner& RequestOwner) {
			Entered = true;
			while (!RequestOwner.IsCanceled()) std::this_thread::yield();
			return FBuildFunctionResult{.bCanceled = true};
		}, Owner, &WorkerError);
	ASSERT_TRUE(Registration.IsValid()) << WorkerError;
	FBuildFunctionResult Result;
	std::thread Worker([&] {
		ExecuteLocalBuildFunction(
			MakeDefinition("UnloadActive"), {}, Result, {}, &WorkerError);
	});
	while (!Entered) std::this_thread::yield();
	Registration.Reset();
	Worker.join();
	EXPECT_TRUE(Result.bCanceled);
	EXPECT_EQ(Owner->GetSnapshot().ActiveRequestCount, 0u);
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

TEST(FAssetBuildCoreTests, AssetFamilyFreeLocalFunctionUsesPortableValuesOnly)
{
	std::string Error;
	auto Registration = RegisterLocalBuildFunction(
		{"Durin.Sample", "ReverseBytes"},
		[](const FBuildDefinition& Definition, const FBuildPolicy& Policy,
			const FBuildRequestOwner&) {
			if (!Policy.bAllowLocalBuild) return FBuildFunctionResult{};
			std::vector<uint8> Bytes(
				Definition.Inputs[0].GetBytes().begin(),
				Definition.Inputs[0].GetBytes().end());
			std::ranges::reverse(Bytes);
			return FBuildFunctionResult{.bSucceeded = true,
				.Values = {FBuildValue::FromOwned("Reversed", std::move(Bytes))}};
		}, {}, &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	FBuildDefinition Definition = MakeDefinition();
	Definition.Function = {"Durin.Sample", "ReverseBytes"};
	FBuildFunctionResult Result;
	ASSERT_TRUE(ExecuteLocalBuildFunction(Definition, {}, Result, {}, &Error)) << Error;
	ASSERT_TRUE(Result.bSucceeded);
	EXPECT_TRUE(std::ranges::equal(Result.Values[0].GetBytes(),
		std::array<uint8, 3>{3, 2, 1}));
}

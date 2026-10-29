#include "CacheDispatchPrivate.h"
#pragma once

#include "CoreMinimal.h"
#include "EngineTestSupport.h"
#include "ShaderBuildTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialCompileTestSupport.h"
#include "Threading/ThreadEvent.h"

#include <gtest/gtest.h>

namespace Durin::Testing
{
inline auto CheckDetachedMaterialResultFanout(bool Shutdown) -> void
{
	using namespace Durin;
	struct FGateCache final : DerivedData::ICacheBackend
	{
		mutable FThreadEvent Entered, Release;
		auto Get(const DerivedData::FCacheGetBackendRequest& Request) const -> DerivedData::FCacheGetResult override { return DerivedData::GetCacheBackend().Get(Request); }
		auto Put(const DerivedData::FCachePutBackendRequest& Request) const -> DerivedData::FCachePutResult override
		{
			Entered.Trigger();
			Release.WaitFor(10);
			return DerivedData::GetCacheBackend().Put(Request);
		}
	};
	SCOPED_TRACE(Shutdown ? "shutdown during fanout" : "cancel one follower during fanout");
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto Cache = std::make_shared<FGateCache>(); ShaderBuildPrivate::ResetServiceForTests({.Cache = Cache});
	auto* A = NewObject<DMaterial>(nullptr, "FanoutCanceled"); auto* B = NewObject<DMaterial>(nullptr, "FanoutLive");
	A->SetEditCompileMode(EMaterialEditCompileMode::Manual); B->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FThreadEvent Entered, Release;
	struct FRestore
	{
		std::shared_ptr<FGateCache> Cache; FThreadEvent& Release; DMaterial* A; DMaterial* B;
		~FRestore() { Cache->Release.Trigger(); Release.Trigger(); MaterialBuildPrivate::SetResultFanoutHookForTests({}); FAssetCompilingManager::Get().FinishAllCompilation(); ShaderBuildPrivate::ResetServiceForTests(); MarkAsGarbage(A); MarkAsGarbage(B); CollectGarbage(); }
	} Restore{Cache, Release, A, B};
	MaterialBuildPrivate::SetResultFanoutHookForTests([&] { Entered.Trigger(); EXPECT_TRUE(Release.WaitFor(10)); });
	FMaterialCompilerEnvironment Environment; ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment));
	auto Snapshot = SnapshotMaterialCompilerInput(*A, Environment); ASSERT_TRUE(Snapshot);
	const auto Before = GetMaterialCompilationDiagnostics();
	ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*A, Snapshot.Snapshot->Input, true)); ASSERT_TRUE(Cache->Entered.WaitFor(5));
	ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*B, Snapshot.Snapshot->Input, true));
	Cache->Release.Trigger(); ASSERT_TRUE(Entered.WaitFor(5));
	EXPECT_TRUE(HasPendingMaterialCompilation(*A)); EXPECT_TRUE(HasPendingMaterialCompilation(*B));
	EXPECT_EQ(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, Before.OutstandingConsumerCount + 2);
	if (Shutdown)
	{
		std::thread Releaser([&] {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (IsMaterialCompilationAcceptingRequests() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			Release.Trigger();
		});
		ShutdownAssetCompilingManager(); Releaser.join();
		EXPECT_EQ(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, 0u);
	}
	else
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*A);
		EXPECT_FALSE(HasPendingMaterialCompilation(*A)); EXPECT_TRUE(HasPendingMaterialCompilation(*B));
		EXPECT_EQ(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, Before.OutstandingConsumerCount + 1);
		Release.Trigger(); FAssetCompilingManager::Get().FinishCompilationForObject(*B);
		ASSERT_TRUE(B->GetMaterialCompileStatus().IsCurrent());
		EXPECT_EQ(A->GetMaterialCompileStatus().State, EMaterialCompileState::Canceled);
		EXPECT_EQ(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, Before.OutstandingConsumerCount);
		MaterialBuildPrivate::SetResultFanoutHookForTests([] { throw std::runtime_error("fanout failure"); });
		ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*A, Snapshot.Snapshot->Input, true));
		FAssetCompilingManager::Get().FinishAllCompilation();
		EXPECT_EQ(A->GetMaterialCompileStatus().State, EMaterialCompileState::Failed);
		EXPECT_FALSE(HasPendingMaterialCompilation(*A));
		EXPECT_EQ(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, Before.OutstandingConsumerCount);
	}
}
}

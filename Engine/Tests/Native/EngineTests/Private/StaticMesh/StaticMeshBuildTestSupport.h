#pragma once

#include "EngineTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Physics/BodySetup.h"
#include "Threading/RunnableThread.h"
#include <gtest/gtest.h>
#include <condition_variable>
#include <thread>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <sys/resource.h>
#endif

namespace StaticMeshBuildTestSupport
{
	class FScopedDerivedDataCacheRestore
	{
	public:
		FScopedDerivedDataCacheRestore()
			: PreviousDirectory(Durin::FPaths::DerivedDataCacheDir())
		{
		}

		~FScopedDerivedDataCacheRestore()
		{
			Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDirectory);
		}

	private:
		std::string PreviousDirectory;
	};

	inline auto MakeResidencyGeometry() -> Durin::FStaticMeshDecodedGeometry
	{
		Durin::FStaticMeshDecodedGeometry Geometry;
		Geometry.MaterialSlots.push_back({"Material", 0, "Material"});
		auto& Mesh = Geometry.Meshes.emplace_back();
		Mesh.Name = "Fixture";
		Mesh.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
		Mesh.Indices = {0, 1, 2};
		return Geometry;
	}

	// Holds workers at a selected phase until the owner finishes its assertions.
	struct FStaticMeshWorkerBarrier
	{
		std::mutex Mutex;
		std::condition_variable Changed;
		uint32 Entered = 0;
		bool bReleased = false;
		explicit FStaticMeshWorkerBarrier(Durin::EStaticMeshCompilationPhase Selected = Durin::EStaticMeshCompilationPhase::Building, bool bFirstOnly = false)
		{
			Durin::AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([this, Selected, bFirstOnly](uint64, Durin::EStaticMeshCompilationPhase Phase) {
				if (Phase != Selected) return;
				EXPECT_FALSE(Durin::IsInGameThread());
				std::unique_lock Lock(Mutex);
				if (bFirstOnly && Entered != 0) return;
				++Entered;
				Changed.notify_all();
				Changed.wait(Lock, [&] { return bReleased; });
			});
		}
		auto Wait(uint32 Count, std::chrono::seconds Timeout = std::chrono::seconds(5)) -> bool
		{
			std::unique_lock Lock(Mutex);
			return Changed.wait_for(Lock, Timeout, [&] { return Entered >= Count; });
		}
		auto Release() -> void
		{
			std::lock_guard Lock(Mutex);
			bReleased = true;
			Changed.notify_all();
		}
		~FStaticMeshWorkerBarrier()
		{
			Release();
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			Durin::AssetPrivate::SetStaticMeshCompilationPhaseHookForTests({});
		}
	};

	// Shared correctness assertions; only qualification callers enable scale measurements.
	inline auto CheckSourceResidency(bool bMeasure) -> void
	{
		using namespace Durin;
		for (const uint32 Triangles : (bMeasure ? std::vector<uint32>{1u, 100000u} : std::vector<uint32>{1u}))
		{
#if defined(__APPLE__)
			std::atomic<size_t> PeakBytes{0};
			std::jthread Sampler([&](std::stop_token Stop) {
				while (bMeasure && !Stop.stop_requested())
				{
					malloc_statistics_t Stats{};
					malloc_zone_statistics(malloc_default_zone(), &Stats);
					PeakBytes.store(std::max(PeakBytes.load(), Stats.size_in_use));
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});
#endif
			FStaticMeshDecodedGeometry Input;
			Input.MaterialSlots.push_back({"Material", 0, "Material"});
			auto& Mesh = Input.Meshes.emplace_back();
			Mesh.Name = "Fixture";
			for (uint32 Triangle = 0; Triangle < Triangles; ++Triangle)
			{
				const float X = static_cast<float>(Triangle);
				Mesh.Positions.insert(Mesh.Positions.end(), {{X, 0, 0}, {X + 1, 0, 0}, {X, 1, 0}});
				Mesh.Indices.insert(Mesh.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
			}
			std::string Error;
			FStaticMeshSource Source;
			ASSERT_TRUE(Source.Initialize(std::move(Input), Error)) << Error;
			auto Decoded = Source.AcquireGeometry(Error);
			ASSERT_TRUE(Error.empty()) << Error;
			EXPECT_EQ(Decoded->Meshes.front().Positions.size(), Triangles * 3);
			EXPECT_EQ(Source.AcquireGeometry(Error), Decoded);
			EXPECT_EQ(Source.GetGeometryBulk().GetPayloadSize(), Triangles == 1 ? 195u : 4800147u);
			EXPECT_EQ(Source.GetIdentity().HashLow, Triangles == 1 ? 4982799754724307949ull : 17565407108445809865ull);
			EXPECT_EQ(Source.GetIdentity().HashHigh, Triangles == 1 ? 10298414200299834774ull : 892654471079648671ull);
			FStaticMeshBuildResult Product;
			ASSERT_TRUE(BuildStaticMeshDerivedData({.Source = Source, .bPersistDerivedData = false}, Product, Error)) << Error;
			const uint64 Retained = Mesh.Positions.capacity() * sizeof(FVector3f)
				+ Mesh.Indices.capacity() * sizeof(uint32);
			if (bMeasure) std::cout << "residency_fixture triangles=" << Triangles << " retained_array_capacity_bytes=" << Retained
				<< " payload_bytes=" << Source.GetGeometryBulk().GetPayloadSize()
				<< " identity=" << Source.GetIdentity().HashLow << ":" << Source.GetIdentity().HashHigh;
#if defined(__APPLE__)
			Sampler.request_stop();
			Sampler.join();
			if (bMeasure) std::cout << " process_sampled_peak_allocated_bytes=" << PeakBytes.load();
#endif
			if (bMeasure) std::cout << std::endl;
		}
	}

	inline auto CheckCandidateBudgets(bool bMeasure) -> void
	{
		using namespace Durin;
		FScopedDerivedDataCacheRestore RestoreCache;
		FPaths::SetDerivedDataCacheDirForTests(
			(Testing::GetTestWorkDirectory() / "AuthoredBudgetCache").generic_string());
		for (const uint32 TriangleCount : {1u, bMeasure ? 100000u : 32u})
		{
			FStaticMeshDecodedGeometry Geometry;
			Geometry.MaterialSlots.push_back({"Material", 0, "Material"});
			auto& Section = Geometry.Meshes.emplace_back();
			Section.Name = "BudgetFixture";
			Section.Positions.reserve(TriangleCount * 3);
			Section.Indices.reserve(TriangleCount * 3);
			for (uint32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
			{
				const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100),
					float(Triangle / 10000));
				Section.Positions.insert(Section.Positions.end(),
					{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
				Section.Indices.insert(Section.Indices.end(),
					{Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
			}
			std::string Error;
			FStaticMeshSource Source;
			ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error)) << Error;
			Source.ReleaseGeometry();
			const auto Start = std::chrono::steady_clock::now();
			FStaticMeshBuildResult Render;
			ASSERT_TRUE(BuildStaticMeshDerivedData(
				{.Source = Source, .bPersistDerivedData = false}, Render, Error)) << Error;
			ASSERT_EQ(Render.Origin, EStaticMeshBuildOrigin::Rebuilt);
			const auto RenderEnd = std::chrono::steady_clock::now();
			ASSERT_NE(Render.RenderData, nullptr);
			const auto& LOD = Render.RenderData->LODResources.front();
			const auto Acceleration = BuildStaticMeshRayQueryAcceleration(LOD);
			ASSERT_NE(Acceleration, nullptr);
			FStaticMeshCollisionBuildResult Collision;
			const auto CollisionStart = std::chrono::steady_clock::now();
			ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Render.RenderData,
				EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
				EBodySetupCollisionQueryPolicy::SimpleAndComplex,
				Collision, Error, false)) << Error;
			ASSERT_TRUE(Collision.Complex);
			const auto CollisionEnd = std::chrono::steady_clock::now();
			const auto Nanoseconds = [](auto Duration) {
				return std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count();
			};
			if (bMeasure) std::cout << "authored_budget_fixture triangles=" << TriangleCount
				<< " canonical_bytes=" << Source.GetGeometryBulk().GetPayloadSize()
				<< " render_payload_bytes=" << Render.PayloadBytes
				<< " ray_retained_bytes=" << Acceleration->RetainedBytes
				<< " collision_retained_bytes=" << Collision.Complex.GetRetainedBytes()
				<< " decode_render_ns=" << Nanoseconds(RenderEnd - Start)
				<< " ray_ns=" << Acceleration->BuildNanoseconds
				<< " collision_ns=" << Nanoseconds(CollisionEnd - CollisionStart) << std::endl;
			EXPECT_FALSE(Source.IsGeometryResident());
			EXPECT_LE(Acceleration->RetainedBytes,
				std::max<uint64>(1024, uint64(TriangleCount) * 96));
		}
	}

	inline auto CheckCandidatePublicationAndCancellation(bool bMeasure) -> void
	{
		using namespace Durin;
		FScopedDerivedDataCacheRestore RestoreCache;
		FPaths::SetDerivedDataCacheDirForTests(
			(Testing::GetTestWorkDirectory() / "CompleteCandidateTimingCache").generic_string());
		FStaticMeshDecodedGeometry Geometry = MakeResidencyGeometry();
		auto& Section = Geometry.Meshes.front();
		Section.Positions.clear();
		Section.Indices.clear();
		for (uint32 Triangle = 0; Triangle < (bMeasure ? 100000u : 32u); ++Triangle)
		{
			const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100), float(Triangle / 10000));
			Section.Positions.insert(Section.Positions.end(),
				{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
			Section.Indices.insert(Section.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
		}
		std::string Error;
		FStaticMeshSource Source;
		ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error));
		Source.ReleaseGeometry();
		auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("CompleteCandidateTiming"));
		auto* Body = NewObject<DBodySetup>(Mesh, FName("BodySetup"));
		Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		ASSERT_TRUE(Mesh->SetBodySetup(Body));
		const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
		auto Request = MakeStaticMeshAuthoredBuildRequest(Source, Snapshot);
		Request.bPersistDerivedData = false;
		std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate;
		FStaticMeshBuildExecutionMetrics Metrics;
		const auto Start = std::chrono::steady_clock::now();
		auto LastCheckpoint = Start;
		uint64 MaximumGapNanoseconds = 0;
		const auto Outcome = BuildStaticMeshAuthoredCandidate(std::move(Request), Candidate, Error,
			{.ShouldCancel = [&] {
				const auto Now = std::chrono::steady_clock::now();
				MaximumGapNanoseconds = std::max(MaximumGapNanoseconds, static_cast<uint64>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Now - LastCheckpoint).count()));
				LastCheckpoint = Now;
				return false;
			}, .Metrics = &Metrics});
		const auto Built = std::chrono::steady_clock::now();
		ASSERT_TRUE(Outcome) << Error;
		ASSERT_NE(Candidate, nullptr);
		const auto Ray = Candidate->GetRenderData()->LODResources.front().RayQueryAcceleration;
		ASSERT_TRUE(ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Candidate), Snapshot, Error)) << Error;
		const auto Published = std::chrono::steady_clock::now();
		EXPECT_EQ(Mesh->GetRenderData()->LODResources.front().RayQueryAcceleration, Ray);
		EXPECT_FALSE(Source.IsGeometryResident());
		EXPECT_GT(Metrics.CancellationCheckpoints, bMeasure ? 1000u : 4u);
		if (bMeasure) std::cout << "complete_candidate_fixture triangles=" << (bMeasure ? 100000u : 32u) << " build_ns="
			<< std::chrono::duration_cast<std::chrono::nanoseconds>(Built - Start).count()
			<< " publication_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(Published - Built).count()
			<< " checkpoints=" << Metrics.CancellationCheckpoints
			<< " maximum_checkpoint_gap_ns=" << MaximumGapNanoseconds << std::endl;
		uint64 MaximumCancellationNanoseconds = 0;
		for (const uint64 StopAfter : {Metrics.CancellationCheckpoints / 4,
			Metrics.CancellationCheckpoints / 2, Metrics.CancellationCheckpoints * 3 / 4})
		{
			auto CancelRequest = MakeStaticMeshAuthoredBuildRequest(Source, Snapshot);
			CancelRequest.bPersistDerivedData = false;
			uint64 Checks = 0;
			bool bRequested = false;
			std::chrono::steady_clock::time_point RequestedAt;
			std::unique_ptr<FStaticMeshAuthoredCandidate> CancelledCandidate;
			const auto Cancelled = BuildStaticMeshAuthoredCandidate(std::move(CancelRequest),
				CancelledCandidate, Error, {.ShouldCancel = [&] {
					if (bRequested) return true;
					if (++Checks == StopAfter)
					{
						bRequested = true;
						RequestedAt = std::chrono::steady_clock::now();
					}
					return false;
				}});
			ASSERT_TRUE(bRequested);
			EXPECT_EQ(Cancelled.Status, EStaticMeshBuildStatus::Cancelled);
			EXPECT_FALSE(CancelledCandidate);
			MaximumCancellationNanoseconds = std::max(MaximumCancellationNanoseconds,
				static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - RequestedAt).count()));
		}
		if (bMeasure) std::cout << "complete_candidate_cancellation_fixture samples=3 maximum_cancel_to_return_ns="
			<< MaximumCancellationNanoseconds << std::endl;

	}

	inline auto CheckConcurrentCandidates(bool bMeasure) -> void
	{
		using namespace Durin;
		FAssetCompilingManager::Get().FinishAllCompilation();
		FScopedDerivedDataCacheRestore RestoreCache;
		FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "ConcurrentAuthoredQualification").generic_string());
		auto Geometry = MakeResidencyGeometry();
		auto& Section = Geometry.Meshes.front();
		Section.Positions.clear();
		Section.Indices.clear();
		for (uint32 Triangle = 0; Triangle < (bMeasure ? 100000u : 32u); ++Triangle)
		{
			const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100), float(Triangle / 10000));
			Section.Positions.insert(Section.Positions.end(),
				{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
			Section.Indices.insert(Section.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
		}
		FStaticMeshSource Source;
		std::string Error;
		ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error));
		Source.ReleaseGeometry();
#if defined(__APPLE__)
		std::atomic<size_t> SampledAllocationPeak = 0;
		std::jthread AllocationSampler([&](std::stop_token Stop) {
			while (bMeasure && !Stop.stop_requested())
			{
				malloc_statistics_t Stats{};
				malloc_zone_statistics(nullptr, &Stats);
				SampledAllocationPeak.store(std::max(SampledAllocationPeak.load(), Stats.size_in_use));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
#endif
		std::array<DStaticMesh*, 2> Meshes;
		std::array<std::optional<FStaticMeshCompilationDiagnostic>, 2> Results;
		FStaticMeshWorkerBarrier Barrier(EStaticMeshCompilationPhase::Mailbox);
		for (size_t Index = 0; Index < Meshes.size(); ++Index)
		{
			Meshes[Index] = NewObject<DStaticMesh>(nullptr, FName(std::format("ConcurrentCandidate{}", Index)));
			auto* Body = NewObject<DBodySetup>(Meshes[Index], FName("BodySetup"));
			Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
			ASSERT_TRUE(Meshes[Index]->SetBodySetup(Body));
			ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[Index], {.Source = Source, .bPersistDerivedData = false}, Error,
				[&, Index](const auto& Result) { EXPECT_TRUE(IsInGameThread()); Results[Index] = Result; })) << Error;
		}
		ASSERT_TRUE(Barrier.Wait(2, std::chrono::seconds(bMeasure ? 30 : 5)));
		const auto Held = GetStaticMeshCompilationManagerDiagnostics();
		EXPECT_EQ(2u, Held.RunningWorkers);
		EXPECT_EQ(2u, Held.OutstandingRecords);
		EXPECT_LE(Held.ReservedBytes, 1024ull * 1024 * 1024);
#if defined(__APPLE__)
		malloc_statistics_t AllocationStats{};
		malloc_zone_statistics(nullptr, &AllocationStats);
		rusage Usage{};
		ASSERT_EQ(0, getrusage(RUSAGE_SELF, &Usage));
		if (bMeasure) std::cout << "concurrent_candidate_memory reserved_bytes=" << Held.ReservedBytes
			<< " default_zone_in_use=" << AllocationStats.size_in_use
			<< " process_peak_rss_bytes=" << Usage.ru_maxrss << std::endl;
#endif
		CancelStaticMeshCompilation(*Meshes[0]);
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		ASSERT_TRUE(Results[0].has_value());
		EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, Results[0]->Status);
		EXPECT_FALSE(Results[1].has_value());
		EXPECT_EQ(Held.ReservedBytes, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
		EXPECT_EQ(nullptr, Meshes[0]->GetRenderData());
		EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
		Barrier.Release();
		FAssetCompilingManager::Get().FinishAllCompilation();
		ASSERT_TRUE(Results[1].has_value());
#if defined(__APPLE__)
		AllocationSampler.request_stop();
		AllocationSampler.join();
		if (bMeasure) std::cout << "concurrent_candidate_allocation sampled_default_zone_peak_bytes="
			<< SampledAllocationPeak.load() << " sample_interval_ms=1" << std::endl;
#endif
		const auto& Completed = *Results[1];
		EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, Completed.Status);
		ASSERT_TRUE(Completed.Render.has_value());
		ASSERT_TRUE(Completed.Collision.has_value());
		EXPECT_EQ(EStaticMeshBuildOrigin::Rebuilt, Completed.Render->Origin);
		EXPECT_TRUE(Completed.Render->DerivedDataKey.IsValid());
		EXPECT_GT(Completed.Render->PayloadBytes, 0u);
		EXPECT_GT(Completed.CaptureNanoseconds, 0u);
		EXPECT_GT(Completed.WorkerNanoseconds, 0u);
		EXPECT_GT(Completed.PublicationNanoseconds, 0u);
		FCollisionGeometryRef Collision;
		EXPECT_TRUE(Meshes[1]->GetBodySetup()->BuildComplexGeometry(Collision));
		EXPECT_FALSE(Meshes[1]->GetSource().IsGeometryResident());
		EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
		if (bMeasure) std::cout << "concurrent_candidate_cost capture_ns=" << Completed.CaptureNanoseconds
			<< " worker_ns=" << Completed.WorkerNanoseconds
			<< " publication_ns=" << Completed.PublicationNanoseconds << std::endl;
	}
}

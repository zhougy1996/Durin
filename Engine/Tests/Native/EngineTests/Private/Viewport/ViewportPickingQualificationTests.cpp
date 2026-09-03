#include "ViewportTestSupport.h"
#include "Actors/TerrainActor.h"
#include "Components/TerrainComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Viewport/ViewportPickingSceneIndex.h"
#include "Viewport/ViewportPickingService.h"
#include "Terrain/TerrainHeightmap.h"

#include <gtest/gtest.h>

namespace
{
	struct FFakePickingState
	{
		std::unordered_map<uint64, Durin::Editor::Level::FViewportPickingBackendRequest> Requests;
		std::unordered_map<uint64, Durin::Editor::Level::FViewportPickingBackendCompletion> Completions;
		std::unordered_set<uint64> Cancelled;
	};

	class FControlledPickingBackend final : public Durin::Editor::Level::IViewportPickingBackend
	{
	public:
		explicit FControlledPickingBackend(std::shared_ptr<FFakePickingState> InState)
			: State(std::move(InState))
		{
		}

		auto Submit(Durin::Editor::Level::FViewportPickingBackendRequest Request)
			-> Durin::Editor::Level::FViewportPickingBackendCompletion override
		{
			State->Requests.emplace(Request.Ticket.Id, std::move(Request));
			return {Durin::Editor::Level::EViewportPickStatus::Pending, std::nullopt};
		}

		auto Poll(Durin::Editor::Level::FViewportPickTicket Ticket)
			-> Durin::Editor::Level::FViewportPickingBackendCompletion override
		{
			const auto It = State->Completions.find(Ticket.Id);
			return It == State->Completions.end()
				? Durin::Editor::Level::FViewportPickingBackendCompletion{
					Durin::Editor::Level::EViewportPickStatus::Pending, std::nullopt}
				: It->second;
		}

		auto Cancel(Durin::Editor::Level::FViewportPickTicket Ticket) -> void override
		{
			State->Cancelled.insert(Ticket.Id);
		}

	private:
		std::shared_ptr<FFakePickingState> State;
	};

	struct FPickingFixture
	{
		Durin::DWorld* World = nullptr;
		Durin::DLevel* Level = nullptr;
		Durin::AStaticMeshActor* Actor = nullptr;
		Durin::Editor::Level::FLevelEditorViewportClient Client;
		Durin::FSceneView View;

		FPickingFixture()
		{
			InitializeDObjectSystem();
			World = Durin::NewObject<Durin::DWorld>(nullptr, "PickingQualificationWorld");
			Level = Durin::NewObject<Durin::DLevel>(World, "PickingQualificationLevel");
			if (!World->SetCurrentLevel(Level)) throw std::runtime_error("fixture setup failed");
			Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle(Level);
			Actor = Level->SpawnActor<Durin::AStaticMeshActor>("Target");
			if (!Actor) throw std::runtime_error("fixture setup failed");
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->GetStaticMeshComponent()->SetWorldLocation(
				Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 3.0);
			Client.InitializeForLevel(Level);
			if (!Client.BuildViewMatrices(800, 600, View)) throw std::runtime_error("fixture setup failed");
		}

		~FPickingFixture()
		{
			Durin::MarkObjectHierarchyAsGarbage(World);
			Durin::CollectGarbage();
		}
	};

	auto MakeStaticRequest(Durin::AStaticMeshActor* Actor)
		-> Durin::Editor::Level::FViewportPickingBackendRequest
	{
		auto* Component = Actor->GetStaticMeshComponent();
		return {{1}, {0.1, 0.1, 0.0}, {0.0, 0.0, 1.0},
			{{1, Component->GetPrimitiveSceneId(), Actor, Component,
				Component->GetPrimitiveSceneId().Value, Component->GetRegistrationGeneration()}}};
	}

	auto CreateGridStaticMesh(Durin::DLevel* Level, uint32 TriangleCount) -> Durin::DStaticMesh*
	{
		const uint32 CellCount = (TriangleCount + 1) / 2;
		const uint32 Width = std::max<uint32>(1,
			static_cast<uint32>(std::ceil(std::sqrt(static_cast<double>(CellCount)))));
		const uint32 Height = (CellCount + Width - 1) / Width;
		Durin::FStaticMeshImportedData Imported;
		Imported.MaterialSlots.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
		auto& Mesh = Imported.Meshes.emplace_back();
		Mesh.Name = "PickingGrid";
		Mesh.SourceMaterialIndex = 0;
		Mesh.Positions.reserve(static_cast<size_t>(Width + 1) * (Height + 1));
		for (uint32 Y = 0; Y <= Height; ++Y)
			for (uint32 X = 0; X <= Width; ++X)
				Mesh.Positions.emplace_back(static_cast<float>(X), static_cast<float>(Y), 0.0f);
		Mesh.Indices.reserve(static_cast<size_t>(TriangleCount) * 3);
		for (uint32 Cell = 0; Cell < CellCount && Mesh.Indices.size() / 3 < TriangleCount; ++Cell)
		{
			const uint32 X = Cell % Width;
			const uint32 Y = Cell / Width;
			const uint32 A = Y * (Width + 1) + X;
			const uint32 B = A + 1;
			const uint32 C = A + Width + 1;
			const uint32 D = C + 1;
			Mesh.Indices.insert(Mesh.Indices.end(), {A, B, D});
			if (Mesh.Indices.size() / 3 < TriangleCount)
				Mesh.Indices.insert(Mesh.Indices.end(), {A, D, C});
		}
		auto* Result = Durin::NewObject<Durin::DStaticMesh>(Level,
			std::format("PickingGrid{}", TriangleCount));
		std::string Error;
		if (!Durin::BuildStaticMeshSynchronously(
			*Result, Imported, Error)) throw std::runtime_error(Error);
		return Result;
	}
}

TEST(FViewportPickingQualificationTests, RepresentativeStaticTriangleCountsBuildAndCompareDeterministically)
{
	FPickingFixture Fixture;
	Fixture.Actor->GetStaticMeshComponent()->SetWorldLocation({0.0, 0.0, 3.0});
	for (uint32 TriangleCount : {10'000u, 250'000u})
	{
		Durin::DStaticMesh* Mesh = CreateGridStaticMesh(Fixture.Level, TriangleCount);
		Fixture.Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		const auto* Data = Mesh->GetRenderData();
		ASSERT_NE(Data, nullptr);
		ASSERT_TRUE(Data->LODResources[0].RayQueryAcceleration);
		const auto Completion = Durin::Editor::Level::MakeViewportPickingBackend(
			Durin::Editor::Level::EViewportPickingBackendPolicy::Compare)->Submit(MakeStaticRequest(Fixture.Actor));
		ASSERT_TRUE(Completion.Hit);
		EXPECT_EQ(Completion.Diagnostics.ParityMismatches, 0u);
		EXPECT_LE(Completion.Diagnostics.StaticCandidateTriangles,
			std::max<uint32>(1, TriangleCount / 100));
		RecordProperty(std::format("triangles_{}_bytes", TriangleCount),
			std::to_string(Data->LODResources[0].RayQueryAcceleration->RetainedBytes));
		RecordProperty(std::format("triangles_{}_build_ns", TriangleCount),
			std::to_string(Data->LODResources[0].RayQueryAcceleration->BuildNanoseconds));
	}
}

TEST(FViewportPickingQualificationTests, MillionTriangleFixtureMeetsDeterministicCandidateAndMemoryGates)
{
	FPickingFixture Fixture;
	constexpr uint32 TriangleCount = 1'000'000;
	Durin::DStaticMesh* Mesh = CreateGridStaticMesh(Fixture.Level, TriangleCount);
	ASSERT_NE(Mesh, nullptr);
	Fixture.Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Fixture.Actor->GetStaticMeshComponent()->SetWorldLocation({0.0, 0.0, 3.0});
	const auto* Data = Mesh->GetRenderData();
	ASSERT_NE(Data, nullptr);
	ASSERT_TRUE(Data->LODResources[0].RayQueryAcceleration);
	EXPECT_LE(Data->LODResources[0].RayQueryAcceleration->RetainedBytes,
		Durin::MaximumStaticMeshRayQueryAccelerationBytes);
	RecordProperty("static_acceleration_bytes",
		std::to_string(Data->LODResources[0].RayQueryAcceleration->RetainedBytes));
	RecordProperty("static_acceleration_build_ns",
		std::to_string(Data->LODResources[0].RayQueryAcceleration->BuildNanoseconds));

	const Durin::Editor::Level::FViewportPickingBackendRequest Request = MakeStaticRequest(Fixture.Actor);
	auto AcceleratedBackend = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated);
	const auto Warm = AcceleratedBackend->Submit(Request);
	ASSERT_TRUE(Warm.Hit);
	EXPECT_LE(Warm.Diagnostics.StaticCandidateTriangles, TriangleCount / 100);
	EXPECT_LE(Warm.Diagnostics.StaticTestedTriangles, TriangleCount / 100);
	RecordProperty("accelerated_candidate_triangles", std::to_string(Warm.Diagnostics.StaticCandidateTriangles));
	const auto Measure = [&Request](Durin::Editor::Level::EViewportPickingBackendPolicy Policy)
	{
		auto Backend = Durin::Editor::Level::MakeViewportPickingBackend(Policy);
		std::array<uint64, 3> Samples{};
		for (uint64& Sample : Samples)
		{
			const auto Start = std::chrono::steady_clock::now();
			const auto Completion = Backend->Submit(Request);
			const auto End = std::chrono::steady_clock::now();
			if (!Completion.Hit) throw std::runtime_error("timed picking query missed");
			Sample = static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(End - Start).count());
		}
		std::ranges::sort(Samples);
		return Samples[1];
	};
	const uint64 ReferenceNanoseconds = Measure(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Reference);
	const uint64 AcceleratedNanoseconds = Measure(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated);
	RecordProperty("reference_median_ns", std::to_string(ReferenceNanoseconds));
	RecordProperty("accelerated_median_ns", std::to_string(AcceleratedNanoseconds));
	EXPECT_LE(AcceleratedNanoseconds, ReferenceNanoseconds / 4);
	const auto Compared = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Compare)->Submit(Request);
	ASSERT_TRUE(Compared.Hit);
	EXPECT_EQ(Compared.Diagnostics.ParityMismatches, 0u);
}

TEST(FViewportPickingQualificationTests, TenThousandPrimitiveSparseFixtureMeetsSceneCandidateAndMemoryGates)
{
	FPickingFixture Fixture;
	Durin::DStaticMesh* Mesh = Fixture.Actor->GetStaticMeshComponent()->GetStaticMesh();
	auto Index = std::make_shared<Durin::Editor::Level::FViewportPickingSceneIndex>();
	Index->SetLevel(Fixture.Level);
	Fixture.Client.SetPickingSceneIndex(Index);
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	for (uint32 ActorIndex = 1; ActorIndex < 10'000; ++ActorIndex)
	{
		auto* Actor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>(
			Durin::FName(std::format("LargeSparse{}", ActorIndex)));
		ASSERT_NE(Actor, nullptr);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		const uint32 X = ActorIndex % 100;
		const uint32 Y = ActorIndex / 100;
		Actor->GetStaticMeshComponent()->SetWorldLocation({100.0 + X * 2.0, 100.0 + Y * 2.0, 3.0});
		const uint32 PrimitiveCount = ActorIndex + 1;
		if (PrimitiveCount == 100 || PrimitiveCount == 2'000)
		{
			const auto Checkpoint = Fixture.Client.SubmitViewportPick(
				Fixture.Level, Fixture.View, {400.0f, 300.0f});
			RecordProperty(std::format("scene_{}_candidates", PrimitiveCount),
				std::to_string(State->Requests.at(Checkpoint.Ticket.Id).Targets.size()));
			RecordProperty(std::format("scene_{}_retained_bytes", PrimitiveCount),
				std::to_string(Index->GetDiagnostics().RetainedBytes));
		}
	}
	Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	const uint64 NodeVisitsBefore = Index->GetDiagnostics().NodeVisits;
	std::array<uint64, 5> AcceleratedSamples{};
	Durin::Editor::Level::FViewportPickSubmission Pick;
	for (uint64& Sample : AcceleratedSamples)
	{
		const auto Start = std::chrono::steady_clock::now();
		Pick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
		Sample = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - Start).count());
	}
	const size_t CandidateCount = State->Requests.at(Pick.Ticket.Id).Targets.size();
	const uint64 WarmNodeVisits = Index->GetDiagnostics().NodeVisits - NodeVisitsBefore;
	std::vector<Durin::Editor::Level::FViewportPickingSceneCandidate> DenseCandidates;
	ASSERT_TRUE(Index->QueryRay({99.0, 100.1, 3.0}, {1.0, 0.0, 0.0}, DenseCandidates));
	EXPECT_GE(DenseCandidates.size(), 90u);
	Durin::Editor::Level::FLevelEditorViewportClient ReferenceClient;
	ReferenceClient.InitializeForLevel(Fixture.Level);
	auto ReferenceState = std::make_shared<FFakePickingState>();
	ReferenceClient.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(ReferenceState));
	ReferenceClient.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	std::array<uint64, 5> ReferenceSamples{};
	Durin::Editor::Level::FViewportPickSubmission ReferencePick;
	for (uint64& Sample : ReferenceSamples)
	{
		const auto Start = std::chrono::steady_clock::now();
		ReferencePick = ReferenceClient.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
		Sample = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - Start).count());
	}
	ASSERT_EQ(ReferenceState->Requests.at(ReferencePick.Ticket.Id).Targets.size(), 10'000u);
	std::ranges::sort(AcceleratedSamples);
	std::ranges::sort(ReferenceSamples);
	const uint64 AcceleratedNanoseconds = AcceleratedSamples[2];
	const uint64 ReferenceNanoseconds = ReferenceSamples[2];
	EXPECT_LE(CandidateCount, 500u);
	EXPECT_LE(Index->GetDiagnostics().RetainedBytes, 64ull * 1024ull * 1024ull);
	RecordProperty("scene_candidates", std::to_string(CandidateCount));
	RecordProperty("scene_dense_candidates", std::to_string(DenseCandidates.size()));
	RecordProperty("scene_retained_bytes", std::to_string(Index->GetDiagnostics().RetainedBytes));
	RecordProperty("scene_build_ns", std::to_string(Index->GetDiagnostics().BuildNanoseconds));
	RecordProperty("scene_accelerated_median_ns", std::to_string(AcceleratedNanoseconds));
	RecordProperty("scene_reference_median_ns", std::to_string(ReferenceNanoseconds));
	RecordProperty("scene_warm_node_visits", std::to_string(WarmNodeVisits));
}

TEST(FViewportPickingQualificationTests, MaximumTerrainMeetsCellParityAndRelativeTimeGates)
{
	FPickingFixture Fixture;
	auto* Actor = Fixture.Level->SpawnActor<Durin::ATerrainActor>("QualifiedTerrain");
	ASSERT_NE(Actor, nullptr);
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(Fixture.Level, "QualifiedHeightmap");
	std::vector<uint16> Samples(1025u * 1025u, 0);
	Samples[512u * 1025u + 512u] = 65'535;
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(1025, 1025, Samples, Error)) << Error;
	auto* Component = Actor->GetTerrainComponent();
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->SetSampleSpacing(1.0, 1.0));
	ASSERT_TRUE(Component->SetHeightRange(1.0, -0.5));
	Durin::Editor::Level::FViewportPickingBackendRequest Request{
		{1}, {10.5, 10.5, 4.0}, {0.0, 0.0, -1.0},
		{{1, Component->GetPrimitiveSceneId(), Actor, Component, 1,
			Component->GetRegistrationGeneration()}}};
	auto Measure = [&Request](Durin::Editor::Level::EViewportPickingBackendPolicy Policy,
		Durin::Editor::Level::FViewportPickingBackendCompletion& OutCompletion)
	{
		auto Backend = Durin::Editor::Level::MakeViewportPickingBackend(Policy);
		std::array<uint64, 3> Times{};
		for (uint64& Time : Times)
		{
			const auto Start = std::chrono::steady_clock::now();
			OutCompletion = Backend->Submit(Request);
			Time = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
			if (!OutCompletion.Hit) throw std::runtime_error("timed Terrain query missed");
		}
		std::ranges::sort(Times);
		return Times[1];
	};
	Durin::Editor::Level::FViewportPickingBackendCompletion Reference;
	Durin::Editor::Level::FViewportPickingBackendCompletion Accelerated;
	const uint64 ReferenceNs = Measure(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Reference, Reference);
	const uint64 AcceleratedNs = Measure(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated, Accelerated);
	EXPECT_LE(Accelerated.Diagnostics.TerrainVisitedCells, (1024u * 1024u) / 100u);
	EXPECT_LE(AcceleratedNs, ReferenceNs / 4);
	const auto Compared = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Compare)->Submit(Request);
	ASSERT_TRUE(Compared.Hit);
	EXPECT_EQ(Compared.Diagnostics.TerrainParityMismatches, 0u);
	RecordProperty("terrain_reference_median_ns", std::to_string(ReferenceNs));
	RecordProperty("terrain_accelerated_median_ns", std::to_string(AcceleratedNs));
	RecordProperty("terrain_visited_cells", std::to_string(Accelerated.Diagnostics.TerrainVisitedCells));
	RecordProperty("terrain_tested_triangles", std::to_string(Accelerated.Diagnostics.TerrainTestedTriangles));
}

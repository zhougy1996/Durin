#include "StaticMesh/StaticMeshBuildTestSupport.h"
#include "StaticMesh/StaticMeshTestEnvironment.h"
#include "DerivedDataBuildTestSupport.h"
#include "NativeQualificationSupport.h"
#include "StaticMesh/IMeshBuilderModule.h"

using namespace StaticMeshBuildTestSupport;

namespace
{
	auto MakeTransferGeometry(uint32 TriangleCount) -> Durin::FStaticMeshDecodedGeometry
	{
		using namespace Durin;
		FStaticMeshDecodedGeometry Geometry = MakeResidencyGeometry();
		auto& Mesh = Geometry.Meshes.front();
		Mesh.Positions.clear(); Mesh.Indices.clear();
		Mesh.Positions.reserve(TriangleCount * 3); Mesh.Indices.reserve(TriangleCount * 3);
		for (uint32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
		{
			const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100), float(Triangle / 10000));
			Mesh.Positions.insert(Mesh.Positions.end(), {Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
			Mesh.Indices.insert(Mesh.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
		}
		return Geometry;
	}

	struct FTransferAccounting
	{
		uint64 PayloadBytes = 0;
		uint64 RoundTripCopiedBytes = 0;
		uint64 MaximumConversionZoneIncrease = 0;
	};

	// Representation experiment only: retains actual typed recipe allocations
	// behind an aliasing byte owner, then measures the alternative explicit
	// byte-vector/native-vector conversion. No production output API is changed.
	template<typename T>
	auto MeasureArrayTransfer(std::vector<T> Values, FTransferAccounting& Accounting) -> void
	{
		static_assert(std::is_trivially_copyable_v<T>);
		if (Values.empty()) return;
		const auto* Original = Values.data();
		const auto Count = Values.size();
		const auto Bytes = Count * sizeof(T);
		auto Owner = std::make_shared<const std::vector<T>>(std::move(Values));
		std::weak_ptr<const std::vector<T>> Lifetime = Owner;
		std::shared_ptr<const std::byte> Block(Owner, reinterpret_cast<const std::byte*>(Owner->data()));
		EXPECT_EQ(Owner->data(), Original);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(Block.get()) % alignof(T), 0u);
		Owner.reset();
		EXPECT_FALSE(Lifetime.expired());
		const Durin::FByteView View(Block.get(), Bytes);
		const auto Hash = Durin::FXxHash128::HashBuffer(View);
		Durin::Testing::FQualificationAllocationSampler Allocations;
		auto ByteCopy = Durin::FSharedByteBuffer::Copy(View);
		std::vector<T> NativeCopy(Count);
		std::memcpy(NativeCopy.data(), ByteCopy.data(), Bytes);
		const auto Allocation = Allocations.Finish();
		EXPECT_NE(ByteCopy.data(), Block.get());
		EXPECT_NE(NativeCopy.data(), Original);
		EXPECT_EQ(Durin::FXxHash128::HashBuffer(std::as_bytes(std::span(NativeCopy))), Hash);
		// The original native objects remain alive; this is not a cast of an
		// archive byte allocation into objects whose lifetimes never began.
		EXPECT_EQ(Durin::FXxHash128::HashBuffer(std::as_bytes(std::span(Original, Count))), Hash);
		auto Consumer = Block;
		Block.reset();
		EXPECT_FALSE(Lifetime.expired());
		EXPECT_EQ(Durin::FXxHash128::HashBuffer(Durin::FByteView(Consumer.get(), Bytes)), Hash);
		Consumer.reset();
		EXPECT_TRUE(Lifetime.expired());
		Accounting.PayloadBytes += Bytes;
		Accounting.RoundTripCopiedBytes += Bytes * 2;
		Accounting.MaximumConversionZoneIncrease = std::max(Accounting.MaximumConversionZoneIncrease,
			Allocation.GetPeakIncrease());
	}
}

TEST(FStaticMeshBuildQualificationTests, RepresentativeGeometry)
{
	CheckSourceResidency(true);
}

TEST(FStaticMeshBuildQualificationTests, RepresentativeDetachedBuildBudgets)
{
	CheckDetachedBuildBudgets(true);
}

TEST(FStaticMeshBuildQualificationTests, MeasuresRenderBuildAndPublicationSeparately)
{
	CheckRenderPublicationAndCancellation(true);
}

TEST(FStaticMeshBuildQualificationTests, ConcurrentLargeRenderBuildsSeparateCostsAndRetainCancelledBytes)
{
	CheckConcurrentRenderBuilds(true);
}

TEST(FStaticMeshBuildQualificationTests, ColdAndWarmRenderAndCollision)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	const auto Root = Testing::CreateTestFixtureDirectory("BuildSessionMeshBaseline");
	Testing::FQualificationLogSession Log((Root / "Logs").generic_string());
	ASSERT_TRUE(Log.IsStarted());
	constexpr uint32 TriangleCount = 100000;
	auto Geometry = MakeTransferGeometry(TriangleCount);
	auto& Mesh = Geometry.Meshes.front();
	// Capture collision independently of render output. Hashing of these arrays
	// still occurs inside Cook on both paths and is included in its measurement.
	FCookBodySetupInfo CollisionInput{.Mode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0};
	CollisionInput.TriangleMeshDesc.Positions = Mesh.Positions;
	CollisionInput.TriangleMeshDesc.Indices = Mesh.Indices;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	const auto SourceBytes = Source.GetGeometryBulk().GetPayloadSize();
	Source.ReleaseGeometry();
	const auto Probe = Testing::AttachBuildSourceReadProbe(Source, Source.GetGeometryBulk(), "Geometry");
	ASSERT_TRUE(Probe);
	std::array<std::vector<double>, 4> Samples;
	std::array<uint64, 4> PeakIncrease{}, PeakAllocated{};
	bool bAllocationSampleAvailable = false;
	uint64 CollisionBytes = 0;
	for (uint32 Round = 0; Round < 4; ++Round)
	{
		FPaths::SetDerivedDataCacheDirForTests((Root / std::to_string(Round)).generic_string());
		for (uint32 Warm = 0; Warm < 2; ++Warm)
		{
			const auto Before = Probe->GetReadStats();
			Testing::FQualificationAllocationSampler RenderAllocations;
			const auto RenderStart = std::chrono::steady_clock::now();
			auto Render = BuildRenderForTest({.Source = Source});
			const auto RenderEnd = std::chrono::steady_clock::now();
			const auto RenderAllocation = RenderAllocations.Finish();
			ASSERT_TRUE(Render) << Render.error().ToString();
			ASSERT_EQ((*Render)->LODResources.size(), 1u);
			EXPECT_EQ((*Render)->LODResources[0].GetNumIndices(), TriangleCount * 3);
			const auto After = Probe->GetReadStats();
			EXPECT_EQ(After.RequestCount - Before.RequestCount, Warm ? 0u : 1u);
			Render->reset();
			Testing::FQualificationAllocationSampler CollisionAllocations;
			const auto CollisionStart = std::chrono::steady_clock::now();
			auto Collision = FPhysicsCookHelper::Cook(CollisionInput);
			const auto CollisionEnd = std::chrono::steady_clock::now();
			const auto CollisionAllocation = CollisionAllocations.Finish();
			ASSERT_TRUE(Collision) << Collision.error().ToString();
			ASSERT_TRUE(Collision->Complex);
			EXPECT_EQ(Collision->Complex.GetTriangleCount(), TriangleCount);
			CollisionBytes = Collision->Complex.GetRetainedBytes();
			if (Round)
			{
				Samples[Warm].push_back(std::chrono::duration<double, std::milli>(RenderEnd - RenderStart).count());
				Samples[2 + Warm].push_back(std::chrono::duration<double, std::milli>(CollisionEnd - CollisionStart).count());
				PeakIncrease[Warm] = std::max(PeakIncrease[Warm], RenderAllocation.GetPeakIncrease());
				PeakAllocated[Warm] = std::max(PeakAllocated[Warm], RenderAllocation.PeakBytes);
				PeakIncrease[2 + Warm] = std::max(PeakIncrease[2 + Warm], CollisionAllocation.GetPeakIncrease());
				PeakAllocated[2 + Warm] = std::max(PeakAllocated[2 + Warm], CollisionAllocation.PeakBytes);
				bAllocationSampleAvailable = RenderAllocation.bAvailable && CollisionAllocation.bAvailable;
			}
		}
	}
	for (auto& Values : Samples) std::ranges::sort(Values);
	std::cout << "derived_data_mesh triangles=" << TriangleCount
		<< " render_cold_median_ms=" << Samples[0][1] << " render_warm_median_ms=" << Samples[1][1]
		<< " collision_cold_median_ms=" << Samples[2][1] << " collision_warm_median_ms=" << Samples[3][1]
		<< " source_bytes=" << SourceBytes << " collision_retained_bytes=" << CollisionBytes
		<< " allocation_sample_available=" << bAllocationSampleAvailable
		<< " render_cold_sampled_zone_peak_bytes=" << PeakAllocated[0] << " render_warm_sampled_zone_peak_bytes=" << PeakAllocated[1]
		<< " collision_cold_sampled_zone_peak_bytes=" << PeakAllocated[2] << " collision_warm_sampled_zone_peak_bytes=" << PeakAllocated[3]
		<< " render_cold_sampled_zone_increase_bytes=" << PeakIncrease[0] << " render_warm_sampled_zone_increase_bytes=" << PeakIncrease[1]
		<< " collision_cold_sampled_zone_increase_bytes=" << PeakIncrease[2] << " collision_warm_sampled_zone_increase_bytes=" << PeakIncrease[3]
		<< " render_cold_source_requests=1 render_warm_source_requests=0"
		<< " warmup_pairs=1 measured_pairs=3" << std::endl;
	Log.Stop();
	Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticMeshBuildQualificationTests, TypedStorageTransferAlternatives)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	const auto Root = Testing::CreateTestFixtureDirectory("TypedStorageTransfer");
	FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
	auto Geometry = MakeTransferGeometry(100000);
	FCookBodySetupInfo CollisionInput{.Mode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0};
	CollisionInput.TriangleMeshDesc.Positions = Geometry.Meshes.front().Positions;
	CollisionInput.TriangleMeshDesc.Indices = Geometry.Meshes.front().Indices;
	CollisionInput.bPersistDerivedData = false;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	auto Read = Source.AcquireGeometry();
	ASSERT_TRUE(Read);
	const std::array<FStaticMeshBuildMaterialSlot, 1> Slots{{{.Name = FName("Material"), .SourceName = "Material"}}};
	auto* Module = IMeshBuilderModule::Get();
	ASSERT_NE(Module, nullptr);
	auto Product = Module->BuildRender({.Geometry = *Read, .MaterialSlots = Slots});
	ASSERT_TRUE(Product) << FormatStaticMeshRenderBuildError(Product.error());
	FTransferAccounting MeshAccounting;
	for (auto& LOD : Product->LODs)
	{
		MeasureArrayTransfer(std::move(LOD.Positions), MeshAccounting);
		MeasureArrayTransfer(std::move(LOD.Normals), MeshAccounting);
		MeasureArrayTransfer(std::move(LOD.Tangents), MeshAccounting);
		for (auto& Channel : LOD.TexCoords) MeasureArrayTransfer(std::move(Channel), MeshAccounting);
		MeasureArrayTransfer(std::move(LOD.Colors), MeshAccounting);
		MeasureArrayTransfer(std::move(LOD.Indices), MeshAccounting);
	}
	auto Collision = FPhysicsCookHelper::Cook(CollisionInput);
	ASSERT_TRUE(Collision) << Collision.error().ToString();
	ASSERT_TRUE(Collision->Complex);
	// Snapshot outside the experiment. Current PhysicsCore exposes immutable
	// element access, so these copies are reported separately; Stage 4 must
	// change producer ownership before claiming zero-copy production cooking.
	auto MeasureGeometry = [](const FCollisionGeometryRef& Native) {
		FTransferAccounting Accounting;
		auto MeasureNative = [&]<typename T>(uint32 Count, auto Get) {
			std::vector<T> Values;
			Values.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index) Values.push_back(Get(Index));
			MeasureArrayTransfer(std::move(Values), Accounting);
		};
		MeasureNative.operator()<FVector3>(Native.GetVertexCount(), [&](uint32 I) { return *Native.GetVertex(I); });
		MeasureNative.operator()<FCollisionGeometryTriangle>(Native.GetTriangleCount(), [&](uint32 I) { return *Native.GetTriangle(I); });
		MeasureNative.operator()<FCollisionGeometryNode>(Native.GetNodeCount(), [&](uint32 I) { return *Native.GetNode(I); });
		MeasureNative.operator()<uint32>(Native.GetLeafTriangleCount(), [&](uint32 I) { return Native.GetLeafTriangle(I); });
		MeasureNative.operator()<FCollisionHullPlane>(Native.GetHullPlaneCount(), [&](uint32 I) { return *Native.GetHullPlane(I); });
		MeasureNative.operator()<FCollisionHullHalfEdge>(Native.GetHullHalfEdgeCount(), [&](uint32 I) { return *Native.GetHullHalfEdge(I); });
		MeasureNative.operator()<FCollisionHullFace>(Native.GetHullFaceCount(), [&](uint32 I) { return *Native.GetHullFace(I); });
		return Accounting;
	};
	const auto PhysicsAccounting = MeasureGeometry(Collision->Complex);
	const std::array<FVector3, 4> Points{FVector3(0, 0, 0), FVector3(1, 0, 0), FVector3(0, 1, 0), FVector3(0, 0, 1)};
	auto Hull = FCollisionGeometryRef::BuildConvexHull(Points);
	ASSERT_TRUE(Hull);
	ASSERT_GT(Hull.GetHullHalfEdgeCount(), 0u);
	ASSERT_GT(Hull.GetHullPlaneCount(), 0u);
	ASSERT_GT(Hull.GetHullFaceCount(), 0u);
	const auto HullAccounting = MeasureGeometry(Hull);
	EXPECT_GT(MeshAccounting.PayloadBytes, 0u);
	EXPECT_GT(PhysicsAccounting.PayloadBytes, 0u);
	std::cout << "typed_storage_transfer mesh_payload_bytes=" << MeshAccounting.PayloadBytes
		<< " mesh_byte_roundtrip_copied_bytes=" << MeshAccounting.RoundTripCopiedBytes
		<< " mesh_max_stream_conversion_zone_increase=" << MeshAccounting.MaximumConversionZoneIncrease
		<< " collision_payload_bytes=" << PhysicsAccounting.PayloadBytes
		<< " collision_byte_roundtrip_copied_bytes=" << PhysicsAccounting.RoundTripCopiedBytes
		<< " collision_max_stream_conversion_zone_increase=" << PhysicsAccounting.MaximumConversionZoneIncrease
		<< " hull_payload_bytes=" << HullAccounting.PayloadBytes
		<< " hull_byte_roundtrip_copied_bytes=" << HullAccounting.RoundTripCopiedBytes
		<< " retained_array_transfer_copied_bytes=0 physics_fixture_snapshot_bytes="
		<< PhysicsAccounting.PayloadBytes + HullAccounting.PayloadBytes
		<< std::endl;
	Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticMeshBuildQualificationTests, CollisionCookedBlocksRetainNativeArraysThroughPublication)
{
	using namespace Durin;
	const auto Geometry = MakeTransferGeometry(100000);
	const auto& Mesh = Geometry.Meshes.front();
	std::vector<FVector3> Positions;
	Positions.reserve(Mesh.Positions.size());
	for (const auto& Position : Mesh.Positions) Positions.emplace_back(Position);
	const auto Cooked = FCollisionCookedData::BuildTriangleMesh(Positions, Mesh.Indices);
	ASSERT_TRUE(Cooked);
	const auto Native = Cooked.GetBlocks();
	ASSERT_TRUE(FCollisionCookedData::ValidateBlocks(Native));
	Testing::FQualificationAllocationSampler ColdAllocations;
	const auto Cold = FCollisionCookedData::FromBlocks(Native);
	const auto ColdGeometry = FCollisionGeometryRef::MakeCooked(Cold);
	const auto ColdAllocation = ColdAllocations.Finish();
	ASSERT_TRUE(ColdGeometry);
	EXPECT_EQ(ColdGeometry.GetVertex(0), Native.Vertices.GetNativeView<FVector3>()->data());
	EXPECT_EQ(ColdGeometry.GetTriangle(0), Native.Triangles.GetNativeView<FCollisionGeometryTriangle>()->data());
	EXPECT_EQ(ColdGeometry.GetNode(0), Native.Nodes.GetNativeView<FCollisionGeometryNode>()->data());
	EXPECT_TRUE(Cold.GetLeafTriangles().SharesStorageWith(Native.LeafTriangles));
	const uint64 PayloadBytes = Native.Vertices.size() + Native.Triangles.size()
		+ Native.Nodes.size() + Native.LeafTriangles.size();
	auto Raw = Native;
	Raw.Vertices = FSharedByteBuffer::Copy(Native.Vertices.GetBytes());
	Raw.Triangles = FSharedByteBuffer::Copy(Native.Triangles.GetBytes());
	Raw.Nodes = FSharedByteBuffer::Copy(Native.Nodes.GetBytes());
	Raw.LeafTriangles = FSharedByteBuffer::Copy(Native.LeafTriangles.GetBytes());
	Testing::FQualificationAllocationSampler WarmAllocations;
	const auto Warm = FCollisionCookedData::FromBlocks(Raw);
	const auto WarmGeometry = FCollisionGeometryRef::MakeCooked(Warm);
	const auto WarmAllocation = WarmAllocations.Finish();
	ASSERT_TRUE(WarmGeometry);
	EXPECT_EQ(WarmGeometry.GetVertex(0), Warm.GetVertices().GetNativeView<FVector3>()->data());
	EXPECT_EQ(WarmGeometry.GetTriangle(0), Warm.GetTriangles().GetNativeView<FCollisionGeometryTriangle>()->data());
	EXPECT_EQ(WarmGeometry.GetNode(0), Warm.GetNodes().GetNativeView<FCollisionGeometryNode>()->data());
	EXPECT_NE(WarmGeometry.GetVertex(0), ColdGeometry.GetVertex(0));
	EXPECT_TRUE(std::ranges::equal(Warm.GetLeafTriangles().GetBytes(), Native.LeafTriangles.GetBytes()));
	std::cout << "collision_cooked_blocks triangles=100000 float_to_double_recipe_bytes="
		<< Positions.size() * sizeof(FVector3)
		<< " payload_bytes=" << PayloadBytes
		<< " cold_representation_transfer_bytes=0 warm_native_conversion_bytes=" << PayloadBytes
		<< " cold_sampled_zone_increase_bytes=" << ColdAllocation.GetPeakIncrease()
		<< " warm_sampled_zone_increase_bytes=" << WarmAllocation.GetPeakIncrease()
		<< " allocation_sample_available=" << (ColdAllocation.bAvailable && WarmAllocation.bAvailable)
		<< std::endl;
}

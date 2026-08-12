#include "StaticMesh/StaticMesh.h"

#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "Source/SourcePath.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

#include "RHI.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"

#include <cmath>
#include <limits>

namespace Durin
{
	namespace
	{
		constexpr uint32 kStaticMeshRayQueryLeafTriangles = 8;

		struct FStaticMeshRayQueryBuildTriangle
		{
			uint32 Ordinal = 0;
			FBox Bounds;
			FVector3 Centroid{0.0};
		};

		auto UnionRayQueryBounds(const FBox& A, const FBox& B) -> FBox
		{
			if (!A.bIsValid) return B;
			if (!B.bIsValid) return A;
			return {Math::Min(A.Min, B.Min), Math::Max(A.Max, B.Max)};
		}

		auto BuildStaticMeshRayQueryRange(
			std::vector<FStaticMeshRayQueryBuildTriangle>& Triangles,
			size_t Begin, size_t End,
			FStaticMeshLODResources::FRayQueryAcceleration& Acceleration) -> uint32
		{
			const uint32 NodeIndex = static_cast<uint32>(Acceleration.Nodes.size());
			Acceleration.Nodes.push_back({});
			FBox Bounds;
			FBox CentroidBounds;
			for (size_t Index = Begin; Index < End; ++Index)
			{
				Bounds = UnionRayQueryBounds(Bounds, Triangles[Index].Bounds);
				CentroidBounds.AddPoint(Triangles[Index].Centroid);
			}
			Acceleration.Nodes[NodeIndex].Bounds = Bounds;
			if (End - Begin <= kStaticMeshRayQueryLeafTriangles)
			{
				auto& Node = Acceleration.Nodes[NodeIndex];
				Node.bLeaf = true;
				Node.First = static_cast<uint32>(Acceleration.TriangleOrdinals.size());
				Node.CountOrSecond = static_cast<uint32>(End - Begin);
				for (size_t Index = Begin; Index < End; ++Index)
					Acceleration.TriangleOrdinals.push_back(Triangles[Index].Ordinal);
				return NodeIndex;
			}
			const FVector3 Extent = CentroidBounds.Max - CentroidBounds.Min;
			uint32 Axis = Extent.y > Extent.x ? 1u : 0u;
			if (Extent.z > Extent[Axis]) Axis = 2u;
			std::stable_sort(Triangles.begin() + Begin, Triangles.begin() + End,
				[Axis](const FStaticMeshRayQueryBuildTriangle& A, const FStaticMeshRayQueryBuildTriangle& B)
				{
					return A.Centroid[Axis] < B.Centroid[Axis]
						|| (A.Centroid[Axis] == B.Centroid[Axis] && A.Ordinal < B.Ordinal);
				});
			const size_t Middle = Begin + (End - Begin) / 2;
			const uint32 Left = BuildStaticMeshRayQueryRange(Triangles, Begin, Middle, Acceleration);
			const uint32 Right = BuildStaticMeshRayQueryRange(Triangles, Middle, End, Acceleration);
			Acceleration.Nodes[NodeIndex].First = Left;
			Acceleration.Nodes[NodeIndex].CountOrSecond = Right;
			return NodeIndex;
		}

		auto CountStaticMeshRayQueryNodes(size_t TriangleCount) -> size_t
		{
			if (TriangleCount <= kStaticMeshRayQueryLeafTriangles) return 1;
			const size_t Left = TriangleCount / 2;
			return 1 + CountStaticMeshRayQueryNodes(Left)
				+ CountStaticMeshRayQueryNodes(TriangleCount - Left);
		}
	}

	auto BuildStaticMeshRayQueryAcceleration(const FStaticMeshLODResources& LOD)
		-> std::shared_ptr<const FStaticMeshLODResources::FRayQueryAcceleration>
	{
		const auto BuildStart = std::chrono::steady_clock::now();
		const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = LOD.IndexBuffer.GetIndices();
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0
			|| Positions.size() > std::numeric_limits<uint32>::max()
			|| Indices.size() > std::numeric_limits<uint32>::max()) return nullptr;
		std::vector<FStaticMeshRayQueryBuildTriangle> Triangles;
		Triangles.reserve(Indices.size() / 3);
		for (uint32 Index = 0; Index < Indices.size(); Index += 3)
		{
			const uint32 I0 = Indices[Index];
			const uint32 I1 = Indices[Index + 1];
			const uint32 I2 = Indices[Index + 2];
			if (I0 >= Positions.size() || I1 >= Positions.size() || I2 >= Positions.size()) return nullptr;
			const FVector3 A(Positions[I0]);
			const FVector3 B(Positions[I1]);
			const FVector3 C(Positions[I2]);
			if (!Math::IsFinite(A) || !Math::IsFinite(B) || !Math::IsFinite(C)) return nullptr;
			FBox Bounds;
			Bounds.AddPoint(A);
			Bounds.AddPoint(B);
			Bounds.AddPoint(C);
			Triangles.push_back({Index / 3, Bounds, (A + B + C) / 3.0});
		}
		auto Acceleration = std::make_shared<FStaticMeshLODResources::FRayQueryAcceleration>();
		Acceleration->SourceVertexCount = static_cast<uint32>(Positions.size());
		Acceleration->SourceIndexCount = static_cast<uint32>(Indices.size());
		Acceleration->Nodes.reserve(CountStaticMeshRayQueryNodes(Triangles.size()));
		Acceleration->TriangleOrdinals.reserve(Triangles.size());
		BuildStaticMeshRayQueryRange(Triangles, 0, Triangles.size(), *Acceleration);
		Acceleration->RetainedBytes = sizeof(FStaticMeshLODResources::FRayQueryAcceleration)
			+ Acceleration->Nodes.capacity() * sizeof(FStaticMeshLODResources::FRayQueryNode)
			+ Acceleration->TriangleOrdinals.capacity() * sizeof(uint32);
		Acceleration->BuildNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - BuildStart).count());
		const uint64 LayoutBudget = std::max<uint64>(1024, Triangles.size() * 96ull);
		if (Acceleration->RetainedBytes > MaximumStaticMeshRayQueryAccelerationBytes
			|| Acceleration->RetainedBytes > LayoutBudget) return nullptr;
		return Acceleration;
	}

	namespace
	{

		auto CheckStaticMeshUpdateThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto InitializeStaticMeshCandidate(
			FStaticMeshRenderData& Candidate,
			std::string& OutError) -> bool
		{
			Candidate.RecalculateBounds();
			if (GDynamicRHI == nullptr)
			{
				OutError.clear();
				return true;
			}

			std::atomic<bool> bInitialized = false;
			ENQUEUE_RENDER_COMMAND(InitStaticMeshCandidateResources)(
				[&Candidate, &bInitialized](
					FRHICommandListImmediate& CommandList) {
					bInitialized.store(
						Candidate.InitResources(CommandList),
						std::memory_order_release);
				});
			FRenderCommandFence InitFence;
			InitFence.BeginFence();
			InitFence.Wait();
			if (!bInitialized.load(std::memory_order_acquire))
			{
				check(Candidate.GetNumInitializedResources() == 0);
				OutError =
					"Static-mesh candidate resource initialization failed.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto RetireStaticMeshRenderData(
			std::unique_ptr<FStaticMeshRenderData>& RenderData) -> void
		{
			if (RenderData == nullptr) return;
			if (GDynamicRHI == nullptr)
			{
				check(RenderData->GetNumInitializedResources() == 0);
				return;
			}

			FStaticMeshRenderData* RenderDataToRelease = RenderData.get();
			ENQUEUE_RENDER_COMMAND(ReleaseRetiredStaticMeshResources)(
				[RenderDataToRelease](FRHICommandListImmediate&) {
					RenderDataToRelease->ReleaseResources();
				});
			FRenderCommandFence ReleaseFence;
			ReleaseFence.BeginFence();
			ReleaseFence.Wait();
			check(RenderData->GetNumInitializedResources() == 0);
		}

		constexpr float VectorTolerance = 1.0e-10f;




		auto IsCanonicalStaticMeshHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}


		auto RestoreStaticMeshRuntimeMetadata(
			const std::vector<FStaticMeshMaterialSlotDefinition>& MaterialSlots,
			FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				OutError = "Cached static-mesh material slot count does not match asset metadata.";
				return false;
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				const FStaticMeshMaterialSlotDefinition& Definition = MaterialSlots[SlotIndex];
				FStaticMeshMaterialSlot& Slot = RenderData.MaterialSlots[SlotIndex];
				// Editable asset metadata is authoritative; the cached payload contributes
				// only the compatible stable slot count and ordering.
				Slot.Name = Definition.Name.ToString();
				Slot.SourceMaterialIndex = Definition.SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
			{
				auto& Sections = RenderData.LODResources[LODIndex].Sections;
				for (size_t SectionIndex = 0; SectionIndex < Sections.size(); ++SectionIndex)
					Sections[SectionIndex].Name = std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			}
			return true;
		}

		auto ValidateStaticMeshMaterialSlotMapping(
			const FStaticMeshPayloadData& Payload,
			const std::vector<FStaticMeshMaterialSlotDefinition>& MaterialSlots,
			std::string& OutError) -> bool
		{
			if (Payload.MaterialSlotCount != MaterialSlots.size())
			{
				OutError = "Static-mesh payload material slot count does not match package metadata.";
				return false;
			}
			return true;
		}




		auto ImportAxisVector(EStaticMeshImportAxis Axis, FVector3f& OutVector, uint32& OutComponent) -> bool
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: OutVector = FVector3f(1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::NegativeX: OutVector = FVector3f(-1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::PositiveY: OutVector = FVector3f(0.0f, 1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::NegativeY: OutVector = FVector3f(0.0f, -1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::PositiveZ: OutVector = FVector3f(0.0f, 0.0f, 1.0f); OutComponent = 2; return true;
			case EStaticMeshImportAxis::NegativeZ: OutVector = FVector3f(0.0f, 0.0f, -1.0f); OutComponent = 2; return true;
			}
			return false;
		}


	}

	auto FStaticMeshImportSettings::IsValid(std::string* OutError) const -> bool
	{
		FVector3f UnusedVector;
		uint32 ForwardComponent = 0;
		uint32 RightComponent = 0;
		uint32 UpComponent = 0;
		const bool bAxesKnown = ImportAxisVector(ForwardAxis, UnusedVector, ForwardComponent)
			&& ImportAxisVector(RightAxis, UnusedVector, RightComponent)
			&& ImportAxisVector(UpAxis, UnusedVector, UpComponent);
		if (!bAxesKnown)
		{
			if (OutError) *OutError = "The import coordinate system contains an unknown axis.";
			return false;
		}
		if (ForwardComponent == RightComponent || ForwardComponent == UpComponent || RightComponent == UpComponent)
		{
			if (OutError) *OutError = "Forward, Right, and Up must use X, Y, and Z exactly once.";
			return false;
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto FStaticMeshImportSettings::MakeDurin() -> FStaticMeshImportSettings
	{
		return {};
	}

	auto FStaticMeshImportSettings::MakeYUpNegativeZForward() -> FStaticMeshImportSettings
	{
		return {
			.ForwardAxis = EStaticMeshImportAxis::NegativeZ,
			.RightAxis = EStaticMeshImportAxis::PositiveX,
			.UpAxis = EStaticMeshImportAxis::PositiveY
		};
	}

	DStaticMesh::DStaticMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DStaticMesh::~DStaticMesh() = default;

	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetBodySetup() const -> DBodySetup*
	{
		return BodySetup.Get();
	}

	auto DStaticMesh::SetBodySetup(DBodySetup* InBodySetup) -> bool
	{
		if (InBodySetup && InBodySetup->GetOuter() != this) return false;
		if (BodySetup == InBodySetup) return true;
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		BodySetup = InBodySetup;
		MarkPackageDirty();
		return true;
	}

	auto DStaticMesh::BuildCollisionCandidate(
		const FStaticMeshRenderData& SourceRenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FCollisionGeometryRef& OutSimple,
		FCollisionGeometryRef& OutComplex,
		EBodySetupCollisionBuildStatus& OutStatus,
		std::string& OutKey,
		std::string& OutDiagnostic,
		uint64& OutPayloadBytes,
		std::string& OutError) const -> bool
	{
		const FStaticMeshAuthoringHandlers Handlers =
			GetStaticMeshAuthoringHandlers();
		if (!Handlers.BuildCollisionProduct)
		{
			OutError = "StaticMesh collision build capability is unavailable.";
			return false;
		}
		FStaticMeshCollisionAuthoringProduct Product;
		if (!Handlers.BuildCollisionProduct(
			SourceRenderData, SourceImportData, Mode, Policy, Product, OutError))
			return false;
		OutSimple = std::move(Product.Simple);
		OutComplex = std::move(Product.Complex);
		OutStatus = Product.Status;
		OutKey = std::move(Product.DerivedDataKey);
		OutDiagnostic = std::move(Product.Diagnostic);
		OutPayloadBytes = Product.PayloadBytes;
		return true;
	}
	auto DStaticMesh::SetCollisionSourceMode(
		EBodySetupCollisionSourceMode Mode,
		std::string& OutError) -> bool
	{
		if (Mode != EBodySetupCollisionSourceMode::None
			&& Mode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			&& Mode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			OutError = "Static-mesh collision source mode is invalid.";
			return false;
		}
		if (Mode == EBodySetupCollisionSourceMode::None)
		{
			if (!BodySetup) { OutError.clear(); return true; }
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			if (!BodySetup->SetCollisionSourceMode(Mode)) return false;
			BodySetup->ClearCollisionGeometry(EBodySetupCollisionBuildStatus::None, {});
			OutError.clear();
			return true;
		}
		if (!RenderData)
		{
			OutError = "Static-mesh collision authoring requires published CPU render data.";
			return false;
		}
		const EBodySetupCollisionQueryPolicy Policy = BodySetup
			? BodySetup->GetCollisionQueryPolicy()
			: EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EBodySetupCollisionBuildStatus Status;
		std::string Key;
		std::string Diagnostic;
		uint64 PayloadBytes = 0;
		if (!BuildCollisionCandidate(*RenderData, Mode, Policy, Simple, Complex,
			Status, Key, Diagnostic, PayloadBytes, OutError)) return false;
		DBodySetup* Setup = BodySetup.Get();
		if (!Setup)
		{
			Setup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			if (!Setup) { OutError = "Static mesh could not allocate BodySetup."; return false; }
			BodySetup = Setup;
		}
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		if (!Setup->SetCollisionSourceMode(Mode)
			|| !Setup->PublishCollisionGeometry(Simple, Complex, Status,
				std::move(Key), std::move(Diagnostic), PayloadBytes))
		{
			OutError = "Static mesh could not publish collision state.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto DStaticMesh::SetCollisionQueryPolicy(
		EBodySetupCollisionQueryPolicy Policy,
		std::string& OutError) -> bool
	{
		if (!BodySetup || BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None)
		{
			if (!BodySetup)
			{
				BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
				if (!BodySetup) { OutError = "Static mesh could not allocate BodySetup."; return false; }
			}
			const bool bChanged = BodySetup->SetCollisionQueryPolicy(Policy);
			OutError = bChanged ? std::string{} : "Static-mesh collision query policy is invalid.";
			return bChanged;
		}
		if (!RenderData) { OutError = "Static mesh has no CPU data for collision policy rebuild."; return false; }
		const EBodySetupCollisionSourceMode Mode = BodySetup->GetCollisionSourceMode();
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EBodySetupCollisionBuildStatus Status;
		std::string Key;
		std::string Diagnostic;
		uint64 PayloadBytes = 0;
		if (!BuildCollisionCandidate(*RenderData, Mode, Policy, Simple, Complex,
			Status, Key, Diagnostic, PayloadBytes, OutError)) return false;
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		if (!BodySetup->SetCollisionQueryPolicy(Policy)
			|| !BodySetup->PublishCollisionGeometry(Simple, Complex, Status,
				std::move(Key), std::move(Diagnostic), PayloadBytes))
		{
			OutError = "Static mesh could not publish collision policy state.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto DStaticMesh::RebuildCollision(std::string& OutError) -> bool
	{
		if (!BodySetup || BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None)
		{
			OutError.clear();
			return true;
		}
		return SetCollisionSourceMode(BodySetup->GetCollisionSourceMode(), OutError);
	}

	auto DStaticMesh::EnsureQualifiedBoxBodySetup() -> DBodySetup*
	{
		if (BodySetup) return BodySetup.Get();
		const std::string ObjectPath = GetObjectPath();
		if (!ObjectPath.starts_with("/Engine/Models/Box")) return nullptr;
		const std::optional<FBox> Bounds = GetLOD0LocalBounds();
		if (!Bounds || !Bounds->bIsValid || !Math::IsFinite(Bounds->Min) || !Math::IsFinite(Bounds->Max)) return nullptr;
		const FVector3 HalfExtent = Bounds->GetExtent();
		if (!FCollisionShape::MakeBox(HalfExtent).IsValid()) return nullptr;
		auto* Setup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
		if (!Setup || !Setup->SetBox(HalfExtent, Bounds->GetCenter())) return nullptr;
		BodySetup = Setup;
		return BodySetup.Get();
	}

	auto DStaticMesh::RefreshQualifiedBoxBodySetup() -> void
	{
		if (!BodySetup || !GetObjectPath().starts_with("/Engine/Models/Box")) return;
		const std::optional<FBox> Bounds = GetLOD0LocalBounds();
		if (Bounds && Bounds->bIsValid) BodySetup->SetBox(Bounds->GetExtent(), Bounds->GetCenter());
	}

	auto DStaticMesh::GetRenderResourceStatus() const
		-> FStaticMeshRenderResourceStatus
	{
		const uint64 Packed =
			RenderResourceStatus.load(std::memory_order_acquire);
		EStaticMeshRenderResourceReadiness Readiness =
			EStaticMeshRenderResourceReadiness::Unavailable;
		switch (UnpackRenderResourceState(Packed))
		{
		case EStaticMeshRenderResourceState::InitializationQueued:
			Readiness = EStaticMeshRenderResourceReadiness::Queued;
			break;
		case EStaticMeshRenderResourceState::Ready:
			Readiness = EStaticMeshRenderResourceReadiness::Ready;
			break;
		case EStaticMeshRenderResourceState::Failed:
			Readiness = EStaticMeshRenderResourceReadiness::Failed;
			break;
		case EStaticMeshRenderResourceState::Uninitialized:
		case EStaticMeshRenderResourceState::ReleaseQueued:
		case EStaticMeshRenderResourceState::Released:
			break;
		}
		return {
			.Readiness = Readiness,
			.Revision = UnpackRenderResourceRevision(Packed)};
	}

	auto DStaticMesh::GetLOD0LocalBounds() const -> std::optional<FBox>
	{
		if (RenderData == nullptr || RenderData->LODResources.empty())
			return std::nullopt;
		const FBox& Bounds = RenderData->LODResources.front().LocalBounds;
		if (!Bounds.bIsValid
			|| !std::isfinite(Bounds.Min.x)
			|| !std::isfinite(Bounds.Min.y)
			|| !std::isfinite(Bounds.Min.z)
			|| !std::isfinite(Bounds.Max.x)
			|| !std::isfinite(Bounds.Max.y)
			|| !std::isfinite(Bounds.Max.z))
		{
			return std::nullopt;
		}
		const FVector3 Size = Bounds.Max - Bounds.Min;
		if (Size.x <= 0.0 || Size.y <= 0.0 || Size.z <= 0.0)
			return std::nullopt;
		return Bounds;
	}

	auto DStaticMesh::InspectCollision() const -> FStaticMeshCollisionInspection
	{
		FStaticMeshCollisionInspection Result{};
		Result.Mode = EBodySetupCollisionSourceMode::None;
		Result.Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		Result.BuildStatus = EBodySetupCollisionBuildStatus::None;
		Result.GeometryKind = ECollisionGeometryKind::Primitive;
		Result.BuilderVersion = StaticMeshCollisionBuilderVersion;
		Result.SchemaVersion = StaticMeshCollisionPayloadSchemaVersion;
		if (!BodySetup) return Result;
		Result.Mode = BodySetup->GetCollisionSourceMode();
		Result.Policy = BodySetup->GetCollisionQueryPolicy();
		Result.BuildStatus = BodySetup->GetCollisionBuildStatus();
		Result.BuildRevision = BodySetup->GetCollisionBuildRevision();
		Result.PayloadBytes = BodySetup->GetCollisionPayloadBytes();
		Result.CacheKey = BodySetup->GetCollisionDerivedDataKey();
		Result.Diagnostic = BodySetup->GetCollisionDiagnostic();
		if (RenderData && !RenderData->LODResources.empty())
			Result.SourceTriangles = static_cast<uint32>(
				RenderData->LODResources.front().IndexBuffer.GetIndices().size() / 3);
		FCollisionGeometryRef Geometry;
		if (!BodySetup->BuildSimpleGeometry(Geometry))
			BodySetup->BuildComplexGeometry(Geometry);
		if (!Geometry)
		{
			Result.bRevisionCoherent = Result.Mode == EBodySetupCollisionSourceMode::None;
			return Result;
		}
		Result.GeometryKind = Geometry.GetKind();
		Result.bHasGeometry = true;
		Result.RetainedTriangles = Geometry.GetTriangleCount();
		Result.RemovedTriangles = Result.SourceTriangles >= Result.RetainedTriangles
			? Result.SourceTriangles - Result.RetainedTriangles : 0;
		Result.Nodes = Geometry.GetNodeCount();
		Result.RuntimeBytes = Geometry.GetRetainedBytes();
		FVector3 Minimum;
		FVector3 Maximum;
		if (Geometry.GetLocalBounds(Minimum, Maximum)) Result.Bounds = FBox(Minimum, Maximum);
		Result.bRevisionCoherent = Result.BuildRevision != 0;
		return Result;
	}

	auto DStaticMesh::LoadRenderResourceState() const
		-> EStaticMeshRenderResourceState
	{
		return UnpackRenderResourceState(
			RenderResourceStatus.load(std::memory_order_acquire));
	}

	auto DStaticMesh::PublishRenderResourceState(
		EStaticMeshRenderResourceState State) -> void
	{
		uint64 Current = RenderResourceStatus.load(std::memory_order_acquire);
		for (;;)
		{
			const uint64 Revision = UnpackRenderResourceRevision(Current);
			check(Revision < (std::numeric_limits<uint64>::max()
				>> RenderResourceStateBits));
			const uint64 Desired = PackRenderResourceStatus(State, Revision + 1);
			if (RenderResourceStatus.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel))
			{
				return;
			}
		}
	}

	auto DStaticMesh::TryPublishRenderResourceState(
		EStaticMeshRenderResourceState Expected,
		EStaticMeshRenderResourceState State) -> bool
	{
		uint64 Current = RenderResourceStatus.load(std::memory_order_acquire);
		for (;;)
		{
			if (UnpackRenderResourceState(Current) != Expected) return false;
			const uint64 Revision = UnpackRenderResourceRevision(Current);
			check(Revision < (std::numeric_limits<uint64>::max()
				>> RenderResourceStateBits));
			const uint64 Desired = PackRenderResourceStatus(State, Revision + 1);
			if (RenderResourceStatus.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel))
			{
				return true;
			}
		}
	}

	auto DStaticMesh::AdvanceRenderResourceRevision() -> void
	{
		PublishRenderResourceState(LoadRenderResourceState());
	}

	auto DStaticMesh::InitResources() -> void
	{
		CheckStaticMeshUpdateThread();
		if (RenderData == nullptr || GDynamicRHI == nullptr) return;

		if (!TryPublishRenderResourceState(
			EStaticMeshRenderResourceState::Uninitialized,
			EStaticMeshRenderResourceState::InitializationQueued))
		{
			return;
		}

#if DURIN_BUILD_DEBUG
		RenderData->SetResourceDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName())));
#endif
		FStaticMeshRenderData* RenderDataToInitialize = RenderData.get();
		ENQUEUE_RENDER_COMMAND(InitStaticMeshResources)(
			[this, RenderDataToInitialize](
				FRHICommandListImmediate& CommandList) {
				const EStaticMeshRenderResourceState Result =
					RenderDataToInitialize->InitResources(CommandList)
					? EStaticMeshRenderResourceState::Ready
					: EStaticMeshRenderResourceState::Failed;
				TryPublishRenderResourceState(
					EStaticMeshRenderResourceState::InitializationQueued,
					Result);
			});
	}

	auto DStaticMesh::ReleaseResources() -> void
	{
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState State =
			LoadRenderResourceState();
		if (State == EStaticMeshRenderResourceState::Released
			|| State == EStaticMeshRenderResourceState::ReleaseQueued)
		{
			return;
		}

		if (RenderData == nullptr
			|| State == EStaticMeshRenderResourceState::Uninitialized)
		{
			check(RenderData == nullptr
				|| RenderData->GetNumInitializedResources() == 0);
			PublishRenderResourceState(
				EStaticMeshRenderResourceState::Released);
			return;
		}

		PublishRenderResourceState(
			EStaticMeshRenderResourceState::ReleaseQueued);
		FStaticMeshRenderData* RenderDataToRelease = RenderData.get();
		ENQUEUE_RENDER_COMMAND(ReleaseStaticMeshResources)(
			[this, RenderDataToRelease](FRHICommandListImmediate&) {
				RenderDataToRelease->ReleaseResources();
				check(
					RenderDataToRelease->GetNumInitializedResources()
					== 0);
				PublishRenderResourceState(
					EStaticMeshRenderResourceState::Released);
			});
	}

	auto DStaticMesh::GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*
	{
		return SlotIndex < MaterialSlots.size() ? &MaterialSlots[SlotIndex] : nullptr;
	}

	auto DStaticMesh::FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, Name, &FStaticMeshMaterialSlotDefinition::Name);
		return It == MaterialSlots.end() ? nullptr : &*It;
	}

	auto DStaticMesh::GetMaterialIndex(FName Name) const -> std::optional<uint32>
	{
		if (Name.IsNone()) return std::nullopt;
		const auto It = std::ranges::find(MaterialSlots, Name, &FStaticMeshMaterialSlotDefinition::Name);
		if (It == MaterialSlots.end()) return std::nullopt;
		return static_cast<uint32>(std::distance(MaterialSlots.begin(), It));
	}

	auto DStaticMesh::RenameMaterialSlot(uint32 SlotIndex, FName Name, std::string& OutError) -> bool
	{
		if (SlotIndex >= MaterialSlots.size())
		{
			OutError = std::format("Static mesh material slot index {} is out of range.", SlotIndex);
			return false;
		}
		if (Name.IsNone())
		{
			OutError = "Static mesh material slot name cannot be None.";
			return false;
		}
		const auto Existing = std::ranges::find(MaterialSlots, Name, &FStaticMeshMaterialSlotDefinition::Name);
		if (Existing != MaterialSlots.end() && Existing != MaterialSlots.begin() + SlotIndex)
		{
			OutError = std::format("Static mesh material slot name '{}' is already in use.", Name.ToString());
			return false;
		}
		if (MaterialSlots[SlotIndex].Name == Name)
		{
			OutError.clear();
			return true;
		}
		MaterialSlots[SlotIndex].Name = Name;
		if (RenderData && SlotIndex < RenderData->MaterialSlots.size())
			RenderData->MaterialSlots[SlotIndex].Name = Name.ToString();
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::CommitRenderDataCandidate(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FStaticMeshMaterialSlotDefinition>* InMaterialSlots,
		std::string& OutError,
		bool bBuildAuthoredCollision) -> bool
	{
		CheckStaticMeshUpdateThread();
		if (InRenderData == nullptr)
		{
			OutError = "Static-mesh publication requires render data.";
			return false;
		}
		if (!ValidateStaticMeshLODScreenSizes(
			InRenderData->LODResources, OutError))
		{
			return false;
		}
		InRenderData->RecalculateBounds();
#if DURIN_WITH_EDITOR
		for (FStaticMeshLODResources& LOD : InRenderData->LODResources)
			LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD);
#endif
		FCollisionGeometryRef CollisionSimple;
		FCollisionGeometryRef CollisionComplex;
		EBodySetupCollisionBuildStatus CollisionStatus = EBodySetupCollisionBuildStatus::None;
		std::string CollisionKey;
		std::string CollisionDiagnostic;
		uint64 CollisionPayloadBytes = 0;
		const bool bHasAuthoredCollision = bBuildAuthoredCollision && BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		if (bHasAuthoredCollision && !BuildCollisionCandidate(
			*InRenderData,
			BodySetup->GetCollisionSourceMode(),
			BodySetup->GetCollisionQueryPolicy(),
			CollisionSimple,
			CollisionComplex,
			CollisionStatus,
			CollisionKey,
			CollisionDiagnostic,
			CollisionPayloadBytes,
			OutError)) return false;

#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName()));
#endif
		if (RenderData == nullptr)
		{
#if DURIN_BUILD_DEBUG
			InRenderData->SetResourceDebugOwner(DebugOwner);
#endif
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
			RenderData = std::move(InRenderData);
			RefreshQualifiedBoxBodySetup();
			if (bHasAuthoredCollision)
			{
				const bool bPublished = BodySetup->PublishCollisionGeometry(
					CollisionSimple, CollisionComplex, CollisionStatus,
					std::move(CollisionKey), std::move(CollisionDiagnostic),
					CollisionPayloadBytes);
				check(bPublished);
			}
			PublishRenderResourceState(
				EStaticMeshRenderResourceState::Uninitialized);
			OutError.clear();
			return true;
		}
#if DURIN_BUILD_DEBUG
		InRenderData->SetResourceDebugOwner(DebugOwner);
#endif
		if (!InitializeStaticMeshCandidate(*InRenderData, OutError))
		{
			return false;
		}

		const EStaticMeshRenderResourceState CandidateState =
			GDynamicRHI != nullptr
				? EStaticMeshRenderResourceState::Ready
				: EStaticMeshRenderResourceState::Uninitialized;
		{
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			std::unique_ptr<FStaticMeshRenderData> OldRenderData =
				std::move(RenderData);
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
			RenderData = std::move(InRenderData);
			RefreshQualifiedBoxBodySetup();
			if (bHasAuthoredCollision)
			{
				const bool bPublished = BodySetup->PublishCollisionGeometry(
					CollisionSimple, CollisionComplex, CollisionStatus,
					std::move(CollisionKey), std::move(CollisionDiagnostic),
					CollisionPayloadBytes);
				check(bPublished);
			}
			PublishRenderResourceState(CandidateState);
			RetireStaticMeshRenderData(OldRenderData);
		}
		OutError.clear();
		return true;
	}

	auto DStaticMesh::BeginDestroy() -> void
	{
		const EStaticMeshRenderResourceState State =
			LoadRenderResourceState();
		const bool bHasQueuedResourceWork =
			State != EStaticMeshRenderResourceState::Uninitialized
			&& State != EStaticMeshRenderResourceState::Released;
		ReleaseResources();
		if (bHasQueuedResourceWork)
		{
			ReleaseResourcesFence.BeginFence();
		}
		Super::BeginDestroy();
	}

	auto DStaticMesh::IsReadyForFinishDestroy() -> bool
	{
		return ReleaseResourcesFence.IsFenceComplete()
			&& Super::IsReadyForFinishDestroy();
	}

	auto DStaticMesh::FinishDestroy() -> void
	{
		check(ReleaseResourcesFence.IsFenceComplete());
		check(LoadRenderResourceState()
			== EStaticMeshRenderResourceState::Released);
		check(RenderData == nullptr
			|| RenderData->GetNumInitializedResources() == 0);
		AdvanceRenderResourceRevision();
		RenderData.reset();
		Super::FinishDestroy();
	}

	auto DStaticMesh::CreateDebugTriangle(DObject* Outer) -> DStaticMesh*
	{
		DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, "DebugStaticMesh");
		Mesh->MaterialSlots.push_back({.Name = FName("Default"), .SourceMaterialIndex = 0});
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.push_back({"Default", 0});
		FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
		LOD.ScreenSize = GenerateDefaultStaticMeshLODScreenSizes(1).front();
		LOD.VertexBuffers.PositionVertexBuffer.Init({
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		});
		LOD.IndexBuffer.Init({0, 1, 2});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(
				3, FVector3f(0.0f, 0.0f, 1.0f)),
			std::vector<FVector4f>(
				3, FVector4f(1.0f, 0.0f, 0.0f, 1.0f)));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			{}, 3, 0);
		LOD.VertexBuffers.ColorVertexBuffer.Init({}, 3);
		LOD.VertexBuffers.Finalize(
			LOD.NumTexCoords, LOD.bHasColorVertexData);
		LOD.Sections.push_back({"Default", 0, 3, 0, 2, 0, {}});
		std::string PublishError;
		if (!Mesh->CommitRenderDataCandidate(
			std::move(RenderData), nullptr, PublishError))
		{
			DURIN_ERROR(
				"Failed to create debug static mesh: {}",
				PublishError);
			MarkAsGarbage(Mesh);
			return nullptr;
		}
		return Mesh;
	}

	auto DStaticMesh::PublishRenderData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FStaticMeshMaterialSlotDefinition> InMaterialSlots,
		bool bSlotMetadataChanged,
		std::string& OutError) -> bool
	{
		if (!CommitRenderDataCandidate(
			std::move(InRenderData), &InMaterialSlots, OutError))
		{
			return false;
		}
		if (bSlotMetadataChanged)
		{
			MarkPackageDirty();
			Asset::ReportAssetLoadMutation(
				this,
				"Engine.StaticMesh.MaterialSlotsV1",
				"Static mesh material-slot identity metadata was upgraded.",
				Asset::EAssetLoadMutationKind::Upgrade);
		}
		return true;
	}

	auto DStaticMesh::SeedMaterialReconciliationFrom(
		const DStaticMesh& Previous) -> void
	{
		MaterialSlots = Previous.MaterialSlots;
		SourceImportData = Previous.SourceImportData;
		if (Previous.BodySetup
			&& Previous.BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
		{
			if (!BodySetup)
				BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			if (BodySetup)
			{
				BodySetup->SetCollisionQueryPolicy(
					Previous.BodySetup->GetCollisionQueryPolicy());
				BodySetup->SetCollisionSourceMode(
					Previous.BodySetup->GetCollisionSourceMode());
			}
		}
	}

	auto DStaticMesh::PublishImportedProduct(
		FStaticMeshAuthoringProduct Product,
		std::string& OutError) -> bool
	{
		const FStaticMeshSourceImportData PreviousSource = SourceImportData;
		SourceImportData = std::move(Product.SourceImportData);
		if (!PublishRenderData(
			std::move(Product.RenderData),
			std::move(Product.MaterialSlots),
			Product.bSlotMetadataChanged,
			OutError))
		{
			SourceImportData = PreviousSource;
			return false;
		}
		DerivedDataDiagnostic = {
			.Status = Product.DerivedDataStatus,
			.Key = std::move(Product.DerivedDataKey),
			.Message = std::move(Product.DiagnosticMessage),
			.bSourceImporterInvoked = Product.bSourceImporterInvoked};
		if (Product.bMarkPackageDirty) MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::SetImportedDefaultMaterial(
		uint32 SourceMaterialIndex,
		DMaterialInterface* Material,
		std::string& OutError) -> bool
	{
		const auto Slot = std::ranges::find(
			MaterialSlots,
			SourceMaterialIndex,
			&FStaticMeshMaterialSlotDefinition::SourceMaterialIndex);
		if (Slot == MaterialSlots.end())
		{
			OutError = std::format(
				"Static mesh has no slot for source material {}.", SourceMaterialIndex);
			return false;
		}
		if (std::ranges::find(
			std::next(Slot),
			MaterialSlots.end(),
			SourceMaterialIndex,
			&FStaticMeshMaterialSlotDefinition::SourceMaterialIndex) != MaterialSlots.end())
		{
			OutError = std::format(
				"Static mesh has ambiguous slots for source material {}.", SourceMaterialIndex);
			return false;
		}
		Slot->DefaultMaterial = Material;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::ExchangeImportedState(
		DStaticMesh& Other,
		std::string& OutError) -> bool
	{
		if (&Other == this)
		{
			OutError.clear();
			return true;
		}
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState OtherState =
			Other.LoadRenderResourceState();
		const bool bCandidateAlreadyReady =
			OtherState == EStaticMeshRenderResourceState::Ready;
		const bool bCandidateCanInitialize =
			(OtherState == EStaticMeshRenderResourceState::Uninitialized
				|| OtherState ==
					EStaticMeshRenderResourceState::Released)
			&& Other.RenderData != nullptr
			&& Other.RenderData->GetNumInitializedResources() == 0;
		if (Other.RenderData == nullptr
			|| (!bCandidateAlreadyReady
				&& !bCandidateCanInitialize))
		{
			OutError =
				"Static-mesh imported-state exchange requires a detached "
				"ready or uninitialized candidate.";
			return false;
		}

#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName()));
#endif
		if (!bCandidateAlreadyReady)
		{
#if DURIN_BUILD_DEBUG
			Other.RenderData->SetResourceDebugOwner(DebugOwner);
#endif
			if (!InitializeStaticMeshCandidate(
				*Other.RenderData, OutError))
			{
				return false;
			}
		}
		// An already initialized render resource keeps the debug owner it had
		// when initialization was queued. Relabeling it violates the render-
		// resource lifecycle and is not needed for transferring ownership.

		const EStaticMeshRenderResourceState IncomingState =
			bCandidateAlreadyReady || GDynamicRHI != nullptr
				? EStaticMeshRenderResourceState::Ready
				: EStaticMeshRenderResourceState::Uninitialized;
		{
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			std::unique_ptr<FStaticMeshRenderData> PreviousRenderData =
				std::move(RenderData);
			std::unique_ptr<FStaticMeshRenderData> IncomingRenderData =
				std::move(Other.RenderData);

			std::swap(SourceImportData, Other.SourceImportData);
			std::swap(NormalizedSize, Other.NormalizedSize);
			std::swap(MaterialSlots, Other.MaterialSlots);
			std::swap(CookedPayload, Other.CookedPayload);
			if (BodySetup && Other.BodySetup)
				BodySetup->ExchangeCollisionState(*Other.BodySetup);
			std::swap(
				DerivedDataDiagnostic,
				Other.DerivedDataDiagnostic);

			RenderData = std::move(IncomingRenderData);
			PublishRenderResourceState(IncomingState);
			RetireStaticMeshRenderData(PreviousRenderData);
			Other.RenderData = std::move(PreviousRenderData);
			Other.PublishRenderResourceState(
				EStaticMeshRenderResourceState::Uninitialized);
		}
		MarkPackageDirty();
		Other.MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PrepareImportedStateExchange(
		DStaticMesh& Candidate,
		std::string& OutError) -> std::unique_ptr<FStaticMeshImportedStateExchange>
	{
		if (&Candidate == this || !RenderData || !Candidate.RenderData)
		{
			OutError = "Static-mesh imported-state exchange requires distinct assets with render data.";
			return nullptr;
		}
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState CandidateState =
			Candidate.LoadRenderResourceState();
		if (CandidateState != EStaticMeshRenderResourceState::Ready)
		{
			if ((CandidateState != EStaticMeshRenderResourceState::Uninitialized
					&& CandidateState != EStaticMeshRenderResourceState::Released)
				|| Candidate.RenderData->GetNumInitializedResources() != 0)
			{
				OutError = "Static-mesh candidate cannot be prepared for imported-state exchange.";
				return nullptr;
			}
#if DURIN_BUILD_DEBUG
			Candidate.RenderData->SetResourceDebugOwner(GetPackage()
				? FName(GetPackage()->GetPackagePath())
				: FName("<transient-static-mesh>"));
#endif
			if (!InitializeStaticMeshCandidate(*Candidate.RenderData, OutError))
			{
				if (OutError.empty())
					OutError = "Static-mesh candidate cannot be prepared for imported-state exchange.";
				return nullptr;
			}
			Candidate.PublishRenderResourceState(
				GDynamicRHI ? EStaticMeshRenderResourceState::Ready
					: EStaticMeshRenderResourceState::Uninitialized);
		}
		OutError.clear();
		return std::unique_ptr<FStaticMeshImportedStateExchange>(
			new FStaticMeshImportedStateExchange(*this, Candidate));
	}

	FStaticMeshImportedStateExchange::FStaticMeshImportedStateExchange(
		DStaticMesh& InTarget,
		DStaticMesh& InCandidate)
		: Target(&InTarget), Candidate(&InCandidate) {}

	FStaticMeshImportedStateExchange::~FStaticMeshImportedStateExchange() = default;

	auto FStaticMeshImportedStateExchange::Swap() noexcept -> void
	{
		check(Target && Candidate && Target != Candidate);
		CheckStaticMeshUpdateThread();
		FStaticMeshRenderStateRecreateContext RecreateContext(Target);
		std::swap(Target->SourceImportData, Candidate->SourceImportData);
		std::swap(Target->NormalizedSize, Candidate->NormalizedSize);
		std::swap(Target->MaterialSlots, Candidate->MaterialSlots);
		std::swap(Target->CookedPayload, Candidate->CookedPayload);
		if (Target->BodySetup && Candidate->BodySetup)
			Target->BodySetup->ExchangeCollisionState(*Candidate->BodySetup);
		std::swap(Target->DerivedDataDiagnostic, Candidate->DerivedDataDiagnostic);
		std::swap(Target->RenderData, Candidate->RenderData);
		Target->RefreshQualifiedBoxBodySetup();
		Candidate->RefreshQualifiedBoxBodySetup();
		const DStaticMesh::EStaticMeshRenderResourceState TargetState =
			Target->LoadRenderResourceState();
		const DStaticMesh::EStaticMeshRenderResourceState CandidateState =
			Candidate->LoadRenderResourceState();
		Target->PublishRenderResourceState(CandidateState);
		Candidate->PublishRenderResourceState(TargetState);
		// Both render-data sets may already be initialized. Their debug owner
		// describes the initialization site and must not be changed after the
		// ownership exchange.
		Target->MarkPackageDirty();
	}

	auto FStaticMeshImportedStateExchange::Commit() noexcept -> void
	{
		if (bCommitted) return;
		Swap();
		bCommitted = true;
	}

	auto FStaticMeshImportedStateExchange::Reverse() noexcept -> void
	{
		if (!bCommitted) return;
		Swap();
		bCommitted = false;
	}

	auto FStaticMeshImportedStateExchange::Finalize() noexcept -> void
	{
		Target = nullptr;
		Candidate = nullptr;
	}

	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		DerivedDataDiagnostic = {};
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedRenderData(OutError);
		if (MaterialSlots.size() > MaximumStaticMeshMaterialSlots)
		{
			OutError = "Static mesh material-slot count is outside the supported range.";
			return false;
		}
		std::unordered_set<FName> SlotNames;
		for (const FStaticMeshMaterialSlotDefinition& Slot : MaterialSlots)
		{
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
			{
				OutError = "Static mesh material-slot names must be non-None and unique.";
				return false;
			}
		}
		if (Asset::IsAssetMigrationLoad())
		{
			OutError.clear();
			return true;
		}
		if (!SourceImportData.HasSource())
		{
			OutError.clear();
			return true;
		}

		const FStaticMeshAuthoringHandlers Handlers = GetStaticMeshAuthoringHandlers();
		if (!Handlers.PostLoadUncooked)
		{
			OutError = "StaticMesh uncooked load policy is unavailable.";
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = OutError;
			return false;
		}
		return Handlers.PostLoadUncooked(*this, DerivedDataDiagnostic, OutError);
	}
	auto DStaticMesh::LoadCookedRenderData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked static mesh '{}': {}", GetObjectPath(), Message);
			OutError = DerivedDataDiagnostic.Message;
			return false;
		};

		if (CookedPayload.PayloadId != StaticMeshPrimaryCookedPayloadId
			|| CookedPayload.LocationKind != static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != StaticMeshPayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod != static_cast<uint32>(Asset::ECookedPayloadCompression::None))
		{
			return FailCooked("required DMSH descriptor is missing or incompatible.");
		}
		const bool bRequiresCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		const Asset::FCookedPayloadDescriptor* CollisionDescriptor = bRequiresCollision
			? &BodySetup->GetCookedCollisionPayloadDescriptor() : nullptr;
		if (bRequiresCollision
			&& (CollisionDescriptor->PayloadId != StaticMeshCollisionCookedPayloadId
				|| CollisionDescriptor->LocationKind != static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
				|| CollisionDescriptor->PayloadSchemaVersion != StaticMeshCollisionPayloadSchemaVersion
				|| CollisionDescriptor->TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
				|| CollisionDescriptor->TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
				|| CollisionDescriptor->CompressionMethod != static_cast<uint32>(Asset::ECookedPayloadCompression::None)))
		{
			return FailCooked("required DCOL descriptor is missing or incompatible.");
		}

		const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				LoadContext.CookRoot, PackagePath, CompanionPath, &OutError))
		{
			return FailCooked(OutError.empty() ? "package companion path could not be resolved." : OutError);
		}

		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			Container,
			&OutError))
		{
			return FailCooked(OutError);
		}
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);
		FCollisionGeometryRef CookedSimple;
		FCollisionGeometryRef CookedComplex;
		if (bRequiresCollision)
		{
			std::span<const uint8> CollisionBytes;
			if (!Asset::ResolveCookedPayload(
				Container, *CollisionDescriptor, CollisionBytes, &OutError))
				return FailCooked(OutError);
			FStaticMeshCollisionPayloadData CollisionPayload;
			FCanonicalMemoryReader CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload);
			CollisionPayload.Serialize(
				CollisionAr, EStaticMeshTargetPlatform::Win64);
			if (CollisionAr.HasError())
				return FailCooked(CollisionAr.GetFailure()->Message);
			if (CollisionPayload.SourceMode != BodySetup->GetCollisionSourceMode()
				|| CollisionPayload.QueryPolicy != BodySetup->GetCollisionQueryPolicy())
				return FailCooked("DCOL policy does not match its cooked BodySetup metadata.");
			FCollisionGeometryRef Geometry;
			if (!MakeStaticMeshCollisionGeometry(CollisionPayload, Geometry, OutError))
				return FailCooked(OutError);
			if (CollisionPayload.SourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				CookedSimple = Geometry;
			else
				CookedComplex = Geometry;
		}

		FStaticMeshPayloadData Payload;
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(PayloadAr, EStaticMeshTargetPlatform::Win64);
		if (PayloadAr.HasError())
			return FailCooked(PayloadAr.GetFailure()->Message);
		if (!ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError)
			|| !MakeStaticMeshRenderData(Payload, CandidateRenderData, OutError)
			|| !RestoreStaticMeshRuntimeMetadata(MaterialSlots, *CandidateRenderData, OutError))
		{
			return FailCooked(OutError);
		}

		if (!CommitRenderDataCandidate(
			std::move(CandidateRenderData), nullptr, OutError, false))
		{
			return FailCooked(OutError);
		}
		if (bRequiresCollision)
		{
			const bool bPublished = BodySetup->PublishCollisionGeometry(
				CookedSimple, CookedComplex,
				EBodySetupCollisionBuildStatus::CookedLoaded,
				{}, "Loaded immutable collision from the cooked DCOL companion payload.",
				CollisionDescriptor->UncompressedSize);
			check(bPublished);
		}
		DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedLoaded;
		DerivedDataDiagnostic.Message = std::format(
			"Loaded cooked static-mesh payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DStaticMesh::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticSourceMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"Static mesh '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		if (!RenderData && !PostLoad(OutError)) return false;
		if (!RenderData)
		{
			OutError = std::format("Static mesh '{}' has no render data to cook.", GetObjectPath());
			return false;
		}

		FStaticMeshPayloadData Payload;
		std::vector<uint8> PayloadBytes;
		if (!MakeStaticMeshPayloadData(*RenderData, Payload, OutError)
			|| !ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError))
		{
			OutError = std::format("Failed to cook static mesh '{}': {}", GetObjectPath(), OutError);
			return false;
		}
		FCanonicalMemoryWriter PayloadAr(
			PayloadBytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(PayloadAr, EStaticMeshTargetPlatform::Win64);
		if (PayloadAr.HasError())
		{
			OutError = std::format("Failed to cook static mesh '{}': {}",
				GetObjectPath(), PayloadAr.GetFailure()->Message);
			return false;
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = StaticMeshPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = StaticMeshPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = StaticMeshPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		std::vector<Asset::FCookedBulkPayload> BulkPayloads;
		BulkPayloads.emplace_back(std::move(BulkPayload));
		const bool bHasAuthoredCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		if (bHasAuthoredCollision)
		{
			FCollisionGeometryRef CollisionGeometry;
			bool bHasGeometry = BodySetup->GetCollisionSourceMode()
				== EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				? BodySetup->BuildSimpleGeometry(CollisionGeometry)
				: BodySetup->BuildComplexGeometry(CollisionGeometry);
			if (!bHasGeometry && !RebuildCollision(OutError))
			{
				OutError = std::format("Failed to cook static-mesh collision '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			if (!bHasGeometry)
			{
				bHasGeometry = BodySetup->GetCollisionSourceMode()
					== EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					? BodySetup->BuildSimpleGeometry(CollisionGeometry)
					: BodySetup->BuildComplexGeometry(CollisionGeometry);
				if (!bHasGeometry)
				{
					OutError = "Rebuilt collision did not publish its required geometry.";
					return false;
				}
			}
			FStaticMeshCollisionPayloadData CollisionPayload;
			std::vector<uint8> CollisionBytes;
			if (!MakeStaticMeshCollisionPayloadData(
				CollisionGeometry, BodySetup->GetCollisionQueryPolicy(), CollisionPayload, OutError))
			{
				OutError = std::format("Failed to encode static-mesh collision '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			FCanonicalMemoryWriter CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload);
			CollisionPayload.Serialize(
				CollisionAr, EStaticMeshTargetPlatform::Win64);
			if (CollisionAr.HasError())
			{
				OutError = std::format("Failed to encode static-mesh collision '{}': {}",
					GetObjectPath(), CollisionAr.GetFailure()->Message);
				return false;
			}
			BulkPayloads.push_back({
				.PayloadId = StaticMeshCollisionCookedPayloadId,
				.Flags = 1,
				.PayloadSchemaVersion = StaticMeshCollisionPayloadSchemaVersion,
				.Compression = Asset::ECookedPayloadCompression::None,
				.Alignment = StaticMeshCollisionPayloadAlignment,
				.Bytes = std::move(CollisionBytes)});
		}

		return Context.AddPackage(
			std::string(VirtualPackagePath),
			std::move(BulkPayloads),
			[this, bRetainDiagnosticSourceMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				const auto RenderDescriptor = std::ranges::find(
					Descriptors, StaticMeshPrimaryCookedPayloadId,
					&Asset::FCookedPayloadDescriptor::PayloadId);
				const auto CollisionDescriptor = std::ranges::find(
					Descriptors, StaticMeshCollisionCookedPayloadId,
					&Asset::FCookedPayloadDescriptor::PayloadId);
				const bool bRequiresCollision = BodySetup
					&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
				if (RenderDescriptor == Descriptors.end()
					|| (bRequiresCollision != (CollisionDescriptor != Descriptors.end()))
					|| Descriptors.size() != (bRequiresCollision ? 2u : 1u))
				{
					if (Error) *Error = "Static-mesh cook did not produce its exact required descriptor set.";
					return false;
				}

				const FStaticMeshSourceImportData SavedSourceImportData = SourceImportData;
				const std::vector<FStaticMeshMaterialSlotDefinition> SavedMaterialSlots = MaterialSlots;
				const Asset::FCookedPayloadDescriptor SavedCookedPayload = CookedPayload;
				const Asset::FCookedPayloadDescriptor SavedCollisionPayload = BodySetup
					? BodySetup->GetCookedCollisionPayloadDescriptor()
					: Asset::FCookedPayloadDescriptor{};
				CookedPayload = *RenderDescriptor;
				if (BodySetup)
					BodySetup->SetCookedCollisionPayloadDescriptor(
						bRequiresCollision ? *CollisionDescriptor : Asset::FCookedPayloadDescriptor{});
				if (!bRetainDiagnosticSourceMetadata)
				{
					SourceImportData = {};
					for (FStaticMeshMaterialSlotDefinition& Slot : MaterialSlots)
					{
						Slot.SourceName.clear();
						Slot.SourceMaterialIndex = 0;
					}
				}

				Asset::FAssetPackageSerializationOptions SerializationOptions;
				SerializationOptions.PropertyFilter = [this, bRetainDiagnosticSourceMetadata](
					const DObject* Object, const FProperty* Property) {
						const FName Name = Property->NamePrivate;
						if (Object == this)
							return bRetainDiagnosticSourceMetadata
								|| Name != FName("SourceImportData");
						if (Object == BodySetup)
							return Name != FName("CollisionBuildRevision")
								&& Name != FName("CollisionBuildStatus");
						return true;
					};
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, SerializationOptions);
				SourceImportData = SavedSourceImportData;
				MaterialSlots = SavedMaterialSlots;
				CookedPayload = SavedCookedPayload;
				if (BodySetup)
					BodySetup->SetCookedCollisionPayloadDescriptor(SavedCollisionPayload);
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			},
			&OutError);
	}

	auto PackStaticMeshTangentBasis(
		const FVector3f& Normal,
		const FVector4f& Tangent) -> FStaticMeshPackedTangentBasis
	{
		auto PackSnorm = [](float Value) -> int16 {
			return static_cast<int16>(std::lround(std::clamp(Value, -1.0f, 1.0f) * 32767.0f));
		};

		FStaticMeshPackedTangentBasis Result;
		Result.Normal = {PackSnorm(Normal.x), PackSnorm(Normal.y), PackSnorm(Normal.z), 0};
		Result.Tangent = {PackSnorm(Tangent.x), PackSnorm(Tangent.y), PackSnorm(Tangent.z), PackSnorm(Tangent.w)};
		return Result;
	}

	auto PackStaticMeshColor(
		const FVector4f& Color) -> FStaticMeshColorVertex
	{
		auto PackUnorm = [](float Value) -> uint8 {
			return static_cast<uint8>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
		};
		FStaticMeshColorVertex Result;
		Result.Color = {PackUnorm(Color.r), PackUnorm(Color.g), PackUnorm(Color.b), PackUnorm(Color.a)};
		return Result;
	}

	auto FStaticMeshVertexBuffer::FTangentsVertexBuffer::Init(
		std::vector<FVector3f> InNormals,
		std::vector<FVector4f> InTangents,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Normals = std::move(InNormals);
		Tangents = std::move(InTangents);
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FStaticMeshVertexBuffer::FTangentsVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Normals.empty()
			|| Tangents.size() != Normals.size()
			|| GetRHI() != nullptr)
		{
			return;
		}

		std::vector<FStaticMeshPackedTangentBasis> PackedTangents(
			Normals.size());
		for (size_t VertexIndex = 0;
			VertexIndex < Normals.size();
			++VertexIndex)
		{
			PackedTangents[VertexIndex] = PackStaticMeshTangentBasis(
				Normals[VertexIndex],
				Tangents[VertexIndex]);
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshTangentsVertexBuffer",
			static_cast<uint32>(
				PackedTangents.size()
				* sizeof(PackedTangents.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = PackedTangents.data();
		Desc.InitialData.Size = static_cast<uint32>(
			PackedTangents.size() * sizeof(PackedTangents.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	auto FStaticMeshVertexBuffer::FTexcoordVertexBuffer::Init(
		std::array<
			std::vector<FVector2f>,
			MaxStaticMeshUVChannels> InTexCoords,
		uint32 NumVertices,
		uint8 InNumTexCoords,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		TexCoords = std::move(InTexCoords);
		NumTexCoords = InNumTexCoords;
		bNeedsCPUAccess = bInNeedsCPUAccess;
		for (auto& Channel : TexCoords)
		{
			if (Channel.empty())
			{
				Channel.assign(NumVertices, FVector2f(0.0f));
			}
		}
	}

	auto FStaticMeshVertexBuffer::FTexcoordVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		const size_t NumVertices = TexCoords[0].size();
		if (NumVertices == 0
			|| !std::ranges::all_of(
				TexCoords,
				[NumVertices](const auto& Channel) {
					return Channel.size() == NumVertices;
				})
			|| GetRHI() != nullptr)
		{
			return;
		}

		std::vector<FStaticMeshTexcoordVertex>
			InterleavedTexCoords(NumVertices);
		for (size_t VertexIndex = 0;
			VertexIndex < NumVertices;
			++VertexIndex)
		{
			for (uint32 Channel = 0;
				Channel < MaxStaticMeshUVChannels;
				++Channel)
			{
				InterleavedTexCoords[VertexIndex].TexCoords[Channel] =
					TexCoords[Channel][VertexIndex];
			}
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshTexCoordVertexBuffer",
			static_cast<uint32>(
				InterleavedTexCoords.size()
				* sizeof(InterleavedTexCoords.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = InterleavedTexCoords.data();
		Desc.InitialData.Size = static_cast<uint32>(
			InterleavedTexCoords.size()
				* sizeof(InterleavedTexCoords.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	auto FColorVertexBuffer::Init(
		std::vector<FVector4f> InColors,
		uint32 NumVertices,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Colors = std::move(InColors);
		if (Colors.empty())
		{
			Colors.assign(NumVertices, FVector4f(1.0f));
		}
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FColorVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Colors.empty() || GetRHI() != nullptr) return;
		std::vector<FStaticMeshColorVertex> PackedColors(Colors.size());
		for (size_t VertexIndex = 0;
			VertexIndex < Colors.size();
			++VertexIndex)
		{
			PackedColors[VertexIndex] =
				PackStaticMeshColor(Colors[VertexIndex]);
		}
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshColorVertexBuffer",
			static_cast<uint32>(
				PackedColors.size() * sizeof(PackedColors.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = PackedColors.data();
		Desc.InitialData.Size = static_cast<uint32>(
			PackedColors.size() * sizeof(PackedColors.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	auto FStaticMeshVertexBuffers::Finalize(
		uint8 NumTexCoords,
		bool bHasColorVertexData) -> void
	{
		const uint32 NumVertices =
			PositionVertexBuffer.GetNumVertices();
		auto& TexCoords =
			StaticMeshVertexBuffer.TexCoordVertexBuffer
				.GetMutableTexCoords();
		for (auto& Channel : TexCoords)
		{
			if (Channel.empty())
			{
				Channel.assign(NumVertices, FVector2f(0.0f));
			}
		}
		StaticMeshVertexBuffer.TexCoordVertexBuffer.SetNumTexCoords(
			NumTexCoords);
		auto& Colors = ColorVertexBuffer.GetMutableColors();
		if (Colors.empty() && !bHasColorVertexData)
		{
			Colors.assign(NumVertices, FVector4f(1.0f));
		}
	}

	namespace
	{
		template<typename ResourceType>
		auto InitStaticMeshResource(
			ResourceType& Resource,
			FRHICommandListBase& RHICmdList) -> void
		{
			if (!Resource.IsInitialized())
			{
				Resource.InitResource(RHICmdList);
			}
			else if constexpr (requires { Resource.GetRHI(); })
			{
				if (Resource.GetRHI() == nullptr)
				{
					Resource.UpdateRHI(RHICmdList);
				}
			}
			else if (!Resource.IsReady())
			{
				Resource.UpdateRHI(RHICmdList);
			}
		}

		auto ReleaseStaticMeshResource(
			FRenderResource& Resource) -> void
		{
			if (Resource.IsInitialized())
			{
				Resource.ReleaseResource();
			}
			else
			{
				Resource.ReleaseRHI();
			}
		}
	}

	auto FStaticMeshVertexBuffers::InitResources(
		FRHICommandListBase& RHICmdList) -> void
	{
		InitStaticMeshResource(PositionVertexBuffer, RHICmdList);
		InitStaticMeshResource(
			StaticMeshVertexBuffer.TangentsVertexBuffer,
			RHICmdList);
		InitStaticMeshResource(
			StaticMeshVertexBuffer.TexCoordVertexBuffer,
			RHICmdList);
		InitStaticMeshResource(ColorVertexBuffer, RHICmdList);
	}

	auto FStaticMeshVertexBuffers::ReleaseResources() -> void
	{
		ReleaseStaticMeshResource(ColorVertexBuffer);
		ReleaseStaticMeshResource(
			StaticMeshVertexBuffer.TexCoordVertexBuffer);
		ReleaseStaticMeshResource(
			StaticMeshVertexBuffer.TangentsVertexBuffer);
		ReleaseStaticMeshResource(PositionVertexBuffer);
	}

	auto FRawStaticIndexBuffer::Init(
		std::vector<uint32> InIndices,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Indices = std::move(InIndices);
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FRawStaticIndexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Indices.empty() || GetRHI() != nullptr) return;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateIndex(
			"StaticMeshIndexBuffer",
			static_cast<uint32>(Indices.size() * sizeof(uint32)),
			sizeof(uint32));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = Indices.data();
		Desc.InitialData.Size =
			static_cast<uint32>(Indices.size() * sizeof(uint32));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	namespace
	{
		auto IsStaticMeshLODGeometryValid(
			const FStaticMeshLODResources& LOD,
			size_t MaterialSlotCount) -> bool
		{
			const auto& Positions =
				LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
			const auto& TangentsVertexBuffer =
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer;
			const auto& Normals =
				TangentsVertexBuffer.GetNormals();
			const auto& Tangents =
				TangentsVertexBuffer.GetTangents();
			const auto& TexCoords =
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.GetTexCoords();
			const auto& Colors =
				LOD.VertexBuffers.ColorVertexBuffer.GetColors();
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			const size_t NumVertices = Positions.size();
			const bool bValidStreams = NumVertices > 0
				&& Normals.size() == NumVertices
				&& Tangents.size() == NumVertices
				&& Colors.size() == NumVertices
				&& std::ranges::all_of(
					TexCoords,
					[NumVertices](const auto& Channel) {
						return Channel.size() == NumVertices;
					});
			const bool bValidIndices =
				!Indices.empty()
				&& Indices.size() % 3 == 0
				&& std::ranges::all_of(
					Indices,
					[NumVertices](uint32 Index) {
						return Index < NumVertices;
					});
			const bool bValidSections =
				!LOD.Sections.empty()
				&& std::ranges::all_of(
					LOD.Sections,
					[&Indices, &Positions, MaterialSlotCount](
						const FStaticMeshSection& Section) {
						return Section.IndexCount > 0
							&& Section.IndexCount % 3 == 0
							&& static_cast<uint64>(Section.FirstIndex)
								+ Section.IndexCount
								<= Indices.size()
							&& Section.MinVertexIndex
								<= Section.MaxVertexIndex
							&& Section.MaxVertexIndex
								< Positions.size()
							&& Section.MaterialSlotIndex
								< MaterialSlotCount;
					});
			return bValidStreams && bValidIndices && bValidSections;
		}
	}

	auto FStaticMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> bool
	{
		check(IsInRenderingThread());
		if (LODVertexFactories.empty())
		{
			LODVertexFactories.resize(LODResources.size());
		}
		if (LODResources.empty()
			|| LODVertexFactories.size() != LODResources.size())
		{
			ReleaseResources();
			return false;
		}
		std::string LODPolicyError;
		if (!ValidateStaticMeshLODScreenSizes(
			LODResources, LODPolicyError))
		{
			ReleaseResources();
			return false;
		}

		for (FStaticMeshLODResources& LOD : LODResources)
		{
			if (!LOD.VertexBuffers.PositionVertexBuffer.IsInitialized()
				&& !LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.IsInitialized()
				&& !LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.IsInitialized()
				&& !LOD.VertexBuffers.ColorVertexBuffer.IsInitialized()
				&& !LOD.IndexBuffer.IsInitialized())
			{
				LOD.VertexBuffers.Finalize(
					LOD.NumTexCoords,
					LOD.bHasColorVertexData);
			}
			if (!IsStaticMeshLODGeometryValid(
				LOD, MaterialSlots.size()))
			{
				ReleaseResources();
				return false;
			}
		}
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			LOD.VertexBuffers.InitResources(RHICmdList);
			InitStaticMeshResource(LOD.IndexBuffer, RHICmdList);
		}
		if (!std::ranges::all_of(
				LODResources,
				[this](const FStaticMeshLODResources& LOD) {
					return LOD.VertexBuffers.IsReady()
						&& LOD.IndexBuffer.IsReady()
						&& IsStaticMeshLODGeometryValid(
							LOD, MaterialSlots.size());
				}))
		{
			ReleaseResources();
			return false;
		}
		for (size_t LODIndex = 0;
			LODIndex < LODResources.size();
			++LODIndex)
		{
			FLocalVertexFactory& VertexFactory =
				LODVertexFactories[LODIndex].VertexFactory;
			if (!VertexFactory.IsInitialized()
				&& !VertexFactory.SetData(
					LODResources[LODIndex].VertexBuffers))
			{
				ReleaseResources();
				return false;
			}
			InitStaticMeshResource(VertexFactory, RHICmdList);
		}
		if (!std::ranges::all_of(
				LODVertexFactories,
				[](const FStaticMeshVertexFactories& Factories) {
					return Factories.VertexFactory.IsReady();
				}))
		{
			ReleaseResources();
			return false;
		}
		return true;
	}

	auto FStaticMeshRenderData::ReleaseResources() -> void
	{
		check(IsInRenderingThread());
		for (FStaticMeshVertexFactories& Factories
			: LODVertexFactories | std::views::reverse)
		{
			ReleaseStaticMeshResource(Factories.VertexFactory);
		}
		for (FStaticMeshLODResources& LOD
			: LODResources | std::views::reverse)
		{
			ReleaseStaticMeshResource(LOD.IndexBuffer);
			LOD.VertexBuffers.ReleaseResources();
		}
	}

#if DURIN_BUILD_DEBUG
	auto FStaticMeshRenderData::SetResourceDebugOwner(FName InOwner) -> void
	{
		if (LODVertexFactories.empty())
		{
			LODVertexFactories.resize(LODResources.size());
		}
		auto SetOwner = [InOwner](FRenderResource& Resource) {
			Resource.SetDebugOwner(InOwner);
		};
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			SetOwner(LOD.VertexBuffers.PositionVertexBuffer);
			SetOwner(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer);
			SetOwner(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer);
			SetOwner(LOD.VertexBuffers.ColorVertexBuffer);
			SetOwner(LOD.IndexBuffer);
		}
		for (FStaticMeshVertexFactories& Factories : LODVertexFactories)
		{
			SetOwner(Factories.VertexFactory);
		}
	}
#endif

	auto FStaticMeshRenderData::GetNumInitializedResources() const -> size_t
	{
		size_t Count = 0;
		auto CountResource = [&Count](const FRenderResource& Resource) {
			if (Resource.IsInitialized()) ++Count;
		};
		for (const FStaticMeshLODResources& LOD : LODResources)
		{
			CountResource(LOD.VertexBuffers.PositionVertexBuffer);
			CountResource(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer);
			CountResource(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer);
			CountResource(LOD.VertexBuffers.ColorVertexBuffer);
			CountResource(LOD.IndexBuffer);
		}
		for (const FStaticMeshVertexFactories& Factories
			: LODVertexFactories)
		{
			CountResource(Factories.VertexFactory);
		}
		return Count;
	}

	auto FStaticMeshRenderData::IsReadyForRendering(uint32 LODIndex) const -> bool
	{
		if (LODVertexFactories.size() != LODResources.size()
			|| LODIndex >= LODResources.size())
		{
			return false;
		}
		const FStaticMeshLODResources& LOD = LODResources[LODIndex];
		return LOD.VertexBuffers.IsReady()
			&& LOD.IndexBuffer.IsReady()
			&& LODVertexFactories[LODIndex].VertexFactory.IsReady()
			&& IsStaticMeshLODGeometryValid(
				LOD, MaterialSlots.size());
	}

	auto FStaticMeshRenderData::RecalculateBounds() -> void
	{
		LocalBounds.Reset();
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			const auto& Positions =
				LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			LOD.LocalBounds.Reset();
			for (const FVector3f& Position : Positions)
			{
				LOD.LocalBounds.AddPoint(FVector3(Position));
			}
			for (FStaticMeshSection& Section : LOD.Sections)
			{
				Section.LocalBounds.Reset();
				const uint64 EndIndex = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
				if (EndIndex > Indices.size()) continue;
				for (uint32 IndexOffset = 0; IndexOffset < Section.IndexCount; ++IndexOffset)
				{
					const uint32 VertexIndex =
						Indices[Section.FirstIndex + IndexOffset];
					if (VertexIndex < Positions.size())
					{
						Section.LocalBounds.AddPoint(
							FVector3(Positions[VertexIndex]));
					}
				}
			}
			for (const FVector3f& Position : Positions)
			{
				LocalBounds.AddPoint(FVector3(Position));
			}
		}
	}

	auto GenerateDefaultStaticMeshLODScreenSizes(
		uint32 LODCount) -> std::vector<float>
	{
		std::vector<float> Result(LODCount, 0.0f);
		for (uint32 LODIndex = 0; LODIndex + 1 < LODCount; ++LODIndex)
		{
			Result[LODIndex] = std::ldexp(
				1.0f, -static_cast<int>(LODIndex + 1));
		}
		return Result;
	}

	auto ValidateStaticMeshLODScreenSizes(
		std::span<const FStaticMeshLODResources> LODResources,
		std::string& OutError) -> bool
	{
		if (LODResources.empty())
		{
			OutError = "Static-mesh LOD policy requires at least one LOD.";
			return false;
		}
		for (size_t LODIndex = 0; LODIndex < LODResources.size(); ++LODIndex)
		{
			const float ScreenSize = LODResources[LODIndex].ScreenSize;
			if (!std::isfinite(ScreenSize)
				|| ScreenSize < 0.0f || ScreenSize > 1.0f
				|| (ScreenSize == 0.0f && std::signbit(ScreenSize)))
			{
				OutError = std::format(
					"Static-mesh LOD {} screen size must be finite and in [0, 1].",
					LODIndex);
				return false;
			}
			if (LODIndex > 0
				&& ScreenSize >= LODResources[LODIndex - 1].ScreenSize)
			{
				OutError = "Static-mesh LOD screen sizes must be strictly descending.";
				return false;
			}
		}
		if (LODResources.back().ScreenSize != 0.0f)
		{
			OutError = "Static-mesh lowest-detail LOD screen size must be exactly zero.";
			return false;
		}
		OutError.clear();
		return true;
	}
}

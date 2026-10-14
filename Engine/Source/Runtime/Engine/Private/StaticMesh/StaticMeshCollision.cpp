#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshGeometry.h"

#include "Math/Operations.h"
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "Threading/RunnableThread.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsMeshInputTask.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "DObject/DObjectArray.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"

namespace Durin
{
	auto GetStaticMeshPositionNormalization(const FBox& Bounds, float NormalizedSize)
		-> std::optional<FStaticMeshPositionNormalization>
	{
		const FVector3f Min(Bounds.Min), Max(Bounds.Max);
		const FVector3f Extent = Max - Min;
		const float Dimension = std::max(Extent.x, std::max(Extent.y, Extent.z));
		if (!Bounds.bIsValid || !Math::IsFinite(NormalizedSize) || NormalizedSize <= 0
			|| !Math::IsFinite(Dimension) || Dimension <= 0) return std::nullopt;
		return FStaticMeshPositionNormalization{(Min + Max) * 0.5f, NormalizedSize / Dimension};
	}

	auto DStaticMesh::ContainsPhysicsTriMeshData() const -> bool
	{
#if DURIN_WITH_EDITORONLY_DATA
		if (GetSource().IsValid()) return true;
#endif
		return RenderData && !RenderData->LODResources.empty();
	}

#if DURIN_WITH_EDITORONLY_DATA
	namespace
	{
		class FStaticMeshPhysicsInputTask final : public FPhysicsMeshInputTask
		{
		public:
			FStaticMeshPhysicsInputTask(FStaticMeshSource InSource, float InSize, uint64 InBytes)
				: Source(std::move(InSource)), Size(InSize), Bytes(InBytes) { Source.ReleaseGeometry(); }
			auto GetWorkingSetBytes() const -> uint64 override { return Bytes; }
			auto Execute(const FAssetBuildTaskContext& Control) const
				-> std::expected<FTriMeshCollisionData, std::string> override
			{
				if (Control.IsCancelled()) return {};
				if (Bytes > Control.MaximumWorkingSetBytes)
					return std::unexpected(MakeAssetBuildDiagnostic("Collision preparation exceeds the reservation.", "Input"));
				const auto Cancelled = [&] { return Control.IsCancelled(); };
				const auto Geometry = Source.AcquireGeometry(Cancelled);
				if (Cancelled()) return {};
				if (!Geometry) return std::unexpected(MakeAssetBuildDiagnostic(FormatStaticMeshSourceError(Geometry.error()), "Input"));
				FTriMeshCollisionData Data;
				FAssetBuildMemoryEstimate Budget{Control.MaximumWorkingSetBytes, 1024 * 1024};
				uint64 VertexCount = 0, IndexCount = 0;
				for (const auto& Mesh : (*Geometry)->Sections)
				{
					if (Cancelled()) return {};
					if (Mesh.Positions.empty() || Mesh.Indices.empty()) continue;
					VertexCount += Mesh.Positions.size(); IndexCount += Mesh.Indices.size();
					if (!Budget.Add(Mesh.Positions.size(), 512) || !Budget.Add(Mesh.Indices.size(), 192)
						|| VertexCount > std::numeric_limits<uint32>::max() || IndexCount > std::numeric_limits<uint32>::max())
						return std::unexpected(MakeAssetBuildDiagnostic("Collision input exceeds the reservation.", "Input"));
				}
				Data.Positions.reserve(VertexCount); Data.Indices.reserve(IndexCount);
				FBox Bounds;
				uint64 Work = 0;
				for (const auto& Mesh : (*Geometry)->Sections)
				{
					if (Mesh.Positions.empty() || Mesh.Indices.empty()) continue;
					const auto Base = static_cast<uint32>(Data.Positions.size());
					for (const auto& Position : Mesh.Positions)
					{
						if ((++Work % 256) == 0 && Cancelled()) return {};
						Data.Positions.push_back(Position); Bounds.AddPoint(FVector3(Position));
					}
					for (uint32 Index : Mesh.Indices)
					{
						if ((++Work % 256) == 0 && Cancelled()) return {};
						Data.Indices.push_back(Base + Mesh.GetVertexIndex(Index));
					}
				}
				const auto Normalization = GetStaticMeshPositionNormalization(Bounds, Size);
				if (!Normalization)
					return std::unexpected(MakeAssetBuildDiagnostic("Collision source has invalid normalization bounds.", "Input"));
				for (auto& Position : Data.Positions)
				{
					if ((++Work % 256) == 0 && Cancelled()) return {};
					Position = (Position - Normalization->Center) * Normalization->Scale;
				}
				if (Cancelled()) return {};
				return Data;
			}
		private:
			FStaticMeshSource Source;
			float Size;
			uint64 Bytes;
		};
	}

#endif

	auto DStaticMesh::CreatePhysicsMeshInputTask() const
		-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string>
	{
#if DURIN_WITH_EDITORONLY_DATA
		const auto& Source = GetSource();
		if (!Source.IsValid()) return IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask();
		FAssetBuildMemoryEstimate Memory{MaximumAssetBuildRequestBytes, 1024 * 1024};
		if (!Memory.Add(Source.GetGeometryBulk().GetPayloadSize(), 64) || !Memory.Add(Source.GetMeshCount(), 1024))
			return std::unexpected(MakeAssetBuildDiagnostic("Collision source exceeds its working-set budget.", "Input"));
		return std::make_unique<FStaticMeshPhysicsInputTask>(Source, NormalizedSize, Memory.Bytes);
#else
		return IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask();
#endif
	}

	auto DStaticMesh::GetPhysicsTriMeshData() const
		-> std::expected<FTriMeshCollisionData, std::string>
	{
#if DURIN_WITH_EDITORONLY_DATA
		const auto& Source = GetSource();
		if (Source.IsValid())
		{
			auto Task = CreatePhysicsMeshInputTask();
			if (!Task) return std::unexpected(Task.error());
			return (*Task)->Execute({.MaximumWorkingSetBytes = (*Task)->GetWorkingSetBytes()});
		}
#endif

		if (RenderData && !RenderData->LODResources.empty())
		{
			const auto& LOD = RenderData->LODResources.front();
			const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			FAssetBuildMemoryEstimate Memory{MaximumAssetBuildRequestBytes, 1024 * 1024};
			if (!Memory.Add(Positions.size(), 512) || !Memory.Add(Indices.size(), 192))
				return std::unexpected(MakeAssetBuildDiagnostic("Collision input exceeds its working-set budget.", "Input"));
			return FTriMeshCollisionData{{Positions.begin(), Positions.end()}, {Indices.begin(), Indices.end()}};
		}
		return std::unexpected(MakeAssetBuildDiagnostic("No collision source is available.", "Input"));
	}

	auto DStaticMesh::GetBodySetup() const -> DBodySetup*
	{
		return BodySetup.Get();
	}

	auto DStaticMesh::SetBodySetup(DBodySetup* InBodySetup) -> bool
	{
		if (InBodySetup && InBodySetup->GetOuter() != this) return false;
		if (BodySetup == InBodySetup) return true;
		if (BodySetup && BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
			BodySetup->InvalidatePhysicsData();
		BodySetup = InBodySetup;
		RefreshCollisionBodies();
		RebuildCollisionData(true);
		MarkPackageDirty();
		return true;
	}

	auto DStaticMesh::SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		require(Mode == EBodySetupCollisionSourceMode::None
			|| Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			|| Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		if (BodySetup && BodySetup->GetCollisionSourceMode() == Mode) return;
		if (!BodySetup && Mode == EBodySetupCollisionSourceMode::None) return;
		if (!BodySetup)
		{
			BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			require(BodySetup);
		}
		BodySetup->SetCollisionSourceMode(Mode);
	}

	auto DStaticMesh::SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		require(Policy == EBodySetupCollisionQueryPolicy::SimpleOnly
			|| Policy == EBodySetupCollisionQueryPolicy::ComplexOnly
			|| Policy == EBodySetupCollisionQueryPolicy::SimpleAndComplex);
		if (BodySetup && BodySetup->GetCollisionQueryPolicy() == Policy) return;
		if (!BodySetup)
		{
			BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			require(BodySetup);
		}
		BodySetup->SetCollisionQueryPolicy(Policy);
	}

	auto DStaticMesh::RefreshCollisionBodies() -> void
	{
		for (auto* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			if (auto* Component = Cast<DStaticMeshComponent>(Object);
				IsValid(Component) && Component->IsRegistered() && Component->GetStaticMesh() == this)
				Component->HandleStaticMeshCollisionDataChanged(this);
			else if (auto* Spline = Cast<DSplineMeshComponent>(Object);
				IsValid(Spline) && Spline->IsRegistered() && Spline->GetStaticMesh() == this)
				Spline->RecreatePhysicsState();
		}
	}

	auto DStaticMesh::NotifyCollisionSettingsChanged() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		ScheduleCollisionData(true);
	}

	auto DStaticMesh::RebuildCollision() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		RebuildCollisionData(false);
	}

	auto DStaticMesh::NotifyCollisionDataChanged() -> void { RefreshCollisionBodies(); }

	auto DStaticMesh::InvalidateCollisionData() -> void
	{
		if (BodySetup && BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
			BodySetup->InvalidatePhysicsData();
	}

	auto DStaticMesh::RebuildCollisionData(bool bAllowUnavailable, bool bPersistDerivedData) -> void
	{
		InvalidateCollisionData();
		ScheduleCollisionData(bAllowUnavailable, bPersistDerivedData);
	}

	auto DStaticMesh::ScheduleCollisionData(bool bAllowUnavailable, bool bPersistDerivedData) -> void
	{
		if (!BodySetup || BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None) return;
		if (bAllowUnavailable && !ContainsPhysicsTriMeshData()) return;
		BodySetup->CreatePhysicsMeshesAsync(*this, bPersistDerivedData);
	}

	auto DStaticMesh::GetCollisionBuildStatus() const -> EPhysicsMeshBuildStatus
	{
		return BodySetup ? BodySetup->GetPhysicsMeshBuildStatus() : EPhysicsMeshBuildStatus::Ready;
	}

	auto DStaticMesh::GetCollisionBuildError() const -> const std::optional<std::string>&
	{
		static const std::optional<std::string> NoError;
		return BodySetup ? BodySetup->GetPhysicsMeshBuildError() : NoError;
	}

	auto DStaticMesh::EnsureQualifiedBoxBodySetup() -> DBodySetup*
	{
		if (BodySetup) return BodySetup.Get();
		const std::string ObjectPath = GetObjectPath();
		if (!ObjectPath.starts_with("/Engine/Models/Box")) return nullptr;
		const std::optional<FBox> Bounds = GetLOD0VolumetricBounds();
		if (!Bounds || !Bounds->bIsValid || !Math::IsFinite(Bounds->Min) || !Math::IsFinite(Bounds->Max)) return nullptr;
		const FVector3 HalfExtent = Bounds->GetExtent();
		if (!FCollisionShape::MakeBox(HalfExtent).IsValid()) return nullptr;
		// This qualified primitive is reconstructed from accepted geometry, not an authored edit.
		FScopedPackageDirtySuppression SuppressDirty;
		auto* Setup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
		if (!Setup || !Setup->SetBox(HalfExtent, Bounds->GetCenter())) return nullptr;
		BodySetup = Setup;
		return BodySetup.Get();
	}

	auto DStaticMesh::RefreshQualifiedBoxBodySetup() -> void
	{
		if (!GetObjectPath().starts_with("/Engine/Models/Box")) return;
		if (!BodySetup)
		{
			if (EnsureQualifiedBoxBodySetup()) RefreshCollisionBodies();
			return;
		}
		const std::optional<FBox> Bounds = GetLOD0VolumetricBounds();
		if (Bounds && Bounds->bIsValid)
		{
			FScopedPackageDirtySuppression SuppressDirty;
			BodySetup->SetBox(Bounds->GetExtent(), Bounds->GetCenter());
		}
	}

}

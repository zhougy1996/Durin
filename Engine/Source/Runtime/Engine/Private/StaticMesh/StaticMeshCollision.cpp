#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "Asset/Asset.h"

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
		if (bHasComplexCollisionSource)
			return !ComplexCollisionPositions.empty() && !ComplexCollisionIndices.empty();
#if DURIN_WITH_EDITORONLY_DATA
		if (GetSource().IsValid()) return true;
#endif
		return RenderData && !RenderData->LODResources.empty();
	}

	auto DStaticMesh::GetComplexCollisionSourceTriangleCount() const -> std::optional<uint32>
	{
		if (bHasComplexCollisionSource)
			return static_cast<uint32>(ComplexCollisionIndices.size() / 3);
		if (!RenderData || RenderData->LODResources.empty()) return std::nullopt;
		const auto LOD = std::clamp(LODForCollision, 0, static_cast<int32>(RenderData->LODResources.size()) - 1);
		return static_cast<uint32>(RenderData->LODResources[LOD].IndexBuffer.GetIndices().size() / 3);
	}

#if DURIN_WITH_EDITORONLY_DATA
	namespace
	{
		class FStaticMeshPhysicsInputTask final : public FPhysicsMeshInputTask
		{
		public:
			FStaticMeshPhysicsInputTask(FStaticMeshSource InSource, FStaticMeshSource InNormalizationSource,
				float InSize, uint64 InBytes, uint32 InLODIndex)
				: Source(std::move(InSource)), NormalizationSource(std::move(InNormalizationSource)),
				  Size(InSize), Bytes(InBytes), LODIndex(InLODIndex)
			{ Source.ReleaseGeometry(); NormalizationSource.ReleaseGeometry(); }
			auto GetWorkingSetBytes() const -> uint64 override { return Bytes; }
			auto Execute(const FAssetBuildTaskContext& Control) const
				-> std::expected<FTriMeshCollisionData, std::string> override
			{
				if (Control.IsCancelled()) return {};
				if (Bytes > Control.MaximumWorkingSetBytes)
					return std::unexpected(MakeAssetBuildDiagnostic("Collision preparation exceeds the reservation.", "Input"));
				const auto Cancelled = [&] { return Control.IsCancelled(); };
				const auto Geometry = Source.AcquireLODGeometry(LODIndex, Cancelled);
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
				// Proxy bounds must never recenter or resize the primary mesh's coordinate system.
				if (NormalizationSource.IsValid() && (LODIndex != 0 || NormalizationSource.GetIdentity() != Source.GetIdentity()))
				{
					const auto Primary = NormalizationSource.AcquireGeometry(Cancelled);
					if (Cancelled()) return {};
					if (!Primary) return std::unexpected(FormatStaticMeshSourceError(Primary.error()));
					Bounds = {};
					for (const auto& Section : (*Primary)->Sections)
						for (const auto& Position : Section.Positions)
						{
							if ((++Work % 256) == 0 && Cancelled()) return {};
							Bounds.AddPoint(FVector3(Position));
						}
				}
				const auto Normalization = NormalizationSource.IsValid()
					? GetStaticMeshPositionNormalization(Bounds, Size)
					: std::optional<FStaticMeshPositionNormalization>{{FVector3f(0), 1.0f}};
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
			FStaticMeshSource NormalizationSource;
			float Size;
			uint64 Bytes;
			uint32 LODIndex;
		};
	}

#endif

	auto DStaticMesh::CreatePhysicsMeshInputTask() const
		-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string>
	{
		// A proxy replaces complex triangles only, never convex/simple source geometry.
		if (BodySetup && BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
			return CreateOwnPhysicsMeshInputTask(*this);
		if (bHasComplexCollisionSource)
			return IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask();
		return CreateOwnPhysicsMeshInputTask(*this, LODForCollision);
	}

	auto DStaticMesh::CreateOwnPhysicsMeshInputTask(const DStaticMesh& NormalizationOwner, int32 LODIndex) const
		-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string>
	{
#if DURIN_WITH_EDITORONLY_DATA
		const auto& Source = GetSource();
		if (Source.IsValid())
		{
			FAssetBuildMemoryEstimate Memory{MaximumAssetBuildRequestBytes, 1024 * 1024};
			if (!Memory.Add(Source.GetGeometryBulk().GetPayloadSize(), 64) || !Memory.Add(Source.GetMeshCount(), 1024))
				return std::unexpected(MakeAssetBuildDiagnostic("Collision source exceeds its working-set budget.", "Input"));
			const auto& Primary = NormalizationOwner.GetSource();
			if (Primary.IsValid() && Primary.GetIdentity() != Source.GetIdentity()
				&& !Memory.Add(Primary.GetGeometryBulk().GetPayloadSize(), 64))
				return std::unexpected("Collision normalization source exceeds its working-set budget.");
			const auto SelectedLOD = static_cast<uint32>(std::clamp(LODIndex, 0, static_cast<int32>(Source.GetNumLODs()) - 1));
			return std::make_unique<FStaticMeshPhysicsInputTask>(Source, Primary, NormalizationOwner.NormalizedSize, Memory.Bytes, SelectedLOD);
		}
#endif
		{
			// Procedural render vertices are already in canonical local space. Bypass
			// the complex provider here so a proxy cannot recursively select another proxy.
			class FProjectionProvider final : public IInterface_CollisionDataProvider
			{
			public:
				FTriMeshCollisionData Data;
				auto ContainsPhysicsTriMeshData() const -> bool override { return !Data.Indices.empty(); }
				auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override { return Data; }
			} Provider;
			if (!RenderData || RenderData->LODResources.empty())
				return std::unexpected("No collision source is available.");
			const auto& LOD = RenderData->LODResources[std::clamp(LODIndex, 0, static_cast<int32>(RenderData->LODResources.size()) - 1)];
			const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			Provider.Data = {{Positions.begin(), Positions.end()}, {Indices.begin(), Indices.end()}};
			return Provider.CreatePhysicsMeshInputTask();
		}
	}

	auto DStaticMesh::GetPhysicsTriMeshData() const
		-> std::expected<FTriMeshCollisionData, std::string>
	{
		if (bHasComplexCollisionSource)
		{
			if (ComplexCollisionPositions.empty() || ComplexCollisionIndices.empty())
				return std::unexpected("The selected complex collision mesh has no usable input.");
			return FTriMeshCollisionData{ComplexCollisionPositions, ComplexCollisionIndices};
		}
#if DURIN_WITH_EDITORONLY_DATA
		const auto& Source = GetSource();
		if (Source.IsValid())
		{
			auto Task = CreateOwnPhysicsMeshInputTask(*this, LODForCollision);
			if (!Task) return std::unexpected(Task.error());
			return (*Task)->Execute({.MaximumWorkingSetBytes = (*Task)->GetWorkingSetBytes()});
		}
#endif

		if (RenderData && !RenderData->LODResources.empty())
		{
			const auto& LOD = RenderData->LODResources[std::clamp(LODForCollision, 0, static_cast<int32>(RenderData->LODResources.size()) - 1)];
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

	auto DStaticMesh::GetComplexCollisionMesh() const -> DStaticMesh*
	{
#if DURIN_WITH_EDITORONLY_DATA
		return ComplexCollisionMesh.Get();
#else
		return nullptr;
#endif
	}

	auto DStaticMesh::SetComplexCollisionMesh(DStaticMesh* Mesh) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
#if DURIN_WITH_EDITORONLY_DATA
		if (Mesh == this || (Mesh && !IsValid(Mesh))) return false;
		if (ComplexCollisionMesh == Mesh) return true;
		ComplexCollisionMesh = Mesh;
		NotifyCollisionSourceChanged(true);
		MarkPackageDirty();
		return true;
#else
		return false;
#endif
	}

	auto DStaticMesh::SetLODForCollision(int32 LODIndex) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		if (LODForCollision == LODIndex) return;
		LODForCollision = LODIndex;
		NotifyCollisionSourceChanged(true);
		MarkPackageDirty();
	}

	auto DStaticMesh::RefreshComplexCollisionSource() -> void
	{
#if DURIN_WITH_EDITORONLY_DATA
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		ComplexCollisionPositions.clear();
		ComplexCollisionIndices.clear();
		bHasComplexCollisionSource = ComplexCollisionMesh != nullptr || LODForCollision != 0;
		if (!bHasComplexCollisionSource) return;
		auto* Selected = ComplexCollisionMesh ? ComplexCollisionMesh.Get() : this;
		if (ComplexCollisionMesh == this || !IsValid(Selected)) return;
		auto Task = Selected->CreateOwnPhysicsMeshInputTask(*this, Selected->LODForCollision);
		if (!Task) { DURIN_ERROR("Complex collision '{}': {}", GetObjectPath(), Task.error()); return; }
		auto Input = (*Task)->Execute({.MaximumWorkingSetBytes = (*Task)->GetWorkingSetBytes()});
		if (!Input) { DURIN_ERROR("Complex collision '{}': {}", GetObjectPath(), Input.error()); return; }
		ComplexCollisionPositions = std::move(Input->Positions);
		ComplexCollisionIndices = std::move(Input->Indices);
#endif
	}

	auto DStaticMesh::NotifyCollisionSourceChanged(bool bRebuildComplexBody) -> void
	{
		if (bRefreshingCollisionSource) return;
		bRefreshingCollisionSource = true;
		RefreshComplexCollisionSource();
		++CollisionSourceRevision;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			bRefreshingCollisionSource = false;
			return;
		}
		// Body completion notifications only refresh bodies; they must never feed back
		// into source invalidation or recursively rebuild proxy dependency chains.
		if (bRebuildComplexBody && BodySetup
			&& BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			RebuildCollisionData(true);
		}
		const auto Objects = GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
		std::vector<DStaticMesh*> ChangedMeshes{this};
		for (auto* Object : Objects)
		{
#if DURIN_WITH_EDITORONLY_DATA
			if (auto* Dependent = Cast<DStaticMesh>(Object); IsValid(Dependent)
				&& Dependent != this && Dependent->ComplexCollisionMesh == this)
			{
				Dependent->RefreshComplexCollisionSource();
				++Dependent->CollisionSourceRevision;
				ChangedMeshes.push_back(Dependent);
				if (Dependent->BodySetup && Dependent->BodySetup->GetCollisionSourceMode()
					== EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
				{
					Dependent->RebuildCollisionData(true);
				}
			}
#endif
		}
		for (auto* Object : Objects)
			if (auto* Spline = Cast<DSplineMeshComponent>(Object); IsValid(Spline)
				&& std::ranges::find(ChangedMeshes, Spline->GetStaticMesh()) != ChangedMeshes.end())
				Spline->HandleStaticMeshCollisionSourceChanged(Spline->GetStaticMesh(), EBodySetupChange::GeneratedGeometry);
		bRefreshingCollisionSource = false;
	}

	auto DStaticMesh::SetBodySetup(DBodySetup* InBodySetup) -> bool
	{
		if (InBodySetup && InBodySetup->GetOuter() != this) return false;
		if (BodySetup == InBodySetup) return true;
		auto* Previous = BodySetup.Get();
		BodySetup = InBodySetup;
		// Detach first so retiring the old owner's work cannot refresh this mesh.
		if (Previous) Previous->InvalidatePhysicsData();
		if (BodySetup)
		{
			const FScopedBodySetupUpdate Update(*BodySetup);
			InvalidateCollisionData();
			BodySetup->MarkChanged(EBodySetupChange::SimpleGeometry | EBodySetupChange::QuerySettings);
			ScheduleCollisionData(true);
		}
		else RefreshCollisionBodies();
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

	auto DStaticMesh::RefreshCollisionBodies(EBodySetupChange Changes) -> void
	{
		// One traversal for final body publication and dependent spline input changes.
		for (auto* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			if (auto* Component = Cast<DStaticMeshComponent>(Object);
				IsValid(Component) && Component->IsRegistered() && Component->GetStaticMesh() == this)
				Component->HandleStaticMeshCollisionDataChanged(this);
			else if (auto* Spline = Cast<DSplineMeshComponent>(Object); IsValid(Spline) && Spline->GetStaticMesh() == this)
			{
				const auto Mode = Spline->GetSplineMeshCollisionMode();
				if (Mode != ESplineMeshCollisionMode::DeformedSimpleCollision
					&& Mode != ESplineMeshCollisionMode::DeformedSimpleAndComplex) continue;
				const auto* Applied = Spline->GetBodySetup();
				const auto TraceMode = BodySetup ? BodySetup->GetCollisionTraceMode() : EBodySetupCollisionTraceMode::SimpleAndComplex;
				const bool bTraceChanged = !Applied || Applied->GetCollisionTraceMode() != TraceMode;
				const bool bGeneratedSimpleChanged = EnumHasAnyFlags(Changes, EBodySetupChange::GeneratedGeometry)
					&& BodySetup && BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					&& BodySetup->GetSimpleCollision().empty();
				if (EnumHasAnyFlags(Changes, EBodySetupChange::SimpleGeometry | EBodySetupChange::SourceRecipe)
					|| bGeneratedSimpleChanged || bTraceChanged)
				{
					const auto Inputs = EnumHasAnyFlags(Changes, EBodySetupChange::SimpleGeometry | EBodySetupChange::SourceRecipe)
						|| bGeneratedSimpleChanged ? EBodySetupChange::SimpleGeometry : EBodySetupChange::None;
					Spline->HandleStaticMeshCollisionSourceChanged(this, Inputs | EBodySetupChange::QuerySettings);
				}
			}
		}
	}

	auto DStaticMesh::HandleBodySetupChanged(EBodySetupChange Changes) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (EnumHasAnyFlags(Changes, EBodySetupChange::SourceRecipe)
			&& BodySetup && BodySetup->bPhysicsDataInvalidated) ScheduleCollisionData(true);
		RefreshCollisionBodies(Changes);
	}

	auto DStaticMesh::RebuildCollision() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		// Retry acquisition as well as cooking; a previously unavailable proxy must
		// not remain trapped behind its last empty resolved snapshot.
		if (bHasComplexCollisionSource && !GetAssetRuntimeConfiguration().RequiresCookedPayload())
			NotifyCollisionSourceChanged();
		RebuildCollisionData(false);
	}

	auto DStaticMesh::InvalidateCollisionData() -> void
	{
		if (BodySetup && BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
			BodySetup->InvalidatePhysicsData();
	}

	auto DStaticMesh::RebuildCollisionData(bool bAllowUnavailable, bool bPersistDerivedData) -> void
	{
		if (!BodySetup) return;
		const FScopedBodySetupUpdate Update(*BodySetup);
		InvalidateCollisionData();
		ScheduleCollisionData(bAllowUnavailable, bPersistDerivedData);
	}

	auto DStaticMesh::ScheduleCollisionData(bool bAllowUnavailable, bool bPersistDerivedData) -> void
	{
		if (!BodySetup || BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None) return;
		if (bAllowUnavailable && !bHasComplexCollisionSource && !ContainsPhysicsTriMeshData()) return;
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

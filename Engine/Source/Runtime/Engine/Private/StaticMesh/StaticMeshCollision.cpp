#include "StaticMesh/StaticMesh.h"

#include "Math/Operations.h"
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "Threading/RunnableThread.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

namespace Durin
{
	auto FormatStaticMeshCollisionError(const FStaticMeshCollisionError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshCollisionError::None: return {};
		case EStaticMeshCollisionError::MissingRenderData: return "Static-mesh collision build requires published CPU render data.";
		case EStaticMeshCollisionError::DerivedData: return Error.DerivedDataCause ? Error.DerivedDataCause->ToString() : "StaticMesh collision build failed.";
		case EStaticMeshCollisionError::Publication: return "Static mesh could not publish collision geometry.";
		}
		return {};
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
		CollisionBuildError = {};
		NotifyStaticMeshCompilationMutation(*this);
		MarkPackageDirty();
		return true;
	}

	auto DStaticMesh::BuildCollisionCandidate(
		const FStaticMeshRenderData& SourceRenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FCollisionGeometryRef& OutSimple,
		FCollisionGeometryRef& OutComplex) const -> std::expected<void, FStaticMeshDerivedDataError>
	{
		auto Built = FStaticMeshBuilder::BuildCollision(SourceRenderData, Mode, Policy);
		if (!Built) return std::unexpected(std::move(Built.error()));
		OutSimple = std::move(Built->Simple);
		OutComplex = std::move(Built->Complex);
		return {};
	}
	auto DStaticMesh::SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		require(Mode == EBodySetupCollisionSourceMode::None
			|| Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			|| Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		if (BodySetup && BodySetup->GetCollisionSourceMode() == Mode) return;
		if (!BodySetup && Mode == EBodySetupCollisionSourceMode::None) return;
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		if (!BodySetup)
		{
			BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			require(BodySetup);
			NotifyStaticMeshCompilationMutation(*this);
		}
		BodySetup->SetCollisionSourceMode(Mode);
		RebuildCollisionData(true);
	}

	auto DStaticMesh::SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		require(Policy == EBodySetupCollisionQueryPolicy::SimpleOnly
			|| Policy == EBodySetupCollisionQueryPolicy::ComplexOnly
			|| Policy == EBodySetupCollisionQueryPolicy::SimpleAndComplex);
		if (BodySetup && BodySetup->GetCollisionQueryPolicy() == Policy) return;
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		if (!BodySetup)
		{
			BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			require(BodySetup);
			NotifyStaticMeshCompilationMutation(*this);
		}
		BodySetup->SetCollisionQueryPolicy(Policy);
		RebuildCollisionData(true);
	}

	auto DStaticMesh::RebuildCollision() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		RebuildCollisionData(false);
	}

	auto DStaticMesh::RebuildCollisionData(bool bAllowUnavailable) -> void
	{
		CollisionBuildError = {};
		if (!BodySetup) return;
		BodySetup->ClearCollisionGeometry();
		const auto Mode = BodySetup->GetCollisionSourceMode();
		if (Mode == EBodySetupCollisionSourceMode::None) return;
		if (!RenderData)
		{
			if (bAllowUnavailable || HasPendingStaticMeshCompilation(*this)) return;
			CollisionBuildError = {.Code = EStaticMeshCollisionError::MissingRenderData, .Mode = Mode, .Policy = BodySetup->GetCollisionQueryPolicy()};
		}
		else
		{
			FCollisionGeometryRef Simple;
			FCollisionGeometryRef Complex;
			if (const auto Built = BuildCollisionCandidate(*RenderData, Mode, BodySetup->GetCollisionQueryPolicy(),
				Simple, Complex); Built)
			{
				if (BodySetup->SetCollisionGeometry(Simple, Complex)) return;
				CollisionBuildError = {.Code = EStaticMeshCollisionError::Publication, .Mode = Mode, .Policy = BodySetup->GetCollisionQueryPolicy()};
			}
			else CollisionBuildError = {.Code = EStaticMeshCollisionError::DerivedData, .Mode = Mode,
				.Policy = BodySetup->GetCollisionQueryPolicy(), .DerivedDataCause = std::make_shared<FStaticMeshDerivedDataError>(Built.error())};
		}
		DURIN_ERROR("Static mesh '{}' collision build failed: {}", GetObjectPath(), FormatStaticMeshCollisionError(CollisionBuildError));
	}

	auto DStaticMesh::GetCollisionBuildStatus() const -> EStaticMeshCollisionBuildStatus
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		// Published geometry is authoritative, including cooked and async publication.
		if (!BodySetup || BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None)
			return EStaticMeshCollisionBuildStatus::Ready;
		if (BodySetup->GetResidentSimpleGeometry() || BodySetup->GetResidentComplexGeometry())
			return EStaticMeshCollisionBuildStatus::Ready;
		if (HasPendingStaticMeshCompilation(*this)) return EStaticMeshCollisionBuildStatus::Pending;
		if (CollisionBuildError.Code != EStaticMeshCollisionError::None) return EStaticMeshCollisionBuildStatus::Failed;
		return EStaticMeshCollisionBuildStatus::Unavailable;
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
		auto* Setup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
		if (!Setup || !Setup->SetBox(HalfExtent, Bounds->GetCenter())) return nullptr;
		BodySetup = Setup;
		return BodySetup.Get();
	}

	auto DStaticMesh::RefreshQualifiedBoxBodySetup() -> void
	{
		if (!BodySetup || !GetObjectPath().starts_with("/Engine/Models/Box")) return;
		const std::optional<FBox> Bounds = GetLOD0VolumetricBounds();
		if (Bounds && Bounds->bIsValid) BodySetup->SetBox(Bounds->GetExtent(), Bounds->GetCenter());
	}

}

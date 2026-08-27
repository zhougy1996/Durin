#include "StaticMesh/StaticMesh.h"

#include "Math/Operations.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

namespace Durin
{
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
		FStaticMeshSourceImportData SourceImportData;
		const DAssetImportData* ImportData = GetAssetImportData();
		if (const FSourceFile* Source = ImportData
				? ImportData->GetSourceData().FindByRole("source") : nullptr)
		{
			SourceImportData.SourceFilename = Source->Hint;
			SourceImportData.SourceContentHash = Source->GetContentHash().ToString();
		}
		FStaticMeshCollisionBuildProduct Product;
		if (!InvokeStaticMeshCollisionBuildFeature(
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
			OutError = "Static-mesh collision build requires published CPU render data.";
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

}

#include "StaticMesh/StaticMeshBuild.h"

#include "Asset/Asset.h"

namespace Durin
{
	auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot
	{
		return {.MaterialSlots = std::vector<FMeshMaterialSlotDefinition>(
				Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()),
			.NormalizedSize = Mesh.GetNormalizedSize()};
	}

	auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshImportedData Source, FStaticMeshBuildResult Product, std::string& OutError,
		bool bMarkPackageDirty) -> bool
	{
		if (!Mesh.SetImportedRenderData(std::move(Source),
			std::move(Product.RenderData), std::move(Product.MaterialSlots),
			Product.NormalizedSize, OutError)) return false;
		if (Product.bSlotMetadataChanged)
		{
			ReportAssetLoadMutation(&Mesh, "Engine.StaticMesh.MaterialSlotsV1",
				"Static mesh material-slot identity metadata was upgraded.",
				EAssetLoadMutationKind::Upgrade);
		}
		if (bMarkPackageDirty || Product.bSlotMetadataChanged) Mesh.MarkPackageDirty();
		return true;
	}

	auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		const FStaticMeshImportedData& ImportedData,
		std::string& OutError) -> bool
	{
		FStaticMeshBuildResult Product;
		return BuildStaticMeshDerivedData({.Reconciliation = CaptureStaticMeshReconciliation(Mesh),
			.ImportedData = ImportedData}, Product, OutError)
			&& ApplyStaticMeshBuildResult(Mesh, ImportedData, std::move(Product), OutError);
	}

	auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		FStaticMeshDecodedGeometry Geometry, std::string& OutError) -> bool
	{
		FStaticMeshImportedData Source;
		return Source.Initialize(std::move(Geometry), OutError)
			&& BuildStaticMeshSynchronously(Mesh, Source, OutError);
	}
}

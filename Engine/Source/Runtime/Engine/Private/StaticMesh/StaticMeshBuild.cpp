#include "StaticMesh/StaticMeshBuild.h"

#include "Asset/Asset.h"

namespace Durin
{
	auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot
	{
		return {.MaterialSlots = std::vector<FMeshMaterialSlotDefinition>(
				Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()),
			.NormalizedSize = Mesh.GetNormalizedSize(),
			.StableObjectPath = Mesh.GetObjectPath()};
	}

	auto BuildStaticMeshImportedData(
		const FStaticMeshReconciliationSnapshot& Reconciliation,
		const FStaticMeshImportedData& ImportedData,
		std::string_view SourceLabel,
		FStaticMeshBuildResult& OutProduct,
		std::string& OutError) -> bool
	{
		if (!BuildStaticMeshDerivedData({.Reconciliation = Reconciliation,
			.ImportedData = ImportedData, .SourceLabel = std::string(SourceLabel)},
			OutProduct, OutError)) return false;
		return true;
	}

	auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshBuildResult Product, std::string& OutError,
		bool bMarkPackageDirty) -> bool
	{
		if (!Mesh.SetImportedRenderData(std::move(Product.ImportedData),
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
		std::string_view SourceLabel, std::string& OutError) -> bool
	{
		FStaticMeshBuildResult Product;
		return BuildStaticMeshImportedData(CaptureStaticMeshReconciliation(Mesh),
			ImportedData, SourceLabel, Product, OutError)
			&& ApplyStaticMeshBuildResult(Mesh, std::move(Product), OutError);
	}
}

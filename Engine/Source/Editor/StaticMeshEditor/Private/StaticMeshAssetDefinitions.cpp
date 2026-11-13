#include "StaticMeshAssetDefinitions.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"
#include "Widgets/MStaticMeshInspector.h"
#include "StaticMesh/StaticMesh.h"
#include "Workspace/StaticMeshEditorWorkspace.h"

namespace Durin::StaticMeshEditor
{
	auto DStaticMeshAssetDefinition::GetAssetIcon() const -> std::string { return Icons::Cube; }
	DStaticMeshAssetDefinition::DStaticMeshAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DStaticMeshAssetDefinition::GetAssetClassPath() const -> std::string { return DStaticMesh::StaticClass()->GetQualifiedName().ToString(); }
	auto DStaticMeshAssetDefinition::OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult
	{
		return OpenAssetsWithToolkit(Args, Workspace::Type,
			[](FEditorDocumentManager& Documents) { return std::make_shared<MStaticMeshInspector>(Documents); });
	}
}

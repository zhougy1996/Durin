#include "StaticMeshAssetDefinitions.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"
#include "Widgets/MStaticMeshInspector.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::StaticMesh
{
	auto DStaticMeshAssetDefinition::GetAssetIcon() const -> std::string { return Icons::Cube; }
	DStaticMeshAssetEditor::DStaticMeshAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DStaticMeshAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<MStaticMeshInspector>(*GetDocumentManager()); }
	DStaticMeshAssetDefinition::DStaticMeshAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DStaticMeshAssetDefinition::GetAssetClassPath() const -> std::string { return DStaticMesh::StaticClass()->GetQualifiedName().ToString(); }
	auto DStaticMeshAssetDefinition::GetAssetEditorClass() const -> DClass* { return DStaticMeshAssetEditor::StaticClass(); }
}

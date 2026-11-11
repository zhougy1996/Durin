#include "MaterialAssetDefinitions.h"
#include "DObject/Class.h"
#include "Widgets/MMaterialEditor.h"
#include "Widgets/MMaterialFunctionEditor.h"
#include "Widgets/MMaterialParameterCollectionEditor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"

namespace Durin::MaterialEditor
{
	DMaterialAssetEditor::DMaterialAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<MMaterialEditor>(*GetDocumentManager()); }
	DMaterialFunctionAssetEditor::DMaterialFunctionAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialFunctionAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<MMaterialFunctionEditor>(*GetDocumentManager()); }
	DMaterialParameterCollectionAssetEditor::DMaterialParameterCollectionAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialParameterCollectionAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<MMaterialParameterCollectionEditor>(*GetDocumentManager()); }
	DMaterialAssetDefinition::DMaterialAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialAssetDefinition::GetAssetClassPath() const -> std::string { return DMaterial::StaticClass()->GetQualifiedName().ToString(); }
	auto DMaterialAssetDefinition::GetAssetEditorClass() const -> DClass* { return DMaterialAssetEditor::StaticClass(); }
	DMaterialInstanceAssetDefinition::DMaterialInstanceAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialInstanceAssetDefinition::GetAssetClassPath() const -> std::string { return DMaterialInstance::StaticClass()->GetQualifiedName().ToString(); }
	auto DMaterialInstanceAssetDefinition::GetAssetEditorClass() const -> DClass* { return DMaterialAssetEditor::StaticClass(); }
	DMaterialFunctionAssetDefinition::DMaterialFunctionAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialFunctionAssetDefinition::GetAssetClassPath() const -> std::string { return DMaterialFunction::StaticClass()->GetQualifiedName().ToString(); }
	auto DMaterialFunctionAssetDefinition::GetAssetEditorClass() const -> DClass* { return DMaterialFunctionAssetEditor::StaticClass(); }
	DMaterialParameterCollectionAssetDefinition::DMaterialParameterCollectionAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMaterialParameterCollectionAssetDefinition::GetAssetClassPath() const -> std::string { return DMaterialParameterCollection::StaticClass()->GetQualifiedName().ToString(); }
	auto DMaterialParameterCollectionAssetDefinition::GetAssetEditorClass() const -> DClass* { return DMaterialParameterCollectionAssetEditor::StaticClass(); }
}

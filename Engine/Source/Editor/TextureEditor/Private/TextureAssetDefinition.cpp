#include "TextureAssetDefinition.h"
#include "Texture/Texture2D.h"

#include "Widgets/MTextureEditor.h"
#include "Widgets/MVolumeTextureEditor.h"
#include "Texture/VolumeTexture.h"
#include "Texture/TextureCube.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"

namespace Durin::TextureEditor
{
	DTextureCubeAssetDefinition::DTextureCubeAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DTextureCubeAssetDefinition::GetAssetClassPath() const -> std::string { return DTextureCube::StaticClass()->GetQualifiedName().ToString(); }
	auto DTextureCubeAssetDefinition::GetAssetIcon() const -> std::string { return Icons::Cube; }
	auto DVolumeTextureAssetDefinition::GetAssetIcon() const -> std::string { return Icons::Cube; }
	DTextureAssetEditor::DTextureAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DTextureAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{
		return std::make_shared<MTextureEditor>(*GetDocumentManager());
	}
	DTextureAssetDefinition::DTextureAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DTextureAssetDefinition::GetAssetClassPath() const -> std::string
	{
		return DTexture2D::StaticClass()->GetQualifiedName().ToString();
	}
	auto DTextureAssetDefinition::GetAssetEditorClass() const -> DClass* { return DTextureAssetEditor::StaticClass(); }
	DVolumeTextureAssetEditor::DVolumeTextureAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DVolumeTextureAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<MVolumeTextureEditor>(*GetDocumentManager()); }
	DVolumeTextureAssetDefinition::DVolumeTextureAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DVolumeTextureAssetDefinition::GetAssetClassPath() const -> std::string { return DVolumeTexture::StaticClass()->GetQualifiedName().ToString(); }
	auto DVolumeTextureAssetDefinition::GetAssetEditorClass() const -> DClass* { return DVolumeTextureAssetEditor::StaticClass(); }
}

#pragma once

#include "CoreMinimal.h"

// Public entry point for common editor consumers and shared PCHs. Internal
// headers retain their own declaring-header dependencies.
#include "Engine.h"

// Public rendering interfaces used by editor viewports and previews.
#include "RenderCore.h"
#include "IRendererModule.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

// Common material and texture types.
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Texture/TextureCube.h"

// Editor services, property UI, documents and transactions.
#include "MonaImGui.h"
#include "Toolkits/AssetEditorTypes.h"
#include "Editor/EditorEngine.h"
#include "Factories/Factory.h"
#include "Import/EditorReimportHandler.h"
#include "Notifications/Notification.h"
#include "PropertyEditor/PropertyEditing.h"
#include "PropertyEditor/PropertyView.h"
#include "Settings/EditorSettings.h"
#include "Transactions/Transaction.h"
#include "Transactions/TransactionObjectRecord.h"
#include "Transactions/Transactor.h"

// Thumbnail services retained for existing DurinEd.h consumers.
#include "Thumbnail/AssetThumbnailTypes.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/AssetThumbnail.h"

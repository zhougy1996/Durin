#pragma once

#include "SharedPCH_Engine.h"

// Public rendering interfaces used by editor viewports and previews.
#include "RenderCore.h"
#include "IRendererModule.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

// Common material and texture types, including intentional transitive coverage.
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Texture/TextureCube.h"

// Editor services, property UI, documents and transactions.
#include "MonaImGui.h"
#include "Documents/EditorDocumentTypes.h"
#include "Editor/EditorEngine.h"
#include "Factories/Factory.h"
#include "Import/EditorReimportHandler.h"
#include "Notifications/Notification.h"
#include "PropertyEditor/PropertyView.h"
#include "Settings/EditorSettings.h"
#include "Transactions/Transaction.h"
#include "Transactions/TransactionObjectRecord.h"

// Keep editor services here: DurinEd itself consumes SharedPCH_Engine with its
// export definitions. Material types cover repeated parsing in material editing
// and preview consumers without adding graph-editing implementation headers.

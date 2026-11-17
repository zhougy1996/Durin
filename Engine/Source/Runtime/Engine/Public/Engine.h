#pragma once

#include "CoreMinimal.h"

// Public entry point for common Engine consumers and shared PCHs. Internal
// headers retain their own declaring-header dependencies.

// Common object and asset access.
#include "Asset/Asset.h"
#include "DObject/Package.h"

// Engine lifecycle, actors, components and world ownership.
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"

// Backend-independent rendering infrastructure and texture types.
#include "RHI.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

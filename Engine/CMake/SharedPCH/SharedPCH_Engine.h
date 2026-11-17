#pragma once

#include "SharedPCH_DObject.h"

// Common object and asset access.
#include "Asset/Asset.h"
#include "DObject/Package.h"

// Actors, components and world ownership.
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

// Backend-independent rendering infrastructure and texture types.
#include "RHI.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"

// Keep backend-specific headers and editor services in their owning PCHs.

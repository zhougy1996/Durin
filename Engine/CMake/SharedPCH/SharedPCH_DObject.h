#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectMacros.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/GarbageCollectionScheduler.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/AssetPath.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/MathStructs.h"

// Package.h remains outside the shared PCH because it consumes DHT-generated Package.gen.h.

#pragma once

#include "Actors/CameraActor.h"
#include "Actors/Controller.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/GameMode.h"
#include "Actors/Pawn.h"
#include "Actors/PlayerController.h"
#include "Actors/PlayerStart.h"
#include "AssetSystem.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/SkeletalMeshActor.h"
#include "Components/ActorComponent.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PhysicsComponent.h"
#include "Components/PawnMovementComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/EditorTransaction.h"
#include "Editor/ReflectedPropertyView.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "IScene.h"
#include "Math/Color.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>

namespace
{
	auto CreateEmptyWorld(Durin::DObject* Outer = nullptr) -> Durin::DWorld*
	{
		InitializeDObjectSystem();
		return Durin::NewObject<Durin::DWorld>(Outer, "TestWorld");
	}

	auto CreateWorld(Durin::DObject* Outer = nullptr) -> Durin::DWorld*
	{
		Durin::DWorld* World = CreateEmptyWorld(Outer);
		EXPECT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "TestLevel")));
		return World;
	}

	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}
} // namespace

#pragma once
#include "CoreMinimal.h"
#include "PluginFixtureRuntimeAPI.h"
#include "Engine/PrimaryDataAsset.h"
#include "DObject/ObjectPtr.h"
#include "Components/ActorComponent.h"
#include "FixtureAsset.gen.h"
namespace Durin::PluginFixture
{
 DCLASS()
 class DFixtureComponent : public DActorComponent
 {
  GENERATED_BODY()
 public:
  PLUGINFIXTURERUNTIME_API explicit DFixtureComponent(const FObjectInitializer& Initializer)
   : DActorComponent(Initializer) {}
  DPROPERTY()
  int32 Sample = 42;
 };
 DCLASS()
 class DFixtureAsset : public DPrimaryDataAsset
 {
  GENERATED_BODY()
 public:
  PLUGINFIXTURERUNTIME_API explicit DFixtureAsset(const FObjectInitializer& Initializer);
  DPROPERTY()
  int32 Value = 42;
  DPROPERTY()
  Durin::TObjectPtr<Durin::DObject> Reference;
 };
 PLUGINFIXTURERUNTIME_API auto FixtureValue() -> int32;
}

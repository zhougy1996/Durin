#pragma once
#include "CoreMinimal.h"
#include "PluginFixtureRuntimeAPI.h"
#include "Engine/PrimaryDataAsset.h"
#include "FixtureAsset.gen.h"
namespace Durin::PluginFixture
{
 DCLASS()
 class DFixtureAsset : public DPrimaryDataAsset
 {
  GENERATED_BODY()
 public:
  PLUGINFIXTURERUNTIME_API explicit DFixtureAsset(const FObjectInitializer& Initializer);
  DPROPERTY()
  int32 Value = 42;
 };
 PLUGINFIXTURERUNTIME_API auto FixtureValue() -> int32;
}

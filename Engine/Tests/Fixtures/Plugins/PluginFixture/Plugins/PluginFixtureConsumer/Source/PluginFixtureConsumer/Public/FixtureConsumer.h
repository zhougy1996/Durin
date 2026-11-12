#pragma once
#include "CoreMinimal.h"
#include "PluginFixtureConsumerAPI.h"
#include "FixtureAsset.h"
#include "FixtureConsumer.gen.h"
namespace Durin::PluginFixture
{
 DCLASS()
 class DConsumerAsset : public DFixtureAsset
 {
  GENERATED_BODY()
 public:
  PLUGINFIXTURECONSUMER_API explicit DConsumerAsset(const FObjectInitializer& Initializer);
 };
 PLUGINFIXTURECONSUMER_API auto ReadFixtureValue() -> int32;
}

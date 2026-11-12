#include "FixtureConsumer.h"
namespace Durin::PluginFixture
{
 DConsumerAsset::DConsumerAsset(const FObjectInitializer& Initializer) : DFixtureAsset(Initializer) {}
 auto ReadFixtureValue() -> int32 { return FixtureValue(); }
}

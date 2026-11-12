#include "FixtureAsset.h"
namespace Durin::PluginFixture
{
 DFixtureAsset::DFixtureAsset(const FObjectInitializer& Initializer) : DPrimaryDataAsset(Initializer) {}
 auto FixtureValue() -> int32 { return 42; }
}

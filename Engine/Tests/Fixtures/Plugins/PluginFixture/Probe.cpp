#include "FixtureConsumer.h"
#include "DObject/Class.h"
int main() { return Durin::PluginFixture::DConsumerAsset::StaticClass() && Durin::PluginFixture::ReadFixtureValue() == 42 ? 0 : 1; }

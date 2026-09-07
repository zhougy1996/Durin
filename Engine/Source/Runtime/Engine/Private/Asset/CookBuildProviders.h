#pragma once
#include "Serialization/SharedByteBuffer.h"
namespace Durin::AssetPrivate
{
	// Reads the native recipe identity before cache lookup. Host owns provider lifetime.
	auto GetCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool;
}

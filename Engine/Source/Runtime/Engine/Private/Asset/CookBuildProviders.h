#pragma once

#include "Serialization/SharedByteBuffer.h"

namespace Durin::AssetPrivate
{
	// Retains each available native recipe provider invocation until Work returns.
	auto WithCapturedCookBuildProviders(const std::function<bool()>& Work, std::string& Error) -> bool;
	auto VerifyCapturedCookBuildProviders() -> bool;
	auto GetCapturedCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool;
}

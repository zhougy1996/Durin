#pragma once

namespace Durin::StandardAssetImport
{
	auto RegisterStage3PostLoadPolicies() -> bool;
	auto UnregisterStage3PostLoadPolicies() -> void;
}

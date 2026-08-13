#pragma once

namespace Durin::Asset::Import
{
	auto RegisterStage3PostLoadPolicies() -> bool;
	auto UnregisterStage3PostLoadPolicies() -> void;
}

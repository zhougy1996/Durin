#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	class FGenericApplication;

	extern APPLICATIONCORE_API std::shared_ptr<FGenericApplication> GApp;

	APPLICATIONCORE_API auto InitializeApplicationCore() -> bool;

	APPLICATIONCORE_API auto IsApplicationCoreInitialized() -> bool;

	APPLICATIONCORE_API auto ShutdownApplicationCore() -> void;
}

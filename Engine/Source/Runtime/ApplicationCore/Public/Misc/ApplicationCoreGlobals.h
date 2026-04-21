#pragma once

#include "ApplicationCoreAPI.h"

namespace Doge
{
	class FGenericApplication;

	extern APPLICATIONCORE_API std::shared_ptr<FGenericApplication> GApp;

	APPLICATIONCORE_API auto ApplicationInit() -> void;
}
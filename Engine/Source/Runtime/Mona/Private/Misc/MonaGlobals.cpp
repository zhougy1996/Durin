#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaBackendGlobals.h"

namespace Doge
{
	auto MonaInit() -> void
	{
		Mona::BackendInit();
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();
	}
}
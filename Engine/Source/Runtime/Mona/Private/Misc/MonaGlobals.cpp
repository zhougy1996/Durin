#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaBackendGlobals.h"

namespace Doge::Mona
{
	auto MonaInit() -> void
	{
		BackendInit();
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();

		InitMonaBackendEventHandler();
	}
}
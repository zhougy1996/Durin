#include "MonaCoreGlobals.h"

namespace Durin::Mona
{
	namespace
	{
		IMonaUIBackend* GActiveUIBackend = nullptr;
	}

	auto RegisterUIBackend(IMonaUIBackend& Backend) -> bool
	{
		if (GActiveUIBackend != nullptr) return false;
		GActiveUIBackend = &Backend;
		return true;
	}

	auto UnregisterUIBackend(IMonaUIBackend& Backend) -> bool
	{
		if (GActiveUIBackend != &Backend) return false;
		GActiveUIBackend = nullptr;
		return true;
	}

	auto GetActiveUIBackend() -> IMonaUIBackend*
	{
		return GActiveUIBackend;
	}
}

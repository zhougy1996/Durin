#include "MonaCoreGlobals.h"
#include "MonaImGuiBackend.h"

namespace Durin
{
	class FMonaImGuiBackendModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			check(Mona::GMonaUIBackend == nullptr);
			Mona::GMonaUIBackend = new Mona::FMonaImGuiBackend();
			Mona::GMonaUIBackend->Initialize();
		}

		auto ShutdownModule() -> void override
		{
			check(Mona::GMonaUIBackend)
			Mona::GMonaUIBackend->Shutdown();
			delete Mona::GMonaUIBackend;
			Mona::GMonaUIBackend = nullptr;
		}
	};

	IMPLEMENT_MODULE(FMonaImGuiBackendModule, MonaImGuiBackend)
}

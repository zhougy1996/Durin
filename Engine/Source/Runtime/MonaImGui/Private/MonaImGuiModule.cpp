#include "MonaCoreGlobals.h"
#include "MonaImGuiBackend.h"

namespace Durin
{
	class FMonaImGuiModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			check(Mona::GActiveUIBackend == nullptr);
			Mona::GActiveUIBackend = new Mona::FMonaImGuiBackend();
			Mona::GActiveUIBackend->Initialize();
		}

		auto ShutdownModule() -> void override
		{
			check(Mona::GActiveUIBackend)
			Mona::GActiveUIBackend->Shutdown();
			delete Mona::GActiveUIBackend;
			Mona::GActiveUIBackend = nullptr;
		}
	};

	IMPLEMENT_MODULE(FMonaImGuiModule, MonaImGui)
}

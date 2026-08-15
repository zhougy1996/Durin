#include "MonaCoreGlobals.h"
#include "MonaImGuiBackend.h"

namespace Durin
{
	// Installs and removes the process-wide Mona ImGui backend.
	class FMonaImGuiModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			check(Mona::GActiveUIBackend == nullptr);
			Mona::GActiveUIBackend = new MonaImGui::FMonaImGuiBackend();
			Mona::GActiveUIBackend->Initialize();
		}

		auto ShutdownModule() -> void override
		{
			check(Mona::GActiveUIBackend);
			Mona::GActiveUIBackend->Shutdown();
			delete Mona::GActiveUIBackend;
			Mona::GActiveUIBackend = nullptr;
		}
	};

	IMPLEMENT_MODULE(FMonaImGuiModule, MonaImGui)
}

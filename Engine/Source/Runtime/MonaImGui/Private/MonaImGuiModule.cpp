#include "MonaCoreGlobals.h"
#include "MonaImGui.h"

namespace Durin
{
	class FMonaImGuiModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			check(Mona::GMonaUI == nullptr);
			Mona::GMonaUI = new Mona::FMonaImGui();
			Mona::GMonaUI->Initialize();
		}

		auto ShutdownModule() -> void override
		{
			check(Mona::GMonaUI)
			Mona::GMonaUI->Shutdown();
			delete Mona::GMonaUI;
			Mona::GMonaUI = nullptr;
		}
	};

	IMPLEMENT_MODULE(FMonaImGuiModule, MonaImGui)
}

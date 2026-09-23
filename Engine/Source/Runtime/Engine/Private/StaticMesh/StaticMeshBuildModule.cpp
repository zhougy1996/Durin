#include "StaticMesh/IMeshBuilderModule.h"

namespace Durin
{
	auto IMeshBuilderModule::Get() -> IMeshBuilderModule*
	{
#if DURIN_WITH_EDITOR
		const auto Info = FModuleManager::Get().FindModule("MeshBuilder");
		if (Info && Info->State.load() == EModuleState::Active)
			return static_cast<IMeshBuilderModule*>(Info->Module.get());
#endif
		return nullptr;
	}
}

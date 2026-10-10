#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/IMeshBuilderModule.h"
#if DURIN_WITH_EDITOR
#include "StaticMeshBuildFunction.h"
#endif

namespace Durin
{
	auto IMeshBuilderModule::StartupModule() -> void
	{
#if DURIN_WITH_EDITOR
		StaticMeshPrivate::RegisterBuildFunction();
#endif
	}

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

#endif

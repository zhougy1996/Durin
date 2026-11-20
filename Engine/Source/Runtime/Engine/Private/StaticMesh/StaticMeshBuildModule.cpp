#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/IMeshBuilderModule.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunctionFactory.h"
#include "StaticMeshBuildPrivate.h"
#endif

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		std::optional<DerivedData::TBuildFunctionFactory<StaticMeshPrivate::MakeRenderBuildFunction>> StaticMeshBuildFactory;
	}
#endif

	auto IMeshBuilderModule::StartupModule() -> void
	{
#if DURIN_WITH_EDITOR
		StaticMeshBuildFactory.emplace();
#endif
	}
	auto IMeshBuilderModule::ShutdownModule() -> void
	{
#if DURIN_WITH_EDITOR
		StaticMeshPrivate::ShutdownBuildSessions();
		StaticMeshBuildFactory.reset();
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

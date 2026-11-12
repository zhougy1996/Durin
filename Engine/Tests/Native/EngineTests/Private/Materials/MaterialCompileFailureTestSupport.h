#pragma once
#include "CoreMinimal.h"
#include "Asset/OfflinePreparation.h"
#include "Materials/MaterialInterface.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::Testing
{
	inline auto FailMaterialCompilation(DMaterialInterface& Material) -> bool
	{
		MIR::FCompilerInput Invalid;
		Invalid.StaticProperties = Material.GetStaticProperties();
		if (!BuildDefaultMaterialCompilerEnvironment(Invalid.Environment)) return false;
		FScopedOfflinePreparation Offline;
		return !Private::FMaterialCompilationLifecycle::Submit(Material, std::move(Invalid), false)
			&& Material.GetMaterialCompileStatus().State == EMaterialCompileState::Failed;
	}
}

#pragma once

#include "EngineAPI.h"
#include "IScene.h"

namespace Durin
{
	class FSkyBoxSceneProxy final
	{
	public:
		explicit FSkyBoxSceneProxy(FSkyBoxSceneData InData)
			: Data(std::move(InData)) {}

		auto GetData() const -> const FSkyBoxSceneData& { return Data; }

	private:
		FSkyBoxSceneData Data;
	};
}

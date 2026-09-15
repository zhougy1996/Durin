#pragma once

#include "MaterialGraphOperations.h"

namespace Durin::Editor::Material
{
	inline auto CreationFamily(EMaterialProgramOpcode Opcode) -> EMaterialProgramOpcode
	{
		switch (Opcode)
		{
		case EMaterialProgramOpcode::MakeFloat3:
		case EMaterialProgramOpcode::MakeFloat4: return EMaterialProgramOpcode::MakeFloat2;
		case EMaterialProgramOpcode::Splat3:
		case EMaterialProgramOpcode::Splat4: return EMaterialProgramOpcode::Splat2;
		default: return Opcode;
		}
	}

	inline auto CreationWidthSlot(const FMaterialGraphCatalogEntry& Entry) -> int
	{
		if (Entry.Opcode == EMaterialProgramOpcode::Parameter && Entry.ResultType != EMaterialProgramValueType::Float) return 0;
		if (CreationFamily(Entry.Opcode) == EMaterialProgramOpcode::MakeFloat2) return 1;
		if (CreationFamily(Entry.Opcode) == EMaterialProgramOpcode::Splat2) return 2;
		if (Entry.Opcode == EMaterialProgramOpcode::Swizzle) return 3;
		return -1;
	}
}

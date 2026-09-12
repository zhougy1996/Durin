#pragma once
#include "MaterialGraphDocument.h"

namespace Durin::Editor::Material
{
	auto DrawMaterialFunctionCallInputs(DObject& Owner, const FGuid& CallId,
		DTransactor& Transactions, std::string& Error) -> void;
	// One document's pending call insertion, committed only after required pins bind.
	class FMaterialFunctionCallPicker
	{
	public:
		auto Draw(DObject& Owner, DTransactor& Transactions, std::string& Error) -> void;
	private:
		TObjectPtr<DMaterialFunctionInterface> Function;
		std::array<char, 128> Search{};
		std::vector<FMaterialFunctionInputBinding> Inputs;
	};
}

#include "Materials/MaterialFunctionTypes.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto IsType(EMaterialProgramValueType Type) -> bool
			{ return Type <= EMaterialProgramValueType::Surface; }
		auto IsFinite(const FMaterialProgramLiteral& Value) -> bool
			{ return std::isfinite(Value.X) && std::isfinite(Value.Y)
				&& std::isfinite(Value.Z) && std::isfinite(Value.W); }
		auto IsDisconnected(const FMaterialProgramLink& Link) -> bool
			{ return !Link.SourceNodeId.IsValid() && !Link.SourceOutputId.IsValid()
				&& Link.SourceOutputIndex == 0; }
		auto Error(FMaterialProgramValidationResult& Result, std::string Message,
			FGuid NodeId = {}, FGuid PortId = {},
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> void
		{
			Result.bSucceeded = false;
			if (Result.Diagnostics.size() == MaterialProgramMaxDiagnosticCount) return;
			Message.resize(std::min<size_t>(Message.size(), MaterialProgramMaxDiagnosticMessageBytes));
			Result.Diagnostics.push_back({.Category = Category,
				.LocationKind = NodeId.IsValid() ? EMaterialProgramDiagnosticLocationKind::Node
					: EMaterialProgramDiagnosticLocationKind::Program,
				.NodeId = NodeId, .Message = std::move(Message), .PortId = PortId});
		}
	}

	auto ValidateMaterialFunctionSignature(const FMaterialFunctionSignature& Signature)
		-> FMaterialProgramValidationResult
	{
		FMaterialProgramValidationResult Result;
		if (Signature.Inputs.size() > MaterialFunctionMaxInputs || Signature.Outputs.empty()
			|| Signature.Outputs.size() > MaterialFunctionMaxOutputs)
		{
			Error(Result, "Function signature exceeds input/output bounds.", {}, {},
				EMaterialProgramDiagnosticCategory::Bounds);
			return Result;
		}
		std::unordered_set<FGuid> Ids;
		std::unordered_map<FGuid, size_t> InputIndices;
		for (size_t Index = 0; Index < Signature.Inputs.size(); ++Index)
			InputIndices.emplace(Signature.Inputs[Index].Id, Index);
		for (bool bInput : {true, false})
			for (const auto& Port : bInput ? Signature.Inputs : Signature.Outputs)
			{
				if (!Port.Id.IsValid() || !Ids.emplace(Port.Id).second)
					Error(Result, "Function port GUID is missing or duplicated.", {}, Port.Id);
				if (!IsType(Port.Type) || Port.Name.empty()
					|| Port.Name.size() > MaterialProgramMaxDisplayNameBytes)
					Error(Result, "Function port type or name is invalid.", {}, Port.Id);
				const auto& Default = Port.Default;
				if (!bInput || Port.bRequired)
				{
					if (Default.Kind != EMaterialFunctionDefaultKind::None || (!bInput && Port.bRequired))
						Error(Result, "Outputs and required inputs cannot declare defaults.", {}, Port.Id);
					continue;
				}
				bool bValid = false;
				switch (Default.Kind)
				{
				case EMaterialFunctionDefaultKind::Numeric:
					bValid = Port.Type <= EMaterialProgramValueType::Float4 && IsFinite(Default.Numeric);
					break;
				case EMaterialFunctionDefaultKind::Texture:
					bValid = Port.Type == EMaterialProgramValueType::Texture2D
						&& IsValidMaterialSampling(Default.Sampler, Default.TextureFallback);
					break;
				case EMaterialFunctionDefaultKind::Surface:
					bValid = Port.Type == EMaterialProgramValueType::Surface
						&& IsDisconnected(Default.Surface.Surface);
					for (uint8 Index = 0; Index < 8; ++Index)
					{
						const auto Output = static_cast<EMaterialSurfaceOutput>(Index);
						bValid &= IsDisconnected(GetMaterialSurfaceOutputLink(Default.Surface, Output))
							&& IsFinite(GetMaterialSurfaceOutputDefault(Default.Surface, Output));
					}
					break;
				case EMaterialFunctionDefaultKind::Input:
					if (const auto It = InputIndices.find(Default.InputId); It != InputIndices.end())
						bValid = Signature.Inputs[It->second].Type == Port.Type;
					break;
				case EMaterialFunctionDefaultKind::UV0:
					bValid = Port.Type == EMaterialProgramValueType::Float2;
					break;
				default: break;
				}
				if (!bValid) Error(Result, "Optional function input has a missing or incompatible default.", {}, Port.Id);
			}
		std::vector<uint8> State(Signature.Inputs.size());
		const auto Visit = [&](auto&& Self, size_t Index) -> void {
			const auto& Port = Signature.Inputs[Index];
			if (State[Index] == 2) return;
			if (State[Index] == 1)
			{
				Error(Result, "Function input defaults contain a cycle.", {}, Port.Id);
				return;
			}
			State[Index] = 1;
			if (!Port.bRequired && Port.Default.Kind == EMaterialFunctionDefaultKind::Input)
				if (const auto It = InputIndices.find(Port.Default.InputId); It != InputIndices.end())
					Self(Self, It->second);
			State[Index] = 2;
		};
		for (size_t Index = 0; Index < State.size(); ++Index) Visit(Visit, Index);
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}

}

#include "MaterialFunctionPreview.h"
#include "MaterialGraphEditInternals.h"

namespace Durin::Editor::Material
{
	auto BuildMaterialFunctionPreview(DMaterialFunctionInterface& Function,
		const FGuid& OutputId, FMaterialGraphDocumentState& OutState,
		FMaterialStaticProperties& OutProperties) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		using Type = EMaterialProgramValueType;
		using Opcode = EMaterialProgramOpcode;
		const auto& Signature = Function.GetFunctionSignature();
		const auto Output = std::ranges::find(Signature.Outputs, OutputId, &FMaterialFunctionPort::Id);
		if (Output == Signature.Outputs.end()) return MakeRejected("The preview output no longer exists.");
		FMaterialFunctionClosure Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = SnapshotMaterialFunctionClosure(Roots, Closure);
		if (!Validation) return MakeRejected("The function cannot be previewed.", std::move(Validation.Diagnostics));
		FMaterialGraphDocumentState State;
		const auto Add = [&](FMaterialProgramNode Node) {
			Node.Id = FGuid::NewGuid();
			const auto Id = Node.Id;
			State.Program.Nodes.push_back(std::move(Node));
			return FMaterialProgramLink{Id};
		};
		std::array<FMaterialProgramLink, 6> RequiredValues;
		FMaterialFunctionCall Call{.NodeId = FGuid::NewGuid(), .Function = &Function};
		for (const auto& Port : Signature.Outputs) Call.Outputs.push_back({Port.Id, Port.Type});
		for (const auto& Port : Signature.Inputs)
			if (Port.bRequired)
			{
				auto& Source = RequiredValues[static_cast<size_t>(Port.Type)];
				if (!Source.SourceNodeId.IsValid())
				{
					if (Port.Type == Type::Surface)
					{
						FMaterialProgramNode Surface{.Opcode = Opcode::MakeSurface, .ResultType = Type::Surface};
						for (uint32 Index = 0; Index < 8; ++Index)
						{
							const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
							Surface.Inputs.push_back(Add({.ResultType = GetMaterialSurfaceOutputType(Attribute),
								.Literal = GetMaterialSurfaceOutputDefault(State.Program.Outputs, Attribute)}));
						}
						Source = Add(std::move(Surface));
					}
					else if (Port.Type == Type::Texture2D)
					{
						const auto Id = FGuid::NewGuid();
						State.Definitions.push_back({.Id = Id, .Name = "PreviewTexture", .Type = EMaterialParameterType::Texture});
						Source = Add({.Opcode = Opcode::TextureParameter, .ResultType = Type::Texture2D, .ParameterId = Id});
					}
					else Source = Add({.ResultType = Port.Type});
				}
				Call.Inputs.push_back({Port.Id, Port.Type, Source});
			}
		State.Program.Nodes.push_back({.Id = Call.NodeId, .Opcode = Opcode::FunctionCall});
		FMaterialProgramLink Value{Call.NodeId, 0, OutputId};
		State.Calls.push_back(std::move(Call));
		FMaterialStaticProperties Properties;
		if (Output->Type == Type::Surface) State.Program.Outputs.Surface = Value;
		else
		{
			Properties.ShadingModel = EMaterialShadingModel::Unlit;
			if (Output->Type == Type::Texture2D)
			{
				const auto Channel = Add({});
				const auto UV = Add({.Opcode = Opcode::UVChannel, .ResultType = Type::Float2, .Inputs = {Channel}});
				Value = Add({.Opcode = Opcode::TextureSample2D, .ResultType = Type::Float4, .Inputs = {Value, UV}});
			}
			if (Output->Type == Type::Float)
				Value = Add({.Opcode = Opcode::Splat3, .ResultType = Type::Float3, .Inputs = {Value}});
			else if (Output->Type == Type::Float2)
			{
				const auto X = Add({.Opcode = Opcode::Swizzle, .Inputs = {Value}, .SwizzleLength = 1});
				const auto Y = Add({.Opcode = Opcode::Swizzle, .Inputs = {Value}, .SwizzleLength = 1, .SwizzleX = 1});
				const auto Zero = Add({});
				Value = Add({.Opcode = Opcode::MakeFloat3, .ResultType = Type::Float3, .Inputs = {X, Y, Zero}});
			}
			else if (Output->Type != Type::Float3)
				Value = Add({.Opcode = Opcode::TruncateToFloat3, .ResultType = Type::Float3, .Inputs = {Value}});
			State.Program.Outputs.Emissive = Value;
		}
		for (size_t Index = 0; Index < State.Program.Nodes.size(); ++Index)
			State.Presentation.Nodes.push_back({State.Program.Nodes[Index].Id, static_cast<int32>(Index % 4) * 320,
				static_cast<int32>(Index / 4) * 240});
		State.Presentation.bHasMaterialOutputPosition = true;
		State.Presentation.MaterialOutputX = 1280;
		OutState = std::move(State);
		OutProperties = Properties;
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}
}

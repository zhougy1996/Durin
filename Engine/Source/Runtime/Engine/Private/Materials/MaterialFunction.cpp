#include "Materials/MaterialFunction.h"

#include "DObject/Property.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto AdvanceFunctionRevision(uint64& Revision) -> void
			{ Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1; }
	}

	DMaterialFunctionInterface::DMaterialFunctionInterface(const FObjectInitializer& Initializer)
		: Super(Initializer) {}

	DMaterialFunction::DMaterialFunction(const FObjectInitializer& Initializer)
		: Super(Initializer)
	{
		const FGuid InputId{0x3461fcad, 0x92ac4714, 0x83d68f1e, 0x219814c4};
		const FGuid OutputId{0x690923fd, 0x879544a9, 0x864a3518, 0x81f59b74};
		const FGuid InputNodeId{0xc5e3f95b, 0x818a4f46, 0x90564e64, 0x45f39324};
		const FGuid OutputNodeId{0x61a6db91, 0xb6d741cf, 0xa4e8d91d, 0xc14794f2};
		Graph.Signature.Inputs.push_back({.Id = InputId, .Type = EMaterialProgramValueType::Surface,
			.Name = "Surface", .Default = {.Kind = EMaterialFunctionDefaultKind::Surface}});
		Graph.Signature.Outputs.push_back({.Id = OutputId, .Type = EMaterialProgramValueType::Surface,
			.Name = "Surface"});
		Graph.Nodes.push_back({.Id = InputNodeId, .Opcode = EMaterialProgramOpcode::FunctionInput,
			.ResultType = EMaterialProgramValueType::Surface, .FunctionPortId = InputId});
		Graph.Nodes.push_back({.Id = OutputNodeId, .Opcode = EMaterialProgramOpcode::FunctionOutput,
			.ResultType = EMaterialProgramValueType::Surface,
			.Inputs = {{.SourceNodeId = InputNodeId}}, .FunctionPortId = OutputId});
		Presentation.Nodes = {{InputNodeId, 0, 0}, {OutputNodeId, 320, 0}};
	}

	auto DMaterialFunction::GetFunctionDependencies() const
		-> std::vector<TObjectPtr<DMaterialFunctionInterface>>
	{
		check(IsInGameThread());
		std::vector<TObjectPtr<DMaterialFunctionInterface>> Dependencies;
		for (const auto& Call : Graph.Calls)
			if (Call.Function && std::ranges::find(Dependencies, Call.Function) == Dependencies.end())
				Dependencies.push_back(Call.Function);
		return Dependencies;
	}

	auto DMaterialFunction::BuildFunctionSnapshot(FMaterialFunctionSnapshot& OutSnapshot) const
		-> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		auto Result = ValidateMaterialFunctionGraph(Graph);
		if (!Result) return Result;
		FMaterialFunctionSnapshot Snapshot;
		Snapshot.AssetPath = GetObjectPath();
		Snapshot.Revision = Revision;
		Snapshot.Signature = Graph.Signature;
		Snapshot.Nodes = Graph.Nodes;
		for (const auto& Call : Graph.Calls)
		{
			if (!IsValid(Call.Function.Get()))
			{
				Result.bSucceeded = false;
				Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Dependency,
					.LocationKind = EMaterialProgramDiagnosticLocationKind::Node, .NodeId = Call.NodeId,
					.Message = "Function call references a missing function asset.",
					.FunctionAssetPath = Snapshot.AssetPath});
				return Result;
			}
			Snapshot.Calls.push_back({Call.NodeId, Call.Function->GetObjectPath(), Call.Inputs, Call.Outputs});
			Result = ValidateMaterialFunctionCallSignature(Snapshot.Calls.back(), Call.Function->GetFunctionSignature());
			if (!Result) return Result;
		}
		OutSnapshot = std::move(Snapshot);
		return Result;
	}

	auto DMaterialFunction::SetFunctionGraph(FMaterialFunctionGraph Candidate)
		-> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		auto Result = ValidateMaterialFunctionGraph(Candidate);
		if (!Result || Candidate == Graph) return Result;
		Graph = std::move(Candidate);
		AdvanceFunctionRevision(Revision);
		NotifyMaterialFunctionChanged(*this);
		MarkPackageDirty();
		return Result;
	}

	auto DMaterialFunction::SetFunctionPresentation(FMaterialFunctionPresentation Candidate) -> bool
	{
		check(IsInGameThread());
		if (Candidate.SchemaVersion != CurrentMaterialFunctionPresentationSchemaVersion) return false;
		FMaterialProgram Program;
		Program.Nodes = Graph.Nodes;
		FMaterialGraphPresentation Positions;
		Positions.Nodes = std::move(Candidate.Nodes);
		Candidate.Nodes = SanitizeMaterialGraphPresentation(Positions, Program).Nodes;
		if (Candidate == Presentation) return true;
		Presentation = std::move(Candidate);
		MarkPackageDirty();
		return true;
	}

	auto DMaterialFunction::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Graph"))
		{
			AdvanceFunctionRevision(Revision);
			NotifyMaterialFunctionChanged(*this);
		}
	}
}

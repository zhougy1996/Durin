#include "RDGBuilderInternal.h"

namespace Durin::RDGPrivate
{
	namespace
	{
		auto DependencyKindName(ERDGDependencyKind Kind) -> const char*;
		auto PassTypeName(ERDGPassType Type) -> const char*;
		auto GraphUseName(ERDGUse Use) -> const char*;

		auto DependencyKindName(ERDGDependencyKind Kind) -> const char*
		{
			switch (Kind)
			{
			case ERDGDependencyKind::Value: return "value";
			case ERDGDependencyKind::Execution: return "execution";
			case ERDGDependencyKind::Explicit: return "explicit";
			}
			return "unknown";
		}

		auto PassTypeName(ERDGPassType Type) -> const char*
		{
			switch (Type)
			{
			case ERDGPassType::Graphics: return "graphics";
			case ERDGPassType::Compute: return "compute";
			case ERDGPassType::Copy: return "copy";
			}
			return "unknown";
		}

		auto GraphUseName(ERDGUse Use) -> const char*
		{
			switch (Use)
			{
			case ERDGUse::Read: return "read";
			case ERDGUse::Write: return "write";
			case ERDGUse::ReadWrite: return "read-write";
			}
			return "unknown";
		}

		}

	auto ParameterMemberKindName(ERDGParameterMemberKind Kind)
		-> const char*
	{
		switch (Kind)
		{
		case ERDGParameterMemberKind::Texture: return "texture";
		case ERDGParameterMemberKind::Buffer: return "buffer";
		case ERDGParameterMemberKind::Token: return "token";
		case ERDGParameterMemberKind::ColorAttachment:
			return "color-attachment";
		case ERDGParameterMemberKind::DepthStencilAttachment:
			return "depth-stencil-attachment";
		case ERDGParameterMemberKind::ManagedColorAttachment:
			return "managed-color-attachment";
		case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
			return "managed-depth-stencil-attachment";
		case ERDGParameterMemberKind::ManagedTexture:
			return "managed-texture";
		case ERDGParameterMemberKind::ValueRead: return "value-read";
		case ERDGParameterMemberKind::ValueWrite: return "value-write";
		case ERDGParameterMemberKind::Nested: return "nested";
		}
		return "unknown";
	}
}

namespace Durin
{
	using namespace RDGPrivate;

	auto FRDGBuilder::EnsureDiagnostics() const -> void
	{
		if (Diagnostics) return;
		auto Result = std::make_unique<FDiagnostics>();
		if (!State->bCompiled)
		{
			Diagnostics = std::move(Result);
			return;
		}
		const FGraphPassView Passes{State->Passes,
			Compiled->Retained.size() > State->Passes.size() ? &Compiled->ExportPass : nullptr};
		const auto ResourceUses = BuildResourceUseTable(Passes,
			static_cast<uint32>(Compiled->Resources.size()));
		for (uint32 Index = 0; Index < Compiled->Retained.size(); ++Index)
		{
			const auto& Pass = Passes[Index];
			Result->CullingDecisions.push_back({Pass.Name, !Compiled->Retained[Index],
				Compiled->Retained[Index] ? (Pass.bRoot ? Pass.RootReason : "value dependency")
					: "unreachable from an explicit root"});
			if (Pass.ParameterLayout == nullptr) continue;
			size_t UseIndex = 0;
			for (const auto& Element : Pass.ParameterLayout->Elements)
			{
				const auto& Member = *Pass.ParameterLayout->Leaves[Element.LeafIndex].Metadata;
				// Uses are frozen in layout order. Never reread mutable parameter payloads.
				const bool bPresent = UseIndex < Pass.Uses.size()
					&& Pass.Uses[UseIndex].ParameterPath == Element.FieldPath;
				const FGraphUse Use = bPresent ? Pass.Uses[UseIndex++]
					: MakeParameterUse(Member, Element.FieldPath);
				Result->Parameters.push_back({
					.PassDeclarationIndex = Index,
					.FieldPath = Element.FieldPath,
					.Kind = Member.Kind,
					.ResourceKind = Member.ResourceKind,
					.bPresent = bPresent,
					.ResourceId = bPresent ? Use.ResourceIndex : std::numeric_limits<uint32>::max(),
					.Use = Use.Use,
					.Access = Use.Access,
					.TextureRange = Use.TextureRange,
					.BufferOffset = Use.BufferOffset,
					.BufferSize = Use.BufferSize,
					.bDiscard = Use.bDiscard,
					.bStore = Use.bStore,
					.bPassManagedTransition = Use.bPassManagedTransition,
					.ResultAccess = Use.ResultAccess,
					.ShaderBindingName = std::string(Use.ShaderBindingName),
					.ShaderBindingType = Use.ShaderBindingType});
			}
		}
		for (uint32 Index = 0; Index < Compiled->Resources.size(); ++Index)
		{
			const auto& Resource = Compiled->Resources[Index];
			const auto& Lifetime = Compiled->ResourceLifetimes[Index];
			Result->Resources.push_back({Index, Resource.Name, Resource.Kind, Resource.bExternal, "unused"});
			auto& Capture = Result->Resources.back();
			Capture.ValueType = Resource.ValueTypeName;
			Capture.TextureFormat = Resource.TextureDesc.Format;
			Capture.TextureExtent = Resource.TextureDesc.Extent;
			Capture.TextureArraySize = Resource.TextureDesc.ArraySize;
			Capture.TextureMips = Resource.TextureDesc.NumMips;
			Capture.BufferSize = Resource.BufferDesc.Size;
			Capture.BufferStride = Resource.BufferDesc.Stride;
			Capture.Preparation = Lifetime.bCulled ? "culled"
				: Resource.bExternal ? "external"
				: Resource.Kind == ERDGResourceKind::Token ? "logical" : "requested";
			Capture.AllocationDisposition = Lifetime.bCulled ? "culled"
				: Resource.bExternal ? "external"
				: Resource.Kind == ERDGResourceKind::Token ? "none"
				: Compiled->Backings[Index].AllocationDisposition.empty() ? "pending"
				: Compiled->Backings[Index].AllocationDisposition;
			Capture.PhysicalAllocationId = Compiled->Backings[Index].PhysicalAllocationId;
		}

		// Reconstruct the same deterministic layout only on explicit inspection.
		// This does not schedule, allocate, execute, or alter the compiled plan.
		FRangeWork Work{Compiled->Budget};
		auto Layout = BuildTrackingLayout(Compiled->Resources, ResourceUses, Work);
		requiref(Layout.has_value(), "compiled RDG diagnostic layout failed: {}", ToString(Layout.error()));
		auto Cells = std::move(*Layout);
		std::vector<uint32> Versions(Cells.Ranges.size(), 0);
		std::vector<uint32> VersionPasses(Cells.Ranges.size(), std::numeric_limits<uint32>::max());
		const bool bTraversed = TraverseExecutionStates(Cells, Compiled->Resources,
			Passes, Compiled->Passes, Compiled->ResourceLifetimes, nullptr, State->bAsyncComputeEnabled,
			[&](const FRDGTransitionCapture& Event, size_t) -> bool
			{
				Result->Transitions.push_back(Event);
				return true;
			}, [&](uint32 DeclarationIndex, const FGraphUse& Use, size_t CellIndex, const FRangeCell& Cell, bool bWrites)
			{
				if (bWrites && VersionPasses[CellIndex] != DeclarationIndex)
				{
					++Versions[CellIndex];
					VersionPasses[CellIndex] = DeclarationIndex;
				}
				Result->Uses.push_back({DeclarationIndex, Use.ResourceIndex,
					Use.Use, Use.Access, Cell.TextureRange, Use.BufferOffset,
					Use.BufferSize, Versions[CellIndex], Use.bDiscard, Use.bStore,
					std::string(Use.ParameterPath), std::string(Use.ShaderBindingName),
					Use.ShaderBindingType});
			});
		requiref(bTraversed, "compiled RDG diagnostic traversal failed");
		Diagnostics = std::move(Result);
	}

	auto FRDGBuilder::GetPasses() const
		-> std::span<const FRDGCompiledPass> { return Compiled->Passes; }
	auto FRDGBuilder::GetDependencies() const
		-> std::span<const FRDGDependency> { return Compiled->Dependencies; }
	auto FRDGBuilder::GetResourceLifetimes() const
		-> std::span<const FRDGResourceLifetime>
	{
		return Compiled->ResourceLifetimes;
	}
	auto FRDGBuilder::GetCullingDecisions() const
		-> std::span<const FRDGCullingDecision>
	{
		EnsureDiagnostics();
		return Diagnostics->CullingDecisions;
	}
	auto FRDGBuilder::GetFinalBarriers() const -> const FRDGBarrierBatch&
	{ return Compiled->FinalBarriers; }
	auto FRDGBuilder::GetExecutionPlan() const -> const FRDGExecutionPlan&
	{ return Compiled->ExecutionPlan; }

	auto FRDGBuilder::GetSubmissionSyncPoints() const -> std::span<const FRHIGPUSyncPointRef>
	{ return State->SubmissionSyncPoints; }

	auto FRDGBuilder::GetBudget() const -> const FRDGBudget&
	{
		return State->Budget;
	}

	auto FRDGBuilder::GetStatistics() const -> FRDGStatistics
	{
		FRDGStatistics Result;
		Result.DeclaredPasses = static_cast<uint32>(Compiled->Retained.size());
		Result.ScheduledPasses = static_cast<uint32>(Compiled->Passes.size());
		Result.CulledPasses = Result.DeclaredPasses - Result.ScheduledPasses;
		Result.Dependencies = static_cast<uint32>(Compiled->Dependencies.size());
		Result.BufferTransitions = static_cast<uint32>(
			Compiled->FinalBarriers.GetBufferTransitions().size());
		Result.TextureTransitions = static_cast<uint32>(
			Compiled->FinalBarriers.GetTextureTransitions().size());
		auto CountSubresources = [&](const FRDGBarrierBatch& Batch) {
			for (const auto& Transition : Batch.GetTextureTransitions())
				Result.TextureTransitionSubresources += Transition.Range.NumMips * Transition.Range.NumArrayLayers;
		};
		CountSubresources(Compiled->FinalBarriers);
		for (const auto& Pass : Compiled->Passes)
		{
			CountSubresources(Pass.Barriers);
			Result.BufferTransitions += static_cast<uint32>(Pass.Barriers.GetBufferTransitions().size());
			Result.TextureTransitions += static_cast<uint32>(Pass.Barriers.GetTextureTransitions().size());
		}
		Result.Phases = State->Phases;
		Result.CompileMicroseconds = State->CompileMicroseconds;
		Result.ExecuteMicroseconds =
			State->ExecuteMicroseconds;
		Result.bPassRegressionBudgetExceeded = Result.DeclaredPasses
			> Compiled->Budget.RegressionMaxPasses;
		Result.bDependencyRegressionBudgetExceeded = Result.Dependencies
			> Compiled->Budget.RegressionMaxDependencies;
		Result.bBufferTransitionRegressionBudgetExceeded = Result.BufferTransitions
			> Compiled->Budget.RegressionMaxBufferTransitions;
		Result.bTextureTransitionRegressionBudgetExceeded = Result.TextureTransitions
			> Compiled->Budget.RegressionMaxTextureTransitions;
		Result.bCompileBudgetExceeded = Result.CompileMicroseconds
			> GetBudget().MaxCompileMicroseconds;
		Result.bExecuteBudgetExceeded = Result.ExecuteMicroseconds
			> GetBudget().MaxExecuteMicroseconds;
		return Result;
	}

	auto FRDGBuilder::Capture() const -> FRDGCapture
	{
		EnsureDiagnostics();
		FRDGCapture Result;
		Result.ExecutionResult = State->ExecutionResult;
		Result.bCompiled = State->bCompiled;
		Result.Budget = State->Budget;
		Result.Statistics = GetStatistics();
		Result.AllocationStatistics = Compiled->AllocationStatistics;
		Result.Resources = Diagnostics->Resources;
		Result.Parameters = Diagnostics->Parameters;
		Result.Uses = Diagnostics->Uses;
		Result.Transitions = Diagnostics->Transitions;
		Result.Dependencies = Compiled->Dependencies;
		Result.ResourceLifetimes = Compiled->ResourceLifetimes;
		Result.CullingDecisions = Diagnostics->CullingDecisions;
		Result.ExecutionPlan = Compiled->ExecutionPlan;
		Result.Dump = Dump();
		Result.Passes.reserve(Compiled->Passes.size());
		for (const auto& Pass : Compiled->Passes)
			Result.Passes.push_back({Pass.Name, Pass.Type, Pass.DeclarationIndex,
				Pass.ParameterStructName,
				static_cast<uint32>(Pass.Barriers.GetBufferTransitions().size()),
				static_cast<uint32>(Pass.Barriers.GetTextureTransitions().size())});
		return Result;
	}

	auto FRDGBuilder::Dump() const -> std::string
	{
		if (!State->bCompiled) return "render-graph: no compiled plan";
		EnsureDiagnostics();
		std::ostringstream Output;
		Output << "render-graph passes=" << Compiled->Passes.size()
			<< " edges=" << Compiled->Dependencies.size() << '\n';
		const auto Statistics = GetStatistics();
		Output << "texture-transitions=" << Statistics.TextureTransitions
			<< " texture-subresource-transitions=" << Statistics.TextureTransitionSubresources << '\n';
		for (const auto& Batch : Compiled->ExecutionPlan.Batches)
			Output << "submission " << Batch.Id.Index << " queue="
				<< (Batch.Queue == ERDGQueueAssignment::Graphics ? "graphics" : "async-compute")
				<< " passes=" << Batch.FirstPass << '+' << Batch.NumPasses
				<< " epilogue=" << Batch.bEpilogue << '\n';
		for (const auto& Edge : Compiled->ExecutionPlan.Dependencies)
			Output << "submission-dependency " << Edge.Before.Index << " -> " << Edge.After.Index
				<< " kind=" << static_cast<uint32>(Edge.Kind) << " cause=" << Edge.Cause << '\n';
		for (const auto& Handoff : Compiled->ExecutionPlan.Handoffs)
		{
			Output << "handoff resource=" << Handoff.ResourceId << " submission=" << Handoff.Consumer.Index
				<< " texture=" << Handoff.bTexture << " transition=" << Handoff.TransitionIndex
				<< " source-queue=" << static_cast<uint32>(Handoff.SourceQueue) << " producers=";
			for (const auto Producer : Handoff.Producers) Output << Producer.Index << ',';
			Output << '\n';
		}
		Output << "allocation active-resources="
			<< Compiled->AllocationStatistics.ActiveResources
			<< " retained-resources="
			<< Compiled->AllocationStatistics.RetainedResources
			<< " active-bytes=" << Compiled->AllocationStatistics.ActiveBytes
			<< " retained-bytes=" << Compiled->AllocationStatistics.RetainedBytes
			<< " peak-active-bytes="
			<< Compiled->AllocationStatistics.PeakActiveBytes
			<< " hits=" << Compiled->AllocationStatistics.ReuseHits
			<< " misses=" << Compiled->AllocationStatistics.ReuseMisses
			<< " evictions=" << Compiled->AllocationStatistics.Evictions
			<< " failures=" << Compiled->AllocationStatistics.Failures << '\n';
		for (uint32 Index = 0; Index < Compiled->Passes.size(); ++Index)
		{
			const auto& Pass = Compiled->Passes[Index];
			Output << "pass " << Index << " decl=" << Pass.DeclarationIndex
				<< " type=" << PassTypeName(Pass.Type) << " name=" << Pass.Name
				<< " buffers=" << Pass.Barriers.GetBufferTransitions().size()
				<< " textures=" << Pass.Barriers.GetTextureTransitions().size();
			if (!Pass.ParameterStructName.empty())
				Output << " parameters=" << Pass.ParameterStructName;
			Output << '\n';
		}
		for (const auto& Edge : Compiled->Dependencies)
			Output << "edge " << Edge.BeforePass << "->" << Edge.AfterPass
				<< " kind=" << DependencyKindName(Edge.Kind)
				<< " cause=" << Edge.Cause << '\n';
		Output << "final buffers=" << Compiled->FinalBarriers.GetBufferTransitions().size()
			<< " textures=" << Compiled->FinalBarriers.GetTextureTransitions().size() << '\n';
		for (const auto& Lifetime : Compiled->ResourceLifetimes)
			Output << "lifetime name=" << Lifetime.Name << " first="
				   << Lifetime.FirstPass << " last=" << Lifetime.LastPass
				   << " external=" << Lifetime.bExternal
				   << " culled=" << Lifetime.bCulled << '\n';
		for (const auto& Decision : Diagnostics->CullingDecisions)
			Output << "culling name=" << Decision.Name << " culled="
				<< Decision.bCulled << " reason=" << Decision.Reason << '\n';
		for (const auto& Resource : Diagnostics->Resources)
			Output << "resource id=" << Resource.ResourceId << " name="
				   << Resource.Name << " kind=" << static_cast<uint32>(Resource.Kind)
				   << " external=" << Resource.bExternal << " preparation="
				   << Resource.Preparation << " allocation="
				   << Resource.AllocationDisposition << " allocation-id="
				   << Resource.PhysicalAllocationId << " value-type="
				   << Resource.ValueType << " format="
				   << static_cast<uint32>(Resource.TextureFormat) << " extent="
				   << Resource.TextureExtent.x << 'x' << Resource.TextureExtent.y
				   << " layers=" << Resource.TextureArraySize << " mips="
				   << static_cast<uint32>(Resource.TextureMips) << " buffer-size="
				   << Resource.BufferSize << " stride=" << Resource.BufferStride << '\n';
		for (const auto& Parameter : Diagnostics->Parameters)
		{
			Output << "parameter pass=" << Parameter.PassDeclarationIndex
				<< " field=" << Parameter.FieldPath << " kind="
				<< ParameterMemberKindName(Parameter.Kind) << " present="
				<< Parameter.bPresent << " resource=";
			if (Parameter.bPresent) Output << Parameter.ResourceId;
			else Output << "none";
			Output << " direction=" << GraphUseName(Parameter.Use)
				<< " access=" << static_cast<uint32>(Parameter.Access)
				<< " aspects="
				<< static_cast<uint32>(Parameter.TextureRange.Aspects) << " mip="
				<< Parameter.TextureRange.FirstMip << '+'
				<< Parameter.TextureRange.NumMips << " layer="
				<< Parameter.TextureRange.FirstArrayLayer << '+'
				<< Parameter.TextureRange.NumArrayLayers << " offset="
				<< Parameter.BufferOffset << " size=" << Parameter.BufferSize
				<< " discard=" << Parameter.bDiscard << " store="
				<< Parameter.bStore << " managed="
				<< Parameter.bPassManagedTransition << " result-access="
				<< static_cast<uint32>(Parameter.ResultAccess);
			if (!Parameter.ShaderBindingName.empty())
				Output << " shader-binding=" << Parameter.ShaderBindingName
					<< " binding-type="
					<< static_cast<uint32>(Parameter.ShaderBindingType);
			Output << '\n';
		}
		for (const auto& Use : Diagnostics->Uses)
		{
			Output << "use pass=" << Use.PassDeclarationIndex << " resource="
				<< Use.ResourceId << " direction=" << GraphUseName(Use.Use)
				<< " version=" << Use.Version << " access="
				<< static_cast<uint32>(Use.Access) << " aspects="
				<< static_cast<uint32>(Use.TextureRange.Aspects) << " mip="
				<< Use.TextureRange.FirstMip << '+' << Use.TextureRange.NumMips
				<< " layer=" << Use.TextureRange.FirstArrayLayer << '+'
				<< Use.TextureRange.NumArrayLayers << " offset=" << Use.BufferOffset
				<< " size=" << Use.BufferSize << " discard=" << Use.bDiscard
				<< " store=" << Use.bStore;
			if (!Use.ParameterPath.empty())
				Output << " field=" << Use.ParameterPath;
			if (!Use.ShaderBindingName.empty())
				Output << " shader-binding=" << Use.ShaderBindingName
					<< " binding-type="
					<< static_cast<uint32>(Use.ShaderBindingType);
			Output << '\n';
		}
		for (const auto& Transition : Diagnostics->Transitions)
			Output << "transition resource=" << Transition.ResourceId << " pass="
				<< Transition.PassIndex << " before="
				<< static_cast<uint32>(Transition.Before) << " after="
				<< static_cast<uint32>(Transition.After) << " aspects="
				<< static_cast<uint32>(Transition.TextureRange.Aspects) << " mip="
				<< Transition.TextureRange.FirstMip << '+'
				<< Transition.TextureRange.NumMips << " layer="
				<< Transition.TextureRange.FirstArrayLayer << '+'
				<< Transition.TextureRange.NumArrayLayers << " offset="
				<< Transition.BufferOffset << " size=" << Transition.BufferSize
				<< " discard=" << Transition.bDiscardContents
				<< " kind=" << (Transition.Kind == ERDGTransitionKind::RHIBarrier
					? "rhi-barrier" : "pass-managed")
				<< " final=" << Transition.bFinal << '\n';
		return Output.str();
	}

}

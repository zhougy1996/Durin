#include "RenderGraph.h"

#include "RHICommandList.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <ranges>
#include <sstream>
#include <tuple>

namespace Durin
{
	namespace
	{
		enum class EGraphResourceKind : uint8 { Texture, Buffer, Token };

		struct FGraphResource
		{
			std::string Name;
			EGraphResourceKind Kind = EGraphResourceKind::Texture;
			FRHITexture* Texture = nullptr;
			FRHIBuffer* Buffer = nullptr;
			ERHIAccess InitialAccess = ERHIAccess::Discard;
			ERHIAccess FinalAccess = ERHIAccess::None;
			bool bImported = false;
		};

		struct FGraphUse
		{
			uint32 ResourceIndex = 0;
			EGraphResourceKind Kind = EGraphResourceKind::Texture;
			ERenderGraphUse Use = ERenderGraphUse::Read;
			ERHIAccess Access = ERHIAccess::None;
			bool bDiscard = false;
			FRHITextureSubresourceRange TextureRange{};
			uint64 BufferOffset = 0;
			uint64 BufferSize = 0;
			bool bStore = true;
		};

		struct FGraphPass
		{
			std::string Name;
			ERenderGraphPassType Type = ERenderGraphPassType::Graphics;
			std::vector<FGraphUse> Uses;
			std::vector<uint32> Prerequisites;
			FRenderGraphExecute Execute;
			bool bRoot = false;
			std::string RootReason;
		};

		struct FRangeState
		{
			FGraphUse Use;
			ERHIAccess Access = ERHIAccess::Discard;
			bool bProduced = false;
			uint32 LastPass = std::numeric_limits<uint32>::max();
		};

		auto IsWriteUse(ERenderGraphUse Use) -> bool
		{
			return Use != ERenderGraphUse::Read;
		}

		auto AccessHasWrite(ERHIAccess Access) -> bool
		{
			constexpr ERHIAccess Writes = ERHIAccess::ColorAttachmentReadWrite
				| ERHIAccess::DepthStencilReadWrite
				| ERHIAccess::GraphicsShaderReadWrite
				| ERHIAccess::ComputeShaderReadWrite | ERHIAccess::TransferWrite
				| ERHIAccess::HostWrite;
			return EnumHasAnyFlags(Access, Writes);
		}

		auto BufferRangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			return A.BufferOffset < B.BufferOffset + B.BufferSize
				&& B.BufferOffset < A.BufferOffset + A.BufferSize;
		}

		auto TextureRangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			const auto& X = A.TextureRange;
			const auto& Y = B.TextureRange;
			return EnumHasAnyFlags(X.Aspects, Y.Aspects)
				&& X.FirstMip < Y.FirstMip + Y.NumMips
				&& Y.FirstMip < X.FirstMip + X.NumMips
				&& X.FirstArrayLayer < Y.FirstArrayLayer + Y.NumArrayLayers
				&& Y.FirstArrayLayer < X.FirstArrayLayer + X.NumArrayLayers;
		}

		auto RangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			return A.ResourceIndex == B.ResourceIndex && A.Kind == B.Kind
				&& (A.Kind == EGraphResourceKind::Token
					|| (A.Kind == EGraphResourceKind::Texture
						? TextureRangesOverlap(A, B) : BufferRangesOverlap(A, B)));
		}

		auto RangesEqual(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			if (A.ResourceIndex != B.ResourceIndex || A.Kind != B.Kind)
				return false;
			return A.Kind == EGraphResourceKind::Token
				? true : A.Kind == EGraphResourceKind::Texture
				? A.TextureRange == B.TextureRange
				: A.BufferOffset == B.BufferOffset && A.BufferSize == B.BufferSize;
		}

		auto PassTypeName(ERenderGraphPassType Type) -> const char*
		{
			switch (Type)
			{
			case ERenderGraphPassType::Graphics: return "graphics";
			case ERenderGraphPassType::Compute: return "compute";
			case ERenderGraphPassType::Copy: return "copy";
			}
			return "unknown";
		}
	} // namespace

	struct FRenderGraphBuilder::FState
	{
		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::vector<FGraphPass> Passes;
		std::vector<std::string> DeclarationErrors;
		bool bEnableCulling = false;
		FRenderGraphPrepare Prepare;
		FRenderGraphBudget Budget;
	};

	struct FCompiledRenderGraph::FState
	{
		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::vector<FCompiledRenderGraphPass> Passes;
		std::vector<FRenderGraphDependency> Dependencies;
		std::vector<FRenderGraphResourceLifetime> ResourceLifetimes;
		std::vector<FRenderGraphCullingDecision> CullingDecisions;
		std::vector<FRHIBufferTransition> FinalBufferTransitions;
		std::vector<FRHITextureTransition> FinalTextureTransitions;
		std::vector<FRenderGraphExecute> ExecuteCallbacks;
		FRenderGraphPrepare Prepare;
		FRenderGraphBudget Budget;
		uint64 CompileMicroseconds = 0;
		mutable std::atomic<uint64> ExecuteMicroseconds = 0;
	};

	FRenderGraphBuilder::FRenderGraphBuilder()
		: State(std::make_unique<FState>())
	{
		static std::atomic<uint64> NextOwner{1};
		State->Owner = NextOwner.fetch_add(1, std::memory_order_relaxed);
	}

	FRenderGraphBuilder::~FRenderGraphBuilder() = default;

	auto FRenderGraphBuilder::ImportTexture(std::string_view Name,
		FRHITexture* Texture, ERHIAccess InitialAccess, ERHIAccess FinalAccess)
		-> FRenderGraphTextureHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back({std::string(Name), EGraphResourceKind::Texture,
			Texture, nullptr, InitialAccess, FinalAccess, true});
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateTexture(std::string_view Name,
		FRHITexture* Texture, ERHIAccess FinalAccess)
		-> FRenderGraphTextureHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back({std::string(Name), EGraphResourceKind::Texture,
			Texture, nullptr, ERHIAccess::Discard, FinalAccess, false});
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::ImportBuffer(std::string_view Name,
		FRHIBuffer* Buffer, ERHIAccess InitialAccess, ERHIAccess FinalAccess)
		-> FRenderGraphBufferHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back({std::string(Name), EGraphResourceKind::Buffer,
			nullptr, Buffer, InitialAccess, FinalAccess, true});
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateBuffer(std::string_view Name,
		FRHIBuffer* Buffer, ERHIAccess FinalAccess)
		-> FRenderGraphBufferHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back({std::string(Name), EGraphResourceKind::Buffer,
			nullptr, Buffer, ERHIAccess::Discard, FinalAccess, false});
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateToken(std::string_view Name)
		-> FRenderGraphTokenHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back({std::string(Name), EGraphResourceKind::Token});
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::AddPass(std::string_view Name,
		ERenderGraphPassType Type, FRenderGraphExecute Execute)
		-> FRenderGraphPassHandle
	{
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		State->Passes.push_back({std::string(Name), Type, {}, {},
			std::move(Execute), false, {}});
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::MarkPassRoot(FRenderGraphPassHandle Pass,
		std::string_view Reason) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back("root has an invalid pass handle");
			return;
		}
		State->Passes[Pass.Index].bRoot = true;
		State->Passes[Pass.Index].RootReason = std::string(Reason);
	}

	auto FRenderGraphBuilder::EnablePassCulling() -> void
	{
		State->bEnableCulling = true;
	}

	auto FRenderGraphBuilder::SetExecutionPreparation(FRenderGraphPrepare Prepare)
		-> void
	{
		State->Prepare = std::move(Prepare);
	}

	auto FRenderGraphBuilder::SetBudget(const FRenderGraphBudget& Budget) -> void
	{
		State->Budget = Budget;
	}

	auto FRenderGraphBuilder::AddDependency(FRenderGraphPassHandle Pass,
		FRenderGraphPassHandle Prerequisite) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back(
				"dependency has an invalid destination pass handle");
			return;
		}
		State->Passes[Pass.Index].Prerequisites.push_back(
			Prerequisite.Owner == State->Owner ? Prerequisite.Index
				: std::numeric_limits<uint32>::max());
	}

	auto FRenderGraphBuilder::UseTexture(FRenderGraphPassHandle Pass,
		FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
		ERHIAccess Access, bool bDiscard) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back(
				"texture use has an invalid pass handle");
			return;
		}
		State->Passes[Pass.Index].Uses.push_back({
			Texture.Owner == State->Owner ? Texture.Index
				: std::numeric_limits<uint32>::max(),
			EGraphResourceKind::Texture, Use, Access, bDiscard, Range});
	}

	auto FRenderGraphBuilder::UseBuffer(FRenderGraphPassHandle Pass,
		FRenderGraphBufferHandle Buffer, uint64 Offset, uint64 Size,
		ERenderGraphUse Use, ERHIAccess Access, bool bDiscard) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back(
				"buffer use has an invalid pass handle");
			return;
		}
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Buffer.Owner == State->Owner ? Buffer.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = EGraphResourceKind::Buffer;
		DeclaredUse.Use = Use;
		DeclaredUse.Access = Access;
		DeclaredUse.bDiscard = bDiscard;
		DeclaredUse.BufferOffset = Offset;
		DeclaredUse.BufferSize = Size;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRenderGraphBuilder::UseColorAttachment(FRenderGraphPassHandle Pass,
		FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		UseTexture(Pass, Texture, Range, ERenderGraphUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
			State->Passes[Pass.Index].Uses.back().bStore =
				StoreAction == ERHIRenderTargetStoreAction::Store;
	}

	auto FRenderGraphBuilder::UseDepthStencilAttachment(
		FRenderGraphPassHandle Pass, FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		UseTexture(Pass, Texture, Range, ERenderGraphUse::ReadWrite,
			ERHIAccess::DepthStencilReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
			State->Passes[Pass.Index].Uses.back().bStore =
				StoreAction == ERHIRenderTargetStoreAction::Store;
	}

	auto FRenderGraphBuilder::UseToken(FRenderGraphPassHandle Pass,
		FRenderGraphTokenHandle Token, ERenderGraphUse Use) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back(
				"token use has an invalid pass handle");
			return;
		}
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Token.Owner == State->Owner ? Token.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = EGraphResourceKind::Token;
		DeclaredUse.Use = Use;
		DeclaredUse.bDiscard = Use != ERenderGraphUse::Read;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRenderGraphBuilder::Compile() const -> FRenderGraphCompileResult
	{
		const auto Started = std::chrono::steady_clock::now();
		auto Fail = [](std::string Error) {
			return FRenderGraphCompileResult{nullptr, std::move(Error)};
		};
		if (!State->DeclarationErrors.empty())
			return Fail(State->DeclarationErrors.front());
		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size();
			++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			if (Resource.Name.empty())
				return Fail("resource[" + std::to_string(ResourceIndex)
					+ "] has an empty name");
			if ((Resource.Kind == EGraphResourceKind::Texture
					&& Resource.Texture == nullptr)
				|| (Resource.Kind == EGraphResourceKind::Buffer
					&& Resource.Buffer == nullptr))
				return Fail("resource '" + Resource.Name + "' has no physical resource");
			if (Resource.bImported && Resource.FinalAccess == ERHIAccess::None)
				return Fail("imported resource '" + Resource.Name
					+ "' has no final access");
			if (EnumHasAnyFlags(Resource.FinalAccess, ERHIAccess::Discard))
				return Fail("resource '" + Resource.Name
					+ "' has invalid final access");
			for (uint32 Other = 0; Other < ResourceIndex; ++Other)
				if (State->Resources[Other].Name == Resource.Name)
					return Fail("duplicate resource name '" + Resource.Name + "'");
		}

		const uint32 PassCount = static_cast<uint32>(State->Passes.size());
		if (PassCount > State->Budget.MaxPasses)
			return Fail("render graph budget exceeded: passes actual="
				+ std::to_string(PassCount) + " limit="
				+ std::to_string(State->Budget.MaxPasses));
		std::vector<std::vector<uint32>> Outgoing(PassCount);
		std::vector<uint32> Indegree(PassCount, 0);
		std::vector<FRenderGraphDependency> Dependencies;
		auto AddEdge = [&](uint32 Before, uint32 After, std::string Cause) {
			if (Before == After) return;
			if (std::ranges::find(Outgoing[Before], After) != Outgoing[Before].end())
				return;
			Outgoing[Before].push_back(After);
			++Indegree[After];
			Dependencies.push_back({Before, After, std::move(Cause)});
		};

		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
		{
			const auto& Pass = State->Passes[PassIndex];
			if (Pass.Name.empty())
				return Fail("pass[" + std::to_string(PassIndex) + "] has an empty name");
			for (uint32 Other = 0; Other < PassIndex; ++Other)
				if (State->Passes[Other].Name == Pass.Name)
					return Fail("duplicate pass name '" + Pass.Name + "'");
			for (uint32 Prerequisite : Pass.Prerequisites)
			{
				if (Prerequisite >= PassCount)
					return Fail("pass '" + Pass.Name + "' has an invalid prerequisite");
				AddEdge(Prerequisite, PassIndex, "explicit");
			}
			for (uint32 UseIndex = 0; UseIndex < Pass.Uses.size(); ++UseIndex)
			{
				const auto& Use = Pass.Uses[UseIndex];
				if (Use.ResourceIndex >= State->Resources.size()
					|| State->Resources[Use.ResourceIndex].Kind != Use.Kind)
					return Fail("pass '" + Pass.Name + "' has an invalid resource handle");
				const auto& Resource = State->Resources[Use.ResourceIndex];
				if (Use.Kind != EGraphResourceKind::Token
					&& (Use.Access == ERHIAccess::None
					|| EnumHasAnyFlags(Use.Access, ERHIAccess::Discard))
					)
					return Fail("pass '" + Pass.Name + "' resource '"
						+ Resource.Name + "' has invalid required access");
				if (Use.Kind != EGraphResourceKind::Token
					&& ((Use.Use == ERenderGraphUse::Read && AccessHasWrite(Use.Access))
					|| (Use.Use == ERenderGraphUse::Write && !AccessHasWrite(Use.Access)))
					)
					return Fail("pass '" + Pass.Name + "' resource '"
						+ Resource.Name + "' access disagrees with use mode");
				if (Use.bDiscard && Use.Use == ERenderGraphUse::Read)
					return Fail("pass '" + Pass.Name + "' cannot discard a read");
				std::string TransitionError;
				const bool bValid = Use.Kind == EGraphResourceKind::Token ||
					(Use.Kind == EGraphResourceKind::Texture
					? ValidateTextureTransition({Resource.Texture, Use.TextureRange,
						ERHIAccess::Discard, Use.Access}, TransitionError)
					: ValidateBufferTransition({Resource.Buffer, Use.BufferOffset,
						Use.BufferSize, ERHIAccess::Discard, Use.Access}, TransitionError));
				if (!bValid)
					return Fail("pass '" + Pass.Name + "' resource '"
						+ Resource.Name + "': " + TransitionError);
				for (uint32 OtherUse = 0; OtherUse < UseIndex; ++OtherUse)
					if (RangesOverlap(Use, Pass.Uses[OtherUse]))
						return Fail("pass '" + Pass.Name + "' declares overlapping uses of resource '"
							+ Resource.Name + "'");
			}
		}

		for (uint32 Before = 0; Before < PassCount; ++Before)
			for (uint32 After = Before + 1; After < PassCount; ++After)
				for (const FGraphUse& A : State->Passes[Before].Uses)
					for (const FGraphUse& B : State->Passes[After].Uses)
					{
						if (!RangesOverlap(A, B)) continue;
						if (!RangesEqual(A, B))
							return Fail("resource '" + State->Resources[A.ResourceIndex].Name
								+ "' has partially overlapping declarations");
						if (IsWriteUse(A.Use) || IsWriteUse(B.Use))
							AddEdge(Before, After,
								State->Resources[A.ResourceIndex].Name);
					}

		std::vector<FRangeState> DeclaredStates;
		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
			for (const FGraphUse& Use : State->Passes[PassIndex].Uses)
			{
				const auto& Resource = State->Resources[Use.ResourceIndex];
				auto Found = std::ranges::find_if(DeclaredStates,
					[&](const FRangeState& Range) { return RangesEqual(Range.Use, Use); });
				if (Found == DeclaredStates.end())
				{
					FRangeState Initial;
					Initial.Use = Use;
					Initial.Access = Resource.InitialAccess;
					Initial.bProduced = Resource.bImported;
					DeclaredStates.push_back(Initial);
					Found = std::prev(DeclaredStates.end());
				}
				if (Use.Use != ERenderGraphUse::Write && !Use.bDiscard
					&& !Found->bProduced)
					return Fail("pass '" + State->Passes[PassIndex].Name
						+ "' reads resource '" + Resource.Name
						+ "' before its producer");
				Found->bProduced = Use.bStore
					&& (Found->bProduced || IsWriteUse(Use.Use));
			}

		std::vector<uint32> Order;
		Order.reserve(PassCount);
		std::vector<bool> Emitted(PassCount, false);
		while (Order.size() < PassCount)
		{
			uint32 Selected = PassCount;
			for (uint32 Index = 0; Index < PassCount; ++Index)
				if (!Emitted[Index] && Indegree[Index] == 0)
				{
					Selected = Index;
					break;
				}
			if (Selected == PassCount)
				return Fail("graph contains a dependency cycle");
			Emitted[Selected] = true;
			Order.push_back(Selected);
			for (uint32 After : Outgoing[Selected]) --Indegree[After];
		}

		std::vector<bool> Retained(PassCount, !State->bEnableCulling);
		if (State->bEnableCulling)
		{
			std::vector<std::vector<uint32>> Incoming(PassCount);
			for (uint32 Before = 0; Before < PassCount; ++Before)
				for (uint32 After : Outgoing[Before]) Incoming[After].push_back(Before);
			std::vector<uint32> Pending;
			for (uint32 Index = 0; Index < PassCount; ++Index)
				if (State->Passes[Index].bRoot)
				{
					Retained[Index] = true;
					Pending.push_back(Index);
				}
			while (!Pending.empty())
			{
				const uint32 After = Pending.back();
				Pending.pop_back();
				for (uint32 Before : Incoming[After])
					if (!Retained[Before])
					{
						Retained[Before] = true;
						Pending.push_back(Before);
					}
			}
		}

		auto CompiledState = std::make_unique<FCompiledRenderGraph::FState>();
		CompiledState->Owner = State->Owner;
		CompiledState->Resources = State->Resources;
		CompiledState->Dependencies = std::move(Dependencies);
		CompiledState->Prepare = State->Prepare;
		CompiledState->Budget = State->Budget;
		CompiledState->ResourceLifetimes.reserve(State->Resources.size());
		for (const auto& Resource : State->Resources)
			CompiledState->ResourceLifetimes.push_back({Resource.Name,
				std::numeric_limits<uint32>::max(), 0, Resource.bImported, true});
		for (uint32 Index = 0; Index < PassCount; ++Index)
			CompiledState->CullingDecisions.push_back({State->Passes[Index].Name,
				!Retained[Index], Retained[Index]
					? (State->Passes[Index].bRoot
						? State->Passes[Index].RootReason : "dependency")
					: "unreachable from an explicit root"});
		std::vector<FRangeState> RangeStates;
		for (uint32 ScheduledIndex : Order)
		{
			if (!Retained[ScheduledIndex]) continue;
			const auto& Pass = State->Passes[ScheduledIndex];
			FCompiledRenderGraphPass CompiledPass{
				.Name = Pass.Name, .Type = Pass.Type,
				.DeclarationIndex = ScheduledIndex};
			for (const FGraphUse& Use : Pass.Uses)
			{
				auto& Lifetime =
					CompiledState->ResourceLifetimes[Use.ResourceIndex];
				const uint32 CompiledPassIndex =
					static_cast<uint32>(CompiledState->Passes.size());
				Lifetime.FirstPass = std::min(Lifetime.FirstPass, CompiledPassIndex);
				Lifetime.LastPass = CompiledPassIndex;
				Lifetime.bCulled = false;
				const auto& Resource = State->Resources[Use.ResourceIndex];
				auto Found = std::ranges::find_if(RangeStates,
					[&](const FRangeState& Range) { return RangesEqual(Range.Use, Use); });
				if (Found == RangeStates.end())
				{
					FRangeState Initial;
					Initial.Use = Use;
					Initial.Access = Resource.InitialAccess;
					Initial.bProduced = Resource.bImported;
					RangeStates.push_back(Initial);
					Found = std::prev(RangeStates.end());
				}
				if (Use.Use != ERenderGraphUse::Write && !Use.bDiscard
					&& !Found->bProduced)
					return Fail("pass '" + Pass.Name + "' reads resource '"
						+ Resource.Name + "' before its producer");
				const ERHIAccess Before = Use.bDiscard
					? ERHIAccess::Discard : Found->Access;
				if (Use.Kind == EGraphResourceKind::Token)
				{
					Found->Access = ERHIAccess::None;
					Found->bProduced = Found->bProduced || IsWriteUse(Use.Use);
					Found->LastPass = ScheduledIndex;
					continue;
				}
				if (Before != Use.Access || Use.bDiscard)
				{
					if (Use.Kind == EGraphResourceKind::Texture)
					{
						const FRHITextureTransition Transition{Resource.Texture,
							Use.TextureRange, Before, Use.Access};
						std::string Error;
						if (!ValidateTextureTransition(Transition, Error))
							return Fail("pass '" + Pass.Name + "' resource '"
								+ Resource.Name + "': " + Error);
						CompiledPass.TextureTransitions.push_back(Transition);
					}
					else
					{
						const FRHIBufferTransition Transition{Resource.Buffer,
							Use.BufferOffset, Use.BufferSize, Before, Use.Access};
						std::string Error;
						if (!ValidateBufferTransition(Transition, Error))
							return Fail("pass '" + Pass.Name + "' resource '"
								+ Resource.Name + "': " + Error);
						CompiledPass.BufferTransitions.push_back(Transition);
					}
				}
				Found->Access = Use.bStore ? Use.Access : ERHIAccess::Discard;
				Found->bProduced = Use.bStore
					&& (Found->bProduced || IsWriteUse(Use.Use));
				Found->LastPass = ScheduledIndex;
			}
			CompiledState->Passes.push_back(std::move(CompiledPass));
			CompiledState->ExecuteCallbacks.push_back(Pass.Execute);
		}

		for (const FRangeState& Range : RangeStates)
		{
			const auto& Resource = State->Resources[Range.Use.ResourceIndex];
			if (Range.Use.Kind == EGraphResourceKind::Token) continue;
			if (Resource.FinalAccess == ERHIAccess::None
				|| Resource.FinalAccess == Range.Access) continue;
			if (Range.Use.Kind == EGraphResourceKind::Texture)
			{
				const FRHITextureTransition Transition{Resource.Texture,
					Range.Use.TextureRange, Range.Access, Resource.FinalAccess};
				std::string Error;
				if (!ValidateTextureTransition(Transition, Error))
					return Fail("resource '" + Resource.Name
						+ "' final transition: " + Error);
				CompiledState->FinalTextureTransitions.push_back(Transition);
			}
			else
			{
				const FRHIBufferTransition Transition{Resource.Buffer,
					Range.Use.BufferOffset, Range.Use.BufferSize, Range.Access,
					Resource.FinalAccess};
				std::string Error;
				if (!ValidateBufferTransition(Transition, Error))
					return Fail("resource '" + Resource.Name
						+ "' final transition: " + Error);
				CompiledState->FinalBufferTransitions.push_back(Transition);
			}
		}
		std::ranges::sort(CompiledState->Dependencies,
			[](const auto& A, const auto& B) {
				return std::tie(A.BeforePass, A.AfterPass, A.Cause)
					< std::tie(B.BeforePass, B.AfterPass, B.Cause);
			});
		CompiledState->CompileMicroseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - Started).count());
		uint32 BufferTransitionCount = static_cast<uint32>(
			CompiledState->FinalBufferTransitions.size());
		uint32 TextureTransitionCount = static_cast<uint32>(
			CompiledState->FinalTextureTransitions.size());
		for (const auto& Pass : CompiledState->Passes)
		{
			BufferTransitionCount += static_cast<uint32>(Pass.BufferTransitions.size());
			TextureTransitionCount += static_cast<uint32>(Pass.TextureTransitions.size());
		}
		auto CheckLimit = [&](std::string_view Name, uint32 Actual, uint32 Limit)
			-> FRenderGraphCompileResult {
			return Fail("render graph budget exceeded: " + std::string(Name)
				+ " actual=" + std::to_string(Actual) + " limit="
				+ std::to_string(Limit));
		};
		if (CompiledState->Dependencies.size() > State->Budget.MaxDependencies)
			return CheckLimit("dependencies",
				static_cast<uint32>(CompiledState->Dependencies.size()),
				State->Budget.MaxDependencies);
		if (BufferTransitionCount > State->Budget.MaxBufferTransitions)
			return CheckLimit("buffer-transitions", BufferTransitionCount,
				State->Budget.MaxBufferTransitions);
		if (TextureTransitionCount > State->Budget.MaxTextureTransitions)
			return CheckLimit("texture-transitions", TextureTransitionCount,
				State->Budget.MaxTextureTransitions);
		return {std::unique_ptr<FCompiledRenderGraph>(
			new FCompiledRenderGraph(std::move(CompiledState))), {}};
	}

	FCompiledRenderGraph::FCompiledRenderGraph(std::unique_ptr<FState> InState)
		: State(std::move(InState))
	{
	}

	FCompiledRenderGraph::FCompiledRenderGraph(FCompiledRenderGraph&&) noexcept = default;
	auto FCompiledRenderGraph::operator=(FCompiledRenderGraph&&) noexcept
		-> FCompiledRenderGraph& = default;
	FCompiledRenderGraph::~FCompiledRenderGraph() = default;

	auto FCompiledRenderGraph::GetPasses() const
		-> std::span<const FCompiledRenderGraphPass> { return State->Passes; }
	auto FCompiledRenderGraph::GetDependencies() const
		-> std::span<const FRenderGraphDependency> { return State->Dependencies; }
	auto FCompiledRenderGraph::GetResourceLifetimes() const
		-> std::span<const FRenderGraphResourceLifetime>
	{
		return State->ResourceLifetimes;
	}
	auto FCompiledRenderGraph::GetCullingDecisions() const
		-> std::span<const FRenderGraphCullingDecision>
	{
		return State->CullingDecisions;
	}
	auto FCompiledRenderGraph::GetFinalBufferTransitions() const
		-> std::span<const FRHIBufferTransition> { return State->FinalBufferTransitions; }
	auto FCompiledRenderGraph::GetFinalTextureTransitions() const
		-> std::span<const FRHITextureTransition> { return State->FinalTextureTransitions; }
	auto FCompiledRenderGraph::GetCompileMicroseconds() const -> uint64
	{
		return State->CompileMicroseconds;
	}

	auto FCompiledRenderGraph::GetStatistics() const -> FRenderGraphStatistics
	{
		FRenderGraphStatistics Result;
		Result.DeclaredPasses = static_cast<uint32>(State->CullingDecisions.size());
		Result.ScheduledPasses = static_cast<uint32>(State->Passes.size());
		Result.CulledPasses = Result.DeclaredPasses - Result.ScheduledPasses;
		Result.Dependencies = static_cast<uint32>(State->Dependencies.size());
		Result.BufferTransitions = static_cast<uint32>(
			State->FinalBufferTransitions.size());
		Result.TextureTransitions = static_cast<uint32>(
			State->FinalTextureTransitions.size());
		for (const auto& Pass : State->Passes)
		{
			Result.BufferTransitions += static_cast<uint32>(Pass.BufferTransitions.size());
			Result.TextureTransitions += static_cast<uint32>(Pass.TextureTransitions.size());
		}
		Result.CompileMicroseconds = State->CompileMicroseconds;
		Result.ExecuteMicroseconds =
			State->ExecuteMicroseconds.load(std::memory_order_relaxed);
		Result.bCompileBudgetExceeded = Result.CompileMicroseconds
			> State->Budget.MaxCompileMicroseconds;
		Result.bExecuteBudgetExceeded = Result.ExecuteMicroseconds
			> State->Budget.MaxExecuteMicroseconds;
		return Result;
	}

	auto FCompiledRenderGraph::Capture() const -> FRenderGraphCapture
	{
		FRenderGraphCapture Result;
		Result.Statistics = GetStatistics();
		Result.Dependencies = State->Dependencies;
		Result.ResourceLifetimes = State->ResourceLifetimes;
		Result.CullingDecisions = State->CullingDecisions;
		Result.Dump = Dump();
		Result.Passes.reserve(State->Passes.size());
		for (const auto& Pass : State->Passes)
			Result.Passes.push_back({Pass.Name, Pass.Type, Pass.DeclarationIndex,
				static_cast<uint32>(Pass.BufferTransitions.size()),
				static_cast<uint32>(Pass.TextureTransitions.size())});
		return Result;
	}

	auto FCompiledRenderGraph::Dump() const -> std::string
	{
		std::ostringstream Output;
		Output << "render-graph passes=" << State->Passes.size()
			<< " edges=" << State->Dependencies.size() << '\n';
		for (uint32 Index = 0; Index < State->Passes.size(); ++Index)
		{
			const auto& Pass = State->Passes[Index];
			Output << "pass " << Index << " decl=" << Pass.DeclarationIndex
				<< " type=" << PassTypeName(Pass.Type) << " name=" << Pass.Name
				<< " buffers=" << Pass.BufferTransitions.size()
				<< " textures=" << Pass.TextureTransitions.size() << '\n';
		}
		for (const auto& Edge : State->Dependencies)
			Output << "edge " << Edge.BeforePass << "->" << Edge.AfterPass
				<< " cause=" << Edge.Cause << '\n';
		Output << "final buffers=" << State->FinalBufferTransitions.size()
			<< " textures=" << State->FinalTextureTransitions.size() << '\n';
		for (const auto& Lifetime : State->ResourceLifetimes)
			Output << "lifetime name=" << Lifetime.Name << " first="
				<< Lifetime.FirstPass << " last=" << Lifetime.LastPass
				<< " imported=" << Lifetime.bImported
				<< " culled=" << Lifetime.bCulled << '\n';
		for (const auto& Decision : State->CullingDecisions)
			Output << "culling name=" << Decision.Name << " culled="
				<< Decision.bCulled << " reason=" << Decision.Reason << '\n';
		return Output.str();
	}

	auto FCompiledRenderGraph::Execute(FRHICommandListImmediate& CommandList,
		std::string* OutError) const -> bool
	{
		const auto Started = std::chrono::steady_clock::now();
		auto RecordDuration = [&] {
			State->ExecuteMicroseconds.store(static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - Started).count()),
				std::memory_order_relaxed);
		};
		if (State->Prepare)
		{
			std::string Error;
			if (!State->Prepare(Error))
			{
				if (OutError != nullptr) *OutError = std::move(Error);
				RecordDuration();
				return false;
			}
		}
		const FRenderGraphPassResources Resources(*this);
		for (uint32 Index = 0; Index < State->Passes.size(); ++Index)
		{
			const auto& Pass = State->Passes[Index];
			if (!Pass.BufferTransitions.empty())
				CommandList.TransitionBuffers(Pass.BufferTransitions);
			if (!Pass.TextureTransitions.empty())
				CommandList.TransitionTextures(Pass.TextureTransitions);
			if (State->ExecuteCallbacks[Index])
				State->ExecuteCallbacks[Index](CommandList, Resources);
		}
		if (!State->FinalBufferTransitions.empty())
			CommandList.TransitionBuffers(State->FinalBufferTransitions);
		if (!State->FinalTextureTransitions.empty())
			CommandList.TransitionTextures(State->FinalTextureTransitions);
		if (OutError != nullptr) OutError->clear();
		RecordDuration();
		return true;
	}

	auto FRenderGraphPassResources::GetTexture(
		FRenderGraphTextureHandle Handle) const -> FRHITexture*
	{
		if (Handle.Owner != Graph.State->Owner
			|| Handle.Index >= Graph.State->Resources.size()) return nullptr;
		const auto& Resource = Graph.State->Resources[Handle.Index];
		return Resource.Kind == EGraphResourceKind::Texture
			? Resource.Texture : nullptr;
	}

	auto FRenderGraphPassResources::GetBuffer(
		FRenderGraphBufferHandle Handle) const -> FRHIBuffer*
	{
		if (Handle.Owner != Graph.State->Owner
			|| Handle.Index >= Graph.State->Resources.size()) return nullptr;
		const auto& Resource = Graph.State->Resources[Handle.Index];
		return Resource.Kind == EGraphResourceKind::Buffer
			? Resource.Buffer : nullptr;
	}
} // namespace Durin

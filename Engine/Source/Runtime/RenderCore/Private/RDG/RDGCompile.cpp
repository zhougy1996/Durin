#include <format>
#include "RDGBuilderInternal.h"
#include "Misc/Time.h"
#include "Profiling/Profiling.h"

namespace Durin::RDGPrivate
{
	namespace
	{
		// Exists only while constructing dependency edges in declaration order.
		struct FDependencyCellState final
		{
			bool bProduced = false;
			uint32 Producer = std::numeric_limits<uint32>::max();
			std::vector<uint32> Readers;
		};
		// Owns canonical typed edges and endpoint deduplication during analysis.
		struct FDependencyGraph final
		{
			std::vector<FRDGDependency> Dependencies;
			std::unordered_map<uint64, size_t> EdgeIndices;
			uint32 MaxDependencies = 0;
		};
		// Initialization coverage is independent of whole-buffer synchronization.
		// Disjoint initialized intervals stay sorted and adjacent intervals coalesce.
		struct FBufferContentCoverage final
		{
			std::map<uint64, uint64> Intervals;

			auto Contains(uint64 Begin, uint64 End) const -> bool
			{
				auto It = Intervals.upper_bound(Begin);
				return It != Intervals.begin() && std::prev(It)->second >= End;
			}

			auto Include(uint64 Begin, uint64 End, FRangeWork& Work) -> FRDGLimitResult
			{
				if (Contains(Begin, End)) return {};
				auto It = Intervals.lower_bound(Begin);
				if (It != Intervals.begin() && std::prev(It)->second >= Begin) --It;
				while (It != Intervals.end() && It->first <= End)
				{
					if (!Work.Visit()) return std::unexpected(Work.Error());
					Begin = std::min(Begin, It->first);
					End = std::max(End, It->second);
					It = Intervals.erase(It);
				}
				Intervals.emplace_hint(It, Begin, End);
				return {};
			}
		};
		// Keep subresource analysis exact; compact only consecutive barriers with
		// identical submission provenance. No barrier is moved across another use.
		auto CompactTextureBarriers(std::span<FRDGCompiledPass> Passes,
			FRDGBarrierBatch& FinalBarriers, FRDGExecutionPlan& Execution) -> void;
		auto AccessHasWrite(ERHIAccess Access) -> bool;
		auto IsAccessAllowed(ERDGPassType Type, ERHIAccess Access) -> bool;
		auto IsExportAccessAllowed(ERDGResourceKind Kind, ERHIAccess Access) -> bool;
		auto BufferRangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool;
		auto TextureRangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool;
		auto RangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool;
		auto RangesEqual(const FGraphUse& A, const FGraphUse& B) -> bool;
		auto UseContext(const FGraphPass& Pass, const FGraphUse& Use,
			const FGraphResource* Resource = nullptr, uint32 UseIndex = UINT32_MAX,
			uint32 OtherUseIndex = UINT32_MAX, std::string_view OtherPath = {}) -> FRDGUseErrorContext;
		auto AddDependencyEdge(FDependencyGraph& Graph, uint32 Before,
			uint32 After, const std::string& Cause, ERDGDependencyKind Kind)
			-> std::expected<void, std::variant<FRDGDependencyError, FRDGLimitError>>;
		auto ValidateGraphResources(std::span<const FGraphResource> Resources)
			-> std::expected<void, FRDGIdentityError>;
		auto ValidateGraphPasses(FGraphPassView Passes,
			std::span<const FGraphResource> Resources,
			FDependencyGraph& Graph) -> FRDGCompileResult;
		auto ValidateTypedValueWriters(
			std::span<const FGraphResource> Resources,
			const FResourceUseTable& ResourceUses) -> std::expected<void, FRDGIdentityError>;
		auto ValidateBufferContents(FGraphPassView Passes,
			std::span<const FGraphResource> Resources, FRangeWork& Work) -> FRDGCompileResult;
		auto BuildHazardDependencies(FGraphPassView Passes,
			std::span<const FGraphResource> Resources,
			const FTrackingLayout& Cells, FDependencyGraph& Graph, FRangeWork& Work)
			-> FRDGCompileResult;
		auto FindRetainedPasses(FGraphPassView Passes,
			std::span<const FRDGDependency> Dependencies, bool bEnableCulling)
			-> std::vector<bool>;

		auto CompactTextureBarriers(std::span<FRDGCompiledPass> Passes,
			FRDGBarrierBatch& FinalBarriers, FRDGExecutionPlan& Execution) -> void
		{
			struct FEntry
			{
				FRDGResourceHandoff Handoff;
				FRDGTextureTransition Texture;
			};
			auto Batch = [&](uint32 Index) -> FRDGBarrierBatch& {
				return Index == Passes.size() ? FinalBarriers : Passes[Index].Barriers;
			};
			std::vector<FEntry> Entries;
			Entries.reserve(Execution.Handoffs.size());
			for (auto& Handoff : Execution.Handoffs)
			{
				const auto Texture = Handoff.bTexture
					? Batch(Handoff.Consumer.Index).GetTextureTransitions()[Handoff.TransitionIndex]
					: FRDGTextureTransition{};
				Entries.push_back({std::move(Handoff), Texture});
			}
			for (const bool bLayers : {true, false})
			{
				size_t Count = 0;
				for (size_t Index = 0; Index < Entries.size(); ++Index)
				{
					auto& Entry = Entries[Index];
					if (Count != 0 && Entry.Handoff.bTexture)
					{
						auto& Previous = Entries[Count - 1];
						auto& A = Previous.Texture;
						const auto& B = Entry.Texture;
						const bool bCompatible = Previous.Handoff.bTexture
							&& Previous.Handoff.Consumer == Entry.Handoff.Consumer
							&& Previous.Handoff.SourceQueue == Entry.Handoff.SourceQueue
							&& Previous.Handoff.Producers == Entry.Handoff.Producers
							&& A.ResourceId == B.ResourceId && A.ExpectedBefore == B.ExpectedBefore
							&& A.RequiredAfter == B.RequiredAfter && A.bDiscardContents == B.bDiscardContents
							&& A.Range.Aspects == B.Range.Aspects;
						if (bCompatible && (bLayers
							? A.Range.FirstMip == B.Range.FirstMip && A.Range.NumMips == B.Range.NumMips
								&& A.Range.FirstArrayLayer + A.Range.NumArrayLayers == B.Range.FirstArrayLayer
							: A.Range.FirstArrayLayer == B.Range.FirstArrayLayer
								&& A.Range.NumArrayLayers == B.Range.NumArrayLayers
								&& A.Range.FirstMip + A.Range.NumMips == B.Range.FirstMip))
						{
							if (bLayers) A.Range.NumArrayLayers += B.Range.NumArrayLayers;
							else A.Range.NumMips += B.Range.NumMips;
							continue;
						}
					}
					if (Count != Index) Entries[Count] = std::move(Entry);
					++Count;
				}
				Entries.resize(Count);
			}
			std::vector<FRDGBarrierBatch> Batches(Passes.size() + 1);
			Execution.Handoffs.clear();
			for (auto& Entry : Entries)
			{
				auto& Handoff = Entry.Handoff;
				auto& Destination = Batches[Handoff.Consumer.Index];
				if (Handoff.bTexture)
				{
					Handoff.TransitionIndex = static_cast<uint32>(Destination.GetTextureTransitions().size());
					Destination.AddTransition(Entry.Texture);
				}
				else
				{
					const auto Transition = Batch(Handoff.Consumer.Index).GetBufferTransitions()[Handoff.TransitionIndex];
					Handoff.TransitionIndex = static_cast<uint32>(Destination.GetBufferTransitions().size());
					Destination.AddTransition(Transition);
				}
				Execution.Handoffs.push_back(std::move(Handoff));
			}
			for (uint32 Index = 0; Index < Batches.size(); ++Index)
				Batch(Index) = std::move(Batches[Index]);
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

		auto IsAccessAllowed(ERDGPassType Type, ERHIAccess Access) -> bool
		{
			ERHIAccess Allowed = ERHIAccess::None;
			switch (Type)
			{
			case ERDGPassType::Graphics:
				Allowed = ERHIAccess::VertexBufferRead | ERHIAccess::IndexBufferRead
					| ERHIAccess::GraphicsUniformRead | ERHIAccess::GraphicsShaderRead
					| ERHIAccess::ColorAttachmentReadWrite
					| ERHIAccess::DepthStencilReadWrite
					| ERHIAccess::GraphicsShaderReadWrite;
				break;
			case ERDGPassType::Compute:
				Allowed = ERHIAccess::ComputeUniformRead | ERHIAccess::ComputeShaderRead
					| ERHIAccess::ComputeShaderReadWrite;
				break;
			case ERDGPassType::Copy:
				Allowed = ERHIAccess::TransferRead | ERHIAccess::TransferWrite;
				break;
			}
			return !EnumHasAnyFlags(Access, static_cast<ERHIAccess>(
				static_cast<uint32>(~static_cast<uint32>(Allowed))));
		}

		auto IsExportAccessAllowed(ERDGResourceKind Kind, ERHIAccess Access) -> bool
		{
			constexpr ERHIAccess Allowed = ERHIAccess::VertexBufferRead
				| ERHIAccess::IndexBufferRead | ERHIAccess::GraphicsUniformRead
				| ERHIAccess::ComputeUniformRead | ERHIAccess::GraphicsShaderRead
				| ERHIAccess::ComputeShaderRead | ERHIAccess::TransferRead
				| ERHIAccess::HostRead | ERHIAccess::ColorAttachmentReadWrite
				| ERHIAccess::DepthStencilReadWrite | ERHIAccess::GraphicsShaderReadWrite
				| ERHIAccess::ComputeShaderReadWrite | ERHIAccess::TransferWrite
				| ERHIAccess::HostWrite | ERHIAccess::Present;
			const ERHIAccess TextureOnly = ERHIAccess::Present
				| ERHIAccess::ColorAttachmentReadWrite | ERHIAccess::DepthStencilReadWrite;
			const ERHIAccess BufferOnly = ERHIAccess::VertexBufferRead
				| ERHIAccess::IndexBufferRead | ERHIAccess::GraphicsUniformRead
				| ERHIAccess::ComputeUniformRead;
			return Access != ERHIAccess::None && EnumHasAllFlags(Allowed, Access)
				&& !EnumHasAnyFlags(Access,
					Kind == ERDGResourceKind::Texture ? BufferOnly : TextureOnly);
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
				&& (A.Kind == ERDGResourceKind::Token
					|| (A.Kind == ERDGResourceKind::Texture
						? TextureRangesOverlap(A, B) : BufferRangesOverlap(A, B)));
		}

		auto RangesEqual(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			if (A.ResourceIndex != B.ResourceIndex || A.Kind != B.Kind)
				return false;
			return A.Kind == ERDGResourceKind::Token
				? true : A.Kind == ERDGResourceKind::Texture
				? A.TextureRange == B.TextureRange
				: A.BufferOffset == B.BufferOffset && A.BufferSize == B.BufferSize;
		}

		auto UseContext(const FGraphPass& Pass, const FGraphUse& Use,
			const FGraphResource* Resource, uint32 UseIndex,
			uint32 OtherUseIndex, std::string_view OtherPath) -> FRDGUseErrorContext
		{
			return {.PassName = Pass.Name, .ResourceName = Resource ? Resource->Name : "",
				.ParameterPath = std::string(Use.ParameterPath), .OtherParameterPath = std::string(OtherPath),
				.Kind = Use.Kind, .PassType = Pass.Type, .Use = Use.Use,
				.ResourceIndex = Use.ResourceIndex, .UseIndex = UseIndex, .OtherUseIndex = OtherUseIndex,
				.Access = Use.Access, .ResultAccess = Use.ResultAccess,
				.BufferOffset = Use.BufferOffset, .BufferSize = Use.BufferSize,
				.BufferCapacity = Resource ? Resource->BufferDesc.Size : 0,
				.TextureRange = Use.TextureRange,
				.TextureMips = Resource ? Resource->TextureDesc.NumMips : 0u,
				.TextureLayers = Resource ? Resource->TextureDesc.ArraySize : 0u};
		}

		auto AddDependencyEdge(FDependencyGraph& Graph, uint32 Before,
			uint32 After, const std::string& Cause, ERDGDependencyKind Kind)
			-> std::expected<void, std::variant<FRDGDependencyError, FRDGLimitError>>
		{
			if (Before >= After)
			{
				return std::unexpected(FRDGDependencyError{ERDGDependencyError::DependencyNotForward, Before, After});
			}
			const uint64 Key = (static_cast<uint64>(Before) << 32) | After;
			const auto Found = Graph.EdgeIndices.find(Key);
			if (Found != Graph.EdgeIndices.end())
			{
				auto& Existing = Graph.Dependencies[Found->second];
				if (Existing.Kind == ERDGDependencyKind::Execution
					&& Kind != ERDGDependencyKind::Execution)
				{
					Existing.Kind = Kind;
					Existing.Cause = Cause;
				}
				return {};
			}
			if (Graph.Dependencies.size() >= Graph.MaxDependencies)
			{
				return std::unexpected(FRDGLimitError{ERDGLimit::Dependencies, Graph.Dependencies.size() + 1,
					Graph.MaxDependencies});
			}
			Graph.EdgeIndices.emplace(Key, Graph.Dependencies.size());
			Graph.Dependencies.push_back({Before, After, Cause, Kind});
			return {};
		}

		auto ValidateGraphResources(std::span<const FGraphResource> Resources)
			-> std::expected<void, FRDGIdentityError>
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.ValidateGraphResources");
			std::unordered_set<std::string_view> Names;
			Names.reserve(Resources.size());
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size();
				++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				if (Resource.Name.empty())
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::ResourceNameEmpty,
						FRDGIdentityErrorContext{.Name = Resource.Name,
							.Index = ResourceIndex,
							.Actual = static_cast<uint64>(Resource.FinalAccess)}});
				if (Resource.bExternal && !Resource.Texture && !Resource.Buffer)
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::PhysicalResourceMissing,
						FRDGIdentityErrorContext{.Name = Resource.Name,
							.Index = ResourceIndex,
							.Actual = static_cast<uint64>(Resource.FinalAccess)}});
				if (Resource.bExternal && Resource.FinalAccess == ERHIAccess::None)
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::ExternalFinalAccessMissing,
						FRDGIdentityErrorContext{.Name = Resource.Name,
							.Index = ResourceIndex,
							.Actual = static_cast<uint64>(Resource.FinalAccess)}});
				if (EnumHasAnyFlags(Resource.FinalAccess, ERHIAccess::Discard))
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::FinalAccessInvalid,
						FRDGIdentityErrorContext{.Name = Resource.Name,
							.Index = ResourceIndex,
							.Actual = static_cast<uint64>(Resource.FinalAccess)}});
				if (!Names.insert(Resource.Name).second)
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::ResourceNameDuplicate,
						FRDGIdentityErrorContext{.Name = Resource.Name,
							.Index = ResourceIndex,
							.Actual = static_cast<uint64>(Resource.FinalAccess)}});
			}
			return {};
		}

		auto ValidateGraphPasses(FGraphPassView Passes,
			std::span<const FGraphResource> Resources,
			FDependencyGraph& Graph) -> FRDGCompileResult
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.ValidateGraphPasses");
			std::unordered_set<std::string_view> Names;
			Names.reserve(Passes.size());
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
			{
				const auto& Pass = Passes[PassIndex];
				if (Pass.Name.empty())
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::PassNameEmpty,
						FRDGIdentityErrorContext{.Name = Pass.Name, .Index = PassIndex}});
				if (!Names.insert(Pass.Name).second)
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::PassNameDuplicate,
						FRDGIdentityErrorContext{.Name = Pass.Name, .Index = PassIndex}});
				for (uint32 Prerequisite : Pass.Prerequisites)
				{
					if (Prerequisite >= Passes.size())
						return std::unexpected(FRDGDependencyError{ERDGDependencyError::ProducerHandleInvalid, Prerequisite, PassIndex});
					if (auto Error = AddDependencyEdge(Graph, Prerequisite, PassIndex, "explicit",
						ERDGDependencyKind::Explicit); !Error.has_value()) return Error;
				}
				if (!Pass.bDeclarationsValidated)
					if (auto Error = ValidatePassDeclarations(Pass, Resources); !Error.has_value())
					{
						if (auto* Context = &Error.error().Context) Context->PassIndex = PassIndex;
						return Error;
					}
			}
			return {};
		}

		auto ValidateTypedValueWriters(
			std::span<const FGraphResource> Resources,
			const FResourceUseTable& ResourceUses) -> std::expected<void, FRDGIdentityError>
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.ValidateTypedValueWriters");
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size();
				++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				if (Resource.ValueTypeIdentity == nullptr) continue;
				const size_t Writers = std::ranges::count_if(
					ResourceUses[ResourceIndex], [](const FGraphUse* Use) {
						return Use->Use == ERDGUse::Write;
					});
				if (Writers != 1)
					return std::unexpected(FRDGIdentityError{ERDGIdentityError::ValueWriterCount,
						FRDGIdentityErrorContext{.Name = Resource.Name,
							.TypeName = Resource.ValueTypeName,
							.Expected = 1,
							.Actual = Writers}});
			}
			return {};
		}

		auto ValidateBufferContents(FGraphPassView Passes,
			std::span<const FGraphResource> Resources, FRangeWork& Work) -> FRDGCompileResult
		{
			std::unordered_map<uint32, FBufferContentCoverage> Coverage;
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
			{
				const auto& Pass = Passes[PassIndex];
				// Every input must exist before this pass; sibling output declarations
				// cannot initialize an input merely by appearing earlier in metadata.
				for (const auto& Use : Pass.Uses)
				{
					if (Use.Kind != ERDGResourceKind::Buffer) continue;
					if (!Work.Visit()) return std::unexpected(Work.Error());
					const auto& Resource = Resources[Use.ResourceIndex];
					auto [It, bInserted] = Coverage.try_emplace(Use.ResourceIndex);
					if (bInserted && Resource.HasInitialContents())
						It->second.Intervals.emplace(0, Resource.BufferDesc.Size);
					if (Use.Use != ERDGUse::Write && !Use.bDiscard
						&& !It->second.Contains(Use.BufferOffset, Use.BufferOffset + Use.BufferSize))
						{
							auto Context = UseContext(Pass, Use, &Resource);
							Context.PassIndex = PassIndex;
							return std::unexpected(FRDGUseError{ERDGUseError::BufferProducerMissing, std::move(Context)});
						}
				}
				for (const auto& Use : Pass.Uses)
					if (Use.Kind == ERDGResourceKind::Buffer && IsWriteUse(Use.Use))
						if (auto Error = Coverage.at(Use.ResourceIndex).Include(
							Use.BufferOffset, Use.BufferOffset + Use.BufferSize, Work); !Error.has_value()) return Error;
			}
			return {};
		}

		auto BuildHazardDependencies(FGraphPassView Passes,
			std::span<const FGraphResource> Resources,
			const FTrackingLayout& Cells, FDependencyGraph& Graph, FRangeWork& Work)
			-> FRDGCompileResult
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.BuildHazardDependencies");
			if (auto Error = ValidateBufferContents(Passes, Resources, Work); !Error.has_value()) return Error;
			std::vector<FDependencyCellState> States(Cells.Ranges.size());
			for (size_t Index = 0; Index < Cells.Ranges.size(); ++Index)
				States[Index].bProduced = Resources[Cells.Ranges[Index].ResourceIndex].HasInitialContents();
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
				for (const auto& Use : BuildTrackingUses(Passes[PassIndex].Uses).Uses)
					if (auto Error = Cells.VisitUse(Use, [&](size_t CellIndex, const FRangeCell&) -> FRDGCompileResult
					{
						auto& Cell = States[CellIndex];
						if (!Work.Visit()) return std::unexpected(Work.Error());
						const auto& Resource = Resources[Use.ResourceIndex];
						if (Use.Kind != ERDGResourceKind::Buffer && Use.Use != ERDGUse::Write && !Use.bDiscard
							&& !Cell.bProduced)
							{
								auto Context = UseContext(Passes[PassIndex], Use, &Resource);
								Context.PassIndex = PassIndex;
								return std::unexpected(FRDGUseError{ERDGUseError::ResourceProducerMissing, std::move(Context)});
							}
						if (Use.Use == ERDGUse::Read)
						{
							if (Cell.Producer != std::numeric_limits<uint32>::max())
								if (auto Error = AddDependencyEdge(Graph, Cell.Producer, PassIndex,
									Resource.Name, ERDGDependencyKind::Value); !Error.has_value()) return Error;
							// Passes are visited in declaration order; repeated reads
							// by this pass can only be the last reader.
							if (Cell.Readers.empty() || Cell.Readers.back() != PassIndex)
								Cell.Readers.push_back(PassIndex);
							return {};
						}
						// Resource-level buffer writes preserve earlier producers: a
						// partial write cannot prove that their contents are dead. An
						// explicit full discard can, without dropping WAR/WAW ordering.
						const bool bFullBufferDiscard = Use.Kind == ERDGResourceKind::Buffer
							&& Use.bDiscard && Use.BufferOffset == 0 && Use.BufferSize == Resource.BufferDesc.Size;
						if (((Use.Use == ERDGUse::ReadWrite && !Use.bDiscard)
							|| (Use.Kind == ERDGResourceKind::Buffer && !bFullBufferDiscard))
							&& Cell.Producer != std::numeric_limits<uint32>::max())
							if (auto Error = AddDependencyEdge(Graph, Cell.Producer, PassIndex,
								Resource.Name, ERDGDependencyKind::Value); !Error.has_value()) return Error;
						for (uint32 Reader : Cell.Readers)
							if (auto Error = AddDependencyEdge(Graph, Reader, PassIndex, Resource.Name,
								ERDGDependencyKind::Execution); !Error.has_value()) return Error;
						if (Cell.Readers.empty()
							&& Cell.Producer != std::numeric_limits<uint32>::max())
							if (auto Error = AddDependencyEdge(Graph, Cell.Producer, PassIndex,
								Resource.Name, ERDGDependencyKind::Execution); !Error.has_value()) return Error;
						Cell.Producer = Use.bStore
							? PassIndex : std::numeric_limits<uint32>::max();
						Cell.bProduced = Use.bStore;
						Cell.Readers.clear();
						return {};
					}); !Error.has_value()) return Error;
			return {};
		}

		auto FindRetainedPasses(FGraphPassView Passes,
			std::span<const FRDGDependency> Dependencies, bool bEnableCulling)
			-> std::vector<bool>
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.FindRetainedPasses");
			std::vector<bool> Retained(Passes.size(), !bEnableCulling);
			if (!bEnableCulling) return Retained;

			// Index finalized kinds so Execution-to-Value upgrades propagate retention.
			std::vector<size_t> Offsets(Passes.size() + 1, 0);
			for (const auto& Edge : Dependencies)
				if (Edge.Kind != ERDGDependencyKind::Execution)
					++Offsets[Edge.AfterPass + 1];
			std::partial_sum(Offsets.begin(), Offsets.end(), Offsets.begin());
			std::vector<uint32> Predecessors(Offsets.back());
			{
				auto Cursors = Offsets;
				for (const auto& Edge : Dependencies)
					if (Edge.Kind != ERDGDependencyKind::Execution)
						Predecessors[Cursors[Edge.AfterPass]++] = Edge.BeforePass;
			}

			std::vector<uint32> Pending;
			Pending.reserve(Passes.size());
			for (uint32 Index = 0; Index < Passes.size(); ++Index)
				if (Passes[Index].bRoot)
				{
					Retained[Index] = true;
					Pending.push_back(Index);
				}
			while (!Pending.empty())
			{
				const uint32 After = Pending.back();
				Pending.pop_back();
				for (uint32 Before : std::span(Predecessors).subspan(
					Offsets[After], Offsets[After + 1] - Offsets[After]))
					if (!Retained[Before])
					{
						Retained[Before] = true;
						Pending.push_back(Before);
					}
			}
			return Retained;
		}

		}

	auto IsWriteUse(ERDGUse Use) -> bool
	{
		return Use != ERDGUse::Read;
	}

	auto NeedsRangeBarrier(const FBarrierCellState& Cell, const FGraphUse& Use) -> bool
	{
		// Equal writable access still requires a memory dependency.
		return Cell.Access != Use.Access || AccessHasWrite(Cell.Access) || Use.bDiscard;
	}

	auto AdvanceBarrierState(FBarrierCellState& Cell, const FGraphUse& Use) -> void
	{
		Cell.Access = Use.Kind == ERDGResourceKind::Token ? ERHIAccess::None
			: (Use.bPassManagedTransition ? Use.ResultAccess : Use.Access);
	}

	auto ValidatePassDeclarations(const FGraphPass& Pass,
		std::span<const FGraphResource> Resources)
		-> std::expected<void, FRDGUseError>
	{
		std::unordered_map<uint32, std::vector<uint32>> ResourceUses;
		for (uint32 UseIndex = 0; UseIndex < Pass.Uses.size(); ++UseIndex)
		{
			const auto& Use = Pass.Uses[UseIndex];
			if (Use.ResourceIndex >= Resources.size()
				|| Resources[Use.ResourceIndex].Kind != Use.Kind)
			{
				return std::unexpected(FRDGUseError{ERDGUseError::ResourceHandleInvalid, UseContext(Pass, Use, nullptr, UseIndex)});
			}
			const auto& Resource = Resources[Use.ResourceIndex];
			if (Pass.bExport && !IsExportAccessAllowed(Use.Kind, Use.Access))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::FinalAccessInvalid, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			if (Use.Kind != ERDGResourceKind::Token
				&& (Use.Access == ERHIAccess::None
					|| EnumHasAnyFlags(Use.Access, ERHIAccess::Discard)))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::RequiredAccessInvalid, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			if (Use.Kind != ERDGResourceKind::Token
				&& !Pass.bExport && !IsAccessAllowed(Pass.Type, Use.Access))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::PassAccessIncompatible, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			if (Use.Kind != ERDGResourceKind::Token
				&& !Pass.bExport && ((Use.Use == ERDGUse::Read
						&& AccessHasWrite(Use.Access))
					|| (Use.Use == ERDGUse::Write
						&& !AccessHasWrite(Use.Access))))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::UseAccessMismatch, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			if (Use.bDiscard && Use.Use == ERDGUse::Read)
			{
				return std::unexpected(FRDGUseError{ERDGUseError::ReadDiscardInvalid, UseContext(Pass, Use, nullptr, UseIndex)});
			}
			if (Use.bPassManagedTransition
				&& (Use.ResultAccess == ERHIAccess::None
					|| EnumHasAnyFlags(Use.ResultAccess, ERHIAccess::Discard)))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::ManagedResultAccessInvalid, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			if (Use.Kind == ERDGResourceKind::Buffer
				&& (Use.BufferSize == 0
					|| Use.BufferOffset > Resource.BufferDesc.Size
					|| Use.BufferSize > Resource.BufferDesc.Size
						- Use.BufferOffset))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::BufferRangeInvalid, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			if (Use.Kind == ERDGResourceKind::Texture
				&& (Use.TextureRange.Aspects == ERHITextureAspect::None
					|| Use.TextureRange.NumMips == 0
					|| Use.TextureRange.NumArrayLayers == 0
					|| Use.TextureRange.FirstMip + Use.TextureRange.NumMips
						> Resource.TextureDesc.NumMips
					|| Use.TextureRange.FirstArrayLayer
						+ Use.TextureRange.NumArrayLayers
						> Resource.TextureDesc.ArraySize
					|| !EnumHasAllFlags(
						GetTextureAspects(Resource.TextureDesc.Format),
						Use.TextureRange.Aspects)))
			{
				return std::unexpected(FRDGUseError{ERDGUseError::TextureRangeInvalid, UseContext(Pass, Use, &Resource, UseIndex)});
			}
			auto& EarlierUses = ResourceUses[Use.ResourceIndex];
			for (uint32 OtherUse : EarlierUses)
				if (RangesOverlap(Use, Pass.Uses[OtherUse]))
				{
					return std::unexpected(FRDGUseError{ERDGUseError::UsesOverlap,
						UseContext(Pass, Use, &Resource, UseIndex, OtherUse, Pass.Uses[OtherUse].ParameterPath)});
				}
			EarlierUses.push_back(UseIndex);
		}
		return {};
	}

	auto SafetyLimit(ERDGLimit Dimension, size_t Actual, size_t Limit)
		-> FRDGLimitError
	{
		return {Dimension, Actual, Limit};
	}

	auto BuildTrackingUses(std::span<const FGraphUse> Uses) -> FPassTrackingUses
	{
		FPassTrackingUses Result;
		Result.Uses.reserve(Uses.size());
		std::unordered_map<uint32, size_t> Buffers;
		for (const auto& Use : Uses)
		{
			if (Use.Kind == ERDGResourceKind::Buffer)
			{
				Result.BufferDeclarations[Use.ResourceIndex].push_back(&Use);
				const auto [It, bInserted] = Buffers.try_emplace(Use.ResourceIndex, Result.Uses.size());
				if (!bInserted)
				{
					auto& Combined = Result.Uses[It->second];
					Combined.Access |= Use.Access;
					if (Combined.Use != Use.Use) Combined.Use = ERDGUse::ReadWrite;
					Combined.bDiscard = false;
					continue;
				}
			}
			Result.Uses.push_back(Use);
		}
		return Result;
	}

	auto BuildResourceUseTable(FGraphPassView Passes,
		uint32 ResourceCount) -> FResourceUseTable
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RDG.BuildResourceUseTable");
		FResourceUseTable ResourceUses;
		ResourceUses.Offsets.resize(static_cast<size_t>(ResourceCount) + 1, 0);
		for (size_t Index = 0; Index < Passes.size(); ++Index)
			for (const auto& Use : Passes[Index].Uses)
				++ResourceUses.Offsets[Use.ResourceIndex + 1];
		std::partial_sum(ResourceUses.Offsets.begin(), ResourceUses.Offsets.end(),
			ResourceUses.Offsets.begin());
		ResourceUses.Uses.resize(ResourceUses.Offsets.back());
		auto Cursors = ResourceUses.Offsets;
		for (size_t Index = 0; Index < Passes.size(); ++Index)
			for (const auto& Use : Passes[Index].Uses)
				ResourceUses.Uses[Cursors[Use.ResourceIndex]++] = &Use;
		return ResourceUses;
	}

	auto BuildTrackingLayout(std::span<const FGraphResource> Resources,
		const FResourceUseTable& ResourceUses, FRangeWork& Work)
		-> std::expected<FTrackingLayout, FRDGLimitError>
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RDG.BuildTrackingLayout");
		FTrackingLayout Result;
		Result.Resources.resize(Resources.size());
		for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = Resources[ResourceIndex];
			if (ResourceUses[ResourceIndex].empty()) continue;
			auto& Layout = Result.Resources[ResourceIndex];
			Layout.Begin = Result.Ranges.size();
			auto Add = [&](FRHITextureSubresourceRange Range) -> FRDGLimitResult {
				if (!Work.Visit()) return std::unexpected(Work.Error());
				if (Result.Ranges.size() >= Work.Budget.MaxRangeCells)
					return std::unexpected(SafetyLimit(ERDGLimit::RangeCells, Result.Ranges.size() + 1, Work.Budget.MaxRangeCells));
				if (++Work.Candidates > Work.Budget.MaxRangeCellCandidates)
					return std::unexpected(SafetyLimit(ERDGLimit::RangeCellCandidates, Work.Candidates, Work.Budget.MaxRangeCellCandidates));
				Result.Ranges.push_back({Resource.Kind, ResourceIndex, Range, 0,
					Resource.Kind == ERDGResourceKind::Buffer ? Resource.BufferDesc.Size : 0});
				return {};
			};
			if (Resource.Kind != ERDGResourceKind::Texture)
			{
				if (auto Error = Add({}); !Error) return std::unexpected(Error.error());
				continue;
			}
			Layout.Mips = Resource.TextureDesc.NumMips;
			Layout.Layers = Resource.TextureDesc.ArraySize;
			Layout.Aspects = GetTextureAspects(Resource.TextureDesc.Format);
			for (ERHITextureAspect Aspect : {ERHITextureAspect::Color,
				ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
				if (EnumHasAnyFlags(Layout.Aspects, Aspect))
					for (uint32 Mip = 0; Mip < Layout.Mips; ++Mip)
						for (uint32 Layer = 0; Layer < Layout.Layers; ++Layer)
							if (auto Error = Add({Aspect, Mip, 1, Layer, 1}); !Error) return std::unexpected(Error.error());
		}
		return Result;
	}

}

namespace Durin
{
	using namespace RDGPrivate;

	auto FRDGBuilder::Compile() -> FRDGCompileResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RDG.Compile");
		DURIN_PROFILE_CPU_ZONE_TEXT(std::format("passes={} resources={}", State->Passes.size(), State->Resources.size()));
		FScopedMicrosecondTimer CompileTimer(State->CompileMicroseconds);
		FScopedMicrosecondTimer ValidationTimer(State->Phases.ValidationMicroseconds);
		if (State->PendingConstructions != 0)
			return std::unexpected(ERDGStateError::StorageIncomplete);
		if (!State->DeclarationErrors.empty())
			return std::unexpected(State->DeclarationErrors.front());
		if (State->Resources.size() > State->Budget.MaxResources)
			return std::unexpected(SafetyLimit(ERDGLimit::Resources, State->Resources.size(), State->Budget.MaxResources));
		size_t TotalUses = 0;
		size_t ExplicitDependencyCount = 0;
		for (const auto& Pass : State->Passes)
		{
			TotalUses += Pass.Uses.size();
			ExplicitDependencyCount += Pass.Prerequisites.size();
			if (TotalUses > State->Budget.MaxUses)
				return std::unexpected(SafetyLimit(ERDGLimit::Uses, TotalUses, State->Budget.MaxUses));
		}
		bool bHasExport = false;
		for (const auto& Resource : State->Resources)
			if (Resource.IsExported())
			{
				bHasExport = true;
				if (++TotalUses > State->Budget.MaxUses)
					return std::unexpected(SafetyLimit(ERDGLimit::Uses, TotalUses, State->Budget.MaxUses));
			}
		const size_t TotalPasses = State->Passes.size() + (bHasExport ? 1 : 0);
		if (TotalPasses > State->Budget.MaxPasses)
			return std::unexpected(SafetyLimit(ERDGLimit::Passes, TotalPasses, State->Budget.MaxPasses));
		if (auto Error = ValidateGraphResources(State->Resources);
			!Error.has_value())
			return Error;

		FGraphPass Export;
		Export.Name = "RDG.Export";
		Export.bRoot = true;
		Export.bExport = true;
		Export.RootReason = "graph output";
		for (uint32 Index = 0; Index < State->Resources.size(); ++Index)
		{
			const auto& Resource = State->Resources[Index];
			if (!Resource.IsExported()) continue;
			FGraphUse Use;
			Use.ResourceIndex = Index;
			Use.Kind = Resource.Kind;
			Use.Access = Resource.FinalAccess;
			if (Resource.Kind == ERDGResourceKind::Texture)
				Use.TextureRange = {GetTextureAspects(Resource.TextureDesc.Format),
					0, Resource.TextureDesc.NumMips, 0, Resource.TextureDesc.ArraySize};
			else
				Use.BufferSize = Resource.BufferDesc.Size;
			Export.Uses.push_back(Use);
		}
		if (!Export.Uses.empty())
		{
			while (std::ranges::any_of(State->Passes, [&](const FGraphPass& Pass) {
				return Pass.Name == Export.Name;
			}))
				Export.Name += ".Output";
		}

		const FGraphPassView Passes{State->Passes, bHasExport ? &Export : nullptr};
		const uint32 PassCount = static_cast<uint32>(Passes.size());
		const uint32 ResourceCount = static_cast<uint32>(State->Resources.size());
		FDependencyGraph DependencyGraph{
			.MaxDependencies = State->Budget.MaxDependencies,
		};
		DependencyGraph.Dependencies.reserve(
			std::min<size_t>(ExplicitDependencyCount + TotalUses,
				State->Budget.MaxDependencies));
		if (auto Error = ValidateGraphPasses(Passes,
			State->Resources, DependencyGraph); !Error.has_value())
			return Error;

		const FResourceUseTable ResourceUses = BuildResourceUseTable(
			Passes, ResourceCount);
		if (auto Error = ValidateTypedValueWriters(State->Resources,
			ResourceUses); !Error.has_value())
			return Error;

		ValidationTimer.Stop();
		FScopedMicrosecondTimer RangeTimer(State->Phases.RangeMicroseconds);
		FRangeWork Work{State->Budget};
		auto Layout = BuildTrackingLayout(State->Resources, ResourceUses, Work);
		if (!Layout) return std::unexpected(Layout.error());
		auto Cells = std::move(*Layout);

		RangeTimer.Stop();
		FScopedMicrosecondTimer DependencyTimer(State->Phases.DependencyMicroseconds);
		if (auto Error = BuildHazardDependencies(Passes,
			State->Resources, Cells, DependencyGraph, Work); !Error.has_value())
			return Error;

		DependencyTimer.Stop();
		FScopedMicrosecondTimer CullingTimer(State->Phases.CullingMicroseconds);
		const std::vector<bool> Retained = FindRetainedPasses(Passes,
			DependencyGraph.Dependencies,
			State->bEnableCulling);

		CullingTimer.Stop();
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.BuildExecutionPlan");
			FScopedMicrosecondTimer PlanTimer(State->Phases.PlanMicroseconds);
			auto CompiledState = std::make_unique<FRDGBuilder::FCompiledState>();
			CompiledState->Owner = State->Owner;
			CompiledState->Resources = State->Resources;
			CompiledState->Backings.resize(ResourceCount);
			for (uint32 Index = 0; Index < ResourceCount; ++Index)
			{
				CompiledState->Backings[Index].Texture = State->Resources[Index].Texture;
				CompiledState->Backings[Index].Buffer = State->Resources[Index].Buffer;
			}
			CompiledState->Budget = State->Budget;
			CompiledState->Passes.reserve(PassCount);
			CompiledState->RuntimePasses.reserve(PassCount);
			CompiledState->Dependencies.reserve(
				DependencyGraph.Dependencies.size());
			CompiledState->ResourceLifetimes.reserve(ResourceCount);
			CompiledState->AllocationRequests.reserve(ResourceCount);
			for (const auto& Edge : DependencyGraph.Dependencies)
				if (Retained[Edge.BeforePass] && Retained[Edge.AfterPass])
					CompiledState->Dependencies.push_back(Edge);
			for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
			{
				const auto& Resource = State->Resources[ResourceIndex];
				CompiledState->ResourceLifetimes.push_back({Resource.Name, std::numeric_limits<uint32>::max(), 0, Resource.bExternal, true});
			}

			std::vector<uint32> LastResourcePass(ResourceCount, std::numeric_limits<uint32>::max());
			size_t BufferTransitionCount = 0;
			size_t TextureTransitionCount = 0;
			for (uint32 ScheduledIndex = 0; ScheduledIndex < PassCount; ++ScheduledIndex)
			{
				if (!Retained[ScheduledIndex]) continue;
				const auto& Pass = Passes[ScheduledIndex];
				const uint32 CompiledPassIndex = static_cast<uint32>(CompiledState->Passes.size());
				FRDGCompiledPass CompiledPass{.Name = Pass.Name, .Type = Pass.Type,
					.DeclarationIndex = ScheduledIndex,
					.ParameterStructName = Pass.ParameterLayout != nullptr
						&& Pass.ParameterLayout->Metadata->StructName != nullptr
						? Pass.ParameterLayout->Metadata->StructName : ""};
				FRDGBuilder::FCompiledState::FCompiledPassRuntime Runtime{
					.ParameterizedExecute = ScheduledIndex < State->Passes.size()
						? &State->Passes[ScheduledIndex].ParameterizedExecute : nullptr,
					.ParameterLayout = Pass.ParameterLayout,
					.Parameters = Pass.Parameters,
					.OptionalAliases = Pass.OptionalAliases.View()};
				Runtime.ResourceIndices.reserve(Pass.Uses.size());
				size_t ValueUseCount = 0;
				size_t BufferUseCount = 0;
				size_t TextureUseCount = 0;
				for (const auto& Use : Pass.Uses)
				{
					if (State->Resources[Use.ResourceIndex].ValueTypeIdentity != nullptr)
						++ValueUseCount;
					if (Use.Kind == ERDGResourceKind::Buffer) ++BufferUseCount;
					else if (Use.Kind == ERDGResourceKind::Texture) ++TextureUseCount;
				}
				Runtime.ValueUses.reserve(ValueUseCount);
				CompiledPass.Barriers.Reserve(BufferUseCount, TextureUseCount);
				for (const auto& Use : Pass.Uses)
				{
					if (LastResourcePass[Use.ResourceIndex] != CompiledPassIndex)
					{
						LastResourcePass[Use.ResourceIndex] = CompiledPassIndex;
						Runtime.ResourceIndices.push_back(Use.ResourceIndex);
					}
					if (State->Resources[Use.ResourceIndex].ValueTypeIdentity != nullptr)
						Runtime.ValueUses.emplace_back(Use.ResourceIndex, Use.Use);
					auto& Lifetime = CompiledState->ResourceLifetimes[Use.ResourceIndex];
					Lifetime.FirstPass = std::min(Lifetime.FirstPass, CompiledPassIndex);
					Lifetime.LastPass = CompiledPassIndex;
					Lifetime.bCulled = false;
				}
				CompiledState->Passes.push_back(std::move(CompiledPass));
				CompiledState->RuntimePasses.push_back(std::move(Runtime));
			}

			auto& Execution = CompiledState->ExecutionPlan;
			const uint32 ScheduledCount = static_cast<uint32>(CompiledState->Passes.size());
			std::vector<uint32> DeclarationToSubmission(PassCount, UINT32_MAX);
			for (uint32 Index = 0; Index < ScheduledCount; ++Index)
				DeclarationToSubmission[CompiledState->Passes[Index].DeclarationIndex] = Index;
			std::vector<std::array<uint32, 2>> RangeUsers(Cells.Ranges.size(), {UINT32_MAX, UINT32_MAX});
			std::optional<FRDGLimitError> TransitionError;
			const bool bTraversed = TraverseExecutionStates(Cells, State->Resources,
				Passes, CompiledState->Passes, CompiledState->ResourceLifetimes, &Work, State->bAsyncComputeEnabled,
				[&](const FRDGTransitionCapture& Event, size_t CellIndex) -> bool
				{
					if (Event.Kind != ERDGTransitionKind::RHIBarrier) return true;
					const auto& Resource = State->Resources[Event.ResourceId];
					auto& Barriers = Event.bFinal ? CompiledState->FinalBarriers
						: CompiledState->Passes[Event.PassIndex].Barriers;
					FRDGResourceHandoff Handoff{Event.ResourceId,
						{Event.bFinal ? ScheduledCount : Event.PassIndex},
						static_cast<uint32>(Resource.Kind == ERDGResourceKind::Texture
							? Barriers.GetTextureTransitions().size() : Barriers.GetBufferTransitions().size()),
						Resource.Kind == ERDGResourceKind::Texture};
					Handoff.SourceQueue = Event.SourceQueue;
					for (uint32 Producer : RangeUsers[CellIndex])
						if (Producer != UINT32_MAX && Producer != Handoff.Consumer.Index)
							Handoff.Producers.push_back({Producer});
					Execution.Handoffs.push_back(std::move(Handoff));
					if (Resource.Kind == ERDGResourceKind::Texture)
					{
						if (++TextureTransitionCount > State->Budget.MaxTextureTransitions)
						{
							TransitionError = SafetyLimit(ERDGLimit::TextureTransitions, TextureTransitionCount, State->Budget.MaxTextureTransitions);
							return false;
						}
						Barriers.AddTransition(FRDGTextureTransition{Event.ResourceId, Event.TextureRange,
							Event.Before, Event.After, Event.bDiscardContents});
					}
					else
					{
						if (++BufferTransitionCount > State->Budget.MaxBufferTransitions)
						{
							TransitionError = SafetyLimit(ERDGLimit::BufferTransitions, BufferTransitionCount, State->Budget.MaxBufferTransitions);
							return false;
						}
						Barriers.AddTransition(FRDGBufferTransition{Event.ResourceId, Event.BufferOffset,
							Event.BufferSize, Event.Before, Event.After, Event.bDiscardContents});
					}
					return true;
				}, [&](uint32 Declaration, const FGraphUse&, size_t CellIndex, const FRangeCell&, bool) {
					const bool bAsync = State->bAsyncComputeEnabled && Passes[Declaration].bAsyncComputeEligible;
					RangeUsers[CellIndex][bAsync ? 1 : 0] = DeclarationToSubmission[Declaration];
				});
			if (!bTraversed) return std::unexpected(TransitionError.value_or(Work.Error()));
			CompactTextureBarriers(CompiledState->Passes, CompiledState->FinalBarriers, Execution);

			Execution.Batches.reserve(ScheduledCount + (ScheduledCount != 0));
			for (uint32 Index = 0; Index < ScheduledCount; ++Index)
			{
				const auto Declaration = CompiledState->Passes[Index].DeclarationIndex;
				const bool bAsync = State->bAsyncComputeEnabled && Passes[Declaration].bAsyncComputeEligible;
				Execution.Batches.push_back({.Id = {Index},
					.Queue = bAsync ? ERDGQueueAssignment::AsyncCompute : ERDGQueueAssignment::Graphics,
					.FirstPass = Index, .NumPasses = 1});
			}
			if (ScheduledCount != 0 || !CompiledState->FinalBarriers.GetBufferTransitions().empty()
				|| !CompiledState->FinalBarriers.GetTextureTransitions().empty())
				Execution.Batches.push_back({.Id = {ScheduledCount},
					.FirstPass = ScheduledCount, .bEpilogue = true});
			Execution.Dependencies.reserve(CompiledState->Dependencies.size() + ScheduledCount);
			for (const auto& Edge : CompiledState->Dependencies)
			{
				const uint32 Before = DeclarationToSubmission[Edge.BeforePass];
				const uint32 After = DeclarationToSubmission[Edge.AfterPass];
				require(Before != UINT32_MAX && After != UINT32_MAX && Before < After);
				Execution.Dependencies.push_back({{Before}, {After}, Edge.Kind, Edge.Cause});
			}
			// Preserve FIFO within each logical queue without serializing independent
			// branches. Publication joins both terminal queue prefixes.
			std::array<uint32, 2> QueueTails{UINT32_MAX, UINT32_MAX};
			for (const auto& Batch : Execution.Batches)
			{
				auto& Tail = QueueTails[static_cast<size_t>(Batch.Queue)];
				if (Tail != UINT32_MAX)
					Execution.Dependencies.push_back({{Tail}, Batch.Id, ERDGDependencyKind::Execution, "queue-order"});
				if (Batch.bEpilogue && QueueTails[1] != UINT32_MAX)
					Execution.Dependencies.push_back({{QueueTails[1]}, Batch.Id, ERDGDependencyKind::Execution, "queue-join"});
				Tail = Batch.Id.Index;
			}
			for (const auto& Handoff : Execution.Handoffs)
				for (const auto Producer : Handoff.Producers)
					if (Execution.Batches[Producer.Index].Queue != Execution.Batches[Handoff.Consumer.Index].Queue)
						Execution.Dependencies.push_back({Producer, Handoff.Consumer,
							ERDGDependencyKind::Execution, "resource-handoff"});

			for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
			{
				const auto& Resource = State->Resources[ResourceIndex];
				const auto& Lifetime = CompiledState->ResourceLifetimes[ResourceIndex];
				if (!Lifetime.bCulled && !Resource.bExternal
					&& Resource.Kind != ERDGResourceKind::Token)
				{
					CompiledState->AllocationRequests.push_back({
						.ResourceId = ResourceIndex,
						.Kind = Resource.Kind,
						.TextureDesc = Resource.TextureDesc,
						.BufferDesc = Resource.BufferDesc,
						.FirstPass = Lifetime.FirstPass,
						.LastPass = Lifetime.LastPass,
						.ObservationTag = Resource.ObservationTag,
						.bExtracted = Resource.IsExported()});
				}
			}
			std::ranges::sort(CompiledState->Dependencies,
				[](const auto& A, const auto& B) {
					return std::tie(A.BeforePass, A.AfterPass, A.Cause)
						< std::tie(B.BeforePass, B.AfterPass, B.Cause);
				});
			CompiledState->Retained = Retained;
			if (bHasExport)
				CompiledState->ExportPass = std::move(Export);
			Diagnostics.reset();
			Compiled = std::move(CompiledState);
			State->bCompiled = true;
		}
		return {};
	}

}

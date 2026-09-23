#pragma once

#include "RDG/RDG.h"
#include "RHICommandList.h"
#include "RHIQueueTransfer.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

// Private vocabulary shared by graph authoring, compilation, and execution.
namespace Durin::RDGPrivate
{
	inline constexpr uint32 MaxUploadBatchCount = 64;
	inline constexpr uint64 MaxUploadBatchBytes = 16ull * 1024 * 1024;

	struct FGraphResource
	{
		std::string Name;
		ERDGResourceKind Kind = ERDGResourceKind::Texture;
		FTextureRHIRef Texture;
		FBufferRHIRef Buffer;
		FRHITextureDesc TextureDesc;
		FRHIBufferDesc BufferDesc;
		uint32 ObservationTag = 0;
		ERHIAccess InitialAccess = ERHIAccess::Discard;
		ERHIAccess FinalAccess = ERHIAccess::None;
		bool bExternal = false;
		// Caller-owned publication slots are written only after successful execution.
		FTextureRHIRef* TextureDestination = nullptr;
		FBufferRHIRef* BufferDestination = nullptr;

		const void* ValueTypeIdentity = nullptr;
		std::string ValueTypeName;
		uint32 ValueStorageIndex = std::numeric_limits<uint32>::max();

		auto IsExported() const -> bool
		{
			return TextureDestination != nullptr || BufferDestination != nullptr;
		}

		auto HasInitialContents() const -> bool
		{
			return bExternal && InitialAccess != ERHIAccess::None
				&& !EnumHasAnyFlags(InitialAccess, ERHIAccess::Discard);
		}
	};

	// Physical backing is execution state; declarations stay frozen after Building.
	struct FGraphResourceBacking final
	{
		FTextureRHIRef Texture;
		FBufferRHIRef Buffer;
		uint64 PhysicalAllocationId = 0;
		std::string AllocationDisposition;
	};

	struct FGraphUse
	{
		uint32 ResourceIndex = 0;
		ERDGResourceKind Kind = ERDGResourceKind::Texture;
		ERDGUse Use = ERDGUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		bool bDiscard = false;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bStore = true;
		bool bPassManagedTransition = false;
		ERHIAccess ResultAccess = ERHIAccess::None;
		std::string_view ParameterPath;
		std::string_view ShaderBindingName;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	using FOptionalAlias = std::pair<uint32, uint32>;
	static_assert(sizeof(FOptionalAlias) == 8);

	struct FOptionalAliasTable final
	{
		FOptionalAliasTable() = default;
		explicit FOptionalAliasTable(std::span<const FOptionalAlias> Aliases)
			: Data(Aliases.empty()
				? nullptr : std::make_unique<FOptionalAlias[]>(Aliases.size())),
			  Count(static_cast<uint32>(Aliases.size()))
		{
			std::ranges::copy(Aliases, Data.get());
		}
		FOptionalAliasTable(const FOptionalAliasTable&) = delete;
		auto operator=(const FOptionalAliasTable&)
			-> FOptionalAliasTable& = delete;
		FOptionalAliasTable(FOptionalAliasTable&&) noexcept = default;
		auto operator=(FOptionalAliasTable&&) noexcept
			-> FOptionalAliasTable& = default;

		auto View() const -> std::span<const FOptionalAlias>
		{
			return {Data.get(), Count};
		}

		std::unique_ptr<FOptionalAlias[]> Data;
		uint32 Count = 0;
	};
	static_assert(sizeof(FOptionalAliasTable) == 16);

	struct FGraphPass
	{
		std::string Name;
		ERDGPassType Type = ERDGPassType::Graphics;
		bool bAsyncComputeEligible = false;
		std::vector<FGraphUse> Uses;
		std::vector<uint32> Prerequisites;
		FRDGParameterizedPassExecute ParameterizedExecute;
		FRDGRecordingPassExecute RecordingExecute;
		ERDGRecordingPolicy RecordingPolicy = ERDGRecordingPolicy::Serial;
		// Nonzero only for owned upload helpers; declarations stay independently addressable.
		uint64 BufferUploadBytes = 0;
		bool bRoot = false;
		// Terminal exports consume contents but may hand off a writable access state.
		bool bExport = false;
		std::string RootReason;
		// Only frozen uses may retain validation across later graph declarations.
		bool bDeclarationsValidated = false;
		const FRDGParameterLayout* ParameterLayout = nullptr;
		const void* Parameters = nullptr;
		FOptionalAliasTable OptionalAliases;
	};

	// Borrows frozen builder declarations and a separately owned terminal export.
	// Neither owner may mutate its passes while compiler views are in use.
	struct FGraphPassView final
	{
		std::span<const FGraphPass> Declarations;
		const FGraphPass* Export = nullptr;

		auto size() const -> size_t
		{ return Declarations.size() + (Export != nullptr ? 1 : 0); }

		auto operator[](size_t Index) const -> const FGraphPass&
		{ return Index < Declarations.size() ? Declarations[Index] : *Export; }
	};

	struct FGraphParameterAllocation final
	{
		FGraphParameterAllocation(size_t Size, size_t Alignment,
			void (*InDestroy)(void*))
			: Data(::operator new(Size, std::align_val_t(Alignment))),
			  Alignment(Alignment), Destroy(InDestroy)
		{
		}

		~FGraphParameterAllocation()
		{
			if (bConstructed) Destroy(Data);
			::operator delete(Data, std::align_val_t(Alignment));
		}

		FGraphParameterAllocation(const FGraphParameterAllocation&) = delete;
		auto operator=(const FGraphParameterAllocation&)
			-> FGraphParameterAllocation& = delete;

		void* Data = nullptr;
		size_t Alignment = 0;
		void (*Destroy)(void*) = nullptr;
		const FRDGParameterLayout* Layout = nullptr;
		bool bConstructed = false;
		bool bFrozen = false;
	};

	struct FGraphParameterStorage final
	{
		FGraphParameterStorage() = default;
		~FGraphParameterStorage() { Reset(); }

		FGraphParameterStorage(const FGraphParameterStorage&) = delete;
		auto operator=(const FGraphParameterStorage&)
			-> FGraphParameterStorage& = delete;

		auto Reset() -> void
		{
			for (auto It = Allocations.rbegin(); It != Allocations.rend(); ++It)
				It->reset();
			Allocations.clear();
		}

		std::vector<std::shared_ptr<FGraphParameterAllocation>> Allocations;
	};

	// Immutable geometry shared by analysis and execution-plan traversal.
	struct FRangeCell final
	{
		ERDGResourceKind Kind;
		uint32 ResourceIndex;
		FRHITextureSubresourceRange TextureRange;
		uint64 BufferOffset;
		uint64 BufferSize;
	};

	// Fresh access history for each traversal of retained passes.
	struct FBarrierCellState final
	{
		ERHIAccess Access = ERHIAccess::Discard;
		ERDGQueueAssignment Queue = ERDGQueueAssignment::Graphics;
		bool bUsed = false;
	};

	auto MakeParameterUse(const FRDGParameterMemberMetadata& Member,
		std::string_view FieldPath) -> FGraphUse;

	auto IsWriteUse(ERDGUse Use) -> bool;

	auto NeedsRangeBarrier(const FBarrierCellState& Cell, const FGraphUse& Use) -> bool;

	auto AdvanceBarrierState(FBarrierCellState& Cell, const FGraphUse& Use) -> void;

	auto DescribeTexture(const FRHITexture& Texture) -> FRHITextureDesc;

	auto TextureDescriptionsEqual(const FRHITextureDesc& Left,
		const FRHITextureDesc& Right) -> bool;

	auto BufferDescriptionsEqual(const FRHIBufferDesc& Left,
		const FRHIBufferDesc& Right) -> bool;

	auto ParameterMemberKindName(ERDGParameterMemberKind Kind)
		-> const char*;

	// Resource indices, kinds and shapes remain stable after declaration.
	// Resource final states and graph topology are validated separately.
	auto ValidatePassDeclarations(const FGraphPass& Pass,
		std::span<const FGraphResource> Resources)
		-> std::expected<void, FRDGUseError>;

	// Borrows frozen uses in declaration order within each resource's contiguous slice.
	struct FResourceUseTable final
	{
		std::vector<size_t> Offsets;
		std::vector<const FGraphUse*> Uses;

		auto operator[](uint32 ResourceIndex) const -> std::span<const FGraphUse* const>
		{
			return std::span(Uses).subspan(Offsets[ResourceIndex],
				Offsets[ResourceIndex + 1] - Offsets[ResourceIndex]);
		}
	};

	auto SafetyLimit(ERDGLimit Dimension, size_t Actual, size_t Limit)
		-> FRDGLimitError;

	struct FRangeWork final
	{
		const FRDGBudget& Budget;
		size_t Candidates = 0;
		size_t Visits = 0;
		auto Visit() -> bool { return ++Visits <= Budget.MaxCellVisits; }
		auto Error() const -> FRDGLimitError
		{ return SafetyLimit(ERDGLimit::CellVisits, Visits, Budget.MaxCellVisits); }
	};

	// Fixed texture subresource indices and one tracking cell per buffer or token.
	// All phase consumers borrow this layout as const data.
	struct FTrackingLayout final
	{
		struct FResourceLayout final
		{
			size_t Begin = 0;
			uint32 Mips = 0;
			uint32 Layers = 0;
			ERHITextureAspect Aspects = ERHITextureAspect::None;
		};
		std::vector<FRangeCell> Ranges;
		std::vector<FResourceLayout> Resources;

		template <typename FVisitor>
		auto VisitUse(const FGraphUse& Use, FVisitor&& Visitor) const
			-> std::invoke_result_t<FVisitor&, size_t, const FRangeCell&>
		{
			const auto& Layout = Resources[Use.ResourceIndex];
			if (Use.Kind != ERDGResourceKind::Texture)
				return Visitor(Layout.Begin, Ranges[Layout.Begin]);
			size_t AspectBegin = Layout.Begin;
			for (ERHITextureAspect Aspect : {ERHITextureAspect::Color,
				ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
			{
				if (!EnumHasAnyFlags(Layout.Aspects, Aspect)) continue;
				if (EnumHasAnyFlags(Use.TextureRange.Aspects, Aspect))
					for (uint32 Mip = Use.TextureRange.FirstMip;
						Mip < Use.TextureRange.FirstMip + Use.TextureRange.NumMips; ++Mip)
						for (uint32 Layer = Use.TextureRange.FirstArrayLayer;
							Layer < Use.TextureRange.FirstArrayLayer + Use.TextureRange.NumArrayLayers; ++Layer)
						{
							const size_t Index = AspectBegin + static_cast<size_t>(Mip) * Layout.Layers + Layer;
							if (auto Error = Visitor(Index, Ranges[Index]); !Error) return Error;
						}
				AspectBegin += static_cast<size_t>(Layout.Mips) * Layout.Layers;
			}
			if constexpr (std::same_as<std::invoke_result_t<FVisitor&, size_t, const FRangeCell&>, bool>)
				return true;
			else return {};
		}
	};

	// Buffer declarations retain their binding ranges, but dependency and barrier
	// analysis consume one combined access per resource and pass.
	struct FPassTrackingUses final
	{
		std::vector<FGraphUse> Uses;
		std::unordered_map<uint32, std::vector<const FGraphUse*>> BufferDeclarations;
	};

	auto BuildTrackingUses(std::span<const FGraphUse> Uses) -> FPassTrackingUses;

	// Both compilation and lazy diagnostics consume this event stream. The caller
	// owns the layout; no diagnostic records are retained by normal compilation.
	template <typename FTransitionVisitor, typename FUseVisitor>
	auto TraverseExecutionStates(const FTrackingLayout& Cells,
		std::span<const FGraphResource> Resources, const FGraphPassView& Passes,
		std::span<const FRDGCompiledPass> ScheduledPasses,
		std::span<const FRDGResourceLifetime> Lifetimes, FRangeWork* Work, bool bAsyncEnabled,
		FTransitionVisitor&& OnTransition, FUseVisitor&& OnUse) -> bool
	{
		std::vector<FBarrierCellState> States(Cells.Ranges.size());
		for (size_t Index = 0; Index < Cells.Ranges.size(); ++Index)
			States[Index].Access = Resources[Cells.Ranges[Index].ResourceIndex].InitialAccess;

		for (uint32 PassIndex = 0; PassIndex < ScheduledPasses.size(); ++PassIndex)
		{
			const uint32 DeclarationIndex = ScheduledPasses[PassIndex].DeclarationIndex;
			const auto Queue = bAsyncEnabled && Passes[DeclarationIndex].bAsyncComputeEligible
				? ERDGQueueAssignment::AsyncCompute : ERDGQueueAssignment::Graphics;
			const auto Tracking = BuildTrackingUses(Passes[DeclarationIndex].Uses);
			for (const auto& Use : Tracking.Uses)
			{
				if (auto Error = Cells.VisitUse(Use, [&](size_t CellIndex, const FRangeCell& Range) -> bool
				{
					auto& Cell = States[CellIndex];
					if (Work != nullptr && !Work->Visit()) return false;
					auto Emit = [&](ERHIAccess Before, ERHIAccess After,
						ERDGTransitionKind Kind, bool bDiscard) -> bool {
						return OnTransition(FRDGTransitionCapture{Use.ResourceIndex,
							PassIndex, Before, After, Range.TextureRange,
							Range.BufferOffset, Range.BufferSize, false, bDiscard, Kind,
							Kind == ERDGTransitionKind::RHIBarrier ? Cell.Queue : Queue, Queue}, CellIndex);
					};
					if (Use.Kind != ERDGResourceKind::Token && (NeedsRangeBarrier(Cell, Use) || Cell.Queue != Queue))
						if (auto Error = Emit(Cell.Access, Use.Access,
							ERDGTransitionKind::RHIBarrier, Use.bDiscard
								&& (Use.Kind != ERDGResourceKind::Buffer
									|| (Use.BufferOffset == 0 && Use.BufferSize == Range.BufferSize))); !Error)
							return Error;
					if (Use.bPassManagedTransition)
						if (auto Error = Emit(Use.Access, Use.ResultAccess,
							ERDGTransitionKind::PassManaged, false); !Error)
							return Error;
					AdvanceBarrierState(Cell, Use);
					Cell.Queue = Queue;
					Cell.bUsed = true;
					if (Use.Kind == ERDGResourceKind::Buffer)
					{
						for (const auto* Declared : Tracking.BufferDeclarations.at(Use.ResourceIndex))
							OnUse(DeclarationIndex, *Declared, CellIndex, Range, IsWriteUse(Use.Use));
					}
					else OnUse(DeclarationIndex, Use, CellIndex, Range, IsWriteUse(Use.Use));
					return true;
				}); !Error) return Error;
			}
		}
		for (size_t CellIndex = 0; CellIndex < Cells.Ranges.size(); ++CellIndex)
		{
			const auto& Range = Cells.Ranges[CellIndex];
			const auto& Cell = States[CellIndex];
			const auto& Resource = Resources[Range.ResourceIndex];
			if (!Cell.bUsed || Range.Kind == ERDGResourceKind::Token
				|| Lifetimes[Range.ResourceIndex].bCulled
				|| ((Resource.FinalAccess == ERHIAccess::None || Resource.FinalAccess == Cell.Access)
					&& Cell.Queue == ERDGQueueAssignment::Graphics)) continue;
			if (auto Error = OnTransition(FRDGTransitionCapture{Range.ResourceIndex,
				std::numeric_limits<uint32>::max(), Cell.Access,
				Resource.FinalAccess == ERHIAccess::None ? Cell.Access : Resource.FinalAccess,
				Range.TextureRange, Range.BufferOffset, Range.BufferSize, true, false,
				ERDGTransitionKind::RHIBarrier, Cell.Queue, ERDGQueueAssignment::Graphics}, CellIndex);
				!Error) return Error;
		}
		return true;
	}

	auto BuildResourceUseTable(FGraphPassView Passes,
		uint32 ResourceCount) -> FResourceUseTable;

	// Layout size depends only on resource descriptions, never on use endpoints.
	auto BuildTrackingLayout(std::span<const FGraphResource> Resources,
		const FResourceUseTable& ResourceUses, FRangeWork& Work)
		-> std::expected<FTrackingLayout, FRDGLimitError>;

}

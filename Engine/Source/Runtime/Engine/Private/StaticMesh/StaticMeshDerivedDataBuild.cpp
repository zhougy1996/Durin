#include "StaticMesh/StaticMeshBuild.h"

#include "Asset/AssetDerivedDataBuild.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"

namespace Durin
{
	auto FormatStaticMeshRenderBuildError(const FStaticMeshRenderBuildError& Error) -> std::string
	{
		std::string_view Reason;
		switch (Error.Code)
		{
		case EStaticMeshRenderBuildError::None: return {};
		case EStaticMeshRenderBuildError::MissingGeometry: Reason = "requires decoded geometry."; break;
		case EStaticMeshRenderBuildError::VertexLimit: Reason = "exceeds the uint32 vertex limit."; break;
		case EStaticMeshRenderBuildError::TriangleList: Reason = "index count is not a triangle list."; break;
		case EStaticMeshRenderBuildError::NonFinitePosition: Reason = "contains a non-finite position."; break;
		case EStaticMeshRenderBuildError::IndexRange: Reason = "contains an out-of-range index."; break;
		case EStaticMeshRenderBuildError::WorkingSet: Reason = "predicted working set exceeds its reservation."; break;
		case EStaticMeshRenderBuildError::DuplicateMaterial: Reason = "has a duplicate imported source material index."; break;
		case EStaticMeshRenderBuildError::RenderLimits: Reason = "exceeds uint32 render-data limits."; break;
		case EStaticMeshRenderBuildError::MissingMaterial: Reason = "references a missing source material."; break;
		case EStaticMeshRenderBuildError::EmptyGeometry: Reason = "source has no renderable geometry."; break;
		case EStaticMeshRenderBuildError::Bounds: Reason = "source has invalid bounds."; break;
		case EStaticMeshRenderBuildError::Cancelled: Reason = "build was cancelled."; break;
		}
		return std::format("StaticMesh render build: {} (mesh '{}', section '{}', index {}, actual {}, expected {}).",
			Reason,
			Error.MeshName, Error.SectionName, Error.Index, Error.Actual, Error.Expected);
	}

#if DURIN_WITH_EDITOR
	namespace
	{
		auto RestoreRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData) -> std::expected<void, std::string>
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				return std::unexpected(std::format("Cached material slot count {} does not match {}.", RenderData.MaterialSlots.size(), MaterialSlots.size()));
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				RenderData.MaterialSlots[SlotIndex].Name =
					MaterialSlots[SlotIndex].Name.ToString();
				RenderData.MaterialSlots[SlotIndex].SourceMaterialIndex =
					MaterialSlots[SlotIndex].SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
				for (size_t SectionIndex = 0;
					SectionIndex < RenderData.LODResources[LODIndex].Sections.size();
					++SectionIndex)
					RenderData.LODResources[LODIndex].Sections[SectionIndex].Name =
						std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			return {};
		}

		// Transfers build storage without copying vertex streams or initializing RHI resources.
		auto AssembleRenderData(FStaticMeshRenderBuildProduct& Product,
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			const std::function<bool()>& ShouldCancel) -> std::unique_ptr<FStaticMeshRenderData>
		{
			auto RenderData = std::make_unique<FStaticMeshRenderData>();
			RenderData->LocalBounds = Product.LocalBounds;
			RenderData->MaterialSlots.reserve(MaterialSlots.size());
			for (const auto& Slot : MaterialSlots)
			{
				if (ShouldCancel()) return {};
				RenderData->MaterialSlots.push_back({Slot.Name.ToString(), Slot.SourceMaterialIndex});
			}
			RenderData->LODResources.reserve(Product.LODs.size());
			for (auto& Source : Product.LODs)
			{
				if (ShouldCancel()) return {};
				auto& LOD = RenderData->LODResources.emplace_back();
				auto& Buffers = LOD.VertexBuffers;
				Buffers.PositionVertexBuffer.GetMutablePositions() = std::move(Source.Positions);
				Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetMutableNormals() = std::move(Source.Normals);
				Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetMutableTangents() = std::move(Source.Tangents);
				Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetMutableTexCoords() = std::move(Source.TexCoords);
				Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.SetNumTexCoords(Source.NumTexCoords);
				Buffers.ColorVertexBuffer.GetMutableColors() = std::move(Source.Colors);
				LOD.IndexBuffer.GetMutableIndices() = std::move(Source.Indices);
				LOD.Sections = std::move(Source.Sections);
				LOD.LocalBounds = Source.LocalBounds;
				LOD.ScreenSize = Source.ScreenSize;
				LOD.NumTexCoords = Source.NumTexCoords;
				LOD.bHasColorVertexData = Source.bHasColorVertexData;
			}
			return RenderData;
		}

		auto ArchiveCodecFailure(const FArchive& Ar) -> std::expected<void, std::string>
		{
			const auto* Failure = Ar.GetFailure();
			return std::unexpected(std::format("Payload archive failed at byte {} (Archive code {}, path '{}'): {}",
				Ar.Tell(), Failure ? static_cast<int>(Failure->Code) : -1,
				Failure ? Failure->Path : std::string{}, Ar.GetError()));
		}

		auto EncodeRenderData(const FStaticMeshRenderData& RenderData, FByteBuffer& OutBytes,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
		{
			FStaticMeshPayloadData Payload;
			if (const auto Built = MakeStaticMeshPayloadData(RenderData, Payload, ShouldCancel); !Built)
				return std::unexpected(FormatStaticMeshPayloadError(Built.error()));
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (!Ar.IsError()) return {};
			const auto Failure = ArchiveCodecFailure(Ar);
			OutBytes.clear();
			return Failure;
		}

		auto DecodeRenderData(FByteView Bytes, std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
		{
			FStaticMeshPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (Ar.IsError() || !RequireArchiveEnd(Ar)) return ArchiveCodecFailure(Ar);
			if (const auto Built = MakeStaticMeshRenderData(Payload, OutRenderData, ShouldCancel); !Built)
				return std::unexpected(FormatStaticMeshPayloadError(Built.error()));
			return RestoreRuntimeMetadata(MaterialSlots, *OutRenderData);
		}

		struct FStaticMeshBuildAdapter
		{
			using FProduct = std::unique_ptr<FStaticMeshRenderData>;
			using FError = FStaticMeshBuildFailure;
			const FStaticMeshBuildRequest& Request;
			IMeshBuilderModule& Module;
			const DerivedData::FBuildDefinition& Definition;
			const FAssetBuildTaskContext& Control;
			std::function<bool()> ShouldCancel;
			std::array<DerivedData::FBuildInputReference, 1> Inputs;
			auto GetFunction() const -> DerivedData::FBuildFunctionDescriptor
			{
				auto Function = Definition.GetFunction();
				Function.Version = Module.GetRenderBuilderVersion();
				return Function;
			}
			auto GetInputs() const -> std::span<const DerivedData::FBuildInputReference> { return Inputs; }
			auto MakeError(DerivedData::EBuildFailure Code) const -> FError
			{
				return Code == DerivedData::EBuildFailure::Cancelled
					? FError::Cancelled(EStaticMeshBuildStage::Render)
					: FError{"StaticMesh definition does not match its captured input or producer.", EStaticMeshBuildStage::Source};
			}
			auto IsCancelled(const FError& Error) const -> bool { return Error.IsCancelled(); }
			auto ValidateBindings(const DerivedData::FBuildDefinition&) const -> std::expected<void, FError>
			{
				if (Request.Source.GetIdentity() != Inputs[0].Identity)
					return std::unexpected(MakeError(DerivedData::EBuildFailure::InputMismatch));
				return {};
			}
			auto Resolve() const -> std::expected<FStaticMeshGeometryReadHandle, FError>
			{
				auto Decoded = Request.Source.AcquireGeometry(ShouldCancel);
				if (!Decoded) return std::unexpected(Decoded.error().Code == EStaticMeshSourceError::Cancelled
					? FError::Cancelled(EStaticMeshBuildStage::Source, FormatStaticMeshSourceError(Decoded.error()))
					: FError{FormatStaticMeshSourceError(Decoded.error()), EStaticMeshBuildStage::Source});
				return std::move(*Decoded);
			}
			auto Build(FStaticMeshGeometryReadHandle& Geometry) const -> std::expected<FProduct, FError>
			{
				std::vector<FStaticMeshBuildMaterialSlot> Slots;
				for (const auto& Slot : Request.Reconciliation.MaterialSlots)
					Slots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
				auto Built = Module.BuildRender({.Geometry = std::move(Geometry), .MaterialSlots = Slots,
					.NormalizedSize = Request.Reconciliation.NormalizedSize}, Control);
				if (!Built) return std::unexpected(Built.error().Code == EStaticMeshRenderBuildError::Cancelled
					? FError::Cancelled(EStaticMeshBuildStage::Render, FormatStaticMeshRenderBuildError(Built.error()))
					: FError{FormatStaticMeshRenderBuildError(Built.error()), EStaticMeshBuildStage::Render});
				if (Built->LODs.empty() || !Built->LocalBounds.bIsValid)
					return std::unexpected(FError{"StaticMesh builder returned invalid render data.", EStaticMeshBuildStage::Render});
				auto Product = AssembleRenderData(*Built, Request.Reconciliation.MaterialSlots, ShouldCancel);
				if (!Product) return std::unexpected(FError::Cancelled(EStaticMeshBuildStage::Render));
				return Product;
			}
			auto Validate(const FProduct& Product) const -> std::expected<void, FError>
			{
				if (!Product) return std::unexpected(FError{"StaticMesh builder returned no render data.", EStaticMeshBuildStage::Validation});
				return FinalizeStaticMeshRenderData(*Product, Control);
			}
			auto Decode(const FSharedByteBuffer& Bytes) const -> std::expected<FProduct, FError>
			{
				FProduct Product;
				auto Result = DecodeRenderData(Bytes.GetBytes(), Request.Reconciliation.MaterialSlots, Product, ShouldCancel);
				if (!Result) return std::unexpected(ShouldCancel()
					? FError::Cancelled(EStaticMeshBuildStage::Render)
					: FError{Result.error(), EStaticMeshBuildStage::Render});
				return Product;
			}
			auto Encode(FProduct& Product) const -> std::expected<FByteBuffer, FError>
			{
				FByteBuffer Bytes;
				auto Result = EncodeRenderData(*Product, Bytes, ShouldCancel);
				if (!Result) return std::unexpected(ShouldCancel()
					? FError::Cancelled(EStaticMeshBuildStage::Render)
					: FError{Result.error(), EStaticMeshBuildStage::Render});
				return Bytes;
			}
		};

	}

#endif

	auto BuildStaticMeshRenderData(FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		if (Control.IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
		if (!Request.Source.IsValid() || !std::isfinite(Request.Reconciliation.NormalizedSize)
			|| Request.Reconciliation.NormalizedSize <= 0)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh render source or normalization is invalid.", EStaticMeshBuildStage::Source});
		FAssetBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
		if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
			|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FStaticMeshImportedMesh))
			|| !SourceMemory.Add(Request.Source.GetMaterialSlotCount(), 32768))
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh decoded source exceeds its reservation.", EStaticMeshBuildStage::Validation});
		if (Request.Reconciliation.MaterialSlots.size() > MaximumMeshMaterialSlots)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh material-slot input exceeds the slot limit.", EStaticMeshBuildStage::Render});
		std::unordered_set<FName> SlotNames;
		std::unordered_set<uint32> SourceIndices;
		for (const auto& Slot : Request.Reconciliation.MaterialSlots)
		{
			if (Slot.Name.IsNone() || Slot.SourceName.size() > 4096
				|| !SlotNames.insert(Slot.Name).second || !SourceIndices.insert(Slot.SourceMaterialIndex).second)
				return std::unexpected(FStaticMeshBuildFailure{"StaticMesh requires bounded, uniquely named material slots with unambiguous source indices.", EStaticMeshBuildStage::Render});
		}
		bool bCancelled = false;
		const auto IsCancelled = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
#if !DURIN_WITH_EDITOR
		return std::unexpected(FStaticMeshBuildFailure{"StaticMesh build orchestration is unavailable outside editor builds.", EStaticMeshBuildStage::Render});
#else
		auto* Module = IMeshBuilderModule::Get();
		if (!Module) return std::unexpected(FStaticMeshBuildFailure{
			"The StaticMesh build module is unavailable.", EStaticMeshBuildStage::Render});
		auto Definition = MakeStaticMeshBuildDefinition({.SourceHash = Request.Source.GetIdentity(),
			.ReconciliationHash = BuildStaticMeshReconciliationHash(Request.Reconciliation.MaterialSlots, Request.Reconciliation.NormalizedSize),
			.BuilderVersion = Module->GetRenderBuilderVersion(), .TargetPlatform = EAssetPayloadTargetPlatform::Win64});
		if (!Definition) return std::unexpected(FStaticMeshBuildFailure{
			FormatStaticMeshBuildKeyError(Definition.error()), EStaticMeshBuildStage::Render});
		FStaticMeshBuildAdapter Adapter{.Request = Request, .Module = *Module, .Definition = *Definition,
			.Control = Control, .ShouldCancel = IsCancelled,
			.Inputs = {DerivedData::FBuildInputReference{"Source", Request.Source.GetIdentity(),
				"StaticMeshSource", StaticMeshSourceGeometryPayloadVersion, "StaticMesh.Geometry", 1}}};
		DerivedData::TBuildObservations<FStaticMeshBuildFailure> Observations;
		const uint64 MaximumBytes = std::min(MaximumStaticMeshPayloadBytes, Control.MaximumWorkingSetBytes / 16);
		auto Outcome = DerivedData::ExecuteBuild(*Definition, Adapter,
			{.bWriteCache = Request.bPersistDerivedData, .MaximumValueBytes = MaximumBytes},
			{.ShouldCancel = IsCancelled}, Observations);
		AssetDerivedDataBuild::ReportCacheIssues(*Definition, Observations,
			[](const FStaticMeshBuildFailure& Error) { return Error.ToString(); });
		return Outcome;
#endif
	}

}

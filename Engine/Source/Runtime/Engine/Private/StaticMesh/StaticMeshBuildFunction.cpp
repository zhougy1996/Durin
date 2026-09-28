#include "StaticMeshBuildFunction.h"
#if DURIN_WITH_EDITOR
#include "StaticMeshDerivedDataKey.h"
#include "StaticMeshSharedOutput.h"
#include "StaticMeshSourceCodec.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::StaticMeshPrivate
{
	using namespace DerivedData;
	namespace
	{
		constexpr uint64 MaximumReconciliationBytes = uint64(MaximumMeshMaterialSlots) * (8192 + 32) + 8;
		auto Error(std::string Message, EBuildFailureReason Category = EBuildFailureReason::InvalidInput) -> FBuildFailure
		{ return {.Reason = Category, .Description = std::move(Message)}; }
		auto Cancelled() -> FBuildFailure { return Error("StaticMesh build was cancelled.", EBuildFailureReason::InternalFailure); }
		auto SourceIdentity(uint32 Slots, uint32 Meshes, FXxHash128 PayloadId) -> FXxHash128
		{
			FXxHash128Builder Hash;
			Hash.UpdateValue(StaticMeshSourceGeometryPayloadVersion); Hash.UpdateValue(Slots);
			Hash.UpdateValue(Meshes); Hash.UpdateValue(PayloadId);
			return Hash.Finalize();
		}
		auto SourceReference(FXxHash128 Identity) -> FBuildInputReference
		{ return {"Source", Identity, "StaticMeshSource", StaticMeshSourceGeometryPayloadVersion, "StaticMesh.AuthoredGeometry", 1}; }
		auto ReconciliationReference(FXxHash128 Identity) -> FBuildInputReference
		{ return {"Reconciliation", Identity, "StaticMeshReconciliation", 1, "StaticMesh.MaterialSlots", 1}; }
		auto MaterialCount(const FBuildAction& Action) -> std::expected<uint32, FBuildFailure>
		{
			const uint64* Count = nullptr; const uint64* Target = nullptr;
			for (const auto& Constant : Action.GetConstants())
			{
				if (Constant.Name == "MaterialSlotCount") Count = std::get_if<uint64>(&Constant.Value);
				if (Constant.Name == "TargetPlatform") Target = std::get_if<uint64>(&Constant.Value);
			}
			if (Action.GetConstants().size() != 2 || !Count || !*Count || *Count > MaximumMeshMaterialSlots
				|| !Target || *Target != uint64(EAssetPayloadTargetPlatform::Win64))
				return std::unexpected(Error("StaticMesh action constants are invalid."));
			return uint32(*Count);
		}
		class FRenderResolver final : public IBuildInputResolver
		{
		public:
			explicit FRenderResolver(const FStaticMeshBuildRequest& Request)
				: Geometry(Request.Source.GetGeometryBulk()), SourceHash(Request.Source.GetIdentity()),
				SlotCount(Request.Source.GetMaterialSlotCount()), MeshCount(Request.Source.GetMeshCount()),
				NormalizedSize(Request.Reconciliation.NormalizedSize)
			{
				for (const auto& Slot : Request.Reconciliation.MaterialSlots)
					Slots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
				ReconciliationHash = BuildStaticMeshReconciliationHash(Slots, NormalizedSize);
			}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
			{
				if (Sources.size() != 2 || SourceHash.IsZero() || ReconciliationHash.IsZero()
					|| Slots.empty() || Slots.size() > MaximumMeshMaterialSlots)
					return std::unexpected(Error("StaticMesh captured source or material slots are invalid."));
				for (const auto& Source : Sources)
					if (!((Source.Name == "Source" && Source.Source == "CapturedSource")
						|| (Source.Name == "Reconciliation" && Source.Source == "CapturedReconciliation")))
						return std::unexpected(Error("StaticMesh source binding is invalid."));
				return std::vector{SourceReference(SourceHash), ReconciliationReference(ReconciliationHash)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildFailure> override
			{
				if (Inputs.size() != 2 || std::ranges::find(Inputs, SourceReference(SourceHash)) == Inputs.end()
					|| std::ranges::find(Inputs, ReconciliationReference(ReconciliationHash)) == Inputs.end())
					return std::unexpected(Error("StaticMesh captured input identity changed."));
				if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
				auto Payload = Geometry.GetPayload().Wait();
				if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
				if (!Payload) return std::unexpected(Error(FormatStaticMeshSourceError(
					{.Code = EStaticMeshSourceError::Read, .ReadCause = Payload})));
				if (Payload->GetSize() != Geometry.GetPayloadSize() || Payload->GetSize() > MaximumStaticMeshSourceBytes
					|| SourceIdentity(SlotCount, MeshCount, FXxHash128::HashBuffer(Payload->GetBytes())) != SourceHash)
					return std::unexpected(Error("StaticMesh source bytes do not match captured identity."));
				FBinaryWriter SourceMetadata; SourceMetadata.WriteU32(SlotCount); SourceMetadata.WriteU32(MeshCount);
				FBinaryWriter Settings({.MaximumTotalBytes = MaximumReconciliationBytes});
				Settings.WriteFloat(NormalizedSize); Settings.WriteU32(uint32(Slots.size()));
				for (const auto& Slot : Slots)
				{
					if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
					const auto Name = Slot.Name.ToString();
					if (Name.size() > 4096 || Slot.SourceName.size() > 4096)
						return std::unexpected(Error("StaticMesh material name exceeds its bound."));
					Settings.WriteString(Name); Settings.WriteString(Slot.SourceName); Settings.WriteU32(Slot.SourceMaterialIndex);
				}
				if (Settings.HasError()) return std::unexpected(Error("StaticMesh reconciliation exceeds its bound."));
				return std::vector<FBuildInput>{
					{.Identity = SourceReference(SourceHash), .Metadata = FSharedByteBuffer::Take(SourceMetadata.TakeBytes()),
						.Values = {{"Geometry", *Payload}}},
					{.Identity = ReconciliationReference(ReconciliationHash),
						.Values = {{"Settings", FSharedByteBuffer::Take(Settings.TakeBytes())}}}};
			}
		private:
			FEditorBulkData Geometry;
			FXxHash128 SourceHash, ReconciliationHash;
			uint32 SlotCount, MeshCount;
			float NormalizedSize;
			std::vector<FStaticMeshBuildMaterialSlot> Slots;
		};
		class FRenderFunction final : public IBuildFunction
		{
		public:
			explicit FRenderFunction(IMeshBuilderModule& Module) : Module(Module), Version(Module.GetRenderBuilderVersion()) {}
			auto GetDescriptor() const -> FBuildFunctionDescriptor override { return GetStaticMeshBuildDescriptor(Version); }
			auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildFailure> override
			{
				bool bCancelled = false;
				const auto ShouldCancel = [&] { return bCancelled = bCancelled || Context.IsCancelled(); };
				auto Count = MaterialCount(Context.GetAction()); if (!Count) return std::unexpected(std::move(Count.error()));
				const auto Inputs = Context.GetInputs();
				const FBuildInput* Source = nullptr; const FBuildInput* Settings = nullptr;
				for (const auto& Input : Inputs)
				{
					if (Input.Identity == SourceReference(Input.Identity.Identity)) Source = &Input;
					if (Input.Identity == ReconciliationReference(Input.Identity.Identity)) Settings = &Input;
				}
				if (Inputs.size() != 2 || !Source || !Settings || Source->Values.size() != 1 || Settings->Values.size() != 1
					|| Source->Values[0].Id != "Geometry" || Settings->Values[0].Id != "Settings" || !Settings->Metadata.IsEmpty())
					return std::unexpected(Error("StaticMesh input representation is invalid."));
				FBinaryReader Metadata(Source->Metadata.GetBytes(), {.MaximumTotalBytes = 8});
				uint32 SlotCount = 0, MeshCount = 0;
				if (!Metadata.ReadU32(SlotCount) || !Metadata.ReadU32(MeshCount) || !Metadata.IsAtEnd())
					return std::unexpected(Error("StaticMesh source metadata is invalid."));
				const auto Bytes = Source->Values[0].Data.GetBytes();
				if (SourceIdentity(SlotCount, MeshCount, FXxHash128::HashBuffer(Bytes)) != Source->Identity.Identity)
					return std::unexpected(Error("StaticMesh source semantic identity is invalid."));
				FAssetBuildMemoryEstimate Memory{Context.GetMaximumWorkingSetBytes()};
				if (!Memory.Add(Bytes.size(), 8) || !Memory.Add(MeshCount, sizeof(FStaticMeshImportedMesh)) || !Memory.Add(SlotCount, 32768) || !Memory.Add(*Count, 32768))
					return std::unexpected(Error("StaticMesh decoded source exceeds its reservation."));
				FBinaryReader Reader(Settings->Values[0].Data.GetBytes(), {.MaximumTotalBytes = MaximumReconciliationBytes});
				uint32 ActualCount = 0; float Size = 0;
				if (!Reader.ReadFloat(Size) || !std::isfinite(Size) || Size <= 0 || !Reader.ReadU32(ActualCount) || ActualCount != *Count)
					return std::unexpected(Error("StaticMesh normalization or material count is invalid."));
				std::vector<FStaticMeshBuildMaterialSlot> Slots;
				std::unordered_set<FName> Names; std::unordered_set<uint32> Indices;
				for (uint32 Index = 0; Index < ActualCount; ++Index)
				{
					if (ShouldCancel()) return std::unexpected(Cancelled());
					std::string Name; FStaticMeshBuildMaterialSlot Slot;
					if (!Reader.ReadString(Name, 4096) || !Reader.ReadString(Slot.SourceName, 4096) || !Reader.ReadU32(Slot.SourceMaterialIndex))
						return std::unexpected(Error("StaticMesh material descriptor is invalid."));
					Slot.Name = FName(Name);
					if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second || !Indices.insert(Slot.SourceMaterialIndex).second)
						return std::unexpected(Error("StaticMesh material names or source indices are ambiguous."));
					Slots.push_back(std::move(Slot));
				}
				if (!Reader.IsAtEnd() || BuildStaticMeshReconciliationHash(Slots, Size) != Settings->Identity.Identity)
					return std::unexpected(Error("StaticMesh reconciliation semantic identity is invalid."));

				auto Geometry = DecodeSourceGeometry(Bytes, SlotCount, MeshCount, ShouldCancel);
				if (!Geometry) return std::unexpected(Geometry.error().Code == EStaticMeshSourceError::Cancelled ? Cancelled()
					: Error(FormatStaticMeshSourceError(Geometry.error())));
				auto Product = Module.BuildRender({.Geometry = std::move(*Geometry), .MaterialSlots = Slots, .NormalizedSize = Size},
					{.ShouldCancel = ShouldCancel, .MaximumWorkingSetBytes = Context.GetMaximumWorkingSetBytes()});
				if (!Product) return std::unexpected(Product.error().Code == EStaticMeshRenderBuildError::Cancelled ? Cancelled()
					: Error(FormatStaticMeshRenderBuildError(Product.error()), EBuildFailureReason::ProducerFailure));
				if (Product->LODs.empty() || !Product->LocalBounds.bIsValid)
					return std::unexpected(Error("StaticMesh builder returned invalid render data.", EBuildFailureReason::ProducerFailure));
				auto Output = MakeSharedOutput(std::move(*Product), *Count, ShouldCancel);
				if (ShouldCancel()) return std::unexpected(Cancelled());
				if (!Output) return std::unexpected(Error(std::move(Output.error()), EBuildFailureReason::InvalidOutput));
				return std::move(*Output);
			}
			auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation& Cancel) const
				-> std::expected<void, FBuildFailure> override
			{
				auto Count = MaterialCount(Action); if (!Count) return std::unexpected(std::move(Count.error()));
				FBinaryReader Metadata(Output.GetMetadata().GetBytes()); uint32 Platform = 0, Profile = 0, StoredCount = 0;
				if (!Metadata.ReadU32(Platform) || !Metadata.ReadU32(Profile) || !Metadata.ReadU32(StoredCount) || StoredCount != *Count)
					return std::unexpected(Error("StaticMesh output material count does not match its action.", EBuildFailureReason::InvalidOutput));
				bool bCancelled = false;
				auto Valid = ValidateSharedOutput(Output, [&] { return bCancelled = bCancelled || Cancel.IsCancelled(); });
				if (bCancelled || Cancel.IsCancelled()) return std::unexpected(Cancelled());
				if (!Valid) return std::unexpected(Error(std::move(Valid.error()), EBuildFailureReason::InvalidOutput));
				return {};
			}
		private:
			IMeshBuilderModule& Module;
			uint32 Version;
		};
	}
	auto MakeRenderBuildFunction(IMeshBuilderModule& Module) -> std::shared_ptr<const IBuildFunction>
	{ return std::make_shared<FRenderFunction>(Module); }
	auto MakeRenderInputResolver(const FStaticMeshBuildRequest& Request) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FRenderResolver>(Request); }
}
#endif

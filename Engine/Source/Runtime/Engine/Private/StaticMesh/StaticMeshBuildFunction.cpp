#include "StaticMeshBuildFunction.h"
#include "StaticMesh/StaticMeshBuild.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"
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
		auto Error(std::string Message) -> FBuildInputError { return {std::move(Message)}; }
		auto Cancelled() -> FBuildInputError { return Error("StaticMesh build was cancelled."); }
		auto SourceIdentity(uint32 Slots, uint32 Meshes, FXxHash128 PayloadId) -> FXxHash128
		{
			FXxHash128Builder Hash;
			Hash.UpdateValue(StaticMeshSourceGeometryIdentityVersion); Hash.UpdateValue(Slots);
			Hash.UpdateValue(Meshes); Hash.UpdateValue(PayloadId);
			return Hash.Finalize();
		}
		auto SourceReference(FXxHash128 Identity) -> FBuildInputReference
		{ return {"Source", Identity, "StaticMeshSource", StaticMeshSourceGeometryIdentityVersion, "StaticMesh.AuthoredGeometry", 1}; }
		auto ReconciliationReference(FXxHash128 Identity) -> FBuildInputReference
		{ return {"Reconciliation", Identity, "StaticMeshReconciliation", 1, "StaticMesh.MaterialSlots", 1}; }
		auto MaterialCount(const FBuildContext& Context) -> std::expected<uint32, std::string>
		{
			const auto* Count = Context.FindConstant<uint64>("MaterialSlotCount"); const auto* Target = Context.FindConstant<uint64>("TargetPlatform");
			if (!Count || !*Count || *Count > MaximumMeshMaterialSlots
				|| !Target || *Target != uint64(EAssetPayloadTargetPlatform::Win64))
				return std::unexpected("StaticMesh action constants are invalid.");
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
				-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
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
				-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
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
			auto GetName() const -> std::string_view override { return "Durin.StaticMesh.Render"; }
			auto GetVersion() const -> uint32 override { return StaticMeshRenderBuildFunctionVersion; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ const auto D = GetStaticMeshBuildDescriptor(); Context.SetConstantsSchema(D.ConstantsSchema); Context.SetOutput(D.OutputType, D.OutputSchema); Context.SetCacheBucket(D.Bucket); }
			auto Build(FBuildContext& Context) const -> void override
			{
				auto Fail = [&](std::string Text) { Context.AddError(std::move(Text)); };
				auto* Module = IMeshBuilderModule::Get();
				if (!Module) return Fail("The MeshBuilder module is unavailable.");
				const auto* BuilderVersion = Context.FindConstant<uint64>("BuilderVersion");
				if (!BuilderVersion || !*BuilderVersion || *BuilderVersion != Module->GetBuildVersion())
					return Fail("StaticMesh builder identity does not match the action.");
				bool bCancelled = false;
				const auto ShouldCancel = [&] { return bCancelled = bCancelled || Context.IsCancelled(); };
				auto Count = MaterialCount(Context); if (!Count) return Fail(std::move(Count.error()));
				const FBuildInput* Source = Context.FindInput("Source"); const FBuildInput* Settings = Context.FindInput("Reconciliation");
				if (!Source || !Settings || Source->Identity != SourceReference(Source->Identity.Identity) || Settings->Identity != ReconciliationReference(Settings->Identity.Identity)
					|| Source->Values.size() != 1 || Settings->Values.size() != 1 || Source->Values[0].Name != "Geometry" || Settings->Values[0].Name != "Settings" || !Settings->Metadata.IsEmpty()) return Fail("StaticMesh input representation is invalid.");
				FBinaryReader Metadata(Source->Metadata.GetBytes(), {.MaximumTotalBytes = 8});
				uint32 SlotCount = 0, MeshCount = 0;
				if (!Metadata.ReadU32(SlotCount) || !Metadata.ReadU32(MeshCount) || !Metadata.IsAtEnd())
					return Fail("StaticMesh source metadata is invalid.");
				const auto Bytes = Source->Values[0].Data.GetBytes();
				if (SourceIdentity(SlotCount, MeshCount, FXxHash128::HashBuffer(Bytes)) != Source->Identity.Identity)
					return Fail("StaticMesh source semantic identity is invalid.");
				FAssetBuildMemoryEstimate Memory{Context.GetMaximumWorkingSetBytes()};
				if (!Memory.Add(Bytes.size(), 8) || !Memory.Add(MeshCount, sizeof(FMeshDescriptionSection)) || !Memory.Add(SlotCount, 32768) || !Memory.Add(*Count, 32768))
					return Fail("StaticMesh decoded source exceeds its reservation.");
				FBinaryReader Reader(Settings->Values[0].Data.GetBytes(), {.MaximumTotalBytes = MaximumReconciliationBytes});
				uint32 ActualCount = 0; float Size = 0;
				if (!Reader.ReadFloat(Size) || !std::isfinite(Size) || Size <= 0 || !Reader.ReadU32(ActualCount) || ActualCount != *Count)
					return Fail("StaticMesh normalization or material count is invalid.");
				std::vector<FStaticMeshBuildMaterialSlot> Slots;
				std::unordered_set<FName> Names; std::unordered_set<uint32> Indices;
				for (uint32 Index = 0; Index < ActualCount; ++Index)
				{
					if (ShouldCancel()) return Fail(Cancelled().Description);
					std::string Name; FStaticMeshBuildMaterialSlot Slot;
					if (!Reader.ReadString(Name, 4096) || !Reader.ReadString(Slot.SourceName, 4096) || !Reader.ReadU32(Slot.SourceMaterialIndex))
						return Fail("StaticMesh material descriptor is invalid.");
					Slot.Name = FName(Name);
					if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second || !Indices.insert(Slot.SourceMaterialIndex).second)
						return Fail("StaticMesh material names or source indices are ambiguous.");
					Slots.push_back(std::move(Slot));
				}
				if (!Reader.IsAtEnd() || BuildStaticMeshReconciliationHash(Slots, Size) != Settings->Identity.Identity)
					return Fail("StaticMesh reconciliation semantic identity is invalid.");

				auto Geometry = DecodeSourceGeometry(Bytes, SlotCount, MeshCount, ShouldCancel);
				if (!Geometry) return Fail(Geometry.error().Code == EStaticMeshSourceError::Cancelled ? Cancelled().Description : FormatStaticMeshSourceError(Geometry.error()));
				auto Product = std::make_unique<FStaticMeshRenderData>();
				const bool bBuilt = Module->BuildRender(*Product, {.Geometry = std::move(*Geometry), .MaterialSlots = Slots, .NormalizedSize = Size,
					.Control = {.ShouldCancel = ShouldCancel, .MaximumWorkingSetBytes = Context.GetMaximumWorkingSetBytes()}});
				if (ShouldCancel()) return Fail(Cancelled().Description);
				if (!bBuilt) return Fail("StaticMesh render construction failed; see MeshBuilder logs for details.");
				if (Product->LODResources.empty() || !Product->LocalBounds.bIsValid)
					return Fail("StaticMesh builder returned invalid render data.");
				auto Output = MakeSharedOutput(std::move(Product), *Count, ShouldCancel);
				if (ShouldCancel()) return Fail(Cancelled().Description);
				if (!Output) return Fail(std::move(Output.error()));
				for (const auto& Value : Output->GetValues()) Context.AddValue(Value.Id, Value.Value.GetData());
				for (const auto& Meta : Output->GetMetadata()) Context.AddMeta(Meta.Id, Meta.Object);
			}
		};
	}
	auto MakeRenderBuildFunction() -> std::shared_ptr<const IBuildFunction>
	{ return std::make_shared<FRenderFunction>(); }
	auto RegisterBuildFunction() -> void
	{
		static std::once_flag Once;
		std::call_once(Once, [] {
			if (!GetBuild().Register(MakeRenderBuildFunction()))
				throw std::runtime_error("Failed to register the static-mesh derived-data build function.");
		});
	}
	auto MakeRenderInputResolver(const FStaticMeshBuildRequest& Request) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FRenderResolver>(Request); }
}
#endif

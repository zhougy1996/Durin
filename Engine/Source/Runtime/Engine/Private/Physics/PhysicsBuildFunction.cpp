#include "PhysicsBuildFunction.h"
#if DURIN_WITH_EDITOR
#include "PhysicsCookDerivedDataKey.h"
#include "PhysicsCookInputPrivate.h"
#include "PhysicsSharedOutput.h"
#include <cstring>

namespace Durin::PhysicsPrivate
{
	using namespace DerivedData;
	namespace
	{
		auto Error(std::string Message) -> FBuildInputError { return {std::move(Message)}; }
		auto Cancelled() -> FBuildInputError { return Error("Physics cooking was cancelled."); }
		auto Reference(FXxHash128 Identity) -> FBuildInputReference
		{ return {"Geometry", Identity, "CollisionGeometry", 1, "TriangleMesh.PositionsIndices", 1}; }
		struct FSettings { EBodySetupCollisionSourceMode Mode; EBodySetupCollisionQueryPolicy Policy; };
		auto Settings(const FBuildContext& Context) -> std::expected<FSettings, std::string>
		{
			const auto* Mode = Context.FindConstant<uint64>("SourceMode"); const auto* Policy = Context.FindConstant<uint64>("QueryPolicy");
			const auto* Target = Context.FindConstant<uint64>("TargetPlatform"); const auto* Weld = Context.FindConstant<uint64>("WeldToleranceBits");
			if (!Mode || !Policy || !Target || !Weld || *Weld
				|| *Target != uint64(EAssetPayloadTargetPlatform::Win64) || *Mode < 1 || *Mode > 2 || *Policy > 2)
				return std::unexpected("Physics action constants are invalid.");
			return FSettings{EBodySetupCollisionSourceMode(*Mode), EBodySetupCollisionQueryPolicy(*Policy)};
		}
		class FResolver final : public IBuildInputResolver
		{
		public:
			explicit FResolver(FPhysicsCookInput Input) : Input(std::move(Input)) {}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
			{
				if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
				if (Sources.size() != 1 || Sources[0].Name != "Geometry" || Sources[0].Source != "CapturedGeometry" || Input.GetIdentity().IsZero())
					return std::unexpected(Error("Physics captured source binding is invalid."));
				return std::vector{Reference(Input.GetIdentity())};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
			{
				if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
				if (Inputs.size() != 1 || Inputs[0] != Reference(Input.GetIdentity()))
					return std::unexpected(Error("Physics captured identity changed."));
				const auto Identity = HashInput(Input.GetPositions().GetBytes(), Input.GetIndices().GetBytes(), [&] { return Cancel.IsCancelled(); });
				if (!Identity) return std::unexpected(Cancelled());
				if (*Identity != Input.GetIdentity()) return std::unexpected(Error("Physics captured bytes do not match their identity."));
				return std::vector<FBuildInput>{{.Identity = Inputs[0],
					.Values = {{"Positions", Input.GetPositions()}, {"Indices", Input.GetIndices()}}}};
			}
		private:
			FPhysicsCookInput Input;
		};
		class FFunction final : public IBuildFunction
		{
		public:
			auto GetName() const -> std::string_view override { return "Durin.Physics.Collision"; }
			auto GetVersion() const -> uint32 override { return GetPhysicsCookBuildDescriptor().Version; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ const auto D = GetPhysicsCookBuildDescriptor(); Context.SetConstantsSchema(D.ConstantsSchema); Context.SetOutput(D.OutputType, D.OutputSchema); Context.SetCacheBucket(D.Bucket); }
			auto Build(FBuildContext& Context) const -> void override
			{
				auto Fail = [&](std::string Text) { Context.AddError(std::move(Text)); };
				bool bCancelled = false;
				const auto Cancel = [&] { return bCancelled = bCancelled || Context.IsCancelled(); };
				auto Config = Settings(Context); if (!Config) return Fail(std::move(Config.error()));
				const auto* Input = Context.FindInput("Geometry");
				if (!Input || Input->Identity != Reference(Input->Identity.Identity) || !Input->Metadata.IsEmpty() || Input->Values.size() != 2) return Fail("Physics source representation is invalid.");
				const FSharedByteBuffer *Positions = nullptr, *Indices = nullptr;
				for (const auto& Value : Input->Values)
				{
					if (Value.Name == "Positions") Positions = &Value.Data;
					if (Value.Name == "Indices") Indices = &Value.Data;
				}
				if (!Positions || !Indices || Positions->size() % sizeof(FVector3f) || Indices->size() % sizeof(uint32))
					return Fail("Physics source arrays are malformed.");
				const auto PositionCount = Positions->size() / sizeof(FVector3f), IndexCount = Indices->size() / sizeof(uint32);
				if (auto Valid = CheckCookInput(PositionCount, IndexCount, Config->Mode, Config->Policy, Context.GetMaximumWorkingSetBytes()); !Valid)
					return Fail(Valid.error().ToString());
				const auto Identity = HashInput(Positions->GetBytes(), Indices->GetBytes(), Cancel);
				if (!Identity) return Fail(Cancelled().Description);
				if (*Identity != Input->Identity.Identity) return Fail("Physics source semantic identity is invalid.");
				std::vector<FVector3> ConvertedPositions; ConvertedPositions.reserve(PositionCount);
				for (size_t Index = 0; Index < PositionCount; ++Index)
				{
					if (Index % 256 == 0 && Cancel()) return Fail(Cancelled().Description);
					FVector3f Position; std::memcpy(&Position, Positions->data() + Index * sizeof(Position), sizeof(Position));
					ConvertedPositions.emplace_back(Position);
				}
				std::vector<uint32> ConvertedIndices;
				auto NativeIndices = Indices->GetNativeView<uint32>();
				if (!NativeIndices)
				{
					ConvertedIndices.resize(IndexCount);
					for (size_t Index = 0; Index < IndexCount; ++Index)
					{
						if (Index % 256 == 0 && Cancel()) return Fail(Cancelled().Description);
						std::memcpy(&ConvertedIndices[Index], Indices->data() + Index * sizeof(uint32), sizeof(uint32));
					}
					NativeIndices = std::span<const uint32>(ConvertedIndices);
				}
				FCollisionGeometryBuildDiagnostics Diagnostics;
				auto Cooked = Config->Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					? FCollisionCookedData::BuildConvexHull(ConvertedPositions, &Diagnostics, Cancel)
					: FCollisionCookedData::BuildTriangleMesh(ConvertedPositions, *NativeIndices, &Diagnostics, Cancel);
				if (Cancel() || Diagnostics.Status == ECollisionGeometryBuildStatus::Cancelled) return Fail(Cancelled().Description);
				if (!Cooked) return Fail(std::format("Physics geometry construction failed (status {}).", int(Diagnostics.Status)));
				auto Output = MakeSharedOutput(std::move(Cooked), Config->Mode, Config->Policy, Cancel);
				if (Cancel()) return Fail(Cancelled().Description);
				if (!Output) return Fail(std::move(Output.error()));
				for (const auto& Value : Output->GetValues()) Context.AddValue(Value.Id, Value.Value.GetData());
				for (const auto& Meta : Output->GetMetadata()) Context.AddMeta(Meta.Id, Meta.Object);
			}
		};
	}
	auto MakeCollisionBuildFunction() -> std::shared_ptr<const IBuildFunction> { return std::make_shared<FFunction>(); }
	auto MakeCollisionInputResolver(FPhysicsCookInput Input) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FResolver>(std::move(Input)); }
}
#endif

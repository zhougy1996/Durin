#include "MaterialTranslationCodec.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::MaterialBuildPrivate
{
	namespace
	{
		// One bounded field walk defines both directions; no native struct bytes.
		struct FTransfer
		{
			FBinaryWriter* Writer = nullptr;
			FBinaryReader* Reader = nullptr;
			bool Good = true;
			template<class T> auto Value(T& V) -> void
			{
				if constexpr (std::is_enum_v<T>)
				{ uint32 Raw = uint32(V); Value(Raw); Good &= Raw <= std::numeric_limits<std::underlying_type_t<T>>::max(); if (Reader) V = T(Raw); }
				else if constexpr (std::same_as<T, bool>)
				{ uint32 Raw = V ? 1 : 0; Value(Raw); Good &= Raw <= 1; if (Reader) V = Raw != 0; }
				else if constexpr (std::same_as<T, float>)
				{ if (Writer) Writer->WriteFloat(V == 0.f ? 0.f : V); else Good &= Reader->ReadFloat(V); }
				else if constexpr (std::same_as<T, uint64>)
				{ if (Writer) Writer->WriteU64(V); else Good &= Reader->ReadU64(V); }
				else if constexpr (std::same_as<T, uint32>)
				{ if (Writer) Writer->WriteU32(V); else Good &= Reader->ReadU32(V); }
				else if constexpr (std::integral<T>)
				{ uint32 Raw = V; Value(Raw); Good &= Raw <= std::numeric_limits<T>::max(); if (Reader) V = T(Raw); }
				else if constexpr (std::same_as<T, FGuid>)
				{ if (Writer) Writer->WriteGuid(V); else Good &= Reader->ReadGuid(V); }
				else if constexpr (std::same_as<T, FXxHash128>)
				{ if (Writer) Writer->WriteHash128(V); else Good &= Reader->ReadHash128(V); }
			}
			auto String(std::string& Text, uint64 Limit = MaterialProgramMaxStringBytes) -> void
			{ Good &= Text.size() <= Limit; if (Writer) Writer->WriteString(Text); else Good &= Reader->ReadString(Text, Limit); }
			template<class T, class F> auto Sequence(std::vector<T>& Items, uint32 Limit, F Visit) -> void
			{
				uint32 Count = uint32(Items.size()); Value(Count);
				if (!Good || Count > Limit || (Reader && Count > Reader->GetRemainingBytes())) { Good = false; return; }
				if (Reader) Items.resize(Count);
				for (auto& Item : Items) Visit(Item);
			}
			template<class T> auto Optional(std::optional<T>& Item) -> void
			{ bool Present = Item.has_value(); Value(Present); if (Present) { if (Reader) Item.emplace(); Value(*Item); } else if (Reader) Item.reset(); }
			auto Literal(FMaterialProgramLiteral& Literal) -> void { Value(Literal.X); Value(Literal.Y); Value(Literal.Z); Value(Literal.W); }
			auto Payload(std::monostate&) -> void {}
			auto Payload(FMaterialProgramLiteral& V) -> void { Literal(V); }
			auto Payload(FGuid& V) -> void { Value(V); }
			auto Payload(MIR::FSwizzle& V) -> void { Value(V.Length); for (auto& Part : V.Components) Value(Part); }
			auto Payload(MIR::FCollectionParameter& V) -> void { Value(V.CollectionId); Value(V.ParameterId); }
			auto Payload(FMaterialTransformPayload& V) -> void { Value(V.Source); Value(V.Destination); }
			template<size_t Index = 0, class V, class F> auto Variant(V& Item, uint32 Kind, F Visit) -> void
			{
				if constexpr (Index < std::variant_size_v<V>)
				{ if (Kind == Index) { if (Reader) Item.template emplace<Index>(); Visit(std::get<Index>(Item)); } else Variant<Index + 1>(Item, Kind, Visit); }
				else Good = false;
			}
			auto Node(MIR::FNode& Node) -> void
			{
				Value(Node.Opcode); Value(Node.ResultType); Value(Node.LegalStages); Value(Node.SpatialKind); Value(Node.CoordinateSpace); Value(Node.ScalarBroadcastMask);
				Sequence(Node.Inputs, MaterialProgramMaxNodeInputCount, [&](auto& V) { Value(V); });
				uint32 Kind = uint32(Node.Payload.index()); Value(Kind); Variant(Node.Payload, Kind, [&](auto& V) { Payload(V); });
			}
			auto Surface(MIR::FModule::FSurfaceInput& Input) -> void
			{ Value(Input.bExpression); Value(Input.ExpressionIndex); Value(Input.Type); Value(Input.LegalStages); Value(Input.SpatialKind); Value(Input.CoordinateSpace); Literal(Input.Literal); }
			auto IR(MIR::FModule& IR) -> void
			{
				Value(IR.Version); Sequence(IR.Nodes, MaterialFunctionMaxExpandedNodes, [&](auto& V) { Node(V); });
				Value(IR.SurfaceRoot.bAggregate); Value(IR.SurfaceRoot.AggregateExpressionIndex);
				for (auto& Input : IR.SurfaceRoot.Inputs) Surface(Input);
				Surface(IR.WorldPositionOffset);
			}
			auto Parameters(std::vector<FMaterialCompilerParameterDeclaration>& Items) -> void
			{ Sequence(Items, MaterialRenderMaxFieldCount, [&](auto& V) { Value(V.Id); Value(V.Type); }); }
			auto Layout(FMaterialRenderLayout& Layout) -> void
			{
				Value(Layout.Identity.Version); Value(Layout.Identity.Id); Value(Layout.UniformPayloadSize); Value(Layout.UniformFieldCount); Value(Layout.ResourceFieldCount);
				Sequence(Layout.Fields, MaterialRenderMaxFieldCount, [&](auto& Field) { Value(Field.ParameterId); Value(Field.Storage); Value(Field.Type); Value(Field.CompactIndex); Value(Field.Offset); Value(Field.Size); });
			}
			auto Collections(std::vector<FMaterialParameterCollectionLayout>& Items) -> void
			{ Sequence(Items, MaterialParameterCollectionMaxPerMaterial, [&](auto& V) { Value(V.CollectionId); Value(V.SchemaVersion); Layout(V.UniformLayout); }); }
			auto Properties(FMaterialStaticProperties& Properties) -> void
			{ Value(Properties.BlendMode); Value(Properties.ShadingModel); Value(Properties.OpacityMaskThreshold); }
			auto Limits(FMaterialCompilerResourceLimits& Limits) -> void
			{ Value(Limits.SampledImages); Value(Limits.Samplers); Value(Limits.UniformBuffers); Value(Limits.StageResources); Value(Limits.UniformBufferBytes); }
			auto Sources(std::vector<MIR::FSource>& Items) -> void
			{
				Sequence(Items, MaterialFunctionMaxExpandedNodes, [&](auto& V) { Value(V.ExpressionIndex); Value(V.NodeId); Optional(V.InputIndex); Optional(V.UVFieldIndex); });
			}
			auto Input(MIR::FCompilerInput& Input) -> void
			{
				uint32 Version = TranslationSchema; Value(Version); Good &= Version == TranslationSchema;
				IR(Input.IR); Parameters(Input.Parameters); Collections(Input.Collections); Properties(Input.StaticProperties); Limits(Input.Environment.ResourceLimits);
				Value(Input.Environment.Quality); Value(Input.Environment.FeatureLevel);
				Sequence(Input.Environment.StaticBools, MaterialMaxStaticBoolDeclarations, [&](auto& V) { Value(V.DeclarationId); Value(V.Value); });
				Sources(Input.Sources);
			}
			auto Diagnostic(FMaterialProgramDiagnostic& D) -> void
			{
				Value(D.Category); Value(D.LocationKind); Value(D.NodeId); Value(D.LocationIndex); Optional(D.InputIndex); Optional(D.UVFieldIndex);
				uint32 Kind = uint32(D.Error.Code.index()); Value(Kind);
				Variant(D.Error.Code, Kind, [&](auto& Code) {
					using T = std::decay_t<decltype(Code)>;
					if constexpr (!std::same_as<T, std::monostate>) Value(Code);
					if constexpr (std::same_as<T, EMaterialIRError>) Good &= uint32(Code) <= uint32(EMaterialIRError::UnsupportedGenerationNode);
					else if constexpr (std::same_as<T, EMaterialPropertyError>) Good &= uint32(Code) <= uint32(EMaterialPropertyError::ParentChainContainsUnsupportedMaterialOwner);
					else if constexpr (std::same_as<T, EMaterialLayoutError>) Good &= uint32(Code) > uint32(EMaterialLayoutError::None) && uint32(Code) <= uint32(EMaterialLayoutError::InvalidReflection);
					else Good = false; // Only errors produced by detached translation may persist.
				});
				Optional(D.Error.Index); Value(D.Error.ParameterId); String(D.Error.ParameterName);
				Optional(D.Error.ExpectedParameterType); Optional(D.Error.ActualParameterType); Optional(D.Error.ExpectedType); Optional(D.Error.ActualType);
				Good &= D.Error.ExternalDiagnostic.empty() && !D.Error.ResourceCause && !D.Error.DuplicationCause && !D.Error.ArchiveCode && !D.Error.BulkStatus && !D.Error.ResourceStatus;
				Good &= uint32(D.Category) <= uint32(EMaterialProgramDiagnosticCategory::Binding) && uint32(D.LocationKind) <= uint32(EMaterialProgramDiagnosticLocationKind::SurfaceOutput);
			}
			auto Product(FMaterialTranslatedProgram& Product, FXxHash128& InputIdentity, std::optional<FXxHash128> Semantic = {}) -> void
			{
				uint32 Version = TranslationSchema; Value(Version); Good &= Version == TranslationSchema;
				Value(InputIdentity); Value(Product.bSucceeded); auto& Result = Product.Candidate;
				IR(Result.IR); Parameters(Result.ActiveParameters); Collections(Result.ActiveCollections); Layout(Result.Layout); Properties(Product.StaticProperties); Limits(Product.ResourceLimits);
				String(Result.GeneratedSource, 1024 * 1024); Sources(Product.Sources);
				auto& Q = Result.Requirements;
				Value(Q.bMaterialView); Value(Q.bMaterialPrimitive); Value(Q.bCameraPosition); Value(Q.bViewport); Value(Q.bViewTransforms); Value(Q.bObjectTransforms); Value(Q.bBoundsCenter); Value(Q.bTangentFrame); Value(Q.bVertexNormal);
				Sequence(Result.Diagnostics, MaterialProgramMaxDiagnosticCount, [&](auto& V) { Diagnostic(V); });
				if (Writer && Semantic) Value(*Semantic); else Value(Result.Identity.Digest);
			}
			auto Valid() const -> bool { return Good && (Writer ? !Writer->HasError() : Reader->IsAtEnd()); }
		};
		auto ProducerEnvironment(MIR::FCompilerInput& Input) -> void
		{
			Input.Environment.CompilerIdentity = "Material.Translate";
			Input.Environment.Target = "vulkan-spirv-1.5";
			Input.Environment.BuilderVersion = GetMaterialBuildVersion();
			Input.Environment.AdapterVersion = GetMaterialCompileAdapterVersion();
			Input.Environment.Dependencies.clear();
		}
		auto SemanticIdentity(const FMaterialTranslatedProgram& Immutable) -> FMaterialProgramIdentity
		{
			// The writer field walk does not modify its input; avoid copying full MIR
			// and diagnostic maps solely to emit their canonical fields.
			auto& Product = const_cast<FMaterialTranslatedProgram&>(Immutable);
			FBinaryWriter Writer({.MaximumTotalBytes = TranslationOutputBytes}); FTransfer Transfer{.Writer = &Writer};
			auto& Result = Product.Candidate;
			Transfer.IR(Result.IR); Transfer.Parameters(Result.ActiveParameters); Transfer.Collections(Result.ActiveCollections); Transfer.Layout(Result.Layout);
			Transfer.Properties(Product.StaticProperties); Transfer.Limits(Product.ResourceLimits); Transfer.String(Result.GeneratedSource, 1024 * 1024);
			Writer.WriteU64(GetMaterialBuildVersion()); Writer.WriteU32(CurrentMaterialPassContractVersion);
			return {.Digest = FXxHash128::HashBuffer(Writer.GetBytes())};
		}
		auto Remap(FMaterialProgramDiagnostic& D, const MIR::FCompilerInput& Consumer) -> bool
		{
			if (!D.NodeId.IsValid()) return true;
			if (D.NodeId.A || D.NodeId.B || D.NodeId.C || !D.NodeId.D || D.NodeId.D > Consumer.Sources.size()) return false;
			const auto& Source = Consumer.Sources[D.NodeId.D - 1];
			D.NodeId = Source.NodeId; D.PortId = Source.PortId; D.FunctionAssetPath = Source.FunctionAssetPath; D.CallPath = Source.CallPath;
			return true;
		}
	}
	auto EstimateTranslationCaptureBytes(const MIR::FCompilerInput& Input) -> uint64
	{
		uint64 Bytes = sizeof(Input) + Input.IR.Nodes.capacity() * sizeof(MIR::FNode) + Input.Parameters.capacity() * sizeof(FMaterialCompilerParameterDeclaration)
			+ Input.Sources.capacity() * sizeof(MIR::FSource) + Input.Environment.CompilerIdentity.capacity() + Input.Environment.Target.capacity()
			+ Input.Environment.StaticBools.capacity() * sizeof(FMaterialCompilerEnvironment::FStaticBoolValue);
		for (const auto& Node : Input.IR.Nodes) Bytes += Node.Inputs.capacity() * sizeof(uint32);
		for (const auto& Source : Input.Sources) Bytes += Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
		for (const auto& Collection : Input.Collections) Bytes += sizeof(Collection) + Collection.AssetPath.capacity() + Collection.DefaultPayload.capacity() + Collection.UniformLayout.Fields.capacity() * sizeof(FMaterialRenderField);
		for (const auto& Dependency : Input.Environment.Dependencies) Bytes += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		return Bytes;
	}
	auto EncodeTranslationInput(const MIR::FCompilerInput& Captured) -> std::optional<FSharedByteBuffer>
	{
		if (EstimateTranslationCaptureBytes(Captured) > TranslationInputBytes) return {};
		auto Input = Captured;
		std::ranges::sort(Input.Parameters, {}, &FMaterialCompilerParameterDeclaration::Id);
		std::ranges::sort(Input.Collections, {}, &FMaterialParameterCollectionLayout::CollectionId);
		Input.StaticProperties = CanonicalizeMaterialShaderProperties(Input.StaticProperties);
		for (uint32 Index = 0; Index < Input.Sources.size(); ++Index) Input.Sources[Index].NodeId = FGuid(0, 0, 0, Index + 1);
		FBinaryWriter Writer({.MaximumTotalBytes = TranslationInputBytes}); FTransfer Transfer{.Writer = &Writer}; Transfer.Input(Input);
		if (!Transfer.Valid()) return {};
		return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
	auto DecodeTranslationInput(FByteView Bytes) -> std::optional<MIR::FCompilerInput>
	{
		FBinaryReader Reader(Bytes, {.MaximumTotalBytes = TranslationInputBytes}); FTransfer Transfer{.Reader = &Reader}; MIR::FCompilerInput Input;
		Transfer.Input(Input); if (!Transfer.Valid()) return {}; ProducerEnvironment(Input); return Input;
	}
	auto EncodeTranslationProduct(const FMaterialTranslatedProgram& Product, FXxHash128 Identity) -> std::optional<FSharedByteBuffer>
	{
		const auto Digest = Product ? SemanticIdentity(Product).Digest : Product.Candidate.Identity.Digest;
		FBinaryWriter Writer({.MaximumTotalBytes = TranslationOutputBytes}); FTransfer Transfer{.Writer = &Writer}; Transfer.Product(const_cast<FMaterialTranslatedProgram&>(Product), Identity, Digest);
		if (!Transfer.Valid()) return {}; return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
	auto DecodeTranslationProduct(FByteView Bytes, FXxHash128 InputIdentity, const MIR::FCompilerInput& Consumer) -> std::optional<FMaterialTranslatedProgram>
	{
		FBinaryReader Reader(Bytes, {.MaximumTotalBytes = TranslationOutputBytes}); FTransfer Transfer{.Reader = &Reader}; FMaterialTranslatedProgram Product; FXxHash128 Identity;
		Transfer.Product(Product, Identity); if (!Transfer.Valid() || Identity != InputIdentity) return {};
		auto& Result = Product.Candidate;
		if (Product.ResourceLimits != Consumer.Environment.ResourceLimits || Product.StaticProperties != CanonicalizeMaterialShaderProperties(Consumer.StaticProperties)) return {};
		if (Product)
		{
			if (!Result.Diagnostics.empty() || Result.GeneratedSource.empty() || !MIR::Validate(Result.IR, Result.ActiveParameters)) return {};
			if (Result.Requirements != BuildMaterialProgramRequirements(Result.IR)
				|| !std::ranges::is_sorted(Result.ActiveParameters, {}, &FMaterialCompilerParameterDeclaration::Id)
				|| !std::ranges::is_sorted(Result.ActiveCollections, {}, &FMaterialParameterCollectionLayout::CollectionId)) return {};
			for (const auto& Parameter : Result.ActiveParameters) if (!std::ranges::contains(Consumer.Parameters, Parameter)) return {};
			const auto Layout = CompileMaterialLayout(Result.ActiveParameters, Product.ResourceLimits);
			if (!Layout || Layout.Layout != Result.Layout) return {};
			FByteBuffer Canonical;
			if (!MIR::EncodeCanonical(Result.IR, Canonical)) return {};
			if (SemanticIdentity(Product) != Result.Identity) return {};
			auto Semantic = Consumer; Semantic.Collections = Result.ActiveCollections;
			Result.Identity = BuildMaterialProgramIdentity(Semantic, Canonical, Result.Layout);
			for (auto& Collection : Result.ActiveCollections)
			{
				const auto Current = std::ranges::find(Consumer.Collections, Collection.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
				if (Current == Consumer.Collections.end() || !Collection.HasCompatibleSchema(*Current)) return {};
				Collection.AssetPath = Current->AssetPath; Collection.DefaultPayload = Current->DefaultPayload;
			}
		}
		else if (Result.Diagnostics.empty()) return {};
		for (auto& Source : Product.Sources)
		{
			if (Source.NodeId.A || Source.NodeId.B || Source.NodeId.C || !Source.NodeId.D || Source.NodeId.D > Consumer.Sources.size() || Source.ExpressionIndex >= Result.IR.Nodes.size()) return {};
			Product.SourceOrdinals.push_back(Source.NodeId.D - 1);
			const auto Index = Source.ExpressionIndex; Source = Consumer.Sources[Source.NodeId.D - 1]; Source.ExpressionIndex = Index;
		}
		for (auto& D : Result.Diagnostics) if (!Remap(D, Consumer)) return {};
		Result.CompilerIdentity = Consumer.Environment.CompilerIdentity; Result.Target = Consumer.Environment.Target; Result.PassContractVersion = Consumer.Environment.PassContractVersion;
		Result.Quality = Consumer.Environment.Quality; Result.FeatureLevel = Consumer.Environment.FeatureLevel; Result.StaticBools = Consumer.Environment.StaticBools; Result.Dependencies = Consumer.Environment.Dependencies;
		return Product;
	}
}

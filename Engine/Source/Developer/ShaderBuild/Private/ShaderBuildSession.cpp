#include "ShaderBuildSession.h"
#include "ShaderSharedOutput.h"
#include "ShaderCaptureLimits.h"
#include "SlangShaderCompiler.h"
#include "ShaderBuild/ShaderPaths.h"
#include "Serialization/BinaryFormat.h"
#include "Hash/CanonicalHash.h"

namespace Durin
{
	using namespace DerivedData;
	namespace
	{
		auto Indexed(std::string_view Prefix, uint64 Index, std::string_view Suffix = {}) -> std::string
		{
			std::string Name(Prefix); Name += std::to_string(Index); Name += Suffix; return Name;
		}
		auto FindInputValue(const FBuildInput& Input, std::string_view Id) -> const FBuildValue*
		{
			const auto Found = std::ranges::lower_bound(Input.Values, Id, {}, &FBuildValue::Id);
			return Found != Input.Values.end() && Found->Id == Id ? &*Found : nullptr;
		}
		auto Boundary(FShaderError Error, EBuildOperation Operation) -> FBuildFailure
		{
			return {.Reason = Error.Code == EShaderError::Cancelled ? EBuildFailureReason::InternalFailure : EBuildFailureReason::ProducerFailure,
				.Operation = Operation, .Description = FormatShaderError(Error), .ProducerCode = uint32(Error.Code),
				.DiagnosticIdentity = FXxHash128{uint64(Error.GetSemanticFingerprint()), 0}};
		}
		auto Invalid(EBuildOperation Phase) -> FBuildFailure
		{ return Boundary({.Code = EShaderError::CaptureInputInvalid}, Phase); }
		auto Descriptor() -> FBuildFunctionDescriptor
		{ return {"Durin.Shader.Compile", 2, 1, "Shader.Output", 1, FCacheBucket::FromString("Shaders/CompiledOutput")}; }
		auto Reference(const FShaderVariantKey& Variant) -> FBuildInputReference
		{ return {"Closure", Variant.Value, "ShaderVariant", 6, "Shader.SourceClosure", 2}; }
		struct FOptions
		{
			FShaderCompileOptions Value;
			std::vector<std::string> Names, Roots;
			bool Generated = false;
			auto Bind() -> void { Value.EntryPoints.clear(); for (auto& Name : Names) Value.EntryPoints.push_back(Name.c_str()); }
		};
		// Options is a canonical, bounded byte constant, not a native struct image.
		// Its versioned fields preserve entry, macro and search-root order explicitly.
		auto ReadOptions(std::span<const FBuildConstant> Constants) -> std::optional<FOptions>
		{
			if (Constants.size() != 1 || Constants[0].Name != "Options") return {};
			const auto* Bytes = std::get_if<std::string>(&Constants[0].Value);
			if (!Bytes) return {};
			FBinaryReader Reader(std::as_bytes(std::span(Bytes->data(), Bytes->size())), {.MaximumTotalBytes = 1024 * 1024});
			FOptions Result; uint32 Version = 0, Generated = 0, Count = 0;
			if (!Reader.ReadU32(Version) || Version != 1
				|| !Reader.ReadString(Result.Value.VirtualShaderPath, ShaderCaptureLimits::MaximumPathBytes) || Result.Value.VirtualShaderPath.empty()
				|| !Reader.ReadString(Result.Value.CompilerEnvironment, 32768) || !Reader.ReadU32(Generated) || Generated > 1
				|| !Reader.ReadU32(Count) || !Count || Count > 32) return {};
			Result.Generated = Generated != 0; Result.Names.reserve(Count); Result.Value.Frequencies.reserve(Count);
			std::set<std::pair<std::string, uint32>> UniqueEntries;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				std::string Name; uint32 Frequency = 0;
				if (!Reader.ReadString(Name, 32768) || Name.empty() || !Reader.ReadU32(Frequency)
					|| Frequency > uint32(EShaderFrequency::RayMiss) || !UniqueEntries.emplace(Name, Frequency).second) return {};
				Result.Names.push_back(std::move(Name)); Result.Value.Frequencies.push_back(EShaderFrequency(Frequency));
			}
			if (!Reader.ReadU32(Count) || Count > 512) return {};
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				FShaderMacroDefinition Macro; uint32 HasValue = 0; std::string Value;
				if (!Reader.ReadString(Macro.Name, 32768) || !Reader.ReadU32(HasValue) || HasValue > 1
					|| !Reader.ReadString(Value, 32768) || (!HasValue && !Value.empty())) return {};
				if (HasValue) Macro.Value = std::move(Value);
				Result.Value.Macros.push_back(std::move(Macro));
			}
			if (!Reader.ReadU32(Count) || Count > 256) return {};
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				std::string Root;
				if (!Reader.ReadString(Root, ShaderCaptureLimits::MaximumPathBytes) || Root.empty()) return {};
				Result.Roots.push_back(std::move(Root));
			}
			if (!Reader.IsAtEnd()) return {};
			std::vector<FShaderMacroDefinition> Normalized;
			if (!ShaderCompileUtilities::NormalizeMacros(Result.Value, Normalized)
				|| !std::ranges::equal(Normalized, Result.Value.Macros, [](const auto& A, const auto& B) { return A.Name == B.Name && A.Value == B.Value; })) return {};
			return Result;
		}
		auto Portable(std::string Path) -> std::string
		{
			std::string Mounted;
			if (FShaderPaths::TryMakeVirtualSourcePath(Path, Mounted)) return Mounted;
			return Path.ends_with(".slang") ? Path.substr(0, Path.size() - 6) : Path;
		}
		class FResolver final : public IBuildInputResolver
		{
		public:
			FBuildInputReference Identity;
			std::vector<FShaderPortableDependency> Dependencies;
			std::optional<std::string> Generated;
			FShaderArtifactResolver Capture;
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
			{
				if (Cancel.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}, EBuildOperation::Describe));
				if (Sources.size() != 1 || Sources[0] != FBuildSourceReference{"Closure", "CapturedShaderClosure"}) return std::unexpected(Invalid(EBuildOperation::Describe));
				return std::vector{Identity};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildFailure> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity || Dependencies.size() > ShaderCaptureLimits::MaximumFiles) return std::unexpected(Invalid(EBuildOperation::Resolve));
				if (Cancel.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}, EBuildOperation::Resolve));
				auto Artifacts = Capture();
				if (!Artifacts) return std::unexpected(Boundary(std::move(Artifacts.error()), EBuildOperation::Resolve));
				if (!*Artifacts) return std::unexpected(Invalid(EBuildOperation::Resolve));
				std::map<std::string, std::pair<std::string, const FSharedByteBuffer*>> Files;
				for (const auto& [Path, Bytes] : (*Artifacts)->GetFiles())
				{
					const auto Name = Portable(Path);
					if (!Files.emplace(Name, std::pair{Name + (Path.ends_with(".slang") ? ".slang" : ""), &Bytes}).second)
						return std::unexpected(Invalid(EBuildOperation::Resolve));
				}
				FBuildInput Input{.Identity = Identity};
				Input.Values.reserve(Dependencies.size() + 1 + Generated.has_value());
				FBinaryWriter Metadata({.MaximumTotalBytes = ShaderCaptureLimits::MaximumFileTableBytes});
				Metadata.WriteU32(uint32(Dependencies.size())); uint64 Bytes = 0;
				for (size_t Index = 0; Index < Dependencies.size(); ++Index)
				{
					if (Cancel.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}, EBuildOperation::Resolve));
					const auto& Dependency = Dependencies[Index]; const auto Found = Files.find(Dependency.VirtualPath);
					if (Found == Files.end()) return std::unexpected(Boundary({.Code = EShaderError::DependencyNotCaptured, .ActualIdentity = Dependency.VirtualPath}, EBuildOperation::Resolve));
					const auto& Data = *Found->second.second;
					if (Data.size() > ShaderCaptureLimits::MaximumFileBytes || Data.size() > ShaderCaptureLimits::MaximumTotalBytes - Bytes
						|| Found->second.first.size() > ShaderCaptureLimits::MaximumPathBytes) return std::unexpected(Invalid(EBuildOperation::Resolve));
					Bytes += Data.size();
					if (FXxHash64::HashBuffer(Data) != Dependency.ContentHash) return std::unexpected(Boundary({.Code = EShaderError::DependencyContentConflict, .ActualIdentity = Dependency.VirtualPath}, EBuildOperation::Resolve));
					Metadata.WriteString(Found->second.first);
					Input.Values.push_back({Indexed("File/", Index), Data});
				}
				if (Generated)
					Input.Values.push_back({"Generated", FSharedByteBuffer::Copy(std::as_bytes(std::span(Generated->data(), Generated->size())))});
				if (Metadata.HasError()) return std::unexpected(Invalid(EBuildOperation::Resolve));
				Input.Values.push_back({"FileTable", FSharedByteBuffer::Take(Metadata.TakeBytes())});
				return std::vector{std::move(Input)};
			}
		};
		struct FCompilerService
		{
			FSlangShaderCompiler Compiler;
			const std::string Environment = Compiler.GetEnvironmentIdentity();
			std::function<void(std::string_view)> BeforeGeneratedCompile;
		};
		class FFunction final : public IBuildFunction
		{
		public:
			explicit FFunction(std::shared_ptr<FCompilerService> InService) : Service(std::move(InService)) {}
			auto GetDescriptor() const -> FBuildFunctionDescriptor override { return Descriptor(); }
			auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation& Cancel) const
				-> std::expected<void, FBuildFailure> override
			{
				auto Options = ReadOptions(Action.GetConstants()); if (!Options) return std::unexpected(Invalid(EBuildOperation::Validate));
				Options->Bind(); auto Valid = ShaderSharedOutput::Validate(Options->Value, Output, [&] { return Cancel.IsCancelled(); });
				if (!Valid) return std::unexpected(Boundary(std::move(Valid.error()), EBuildOperation::Validate));
				return {};
			}
			auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildFailure> override
			{
				auto Options = ReadOptions(Context.GetAction().GetConstants());
				if (!Options || Context.GetInputs().size() != 1 || Context.GetAction().GetInputs().size() != 1) return std::unexpected(Invalid(EBuildOperation::Build));
				Options->Bind(); const auto& Input = Context.GetInputs()[0];
				if (Input.Identity != Context.GetAction().GetInputs()[0] || Input.Identity != Reference({.Value = Input.Identity.Identity})) return std::unexpected(Invalid(EBuildOperation::Build));
				const auto* Table = FindInputValue(Input, "FileTable");
				if (!Input.Metadata.IsEmpty() || !Table) return std::unexpected(Invalid(EBuildOperation::Build));
				FBinaryReader Reader(Table->Data.GetBytes(), {.MaximumTotalBytes = ShaderCaptureLimits::MaximumFileTableBytes}); uint32 Count = 0;
				if (!Reader.ReadU32(Count) || Count > ShaderCaptureLimits::MaximumFiles || Input.Values.size() != Count + 1 + size_t(Options->Generated)) return std::unexpected(Invalid(EBuildOperation::Build));
				std::map<std::string, FSharedByteBuffer> Files; FShaderMetaData Meta;
				uint64 Bytes = 0;
				for (uint32 Index = 0; Index < Count; ++Index)
				{
					if (Context.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}, EBuildOperation::Build));
					std::string Path;
					if (!Reader.ReadString(Path, ShaderCaptureLimits::MaximumPathBytes) || Path.empty() || !Path.starts_with('/')
						|| std::filesystem::path(Path).lexically_normal().generic_string() != Path) return std::unexpected(Invalid(EBuildOperation::Build));
					const auto* Found = FindInputValue(Input, Indexed("File/", Index));
					if (!Found || Found->Data.size() > ShaderCaptureLimits::MaximumFileBytes
						|| Found->Data.size() > ShaderCaptureLimits::MaximumTotalBytes - Bytes) return std::unexpected(Invalid(EBuildOperation::Build));
					Bytes += Found->Data.size();
					const auto Name = Path.ends_with(".slang") ? Path.substr(0, Path.size() - 6) : Path;
					if (!Meta.PortableDependencies.empty() && Meta.PortableDependencies.back().VirtualPath >= Name) return std::unexpected(Invalid(EBuildOperation::Build));
					Meta.PortableDependencies.push_back({Name, FXxHash64::HashBuffer(Found->Data.GetBytes())});
					if (!Files.emplace(Path, Found->Data).second) return std::unexpected(Invalid(EBuildOperation::Build));
				}
				if (!Reader.IsAtEnd()) return std::unexpected(Invalid(EBuildOperation::Build));
				FXxHash128Builder Tree; UpdateCanonicalHashString(Tree, "DurinShaderPortableSourceTree_v1");
				UpdateCanonicalHash(Tree, uint64(Meta.PortableDependencies.size()));
				for (const auto& Dependency : Meta.PortableDependencies) { UpdateCanonicalHashString(Tree, Dependency.VirtualPath); UpdateCanonicalHash(Tree, Dependency.ContentHash); }
				Meta.SourceTreeSignature = Tree.Finalize();
				std::string Generated;
				if (Options->Generated)
				{
					const auto* Found = FindInputValue(Input, "Generated");
					if (!Found || Found->Data.IsEmpty() || Found->Data.size() > 1024 * 1024) return std::unexpected(Invalid(EBuildOperation::Build));
					Generated.assign(reinterpret_cast<const char*>(Found->Data.data()), Found->Data.size());
					FXxHash128Builder Combined; Combined.Update("DurinGeneratedShaderSourceTree_v1");
					UpdateCanonicalHash(Combined, FXxHash128::HashBuffer(Generated)); UpdateCanonicalHash(Combined, Meta.SourceTreeSignature);
					Meta.SourceTreeSignature = Combined.Finalize();
				}
				FShaderVariantKey Variant; ShaderCompileUtilities::BuildVariantKey(Options->Value.VirtualShaderPath, Meta, Options->Value.Macros, Options->Value.CompilerEnvironment, Variant);
				if (Variant.Value != Input.Identity.Identity || Options->Value.CompilerEnvironment != Service->Environment)
					return std::unexpected(Boundary({.Code = EShaderError::DependencyContentConflict}, EBuildOperation::Build));
				Options->Value.SourceArtifacts = std::make_shared<const FShaderSourceArtifacts>(std::move(Files), std::move(Options->Roots));
				if (Context.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}, EBuildOperation::Build));
				Context.ReportMetric("Shader.Compilations", 1);
				auto Product = Options->Generated
					? Service->Compiler.CompileSource(Options->Value.VirtualShaderPath.substr(1), Options->Value.VirtualShaderPath, Generated, Options->Value, Service->BeforeGeneratedCompile)
					: Service->Compiler.Compile(Options->Value.VirtualShaderPath, Options->Value);
				if (!Product) return std::unexpected(Boundary(std::move(Product.Error), EBuildOperation::Build));
				auto Output = ShaderSharedOutput::Make(Options->Value, Product, [&] { return Context.IsCancelled(); });
				if (!Output) return std::unexpected(Boundary(std::move(Output.error()), EBuildOperation::Build));
				return std::move(*Output);
			}
		private: std::shared_ptr<FCompilerService> Service;
		};
	}
	auto MakeShaderSessionRequest(const FShaderCompileOptions& Options, const FShaderVariantKey& Variant,
		std::vector<FShaderPortableDependency> Dependencies, std::optional<std::string> GeneratedSource, FShaderArtifactResolver Resolve)
		-> std::expected<FShaderSessionRequest, FShaderError>
	{
		std::vector<FShaderMacroDefinition> Macros;
		if (auto Normalized = ShaderCompileUtilities::NormalizeMacros(Options, Macros); !Normalized) return std::unexpected(std::move(Normalized.error()));
		std::vector<std::string> Roots;
		if (Options.SourceArtifacts) Roots = Options.SourceArtifacts->GetSearchRoots();
		else for (const auto& Mount : FShaderPaths::GetRegisteredMountPoints()) Roots.push_back(Mount.VirtualRoot);
		if (Options.SourceArtifacts) for (auto& Root : Roots)
		{
			std::string Virtual;
			if (FShaderPaths::TryMakeVirtualSourcePath(Root.ends_with('/') ? Root : Root + '/', Virtual)) Root = std::move(Virtual);
		}
		if (Options.VirtualShaderPath.empty() || Options.VirtualShaderPath.size() > ShaderCaptureLimits::MaximumPathBytes
			|| Options.EntryPoints.empty() || Options.EntryPoints.size() > 32 || Options.EntryPoints.size() != Options.Frequencies.size()
			|| Macros.size() > 512 || Roots.size() > 256 || Variant.Value.IsZero() || !Resolve)
			return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		std::set<std::pair<std::string_view, EShaderFrequency>> UniqueEntries;
		for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
		{
			const std::string_view Name = Options.EntryPoints[Index] ? Options.EntryPoints[Index] : "";
			if (Name.empty() || Name.size() > 32768 || uint32(Options.Frequencies[Index]) > uint32(EShaderFrequency::RayMiss)
				|| !UniqueEntries.emplace(Name, Options.Frequencies[Index]).second)
				return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		}
		for (const auto& Root : Roots) if (Root.empty() || Root.size() > ShaderCaptureLimits::MaximumPathBytes)
			return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		FBinaryWriter Writer({.MaximumTotalBytes = 1024 * 1024});
		Writer.Reserve(512);
		Writer.WriteU32(1); Writer.WriteString(Options.VirtualShaderPath); Writer.WriteString(Options.CompilerEnvironment);
		Writer.WriteU32(GeneratedSource.has_value()); Writer.WriteU32(uint32(Options.EntryPoints.size()));
		for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
		{ Writer.WriteString(Options.EntryPoints[Index]); Writer.WriteU32(uint32(Options.Frequencies[Index])); }
		Writer.WriteU32(uint32(Macros.size()));
		for (const auto& Macro : Macros)
		{
			if (Macro.Name.size() > 32768 || (Macro.Value && Macro.Value->size() > 32768))
				return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
			Writer.WriteString(Macro.Name); Writer.WriteU32(Macro.Value.has_value()); Writer.WriteString(Macro.Value.value_or(""));
		}
		Writer.WriteU32(uint32(Roots.size())); for (const auto& Root : Roots) Writer.WriteString(Root);
		if (Writer.HasError() || Options.CompilerEnvironment.size() > 32768) return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		const auto Encoded = Writer.TakeBytes();
		std::vector<FBuildConstant> Constants{{"Options", std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size())}};
		auto Definition = FBuildDefinition::TryCreate("Durin.Shader.Compile", std::move(Constants), {{"Closure", "CapturedShaderClosure"}});
		if (!Definition) return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		auto Resolver = std::make_shared<FResolver>(); Resolver->Identity = Reference(Variant);
		Resolver->Dependencies = std::move(Dependencies); Resolver->Generated = std::move(GeneratedSource); Resolver->Capture = std::move(Resolve);
		auto Inputs = FBuildInputs::TryCreate(Definition->GetSources(), std::move(Resolver));
		if (!Inputs) return std::unexpected(ShaderSessionError(Inputs.error()));
		return FShaderSessionRequest{std::move(*Definition), std::move(*Inputs)};
	}

	auto ShaderSessionError(const FBuildFailure& Error) -> FShaderError
	{
		if (Error.ProducerCode && *Error.ProducerCode == uint32(EShaderError::Cancelled)) return {.Code = EShaderError::Cancelled};
		if (Error.ProducerCode && *Error.ProducerCode > uint32(EShaderError::None)
			&& *Error.ProducerCode <= uint32(EShaderError::MissingResourceCode) && Error.DiagnosticIdentity)
			return FShaderError::FromBuildDiagnostic(EShaderError(*Error.ProducerCode), Error.Description, size_t(Error.DiagnosticIdentity->HashLow));
		return FShaderError::FromBuildDiagnostic(EShaderError::InvalidCompileRequest, Error.Description,
			uint64(Error.Operation) * 256 + uint64(Error.Reason));
	}
	struct FShaderBuildService::FState
	{
		std::shared_ptr<FCompilerService> Compiler = std::make_shared<FCompilerService>();
		std::string Environment = Compiler->Environment;
		std::shared_ptr<IBuild> Service;
		std::shared_ptr<FBuildSession> Session;
		std::mutex Mutex;
		bool Closed = false;
	};
	FShaderBuildService::FShaderBuildService(std::function<void(std::string_view)> Hook) : State(std::make_unique<FState>())
	{
		State->Compiler->BeforeGeneratedCompile = std::move(Hook);
		State->Service = CreateBuild({.CompressRecords = true});
		const auto Registered = State->Service->Register(std::make_shared<FFunction>(State->Compiler)); require(Registered);
		auto Session = State->Service->CreateSession(); require(Session); State->Session = std::move(*Session);
	}
	FShaderBuildService::~FShaderBuildService() { Close(); }
	auto FShaderBuildService::GetCompilerEnvironmentIdentity() const -> const std::string& { return State->Environment; }
	auto FShaderBuildService::Execute(FShaderSessionRequest Request, FBuildRequestOptions Options) -> FBuildCompleteParams
	{
		std::shared_ptr<FBuildSession> Persistent;
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed || !State->Session)
				return FBuildCompleteParams::Canceled(std::nullopt, EBuildStatus::None, {});
			Persistent = State->Session;
		}
		std::optional<FBuildCompleteParams> Completion;
		auto Admitted = Persistent->Build(std::move(Request.Definition), [&](auto Result) {
			Completion = std::move(Result);
		}, std::move(Request.Inputs), std::move(Options));
		if (!Admitted) return FBuildCompleteParams::Error({.Reason = EBuildFailureReason::InputUnavailable,
			.Operation = EBuildOperation::Admission, .Description = std::move(Admitted.error().Description)},
			std::nullopt, EBuildStatus::None, {});
		if (!Completion) return FBuildCompleteParams::Error({.Reason = EBuildFailureReason::InternalFailure,
			.Operation = EBuildOperation::Dispatch, .Description = "Shader build session did not complete inline."},
			std::nullopt, EBuildStatus::None, {});
		return std::move(*Completion);
	}
	auto FShaderBuildService::Close() -> void
	{
		std::shared_ptr<IBuild> Retired;
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed) return;
			State->Closed = true;
			Retired = State->Service;
		}
		if (Retired)
		{
			Retired->Close();
			require(Retired->Drain() == EBuildDrainResult::Drained);
		}
		std::lock_guard Lock(State->Mutex);
		State->Session.reset(); State->Service.reset();
	}
}

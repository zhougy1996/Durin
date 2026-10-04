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
		auto FindInputValue(const FBuildInput& Input, std::string_view Id) -> const FBuildInputValue*
		{
			const auto Found = std::ranges::lower_bound(Input.Values, Id, {}, &FBuildInputValue::Name);
			return Found != Input.Values.end() && Found->Name == Id ? &*Found : nullptr;
		}
		auto Boundary(FShaderError Error) -> FBuildInputError { return {FormatShaderError(Error)}; }
		auto Invalid() -> FBuildInputError { return Boundary({.Code = EShaderError::CaptureInputInvalid}); }
		auto Descriptor() -> FBuildFunctionDescriptor
			{ return {"Durin.Shader.Compile", 5, 1, "Shader.Output", 5, FCacheBucket::FromString("Shader")}; }
		auto Reference(const FShaderVariantKey& Variant) -> FBuildInputReference
			{ return {"Closure", Variant.Value, "ShaderVariant", 7, "Shader.SourceClosure", 2}; }
		struct FOptions
		{
			FShaderCompileOptions Value;
			std::vector<std::string> Names, Roots;
			bool Generated = false;
			auto Bind() -> void { Value.EntryPoints.clear(); for (auto& Name : Names) Value.EntryPoints.push_back(Name.c_str()); }
		};
		// Options is a canonical, bounded byte constant, not a native struct image.
		// Its versioned fields preserve entry, macro and search-root order explicitly.
		auto ReadOptions(const FBuildContext& Context) -> std::optional<FOptions>
		{
			const auto* Bytes = Context.FindConstant<std::string>("Options");
			if (!Bytes) return {};
			FBinaryReader Reader(std::as_bytes(std::span(Bytes->data(), Bytes->size())), {.MaximumTotalBytes = 1024 * 1024});
			FOptions Result; uint32 Version = 0, Generated = 0, Count = 0;
			if (!Reader.ReadU32(Version) || Version != 2
				|| !Reader.ReadString(Result.Value.VirtualShaderPath, ShaderCaptureLimits::MaximumPathBytes) || Result.Value.VirtualShaderPath.empty()
				|| !Reader.ReadString(Result.Value.CompilerEnvironment, 32768)) return {};
			uint32 Platform = 0, Backend = 0, Intermediate = 0, Output = 0;
			if (!Reader.ReadU32(Platform) || !Reader.ReadU32(Backend)
				|| !Reader.ReadU32(Intermediate) || !Reader.ReadU32(Output)
				|| !Reader.ReadU32(Result.Value.Target.MslLanguageVersion)
				|| !Reader.ReadU32(Result.Value.Target.BindingRemapSchema)) return {};
			Result.Value.Target.Platform = EShaderTargetPlatform(Platform);
			Result.Value.Target.Backend = EShaderRuntimeBackend(Backend);
			Result.Value.Target.IntermediateFormat = EShaderCodeFormat(Intermediate);
			Result.Value.Target.OutputFormat = EShaderCodeFormat(Output);
			if ((Result.Value.Target != VulkanShaderTarget && Result.Value.Target != MetalShaderTarget)
				|| !Reader.ReadU32(Generated) || Generated > 1
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
				-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
			{
				if (Cancel.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}));
				if (Sources.size() != 1 || Sources[0] != FBuildSourceReference{"Closure", "CapturedShaderClosure"}) return std::unexpected(Invalid());
				return std::vector{Identity};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
			{
				if (Inputs.size() != 1 || Inputs[0] != Identity || Dependencies.size() > ShaderCaptureLimits::MaximumFiles) return std::unexpected(Invalid());
				if (Cancel.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}));
				auto Artifacts = Capture();
				if (!Artifacts) return std::unexpected(Boundary(std::move(Artifacts.error())));
				if (!*Artifacts) return std::unexpected(Invalid());
				std::map<std::string, std::pair<std::string, const FSharedByteBuffer*>> Files;
				for (const auto& [Path, Bytes] : (*Artifacts)->GetFiles())
				{
					const auto Name = Portable(Path);
					if (!Files.emplace(Name, std::pair{Name + (Path.ends_with(".slang") ? ".slang" : ""), &Bytes}).second)
						return std::unexpected(Invalid());
				}
				FBuildInput Input{.Identity = Identity};
				Input.Values.reserve(Dependencies.size() + 1 + Generated.has_value());
				FBinaryWriter Metadata({.MaximumTotalBytes = ShaderCaptureLimits::MaximumFileTableBytes});
				Metadata.WriteU32(uint32(Dependencies.size())); uint64 Bytes = 0;
				for (size_t Index = 0; Index < Dependencies.size(); ++Index)
				{
					if (Cancel.IsCancelled()) return std::unexpected(Boundary({.Code = EShaderError::Cancelled}));
					const auto& Dependency = Dependencies[Index]; const auto Found = Files.find(Dependency.VirtualPath);
					if (Found == Files.end()) return std::unexpected(Boundary({.Code = EShaderError::DependencyNotCaptured, .ActualIdentity = Dependency.VirtualPath}));
					const auto& Data = *Found->second.second;
					if (Data.size() > ShaderCaptureLimits::MaximumFileBytes || Data.size() > ShaderCaptureLimits::MaximumTotalBytes - Bytes
						|| Found->second.first.size() > ShaderCaptureLimits::MaximumPathBytes) return std::unexpected(Invalid());
					Bytes += Data.size();
					if (FXxHash64::HashBuffer(Data) != Dependency.ContentHash) return std::unexpected(Boundary({.Code = EShaderError::DependencyContentConflict, .ActualIdentity = Dependency.VirtualPath}));
					Metadata.WriteString(Found->second.first);
					Input.Values.push_back({Indexed("File/", Index), Data});
				}
				if (Generated)
					Input.Values.push_back({"Generated", FSharedByteBuffer::Copy(std::as_bytes(std::span(Generated->data(), Generated->size())))});
				if (Metadata.HasError()) return std::unexpected(Invalid());
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
			auto GetName() const -> std::string_view override { return "Durin.Shader.Compile"; }
			auto GetVersion() const -> uint32 override { return 3; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ const auto D = Descriptor(); Context.SetConstantsSchema(D.ConstantsSchema); Context.SetOutput(D.OutputType, D.OutputSchema); Context.SetCacheBucket(D.Bucket); }
			auto Build(FBuildContext& Context) const -> void override
			{
				auto Fail = [&](std::string Text) { Context.AddError(std::move(Text)); };
				auto Options = ReadOptions(Context); const auto* InputPtr = Context.FindInput("Closure");
				if (!Options || !InputPtr) return Fail(Invalid().Description);
				Options->Bind(); const auto& Input = *InputPtr;
				if (Input.Identity != Reference({.Value = Input.Identity.Identity})) return Fail(Invalid().Description);
				const auto* Table = FindInputValue(Input, "FileTable");
				if (!Input.Metadata.IsEmpty() || !Table) return Fail(Invalid().Description);
				FBinaryReader Reader(Table->Data.GetBytes(), {.MaximumTotalBytes = ShaderCaptureLimits::MaximumFileTableBytes}); uint32 Count = 0;
				if (!Reader.ReadU32(Count) || Count > ShaderCaptureLimits::MaximumFiles || Input.Values.size() != Count + 1 + size_t(Options->Generated)) return Fail(Invalid().Description);
				std::map<std::string, FSharedByteBuffer> Files; FShaderMetaData Meta;
				uint64 Bytes = 0;
				for (uint32 Index = 0; Index < Count; ++Index)
				{
					if (Context.IsCancelled()) return Fail(FormatShaderError({.Code = EShaderError::Cancelled}));
					std::string Path;
					if (!Reader.ReadString(Path, ShaderCaptureLimits::MaximumPathBytes) || Path.empty() || !Path.starts_with('/')
						|| std::filesystem::path(Path).lexically_normal().generic_string() != Path) return Fail(Invalid().Description);
					const auto* Found = FindInputValue(Input, Indexed("File/", Index));
					if (!Found || Found->Data.size() > ShaderCaptureLimits::MaximumFileBytes
						|| Found->Data.size() > ShaderCaptureLimits::MaximumTotalBytes - Bytes) return Fail(Invalid().Description);
					Bytes += Found->Data.size();
					const auto Name = Path.ends_with(".slang") ? Path.substr(0, Path.size() - 6) : Path;
					if (!Meta.PortableDependencies.empty() && Meta.PortableDependencies.back().VirtualPath >= Name) return Fail(Invalid().Description);
					Meta.PortableDependencies.push_back({Name, FXxHash64::HashBuffer(Found->Data.GetBytes())});
					if (!Files.emplace(Path, Found->Data).second) return Fail(Invalid().Description);
				}
				if (!Reader.IsAtEnd()) return Fail(Invalid().Description);
				FXxHash128Builder Tree; UpdateCanonicalHashString(Tree, "DurinShaderPortableSourceTree_v1");
				UpdateCanonicalHash(Tree, uint64(Meta.PortableDependencies.size()));
				for (const auto& Dependency : Meta.PortableDependencies) { UpdateCanonicalHashString(Tree, Dependency.VirtualPath); UpdateCanonicalHash(Tree, Dependency.ContentHash); }
				Meta.SourceTreeSignature = Tree.Finalize();
				std::string Generated;
				if (Options->Generated)
				{
					const auto* Found = FindInputValue(Input, "Generated");
					if (!Found || Found->Data.IsEmpty() || Found->Data.size() > 1024 * 1024) return Fail(Invalid().Description);
					Generated.assign(reinterpret_cast<const char*>(Found->Data.data()), Found->Data.size());
					FXxHash128Builder Combined; Combined.Update("DurinGeneratedShaderSourceTree_v1");
					UpdateCanonicalHash(Combined, FXxHash128::HashBuffer(Generated)); UpdateCanonicalHash(Combined, Meta.SourceTreeSignature);
					Meta.SourceTreeSignature = Combined.Finalize();
				}
				FShaderVariantKey Variant; ShaderCompileUtilities::BuildVariantKey(Options->Value.VirtualShaderPath, Meta, Options->Value.Macros, Options->Value.CompilerEnvironment, Options->Value.Target, Variant);
				if (Variant.Value != Input.Identity.Identity || Options->Value.CompilerEnvironment != Service->Environment)
					return Fail(FormatShaderError({.Code = EShaderError::DependencyContentConflict}));
				Options->Value.SourceArtifacts = std::make_shared<const FShaderSourceArtifacts>(std::move(Files), std::move(Options->Roots));
				if (Context.IsCancelled()) return Fail(FormatShaderError({.Code = EShaderError::Cancelled}));
				auto Product = Options->Generated
					? Service->Compiler.CompileSource(Options->Value.VirtualShaderPath.substr(1), Options->Value.VirtualShaderPath, Generated, Options->Value, Service->BeforeGeneratedCompile)
					: Service->Compiler.Compile(Options->Value.VirtualShaderPath, Options->Value);
				if (!Product) return Fail(FormatShaderError(std::move(Product.Error)));
				auto Output = ShaderSharedOutput::Make(Options->Value, Product, [&] { return Context.IsCancelled(); });
				if (!Output) return Fail(FormatShaderError(std::move(Output.error())));
				for (const auto& Value : Output->GetValues()) Context.AddValue(Value.Id, Value.Value.GetData());
				for (const auto& MetaValue : Output->GetMetadata()) Context.AddMeta(MetaValue.Id, MetaValue.Object);
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
		Writer.WriteU32(2); Writer.WriteString(Options.VirtualShaderPath); Writer.WriteString(Options.CompilerEnvironment);
		Writer.WriteU32(uint32(Options.Target.Platform)); Writer.WriteU32(uint32(Options.Target.Backend));
		Writer.WriteU32(uint32(Options.Target.IntermediateFormat)); Writer.WriteU32(uint32(Options.Target.OutputFormat));
		Writer.WriteU32(Options.Target.MslLanguageVersion); Writer.WriteU32(Options.Target.BindingRemapSchema);
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
		FBuildDefinitionBuilder DefinitionBuilder("Durin.Shader.Compile");
		DefinitionBuilder.AddConstant("Options", std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size()))
			.AddInput("Closure", "CapturedShaderClosure");
		auto Definition = std::move(DefinitionBuilder).Build();
		if (!Definition) return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		auto Resolver = std::make_shared<FResolver>(); Resolver->Identity = Reference(Variant);
		Resolver->Dependencies = std::move(Dependencies); Resolver->Generated = std::move(GeneratedSource); Resolver->Capture = std::move(Resolve);
		return FShaderSessionRequest{std::move(*Definition), std::move(Resolver)};
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
		State->Service = CreateBuild();
		const auto Registered = State->Service->Register(std::make_shared<FFunction>(State->Compiler)); require(Registered);
		auto Session = State->Service->CreateSession(); require(Session); State->Session = std::move(*Session);
	}
	FShaderBuildService::~FShaderBuildService() { Close(); }
	auto FShaderBuildService::GetCompilerEnvironmentIdentity() const -> const std::string& { return State->Environment; }
	auto FShaderBuildService::Execute(FShaderSessionRequest Request, FBuildRequestOptions Options)
		-> FBuildCompleteParams
	{
		std::shared_ptr<FBuildSession> Persistent;
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed || !State->Session)
				return FBuildCompleteParams::Canceled(std::nullopt, EBuildStatus::None);
			Persistent = State->Session;
		}
		FBuildRequestOwner Owner;
		Options.InputResolver = std::move(Request.Resolver);
		std::optional<FBuildCompleteParams> Completion;
		auto Admitted = Persistent->Build(std::move(Request.Definition), Owner, [&](auto Result) {
			Completion = std::move(Result);
		}, {}, std::move(Options));
		if (!Admitted) return FBuildCompleteParams::Error(std::nullopt, EBuildStatus::None);
		require(Owner.Wait() == EBuildWaitResult::Completed);
		if (!Completion) return FBuildCompleteParams::Error(std::nullopt, EBuildStatus::None);
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

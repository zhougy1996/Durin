#include "SlangModuleCache.h"

#include "Hash/CanonicalHash.h"
#include "slang-com-ptr.h"

namespace Durin
{
	namespace
	{
		constexpr size_t MaximumModules = 128;
		constexpr uint64 MaximumBytes = 32ull * 1024 * 1024;
		constexpr std::string_view SchemaPrefix = "DURIN_PARAMETER_SCHEMA";

		// These identity salts still reach Slang and the stage/DDC key. Share imported IR
		// across them only for plain source where no schema name can be observed or synthesized.
		auto MayReferenceSchema(std::string_view Text) -> bool
		{
			return Text.contains(SchemaPrefix) || Text.contains("##") || Text.contains("%:")
				|| Text.contains('\\') || Text.contains("??");
		}
		auto ObservesModuleFiles(std::string_view Text) -> bool
		{
			return Text.contains(".slang-module") || Text.contains("__has_include");
		}
	}

	auto FSlangModuleCache::MakeKey(const FShaderCompileOptions& Options, std::string_view SearchPath,
		std::string_view GeneratedSource) -> std::optional<FXxHash128>
	{
		if (!Options.SourceArtifacts || ObservesModuleFiles(GeneratedSource)) return {};
		FXxHash128Builder Hash;
		UpdateCanonicalHashString(Hash, Options.CompilerEnvironment);
		UpdateCanonicalHash(Hash, Options.Target.Platform);
		UpdateCanonicalHash(Hash, Options.Target.Backend);
		UpdateCanonicalHash(Hash, Options.Target.IntermediateFormat);
		UpdateCanonicalHash(Hash, Options.Target.OutputFormat);
		UpdateCanonicalHash(Hash, Options.Target.SettingsDigest);
		UpdateCanonicalHashString(Hash, SearchPath);
		const auto& Sources = *Options.SourceArtifacts;
		UpdateCanonicalHash(Hash, uint64(Sources.GetSearchRoots().size()));
		for (const auto& Root : Sources.GetSearchRoots()) UpdateCanonicalHashString(Hash, Root);

		// Include paths, contents and search order, including files that might newly shadow an import.
		bool KeepSchema = MayReferenceSchema(GeneratedSource);
		UpdateCanonicalHash(Hash, uint64(Sources.GetFiles().size()));
		for (const auto& [Path, Bytes] : Sources.GetFiles())
		{
			UpdateCanonicalHashString(Hash, Path);
			const std::string_view Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			if (Path.ends_with(".slang-module") || ObservesModuleFiles(Text)) return {};
			UpdateCanonicalHashString(Hash, Text);
			KeepSchema = KeepSchema || MayReferenceSchema(Text) || MayReferenceSchema(Path);
		}
		for (const auto& Macro : Options.Macros)
		{
			if (Macro.Value && ObservesModuleFiles(*Macro.Value)) return {};
			if (Macro.Value && MayReferenceSchema(*Macro.Value)) KeepSchema = true;
		}
		for (const auto& Macro : Options.Macros)
		{
			if (!KeepSchema && (Macro.Name == SchemaPrefix || Macro.Name.starts_with("DURIN_PARAMETER_SCHEMA_"))) continue;
			UpdateCanonicalHashString(Hash, Macro.Name);
			UpdateCanonicalHash(Hash, Macro.HasValue());
			if (Macro.Value) UpdateCanonicalHashString(Hash, *Macro.Value);
		}
		return Hash.Finalize();
	}

	auto FSlangModuleCache::Find(FXxHash128 Key) const -> FModules
	{
		FModules Result;
		std::lock_guard Lock(Mutex);
		for (const auto& Entry : Entries)
			if (Entry.Key == Key) Result.emplace(Entry.Path, Entry.IR);
		return Result;
	}

	auto FSlangModuleCache::Store(FXxHash128 Key, slang::ISession& Session, slang::IModule& Root,
		const FShaderSourceArtifacts& Sources, const FModules& Offered) -> void
	{
		for (SlangInt Index = 0; Index < Session.getLoadedModuleCount(); ++Index)
		{
			auto* Module = Session.getLoadedModule(Index);
			if (!Module || Module == &Root || !Module->getFilePath()) continue;
			std::filesystem::path Path(Module->getFilePath());
			Path = Path.lexically_normal();
			if (Path.extension() != ".slang" || !Sources.GetFiles().contains(Path.generic_string())) continue;
			auto BinaryPath = Path.replace_extension(".slang-module").generic_string();
			if (Offered.contains(BinaryPath) || Sources.GetFiles().contains(BinaryPath)) continue;

			Slang::ComPtr<slang::IBlob> Blob;
			if (SLANG_FAILED(Module->serialize(Blob.writeRef())) || !Blob || !Blob->getBufferSize()
				|| Blob->getBufferSize() > MaximumBytes) continue;
			// Only sealed imports can be published for another generated root.
			bool Captured = true;
			for (SlangInt32 File = 0; File < Module->getDependencyFileCount(); ++File)
			{
				const char* Dependency = Module->getDependencyFilePath(File);
				if (!Dependency) { Captured = false; break; }
				const auto Name = std::filesystem::path(Dependency).lexically_normal().generic_string();
				if (!Sources.GetFiles().contains(Name) && !Offered.contains(Name)) { Captured = false; break; }
			}
			if (!Captured) continue;
			const auto Bytes = FSharedByteBuffer::Copy({static_cast<const std::byte*>(Blob->getBufferPointer()), Blob->getBufferSize()});
			if (Bytes.GetRetainedCapacityBytes() > MaximumBytes) continue;
			std::lock_guard Lock(Mutex);
			if (std::ranges::any_of(Entries, [&](const auto& E) { return E.Key == Key && E.Path == BinaryPath; })) continue;
			while (!Entries.empty() && (Entries.size() >= MaximumModules || RetainedBytes + Bytes.GetRetainedCapacityBytes() > MaximumBytes))
			{
				RetainedBytes -= Entries.front().IR.GetRetainedCapacityBytes();
				Entries.pop_front();
			}
			Entries.push_back({Key, std::move(BinaryPath), Bytes});
			RetainedBytes += Bytes.GetRetainedCapacityBytes();
		}
	}
}

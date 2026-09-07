#include "SlangSessionEnvironment.h"

namespace Durin
{
	namespace
	{
		// Slang owns a reference for its session; no request falls through to disk.
		class FArtifactFileSystem final : public ISlangFileSystemExt
		{
		public:
			explicit FArtifactFileSystem(std::shared_ptr<const FShaderSourceArtifacts> InArtifacts)
				: Artifacts(std::move(InArtifacts)) {}

			auto SLANG_MCALL castAs(const SlangUUID& Id) noexcept -> void* override
			{
				if (std::memcmp(&Id, &UnknownId, sizeof(Id)) == 0
					|| std::memcmp(&Id, &CastableId, sizeof(Id)) == 0
					|| std::memcmp(&Id, &FileSystemId, sizeof(Id)) == 0
					|| std::memcmp(&Id, &ExtendedId, sizeof(Id)) == 0)
					return static_cast<ISlangFileSystem*>(this);
				return nullptr;
			}
			auto SLANG_MCALL queryInterface(const SlangUUID& Id, void** Out) noexcept -> SlangResult override
			{
				*Out = castAs(Id);
				if (!*Out) return SLANG_E_NO_INTERFACE;
				addRef();
				return SLANG_OK;
			}
			auto SLANG_MCALL addRef() noexcept -> uint32_t override { return ++References; }
			auto SLANG_MCALL release() noexcept -> uint32_t override
			{
				const auto Remaining = --References;
				if (!Remaining) delete this;
				return Remaining;
			}
			auto SLANG_MCALL loadFile(const char* Path, ISlangBlob** Out) noexcept -> SlangResult override
			{
				*Out = nullptr;
				try
				{
					const auto Found = Artifacts->GetFiles().find(
						std::filesystem::path(Path).lexically_normal().generic_string());
					if (Found == Artifacts->GetFiles().end()) return SLANG_E_NOT_FOUND;
					*Out = slang_createBlob(Found->second.data(), Found->second.size());
					return *Out ? SLANG_OK : SLANG_FAIL;
				}
				catch (...) { return SLANG_FAIL; }
			}

			auto SLANG_MCALL getFileUniqueIdentity(const char* Path, ISlangBlob** Out) noexcept -> SlangResult override
			{
				SlangPathType Type;
				if (SLANG_FAILED(getPathType(Path, &Type))) { *Out = nullptr; return SLANG_E_NOT_FOUND; }
				return getPath(PathKind::Canonical, Path, Out);
			}
			auto SLANG_MCALL calcCombinedPath(SlangPathType Type, const char* From,
				const char* Path, ISlangBlob** Out) noexcept -> SlangResult override
			{
				try
				{
					const std::filesystem::path Base(From);
					const auto Combined = ((Type == SLANG_PATH_TYPE_FILE ? Base.parent_path() : Base)
						/ Path).lexically_normal().generic_string();
					*Out = slang_createBlob(Combined.c_str(), Combined.size() + 1);
					return *Out ? SLANG_OK : SLANG_FAIL;
				}
				catch (...) { *Out = nullptr; return SLANG_FAIL; }
			}
			auto SLANG_MCALL getPathType(const char* Path, SlangPathType* Out) noexcept -> SlangResult override
			{
				try
				{
					const auto Normalized = std::filesystem::path(Path).lexically_normal().generic_string();
					if (Artifacts->GetFiles().contains(Normalized)) { *Out = SLANG_PATH_TYPE_FILE; return SLANG_OK; }
					const auto Prefix = Normalized.ends_with('/') ? Normalized : Normalized + '/';
					const auto Found = Artifacts->GetFiles().lower_bound(Prefix);
					if (Found != Artifacts->GetFiles().end() && Found->first.starts_with(Prefix))
					{ *Out = SLANG_PATH_TYPE_DIRECTORY; return SLANG_OK; }
					return SLANG_E_NOT_FOUND;
				}
				catch (...) { return SLANG_FAIL; }
			}
			auto SLANG_MCALL getPath(PathKind Kind, const char* Path, ISlangBlob** Out) noexcept -> SlangResult override
			{
				*Out = nullptr;
				if (Kind == PathKind::OperatingSystem) return SLANG_E_NOT_IMPLEMENTED;
				try
				{
					const auto Normalized = std::filesystem::path(Path).lexically_normal().generic_string();
					*Out = slang_createBlob(Normalized.c_str(), Normalized.size() + 1);
					return *Out ? SLANG_OK : SLANG_FAIL;
				}
				catch (...) { return SLANG_FAIL; }
			}
			auto SLANG_MCALL clearCache() noexcept -> void override {}
			auto SLANG_MCALL enumeratePathContents(const char*, FileSystemContentsCallBack, void*) noexcept -> SlangResult override
			{ return SLANG_E_NOT_IMPLEMENTED; }
			auto SLANG_MCALL getOSPathKind() noexcept -> OSPathKind override { return OSPathKind::None; }

		private:
			inline static const auto UnknownId = ISlangUnknown::getTypeGuid();
			inline static const auto CastableId = ISlangCastable::getTypeGuid();
			inline static const auto FileSystemId = ISlangFileSystem::getTypeGuid();
			inline static const auto ExtendedId = ISlangFileSystemExt::getTypeGuid();
			std::atomic<uint32_t> References{1};
			std::shared_ptr<const FShaderSourceArtifacts> Artifacts;
		};
	}

	auto FSlangSessionEnvironment::NormalizeMacros(
		const FShaderCompileOptions& Options,
		std::vector<FShaderMacroDefinition>& OutMacros,
		std::string& OutErrorMessage
	) -> bool
	{
		OutMacros = Options.Macros;
		std::ranges::sort(OutMacros, [](const FShaderMacroDefinition& A, const FShaderMacroDefinition& B) {
			if (A.Name != B.Name)
			{
				return A.Name < B.Name;
			}
			if (A.HasValue() != B.HasValue())
			{
				return !A.HasValue();
			}
			return A.Value < B.Value;
		});

		for (size_t Index = 1; Index < OutMacros.size(); ++Index)
		{
			if (OutMacros[Index - 1].Name == OutMacros[Index].Name)
			{
				OutErrorMessage = std::format("Duplicate shader macro definition is not allowed: {}", OutMacros[Index].Name);
				return false;
			}
		}

		return true;
	}

	auto FSlangSessionEnvironment::CreateSession(
		slang::IGlobalSession& GlobalSession,
		const FShaderCompileOptions& Options,
		Slang::ComPtr<slang::ISession>& OutSession,
		std::string& OutErrorMessage,
		std::string_view SearchPath
	) -> bool
	{
		if (Options.SourceArtifacts)
		{
			const auto& Files = Options.SourceArtifacts->GetFiles();
			uint64 TotalBytes = 0;
			if (Files.size() > 65536)
			{
				OutErrorMessage = "Captured shader file count exceeds the limit.";
				return false;
			}
			for (const auto& [Path, Bytes] : Files)
			{
				if (Path.empty() || Path.size() > 4096 || Path.find('\0') != std::string::npos
					|| Path != std::filesystem::path(Path).lexically_normal().generic_string()
					|| Bytes.size() > 64ull * 1024 * 1024
					|| (TotalBytes += Bytes.size()) > 512ull * 1024 * 1024)
				{
					OutErrorMessage = "Captured shader paths or bytes exceed canonical input limits.";
					return false;
				}
			}
		}
		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (!NormalizeMacros(Options, NormalizedMacros, OutErrorMessage))
		{
			return false;
		}

		std::vector<slang::PreprocessorMacroDesc> SlangMacros;
		SlangMacros.reserve(NormalizedMacros.size());
		for (const FShaderMacroDefinition& Macro : NormalizedMacros)
		{
			slang::PreprocessorMacroDesc MacroDesc = {};
			MacroDesc.name = Macro.Name.c_str();
			MacroDesc.value = Macro.Value ? Macro.Value->c_str() : nullptr;
			SlangMacros.push_back(MacroDesc);
		}

		slang::TargetDesc TargetDesc = {};
		TargetDesc.format = TargetFormat;
		TargetDesc.profile = GlobalSession.findProfile(TargetProfileName.data());

		Slang::ComPtr<ISlangFileSystem> FileSystem;
		if (Options.SourceArtifacts)
			FileSystem.attach(new FArtifactFileSystem(Options.SourceArtifacts));
		slang::SessionDesc SessionDesc = {};
		SessionDesc.fileSystem = FileSystem.get();
		SessionDesc.targets = &TargetDesc;
		SessionDesc.targetCount = 1;
		SessionDesc.preprocessorMacros = SlangMacros.empty() ? nullptr : SlangMacros.data();
		SessionDesc.preprocessorMacroCount = static_cast<SlangInt>(SlangMacros.size());

		std::vector<std::string> SearchPaths;
		if (!SearchPath.empty()) SearchPaths.emplace_back(SearchPath);
		if (Options.SourceArtifacts)
		{
			const auto& Roots = Options.SourceArtifacts->GetSearchRoots();
			if (Roots.size() > 256)
			{ OutErrorMessage = "Captured shader search root limit exceeded."; return false; }
			for (const auto& Root : Roots)
			{
				if (Root.empty() || Root.size() > 4096 || Root.find('\0') != std::string::npos)
				{ OutErrorMessage = "Invalid captured shader search root."; return false; }
				SearchPaths.push_back(Root);
			}
		}
		std::vector<const char*> SearchPathPointers;
		for (const auto& Path : SearchPaths) SearchPathPointers.push_back(Path.c_str());
		SessionDesc.searchPaths = SearchPathPointers.data();
		SessionDesc.searchPathCount = SearchPathPointers.size();

		if (SLANG_FAILED(GlobalSession.createSession(SessionDesc, OutSession.writeRef())))
		{
			OutErrorMessage = "createSession failed";
			return false;
		}

		return true;
	}
} // namespace Durin

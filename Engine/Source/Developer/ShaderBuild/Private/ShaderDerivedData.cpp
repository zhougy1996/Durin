#include "ShaderDerivedData.h"

namespace Durin::ShaderDerivedData
{
	namespace
	{
		auto IsValidRequest(const FShaderCompileOptions& Options) -> bool
		{
			if (Options.EntryPoints.empty()
				|| Options.EntryPoints.size() != Options.Frequencies.size()
				|| Options.EntryPoints.size() > MaximumEntryPoints) return false;
			std::set<std::pair<std::string_view, uint32>> Entries;
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				const std::string_view Entry = Options.EntryPoints[Index]
					? std::string_view(Options.EntryPoints[Index]) : std::string_view{};
				const uint32 Frequency =
					static_cast<uint32>(Options.Frequencies[Index]);
				if (Entry.empty()
					|| Frequency > static_cast<uint32>(EShaderFrequency::RayMiss)
					|| !Entries.emplace(Entry, Frequency).second) return false;
			}
			return true;
		}
	}

	auto MakeBuildDefinition(const FShaderVariantKey& VariantKey, const FShaderCompileOptions& Options)
		-> std::expected<DerivedData::FBuildDefinition, FShaderError>
	{
		using namespace DerivedData;
		if (VariantKey.Value.IsZero() || !IsValidRequest(Options))
			return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		std::vector<FBuildConstant> Constants{{"EntryCount", uint64(Options.EntryPoints.size())}};
		for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
		{
			Constants.push_back({std::format("Entry{}.Name", Index), std::string(Options.EntryPoints[Index])});
			Constants.push_back({std::format("Entry{}.Frequency", Index), uint64(Options.Frequencies[Index])});
		}
		auto Definition = FBuildDefinition::TryCreate({"Durin.Shader.Compile", BuilderVersion, 1,
			"Shader.CompiledOutput", PayloadSchemaVersion, FCacheBucket::FromString("Shaders/CompiledOutput")},
			std::move(Constants), {{"Variant", VariantKey.Value, "ShaderVariant", 6, "Shader.SourceClosure", 1}});
		if (!Definition) return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		return std::move(*Definition);
	}

	auto BuildKey(const FShaderVariantKey& VariantKey, const FShaderCompileOptions& Options) -> DerivedData::FCacheKey
	{
		auto Definition = MakeBuildDefinition(VariantKey, Options);
		return Definition ? Definition->GetKey() : DerivedData::FCacheKey{};
	}
}

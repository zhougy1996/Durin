#include "Renderers/MeshVertexFactory.h"
#include "Renderers/MeshRendererShared.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin::RendererPrivate
{
	namespace
	{
		class FGBufferLocalVertexShader final : public FMeshMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferLocalVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MESH_MATERIAL_SHADER(FGBufferLocalVertexShader, FMeshMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FGBufferSplineVertexShader final : public FMeshMaterialShader
		{
		public:
			static auto ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& Options) -> void
			{
				FSplineMeshVertexShader::ModifyCompilationEnvironment(Parameters, Options);
			}
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferSplineVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(SplineMesh);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MESH_MATERIAL_SHADER(FGBufferSplineVertexShader, FMeshMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex,
				"VertexMain");
		};

		DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FGBufferLocalVertexShader);
		DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FGBufferSplineVertexShader);

		template <typename TShader, bool bSpline>
		class TMeshVertexShaderBinding final : public FMeshVertexShaderBinding
		{
		public:
			explicit TMeshVertexShaderBinding(const FMaterialShaderMap& Map) : Shader(Map) {}
			auto GetRHIShader(bool bRequired) const -> FRHIShader* override
			{
				return Shader.GetRHIShader(bRequired);
			}
			auto Bind(FRHICommandListImmediate& CommandList,
				const FRHIUniformBufferRange& Transform,
				const FVertexFactoryBinding& Binding) const -> bool override
			{
				typename TShader::FParameters Parameters;
				Parameters.Transform = Transform;
				if constexpr (bSpline)
				{
					const auto* Spline = dynamic_cast<const FSplineMeshBatchBinding*>(&Binding);
					if (!Spline || Spline->GetLayoutKey() != FSplineMeshBatchBinding{}.GetLayoutKey()) return false;
					const auto Uniform = MakeSplineMeshUniform(Spline->DynamicData.Params);
					Parameters.SplineMesh = CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
				}
				else if (Binding.GetLayoutKey() != FStaticMeshBatchBinding{}.GetLayoutKey()) return false;
				SetShaderParameters(CommandList, Shader, Parameters);
				return true;
			}
		private:
			TMaterialShaderRef<TShader> Shader;
		};

		template <bool bSpline>
		class TMeshVertexFactoryImplementation final : public FMeshVertexFactoryImplementation
		{
		public:
			auto GetType() const -> const FVertexFactoryType& override
			{
				if constexpr (bSpline) return GetSplineVertexFactoryShaderType();
				else return GetLocalVertexFactoryShaderType();
			}
			auto GetLayoutKey() const -> FXxHash64 override
			{
				if constexpr (bSpline) return FSplineMeshBatchBinding{}.GetLayoutKey();
				else return FStaticMeshBatchBinding{}.GetLayoutKey();
			}
			auto GetShaderType(uint32 Pass) const -> FShaderType* override
			{
				if (Pass > MaterialMeshPassShadow) return nullptr;
				if constexpr (bSpline)
					return Pass == MaterialMeshPassGBuffer ? &FGBufferSplineVertexShader::StaticType() : &FSplineMeshVertexShader::StaticType();
				else
					return Pass == MaterialMeshPassGBuffer ? &FGBufferLocalVertexShader::StaticType() : &FStaticMeshVertexShader::StaticType();
			}
			auto Resolve(const FMaterialShaderMap& Map, uint32 Pass) const
				-> std::shared_ptr<const FMeshVertexShaderBinding> override
			{
				if (!GetShaderType(Pass)) return {};
				if constexpr (bSpline)
				{
					if (Pass == MaterialMeshPassGBuffer) return std::make_shared<TMeshVertexShaderBinding<FGBufferSplineVertexShader, true>>(Map);
					return std::make_shared<TMeshVertexShaderBinding<FSplineMeshVertexShader, true>>(Map);
				}
				else
				{
					if (Pass == MaterialMeshPassGBuffer) return std::make_shared<TMeshVertexShaderBinding<FGBufferLocalVertexShader, false>>(Map);
					return std::make_shared<TMeshVertexShaderBinding<FStaticMeshVertexShader, false>>(Map);
				}
			}
		};

		auto RegistryMutex() -> std::mutex&
		{
			static std::mutex Mutex;
			return Mutex;
		}

		struct FRegisteredFactory
		{
			std::shared_ptr<const FMeshVertexFactoryImplementation> Implementation;
			std::vector<FShaderRequestRegistration> Requests;
		};

		auto MakeRegistration(std::shared_ptr<const FMeshVertexFactoryImplementation> Implementation)
			-> FRegisteredFactory
		{
			FRegisteredFactory Result;
			for (uint32 Pass = MaterialMeshPassForward; Pass <= MaterialMeshPassShadow; ++Pass)
			{
				const FShaderType* Shader = Implementation->GetShaderType(Pass);
				if (!Shader) continue;
				FShaderRuntimeRequest Request;
				Request.Category = EShaderRuntimeRequestCategory::FeatureProgram;
				Request.Owner = "MeshVertexFactory";
				Request.Name = Implementation->GetRuntimeRequestName(Pass);
				auto Registration = RegisterShaderRuntimeRequest(std::move(Request),
					EShaderRequestEligibility::GameAndEditor, std::span(&Shader, 1));
				if (!Registration.IsValid()) return {};
				Result.Requests.push_back(std::move(Registration));
			}
			if (!Result.Requests.empty()) Result.Implementation = std::move(Implementation);
			return Result;
		}

		auto Registry() -> std::vector<FRegisteredFactory>&
		{
			static std::vector<FRegisteredFactory> Types = [] {
				std::vector<FRegisteredFactory> Result;
				Result.push_back(MakeRegistration(std::make_shared<TMeshVertexFactoryImplementation<false>>()));
				Result.push_back(MakeRegistration(std::make_shared<TMeshVertexFactoryImplementation<true>>()));
				requiref(Result[0].Implementation && Result[1].Implementation, "Built-in mesh factories must register before shader inventory freeze.");
				return Result;
			}();
			return Types;
		}
		// Library initialization contributes built-ins even when no frame is rendered.
		const bool bRegisteredBuiltins = !Registry().empty();
	}

	auto RegisterMeshVertexFactory(std::shared_ptr<const FMeshVertexFactoryImplementation> Implementation) -> bool
	{
		if (!Implementation || Implementation->GetLayoutKey().IsZero()) return false;
		const auto Key = Implementation->GetType().GetStableKey();
		if (Key.IsZero()) return false;
		std::lock_guard Lock(RegistryMutex());
		if (std::ranges::any_of(Registry(), [Key](const auto& Type) { return Type.Implementation->GetType().GetStableKey() == Key; })) return false;
		auto Registration = MakeRegistration(std::move(Implementation));
		if (!Registration.Implementation) return false;
		Registry().push_back(std::move(Registration));
		return true;
	}

	auto FindMeshVertexFactory(FXxHash64 FactoryKey) -> std::shared_ptr<const FMeshVertexFactoryImplementation>
	{
		std::lock_guard Lock(RegistryMutex());
		for (const auto& Type : Registry())
			if (Type.Implementation->GetType().GetStableKey() == FactoryKey) return Type.Implementation;
		return {};
	}
}

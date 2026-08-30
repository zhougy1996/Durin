#include "RDG.h"

#include "RHICommandList.h"
#include "Shader/Shader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>

namespace Durin
{
	namespace
	{
		struct FTimingSamples final
		{
			std::vector<uint64> Allocation;
			std::vector<uint64> AddPass;
			std::vector<uint64> Compile;
			std::vector<uint64> Resolve;
			std::vector<uint64> Compose;
			std::vector<uint64> Execute;
		};

		template<size_t Count>
		struct TBufferParameters final
		{
			std::array<FRDGBufferParameter, Count> Buffers;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGShaderResourceParameterMemberMetadata<
						TBufferParameters, decltype(Buffers),
						FRDGBufferParameter>("Buffers",
							offsetof(TBufferParameters, Buffers),
							ERDGParameterMemberKind::Buffer,
							ERDGResourceKind::Buffer,
							ERDGParameterRangeKind::BufferBytes,
							ERDGUse::Read, ERHIAccess::ComputeShaderRead,
							ERHIBindingType::StorageBuffer),
				};
				static constexpr const char* Name = Count == 1
					? "FBufferParameters1" : Count == 32
						? "FBufferParameters32" : "FBufferParameters128";
				static const auto Metadata = MakeInlineRDGParametersMetadata<
					TBufferParameters>(Name, Members);
				return &Metadata;
			}
		};

		auto Nanoseconds(std::chrono::steady_clock::time_point Start) -> uint64
		{
			return static_cast<uint64>(std::chrono::duration_cast<
				std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start)
				.count());
		}

		auto MedianAndP95(std::vector<uint64> Samples)
			-> std::pair<uint64, uint64>
		{
			std::ranges::sort(Samples);
			const size_t P95 = std::min(Samples.size() - 1,
				(Samples.size() * 95 + 99) / 100 - 1);
			return {Samples[Samples.size() / 2], Samples[P95]};
		}

		auto StaticLayoutBytes(const FRDGParameterLayout& Layout) -> uint64
		{
			uint64 Bytes = sizeof(Layout);
			const auto AddVector = [&Bytes](const auto& Values) {
				using FValue = typename std::decay_t<decltype(Values)>::value_type;
				Bytes += Values.capacity() * sizeof(FValue);
			};
			AddVector(Layout.Leaves);
			AddVector(Layout.Elements);
			AddVector(Layout.OffsetIndex);
			AddVector(Layout.TextureElements);
			AddVector(Layout.BufferElements);
			AddVector(Layout.ValueElements);
			AddVector(Layout.TokenElements);
			AddVector(Layout.AttachmentElements);
			AddVector(Layout.ShaderBindings);
			const auto AddStringHeap = [&Bytes](const auto& Records,
				auto GetPath) {
				for (const auto& Record : Records)
				{
					const std::string& Path = GetPath(Record);
					const uintptr_t Data = reinterpret_cast<uintptr_t>(Path.data());
					const uintptr_t Begin = reinterpret_cast<uintptr_t>(&Path);
					if (Data < Begin || Data >= Begin + sizeof(Path))
						Bytes += Path.capacity() + 1;
				}
			};
			AddStringHeap(Layout.Leaves,
				[](const auto& Leaf) -> const std::string& { return Leaf.Path; });
			AddStringHeap(Layout.Elements,
				[](const auto& Element) -> const std::string& {
					return Element.FieldPath;
				});
			return Bytes;
		}

		template<size_t Count>
		auto Measure(FRHICommandListImmediate& CommandList) -> FTimingSamples
		{
			constexpr uint32 WarmupCount = 8;
			constexpr uint32 SampleCount = 64;
			FTimingSamples Result;
			for (uint32 Iteration = 0;
				Iteration < WarmupCount + SampleCount; ++Iteration)
			{
				auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
					"RDGParameterLayoutQualification", Count * 4, 4,
					EBufferUsageFlags::StructuredBuffer
						| EBufferUsageFlags::UnorderedAccess));
				auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(
					EShaderFrequency::Compute, FXxHash128{}));
				const std::array Bindings{FShaderParameterBinding{
					.Name = "Buffers",
					.Type = ERHIBindingType::StorageBuffer,
					.ArraySize = static_cast<uint32>(Count),
					.bGraphResource = true}};
				FRDGBuilder Builder;
				const auto Handle = Builder.RegisterExternalBuffer(Buffer,
					"QualificationBuffer", ERHIAccess::ComputeShaderRead,
					ERHIAccess::ComputeShaderRead);

				auto Started = std::chrono::steady_clock::now();
				auto Parameters = Builder.AllocParameters<TBufferParameters<Count>>();
				const uint64 Allocation = Nanoseconds(Started);
				if (!Parameters.IsValid())
				{
					ADD_FAILURE() << "parameter allocation failed";
					return {};
				}
				for (uint32 Index = 0; Index < Count; ++Index)
					Parameters->Buffers[Index] = {Handle, Index * 4, 4};

				uint64 Resolve = 0;
				uint64 Compose = 0;
				Started = std::chrono::steady_clock::now();
				Builder.AddPass("Qualification", ERDGPassType::Compute,
					std::move(Parameters),
					[&](FRHICommandListImmediate&,
						const TBufferParameters<Count>& Values,
						const FRDGParameterResolver& Resolver) {
						const auto ResolveStarted = std::chrono::steady_clock::now();
						for (const FRDGBufferParameter& Parameter : Values.Buffers)
							EXPECT_EQ(Resolver.GetBuffer(Parameter), Buffer.GetReference());
						Resolve = Nanoseconds(ResolveStarted);

						FRHICommandList Commands;
						Commands.SwitchPipeline(ERHIPipeline::Compute);
						const auto ComposeStarted = std::chrono::steady_clock::now();
						SetRDGShaderParametersImpl(Commands, Shader.GetReference(),
							"FRDGParameterLayoutQualificationShader",
							EShaderFrequency::Compute, Bindings,
							Resolver.GetShaderParameters(Values), nullptr, nullptr);
						Compose = Nanoseconds(ComposeStarted);
					});
				const uint64 AddPass = Nanoseconds(Started);

				Started = std::chrono::steady_clock::now();
				auto Compiled = Builder.Compile();
				const uint64 Compile = Nanoseconds(Started);
				if (!Compiled.IsSuccess())
				{
					ADD_FAILURE() << Compiled.Error;
					return {};
				}

				Started = std::chrono::steady_clock::now();
				EXPECT_TRUE(Compiled.Graph->Execute(CommandList));
				const uint64 Execute = Nanoseconds(Started);
				if (Iteration >= WarmupCount)
				{
					Result.Allocation.push_back(Allocation);
					Result.AddPass.push_back(AddPass);
					Result.Compile.push_back(Compile);
					Result.Resolve.push_back(Resolve);
					Result.Compose.push_back(Compose);
					Result.Execute.push_back(Execute);
				}
			}
			return Result;
		}

		template<size_t Count>
		auto Report(FRHICommandListImmediate& CommandList)
			-> void
		{
			const FTimingSamples Samples = Measure<Count>(CommandList);
			const auto Allocation = MedianAndP95(Samples.Allocation);
			const auto AddPass = MedianAndP95(Samples.AddPass);
			const auto Compile = MedianAndP95(Samples.Compile);
			const auto Resolve = MedianAndP95(Samples.Resolve);
			const auto Compose = MedianAndP95(Samples.Compose);
			const auto Execute = MedianAndP95(Samples.Execute);
			const FRDGParameterLayout* Layout =
				GetRDGParameterLayout<TBufferParameters<Count>>();
			ASSERT_NE(Layout, nullptr);
			std::cout << "RDG parameter layout qualification: elements=" << Count
				<< ",warmup=8,samples=64,allocation_median_ns=" << Allocation.first
				<< ",allocation_p95_ns=" << Allocation.second
				<< ",add_pass_median_ns=" << AddPass.first
				<< ",add_pass_p95_ns=" << AddPass.second
				<< ",compile_median_ns=" << Compile.first
				<< ",compile_p95_ns=" << Compile.second
				<< ",resolve_median_ns=" << Resolve.first
				<< ",resolve_p95_ns=" << Resolve.second
				<< ",compose_median_ns=" << Compose.first
				<< ",compose_p95_ns=" << Compose.second
				<< ",execute_median_ns=" << Execute.first
				<< ",execute_p95_ns=" << Execute.second
				<< ",static_layout_bytes=" << StaticLayoutBytes(*Layout) << '\n';
		}
	} // namespace

	TEST(FRDGParameterLayoutQualificationTests, MeasuresDeclarationCompileAndResolve)
	{
		FRHICommandListExecutor Executor;
		auto& CommandList = Executor.GetImmediateCommandList();
		Report<1>(CommandList);
		Report<32>(CommandList);
		Report<128>(CommandList);
	}
} // namespace Durin

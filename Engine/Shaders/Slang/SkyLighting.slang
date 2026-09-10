import ProceduralSky;

struct FSkyLightingUniform
{
    FProceduralSkyUniform Sky;
    uint4 Dispatch; // operation, face, dimension, sample count
    float4 Filter; // roughness, source dimension, source mip count, reserved
};
[[vk::binding(0, 0)]] TextureCube<float4> Source;
[[vk::binding(1, 0)]] SamplerState SourceSampler;
[[vk::binding(2, 0)]] ConstantBuffer<FSkyLightingUniform> Params;
[[vk::binding(3, 0)]] RWTexture2D<float4> Output;

static const float Pi = 3.141592653589793;
float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), float(reversebits(i)) * 2.3283064365386963e-10);
}
float3 Orient(float3 d, float3 n)
{
    float3 t = normalize(cross(abs(n.z) < 0.999 ? float3(0,0,1) : float3(1,0,0), n));
    return t * d.x + cross(n,t) * d.y + n * d.z;
}
float3 SampleGGX(float2 xi, float roughness, float3 n)
{
    float a = roughness * roughness;
    float cosTheta = sqrt((1.0-xi.y) / (1.0 + (a*a-1.0)*xi.y));
    float sinTheta = sqrt(max(1.0-cosTheta*cosTheta,0.0));
    float phi = 2.0 * Pi * xi.x;
    return Orient(float3(cos(phi)*sinTheta,sin(phi)*sinTheta,cosTheta),n);
}
float Geometry(float NoX, float roughness)
{
    float k = roughness * roughness * 0.5;
    return NoX / max(NoX * (1.0-k)+k,1.0e-6);
}

[shader("compute")]
[numthreads(8,8,1)]
void ComputeMain(uint3 tid : SV_DispatchThreadID)
{
    uint dimension = Params.Dispatch.z;
    if (any(tid.xy >= dimension)) return;
    float2 uv = (float2(tid.xy)+0.5)/float(dimension);
    uint operation = Params.Dispatch.x;
    uint samples = Params.Dispatch.w;
    float3 n = SkyCubeDirection(Params.Dispatch.y, uv);
    float3 result = 0.0;
    if (operation == 0)
    {
        // Each ordinary mip integrates the same frozen analytic radiance.
        float weight = 0.0;
        for (uint y=0; y<4; ++y)
        for (uint x=0; x<4; ++x)
        {
            float2 sampleUV = (float2(tid.xy)+(float2(x,y)+0.5)/4.0)/float(dimension);
            float2 plane = sampleUV*2.0-1.0;
            float solidAngle = pow(1.0+dot(plane,plane),-1.5);
            result += EvaluateProceduralSky(Params.Sky, SkyCubeDirection(Params.Dispatch.y,sampleUV))*solidAngle;
            weight += solidAngle;
        }
        result /= weight;
    }
    else if (operation == 1)
    {
        for (uint i=0; i<samples; ++i)
        {
            float2 xi=Hammersley(i,samples);
            float r=sqrt(xi.y), phi=2.0*Pi*xi.x;
            float3 l=Orient(float3(r*cos(phi),r*sin(phi),sqrt(1.0-xi.y)),n);
            result += Source.SampleLevel(SourceSampler,l,0).rgb;
        }
        result *= Pi / float(samples);
    }
    else if (operation == 2)
    {
        float roughness=Params.Filter.x;
        if (roughness == 0.0) result=Source.SampleLevel(SourceSampler,n,0).rgb;
        else
        {
            float weight=0;
            for(uint i=0;i<samples;++i)
            {
                float3 h=SampleGGX(Hammersley(i,samples),roughness,n);
                float3 l=normalize(2.0*dot(n,h)*h-n);
                float NoL=saturate(dot(n,l));
                if(NoL>0)
                {
                    float NoH=saturate(dot(n,h));
                    float a=roughness*roughness;
                    float d=a*a/(Pi*pow(NoH*NoH*(a*a-1.0)+1.0,2.0));
                    float pdf=max(d*0.25,1.0e-6);
                    float omegaSample=1.0/(float(samples)*pdf);
                    float omegaTexel=4.0*Pi/(6.0*Params.Filter.y*Params.Filter.y);
                    float mip=clamp(0.5*log2(omegaSample/omegaTexel),0.0,Params.Filter.z-1.0);
                    result+=Source.SampleLevel(SourceSampler,l,mip).rgb*NoL;
                    weight+=NoL;
                }
            }
            result/=max(weight,1.0e-6);
        }
    }
    else
    {
        float NoV=uv.x, roughness=uv.y;
        float3 v=float3(sqrt(1.0-NoV*NoV),0,NoV);
        float2 integral=0;
        for(uint i=0;i<samples;++i)
        {
            float3 h=SampleGGX(Hammersley(i,samples),roughness,float3(0,0,1));
            float3 l=normalize(2.0*dot(v,h)*h-v);
            float NoL=saturate(l.z), NoH=saturate(h.z), VoH=saturate(dot(v,h));
            if(NoL>0)
            {
                float vis=Geometry(NoV,roughness)*Geometry(NoL,roughness)*VoH/max(NoH*NoV,1.0e-6);
                float fc=pow(1.0-VoH,5.0);
                integral+=float2((1.0-fc)*vis,fc*vis);
            }
        }
        result=float3(integral/float(samples),0);
    }
    Output[tid.xy]=float4(result,1);
}
